#include "runtime_shaders.hpp"
#include "runtime_shader_text.h"
#include <SPIRV/GlslangToSpv.h>
#include <glslang/Include/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <algorithm>
#include <cctype>
#include <tuple>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace lindbergh::vk {
namespace {

// ---- the lowering: tools/vulkan/compile_shaders.py, statement for statement ----

const std::regex &decl_regex() {
    static const std::regex r(R"(\b(attribute|varying)\s+(\w+)\s+(\w+)\s*(\[\s*(\d+)\s*\])?\s*;)");
    return r;
}
const std::regex &sampler_regex() {
    static const std::regex r(R"(\buniform\s+(sampler\w+)\s+(\w+)\s*(\[[^\]]+\])?\s*;)");
    return r;
}
struct Builtin { const char *name, *type; uint32_t location; };
const std::vector<Builtin> &builtin_inputs() {
    static const std::vector<Builtin> b={{"gl_Vertex","vec4",0},{"gl_Normal","vec3",2},{"gl_Color","vec4",3},{"gl_SecondaryColor","vec4",4},
        {"gl_MultiTexCoord0","vec4",8},{"gl_MultiTexCoord1","vec4",9},{"gl_MultiTexCoord2","vec4",10},{"gl_MultiTexCoord3","vec4",11},
        {"gl_MultiTexCoord4","vec4",12},{"gl_MultiTexCoord5","vec4",13},{"gl_MultiTexCoord6","vec4",14},{"gl_MultiTexCoord7","vec4",15}};
    return b;
}
struct LegacyVarying { const char *builtin, *name, *type; int count; bool array; };
const LegacyVarying legacy_varyings[]={{"gl_TexCoord","legacy_texcoord","vec4",8,true},{"gl_FogFragCoord","legacy_fog_coord","float",1,false}};
const std::pair<const char *,const char *> state_names[]={
    {"gl_ModelViewMatrix","legacy.modelview"},{"gl_ProjectionMatrix","legacy.projection"},
    {"gl_ModelViewProjectionMatrix","(legacy.projection * legacy.modelview)"},
    {"gl_TextureMatrix","legacy.texture_matrix"},{"gl_NormalMatrix","legacy.normal_matrix"},
    {"gl_NormalScale","legacy.normal_scale"},{"gl_FrontMaterial","legacy.front_material"},
    {"gl_LightSource","legacy.light_source"},{"gl_Fog","legacy.fog"},
    {"gl_FrontLightModelProduct","legacy.front_light_model"},{"gl_DepthRange","legacy.depth_range"}};

bool word_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c=='_'; }
// re.search(r"\b" + word + r"\b", s)
bool has_word(const std::string &s, const std::string &word) {
    for(size_t at=s.find(word);at!=std::string::npos;at=s.find(word,at+1))
        if((at==0||!word_char(s[at-1]))&&(at+word.size()==s.size()||!word_char(s[at+word.size()])))return true;
    return false;
}
// re.sub with a function
std::string substitute(const std::string &s, const std::regex &r, const std::function<std::string(const std::smatch &)> &fn) {
    std::string out;auto last=s.cbegin();
    for(std::sregex_iterator it(s.begin(),s.end(),r),end;it!=end;++it) {
        out.append(last,(*it)[0].first);out+=fn(*it);last=(*it)[0].second;
    }
    out.append(last,s.cend());return out;
}
// re.sub(r"\b(?:k1|k2|...)\b", lambda m: table[m[0]], s): every key is an
// identifier, so this is a lookup of each whole identifier token.
std::string replace_words(const std::string &s, const std::unordered_map<std::string,std::string> &table) {
    std::string out;out.reserve(s.size()+s.size()/4);
    for(size_t i=0;i<s.size();) {
        if(word_char(s[i])&&(i==0||!word_char(s[i-1]))) {
            size_t j=i;while(j<s.size()&&word_char(s[j]))++j;
            const auto token=s.substr(i,j-i);const auto found=table.find(token);
            out+=found==table.end()?token:found->second;i=j;
        } else out+=s[i++];
    }
    return out;
}

struct Decl { std::string type; int count; bool array; };
std::map<std::string,Decl> declarations(const std::string &source, const char *kind) {
    std::map<std::string,Decl> result;
    for(std::sregex_iterator it(source.begin(),source.end(),decl_regex()),end;it!=end;++it) {
        const auto &m=*it;if(m[1]!=kind)continue;
        result[m[3]]={m[2],m[5].matched?std::stoi(m[5]):1,m[4].matched};
    }
    return result;
}
struct Varying { uint32_t location; std::string type; int count; bool array; };
using Interface=std::map<std::string,Varying>;
bool varyings(const std::string &vertex, const std::string &fragment, Interface &result, std::string &error) {
    auto all=declarations(vertex,"varying");
    for(const auto &[name,decl]:declarations(fragment,"varying")) {
        const auto found=all.find(name);
        if(found!=all.end()&&(found->second.type!=decl.type||found->second.count!=decl.count||found->second.array!=decl.array)) {
            error="Varying type mismatch: "+name;return false;
        }
        all[name]=decl;
    }
    for(const auto &v:legacy_varyings)
        if(has_word(vertex,v.builtin)||has_word(fragment,v.builtin))all[v.name]={v.type,v.count,v.array};
    uint32_t location=4;
    for(const auto &[name,decl]:all) { // sorted by name, as sorted(all_vars.items())
        uint32_t columns=1;
        if(decl.type.rfind("mat",0)==0&&std::isdigit(static_cast<unsigned char>(decl.type.back())))columns=uint32_t(decl.type.back()-'0');
        result[name]={location,decl.type,decl.count,decl.array};location+=columns*uint32_t(decl.count);
    }
    return true;
}

std::string strip_directives(const std::string &source) {
    // re.sub(r"^\s*#(?:version|extension|line)\b[^\n]*", "", source, flags=re.M)
    static const std::regex directive(R"(^\s*#\s*(version|extension|line)\b)");
    std::istringstream in(source);std::string line,out;
    while(std::getline(in,line)) {
        if(!std::regex_search(line,directive))out+=line;
        out+='\n';
    }
    return out;
}

const TBuiltInResource &limits() {
    static const TBuiltInResource r=[]{
        TBuiltInResource r{};
        r.maxLights=32;r.maxClipPlanes=6;r.maxTextureUnits=32;r.maxTextureCoords=32;r.maxVertexAttribs=64;
        r.maxVertexUniformComponents=4096;r.maxVaryingFloats=64;r.maxVertexTextureImageUnits=32;r.maxCombinedTextureImageUnits=80;
        r.maxTextureImageUnits=32;r.maxFragmentUniformComponents=4096;r.maxDrawBuffers=32;r.maxVertexUniformVectors=128;
        r.maxVaryingVectors=8;r.maxFragmentUniformVectors=16;r.maxVertexOutputVectors=16;r.maxFragmentInputVectors=15;
        r.minProgramTexelOffset=-8;r.maxProgramTexelOffset=7;r.maxClipDistances=8;
        r.maxComputeWorkGroupCountX=r.maxComputeWorkGroupCountY=r.maxComputeWorkGroupCountZ=65535;
        r.maxComputeWorkGroupSizeX=1024;r.maxComputeWorkGroupSizeY=1024;r.maxComputeWorkGroupSizeZ=64;
        r.maxComputeUniformComponents=1024;r.maxComputeTextureImageUnits=16;r.maxComputeImageUniforms=8;
        r.maxComputeAtomicCounters=8;r.maxComputeAtomicCounterBuffers=1;r.maxVaryingComponents=60;
        r.maxVertexOutputComponents=64;r.maxGeometryInputComponents=64;r.maxGeometryOutputComponents=128;
        r.maxFragmentInputComponents=128;r.maxImageUnits=8;r.maxCombinedImageUnitsAndFragmentOutputs=8;
        r.maxCombinedShaderOutputResources=8;r.maxFragmentImageUniforms=8;r.maxCombinedImageUniforms=8;
        r.maxGeometryTextureImageUnits=16;r.maxGeometryOutputVertices=256;r.maxGeometryTotalOutputComponents=1024;
        r.maxGeometryUniformComponents=1024;r.maxGeometryVaryingComponents=64;r.maxTessControlInputComponents=128;
        r.maxTessControlOutputComponents=128;r.maxTessControlTextureImageUnits=16;r.maxTessControlUniformComponents=1024;
        r.maxTessControlTotalOutputComponents=4096;r.maxTessEvaluationInputComponents=128;r.maxTessEvaluationOutputComponents=128;
        r.maxTessEvaluationTextureImageUnits=16;r.maxTessEvaluationUniformComponents=1024;r.maxTessPatchComponents=120;
        r.maxPatchVertices=32;r.maxTessGenLevel=64;r.maxViewports=16;r.maxFragmentAtomicCounters=8;
        r.maxCombinedAtomicCounters=8;r.maxAtomicCounterBindings=1;r.maxFragmentAtomicCounterBuffers=1;
        r.maxCombinedAtomicCounterBuffers=1;r.maxAtomicCounterBufferSize=16384;r.maxTransformFeedbackBuffers=4;
        r.maxTransformFeedbackInterleavedComponents=64;r.maxCullDistances=8;r.maxCombinedClipAndCullDistances=8;
        r.maxSamples=4;r.maxDualSourceDrawBuffersEXT=1;
        auto &l=r.limits;
        l.nonInductiveForLoops=l.whileLoops=l.doWhileLoops=l.generalUniformIndexing=l.generalAttributeMatrixVectorIndexing=
            l.generalVaryingIndexing=l.generalSamplerIndexing=l.generalVariableIndexing=l.generalConstantMatrixVectorIndexing=true;
        return r;
    }();
    return r;
}

bool initialize() {
    static const bool ok=glslang::InitializeProcess();
    return ok;
}

// glslangValidator -E -S <stage>: the guest's own GLSL, macros expanded.
bool preprocess(const std::string &source, EShLanguage stage, std::string &out, std::string &error) {
    glslang::TShader shader(stage);
    const char *text=source.c_str();shader.setStrings(&text,1);
    glslang::TShader::ForbidIncluder includer;
    if(!shader.preprocess(&limits(),100,ENoProfile,false,false,EShMsgOnlyPreprocessor,&out,includer)) {
        error=std::string("preprocess: ")+shader.getInfoLog();return false;
    }
    out=strip_directives(out);
    return true;
}

bool convert(std::string source, const char *stage, const Interface &interface,
             const std::map<std::string,uint32_t> &attributes, const std::map<std::string,uint32_t> &samplers,
             std::string &out, std::string &error) {
    const bool vert=std::strcmp(stage,"vert")==0;
    std::vector<std::string> prefix={"#version 450","#extension GL_EXT_multiview : require"};
    const char *colors[]={"front_color","back_color","front_secondary","back_secondary"};
    for(int i=0;i<4;++i)
        prefix.push_back("layout(location="+std::to_string(i)+") "+(vert?"out":"in")+" vec4 legacy_"+colors[i]+";");
    if(!vert)prefix.push_back("layout(location=0) out vec4 legacy_frag_color;");
    bool failed=false;
    source=substitute(source,decl_regex(),[&](const std::smatch &m)->std::string {
        const std::string kind=m[1],type=m[2],name=m[3],array=m[4].matched?std::string(m[4]):std::string();
        if(kind=="varying") {
            const auto found=interface.find(name);
            if(found==interface.end()){failed=true;error="Varying outside the interface: "+name;return m[0];}
            return "layout(location="+std::to_string(found->second.location)+") "+(vert?"out ":"in ")+type+" "+name+array+";";
        }
        if(!vert){failed=true;error="Vertex attribute declared in fragment shader";return m[0];}
        const auto found=attributes.find(name);
        if(found==attributes.end()){failed=true;error="Missing explicit guest attribute binding: "+name;return m[0];}
        return "layout(location="+std::to_string(found->second)+") in "+type+" "+name+array+";";
    });
    if(failed)return false;
    for(const auto &v:legacy_varyings) {
        const auto found=interface.find(v.name);
        if(found!=interface.end())
            prefix.push_back("layout(location="+std::to_string(found->second.location)+") "+(vert?"out ":"in ")+v.type+" "+v.name+
                             (v.array?"["+std::to_string(v.count)+"]":"")+";");
    }
    source=substitute(source,sampler_regex(),[&](const std::smatch &m)->std::string {
        return "layout(set=2, binding="+std::to_string(samplers.at(m[2]))+") uniform "+std::string(m[1])+" "+std::string(m[2])+
               (m[3].matched?std::string(m[3]):std::string())+";";
    });
    std::unordered_map<std::string,std::string> replacements;
    for(const auto &[from,to]:state_names)replacements[from]=to;
    for(const auto &v:legacy_varyings)replacements[v.builtin]=v.name;
    if(vert) {
        std::map<uint32_t,std::string> declared; // {loc: n for n, loc in attributes.items() if n in source}
        for(const auto &[name,location]:attributes)if(has_word(source,name))declared[location]=name;
        for(const auto &b:builtin_inputs()) {
            if(!(has_word(source,b.name)||(std::strcmp(b.name,"gl_Vertex")==0&&has_word(source,"ftransform"))))continue;
            const auto found=declared.find(b.location);
            if(found!=declared.end()) {
                // NV vertex programs alias generic attribute N with a
                // conventional one; the runtime feeds both from array slot N.
                const std::string type=b.type;
                replacements[b.name]=type=="vec4"?found->second:found->second+"."+std::string("xyz").substr(0,size_t(type.back()-'0'));
                continue;
            }
            const std::string replacement=std::string("legacy_input_")+(b.name+3);
            prefix.push_back("layout(location="+std::to_string(b.location)+") in "+b.type+" "+replacement+";");
            replacements[b.name]=replacement;
        }
        replacements.insert({{"gl_FrontColor","legacy_front_color"},{"gl_BackColor","legacy_back_color"},
                             {"gl_FrontSecondaryColor","legacy_front_secondary"},{"gl_BackSecondaryColor","legacy_back_secondary"}});
    } else {
        replacements.insert({{"gl_Color","(gl_FrontFacing ? legacy_front_color : legacy_back_color)"},
                             {"gl_SecondaryColor","(gl_FrontFacing ? legacy_front_secondary : legacy_back_secondary)"},
                             {"gl_FragColor","legacy_frag_color"},{"gl_FragCoord","legacy_frag_coord()"}});
    }
    for(const auto &[from,to]:std::initializer_list<std::pair<const char *,const char *>>{
            {"sampler2D","sampler2DArray"},{"sampler2DShadow","sampler2DArrayShadow"},{"sampler2DRect","sampler2DArray"},
            {"texture2DRect","legacy_texture2DRect"},{"texture2DRectProj","legacy_texture2DRectProj"},{"texture2D","legacy_texture2D"},
            {"textureCube","texture"},{"texture3D","texture"},{"texture2DProj","legacy_texture2DProj"},{"texture2DLod","legacy_texture2DLod"},
            {"texture2DProjLod","legacy_texture2DProjLod"},{"ftransform","legacy_ftransform"},{"shadow2D","legacy_shadow2D"},
            {"shadow2DProj","legacy_shadow2DProj"}})
        replacements[from]=to;
    source=replace_words(source,replacements);
    std::string legacy=lvk_legacy_glsl;
    static const std::regex main_function(R"(\bvoid\s+main\s*\(\s*(?:void\s*)?\))");
    int mains=0;
    source=substitute(source,main_function,[&](const std::smatch &)->std::string{++mains;return "void guest_main()";});
    if(mains!=1){error=std::string("Expected exactly one ")+(vert?"vertex":"fragment")+" main function";return false;}
    if(vert) {
        legacy=legacy.substr(0,legacy.find("vec4 legacy_frag_coord()"));
        if(source.find("legacy_ftransform")!=std::string::npos)
            legacy+="vec4 legacy_ftransform() { return legacy.projection * legacy.modelview * legacy_input_Vertex; }\n";
        prefix.push_back("layout(push_constant) uniform EyeTransforms { mat4 reprojection[2]; } eyes;");
        source+=R"(
layout(constant_id=5) const bool legacy_full_hud=false;
void main() {
    legacy_front_color = vec4(0); legacy_back_color = vec4(0);
    legacy_front_secondary = vec4(0); legacy_back_secondary = vec4(0);
    guest_main();
    vec4 p = eyes.reprojection[gl_ViewIndex] * gl_Position;
    // Full-screen movie/fade layers retain HUD depth ordering. Their XY still
    // covers the eye instead of shrinking onto the finite HUD screen plane.
    // Specialization eliminates this work from ordinary world/HUD pipelines.
    if (legacy_full_hud && gl_Position.w != 0.0)
        p.xy = gl_Position.xy * (p.w / gl_Position.w);
    gl_Position = vec4(p.x, -p.y, (p.z + p.w) * 0.5, p.w);
}
)";
    } else {
        source+=R"(
layout(constant_id=1) const int legacy_alpha_func=0;
void main() {
    guest_main();
    float a=legacy_frag_color.a, r=legacy.alpha_ref;
    bool keep=legacy_alpha_func==0 || legacy_alpha_func==8 ||
        (legacy_alpha_func==2 && a<r) || (legacy_alpha_func==3 && a==r) ||
        (legacy_alpha_func==4 && a<=r) || (legacy_alpha_func==5 && a>r) ||
        (legacy_alpha_func==6 && a!=r) || (legacy_alpha_func==7 && a>=r);
    if(!keep) discard;
}
)";
    }
    std::string sampling=lvk_sampling_glsl;
    if(vert)sampling=sampling.substr(0,sampling.find("// FRAGMENT_BIAS_OVERLOADS"));
    out.clear();
    for(const auto &line:prefix)out+=line+"\n";
    out+=legacy+"\n"+sampling+"\n"+source;
    return true;
}

// glslangValidator -V -R --target-env vulkan1.1 --auto-map-bindings
//   --set-default-uniform-block GuestUniforms 1 0 -l guest.vert guest.frag
bool compile(const std::string &vertex, const std::string &fragment, std::vector<uint32_t> &vs,
             std::vector<uint32_t> &fs, std::string &error) {
    glslang::TShader vshader(EShLangVertex),fshader(EShLangFragment);
    const auto messages=EShMessages(EShMsgSpvRules|EShMsgVulkanRules);
    for(auto [shader,text]:{std::pair{&vshader,&vertex},std::pair{&fshader,&fragment}}) {
        const char *s=text->c_str();shader->setStrings(&s,1);
        shader->setEnvInput(glslang::EShSourceGlsl,shader==&vshader?EShLangVertex:EShLangFragment,glslang::EShClientVulkan,100);
        shader->setEnvClient(glslang::EShClientVulkan,glslang::EShTargetVulkan_1_1);
        shader->setEnvTarget(glslang::EShTargetSpv,glslang::EShTargetSpv_1_3);
        shader->setEnvInputVulkanRulesRelaxed();
        shader->setAutoMapBindings(true);
        shader->setGlobalUniformBlockName("GuestUniforms");shader->setGlobalUniformSet(1);shader->setGlobalUniformBinding(0);
        glslang::TShader::ForbidIncluder includer;
        if(!shader->parse(&limits(),100,false,messages,includer)) {
            error=std::string(shader==&vshader?"vertex: ":"fragment: ")+shader->getInfoLog();return false;
        }
    }
    glslang::TProgram program;program.addShader(&vshader);program.addShader(&fshader);
    if(!program.link(messages)||!program.mapIO()){error=std::string("link: ")+program.getInfoLog();return false;}
    glslang::SpvOptions options;options.disableOptimizer=true;options.validate=false;
    glslang::GlslangToSpv(*program.getIntermediate(EShLangVertex),vs,&options);
    glslang::GlslangToSpv(*program.getIntermediate(EShLangFragment),fs,&options);
    if(vs.size()<5||fs.size()<5){error="no SPIR-V";return false;}
    return true;
}

// ---- reflection: what spirv-cross --reflect gives pack_shaders.py ----

enum : uint32_t {
    OpName=5,OpMemberName=6,OpEntryPoint=15,OpTypeVoid=19,OpTypeBool=20,OpTypeInt=21,OpTypeFloat=22,OpTypeVector=23,
    OpTypeMatrix=24,OpTypeImage=25,OpTypeSampler=26,OpTypeSampledImage=27,OpTypeArray=28,OpTypeRuntimeArray=29,
    OpTypeStruct=30,OpTypePointer=32,OpConstant=43,OpSpecConstant=50,OpFunction=54,OpVariable=59,OpDecorate=71,OpMemberDecorate=72,
};
enum : uint32_t { DecBlock=2,DecRowMajor=4,DecArrayStride=6,DecMatrixStride=7,DecBuiltIn=11,DecLocation=30,DecBinding=33,DecDescriptorSet=34,DecOffset=35 };
enum : uint32_t { StorageUniformConstant=0,StorageInput=1,StorageUniform=2 };

struct Module {
    std::unordered_map<uint32_t,std::vector<uint32_t>> types; // id -> opcode, operands after the result id
    std::unordered_map<uint32_t,uint32_t> constants;
    std::unordered_map<uint32_t,std::string> names;
    std::unordered_map<uint32_t,std::map<uint32_t,std::string>> member_names;
    std::unordered_map<uint32_t,std::map<uint32_t,uint32_t>> decorations;
    std::unordered_map<uint32_t,std::map<uint32_t,std::map<uint32_t,uint32_t>>> member_decorations;
    struct Variable { uint32_t id, pointer, storage; };
    std::vector<Variable> variables;
    std::unordered_set<uint32_t> used;
    // Top-level members of a uniform block that code indexes; whole: it is
    // used other than through a constant first index (then all count).
    std::unordered_map<uint32_t,std::set<uint32_t>> members_used;
    std::unordered_set<uint32_t> whole_used;

    bool decorated(uint32_t id,uint32_t d) const {auto f=decorations.find(id);return f!=decorations.end()&&f->second.count(d);}
    uint32_t decoration(uint32_t id,uint32_t d,uint32_t fallback=0) const {
        auto f=decorations.find(id);if(f==decorations.end())return fallback;auto g=f->second.find(d);return g==f->second.end()?fallback:g->second;
    }
    const std::map<uint32_t,uint32_t> *member(uint32_t type,uint32_t index) const {
        auto f=member_decorations.find(type);if(f==member_decorations.end())return nullptr;
        auto g=f->second.find(index);return g==f->second.end()?nullptr:&g->second;
    }
    const std::vector<uint32_t> &type(uint32_t id) const {
        static const std::vector<uint32_t> none;auto f=types.find(id);return f==types.end()?none:f->second;
    }
};

bool parse(const std::vector<uint32_t> &words, Module &m, std::string &error) {
    if(words.size()<5||words[0]!=0x07230203u){error="not SPIR-V";return false;}
    std::unordered_set<uint32_t> globals;bool in_function=false;
    struct Code { uint32_t op; size_t from,to; };
    std::vector<Code> function_code;
    for(size_t at=5;at<words.size();) {
        const uint32_t count=words[at]>>16,op=words[at]&0xffffu;
        if(!count||at+count>words.size()){error="truncated SPIR-V";return false;}
        const uint32_t *w=&words[at];
        auto text=[&](size_t from){return std::string(reinterpret_cast<const char *>(w+from),strnlen(reinterpret_cast<const char *>(w+from),(count-from)*4));};
        if(op==OpFunction)in_function=true;
        if(in_function)function_code.push_back({op,at+1,at+count});
        else switch(op) {
            case OpName:m.names[w[1]]=text(2);break;
            case OpMemberName:m.member_names[w[1]][w[2]]=text(3);break;
            case OpDecorate:m.decorations[w[1]][w[2]]=count>3?w[3]:1;break;
            case OpMemberDecorate:m.member_decorations[w[1]][w[2]][w[3]]=count>4?w[4]:1;break;
            case OpConstant:case OpSpecConstant:if(count>3)m.constants[w[2]]=w[3];break;
            case OpVariable:m.variables.push_back({w[2],w[1],w[3]});globals.insert(w[2]);break;
            default:
                if(op>=OpTypeVoid&&op<=OpTypePointer&&count>1) {
                    auto &t=m.types[w[1]];t.assign(1,op);t.insert(t.end(),w+2,w+count);
                }
        }
        at+=count;
    }
    // A global is statically used when function code names it (the offline
    // modules are optimized: unused inputs and samplers are gone from them).
    for(const auto &c:function_code) {
        const bool chain=(c.op==65||c.op==66)&&c.to-c.from>=4; // OpAccessChain, OpInBoundsAccessChain
        for(size_t i=c.from;i<c.to;++i) {
            if(!globals.count(words[i]))continue;
            m.used.insert(words[i]);
            const auto constant=chain&&i==c.from+2?m.constants.find(words[c.from+3]):m.constants.end();
            if(constant!=m.constants.end())m.members_used[words[i]].insert(constant->second);
            else m.whole_used.insert(words[i]);
        }
    }
    return true;
}

// type_to_glsl, for the types guest uniforms, inputs and samplers use
std::string type_name(const Module &m,uint32_t id) {
    const auto &t=m.type(id);if(t.empty())return "?";
    switch(t[0]) {
        case OpTypeBool:return "bool";
        case OpTypeInt:return t[2]?"int":"uint";
        case OpTypeFloat:return t[1]==64?"double":"float";
        case OpTypeVector: {
            const auto &c=m.type(t[1]);const std::string p=c.empty()?"":c[0]==OpTypeBool?"b":c[0]==OpTypeInt?(c[2]?"i":"u"):"";
            return p+"vec"+std::to_string(t[2]);
        }
        case OpTypeMatrix: {
            const auto &column=m.type(t[1]);const uint32_t rows=column.size()>2?column[2]:0,columns=t[2];
            return columns==rows?"mat"+std::to_string(columns):"mat"+std::to_string(columns)+"x"+std::to_string(rows);
        }
        case OpTypeSampledImage:case OpTypeImage: {
            const auto &image=t[0]==OpTypeImage?t:m.type(t[1]);
            if(image.size()<8)return "?";
            const auto &sampled=m.type(image[1]);
            std::string r=sampled.empty()||sampled[0]!=OpTypeInt?"":sampled[2]?"i":"u";
            r+="sampler";
            switch(image[2]){case 0:r+="1D";break;case 1:r+="2D";break;case 2:r+="3D";break;case 3:r+="Cube";break;case 4:r+="2DRect";break;case 5:r+="Buffer";break;default:r+="?";}
            if(image[5])r+="MS";
            if(image[4])r+="Array";
            if(image[3]==1)r+="Shadow";
            return r;
        }
    }
    return "?";
}
uint32_t array_length(const Module &m,const std::vector<uint32_t> &t) {
    auto f=m.constants.find(t[2]);return f==m.constants.end()?0:f->second;
}
// spirv-cross get_declared_struct_size and its member sizes
uint32_t declared_size(const Module &m,uint32_t struct_id);
uint32_t member_size(const Module &m,uint32_t struct_id,uint32_t index,uint32_t type) {
    const auto &t=m.type(type);
    if(t.empty())return 0;
    if(t[0]==OpTypeArray)return m.decoration(type,DecArrayStride)*array_length(m,t);
    if(t[0]==OpTypeStruct)return declared_size(m,type);
    const auto *d=m.member(struct_id,index);
    if(t[0]==OpTypeMatrix) {
        const uint32_t stride=d&&d->count(DecMatrixStride)?d->at(DecMatrixStride):16;
        const auto &column=m.type(t[1]);
        return d&&d->count(DecRowMajor)?stride*(column.size()>2?column[2]:1):stride*t[2];
    }
    if(t[0]==OpTypeVector) {const auto &c=m.type(t[1]);return (c.size()>1&&c[0]!=OpTypeBool?c[1]/8:4)*t[2];}
    return t[0]==OpTypeFloat||t[0]==OpTypeInt?t[1]/8:4;
}
uint32_t declared_size(const Module &m,uint32_t struct_id) {
    const auto &t=m.type(struct_id);if(t.size()<2)return 0;
    const uint32_t last=uint32_t(t.size()-2);
    const auto *d=m.member(struct_id,last);const uint32_t offset=d&&d->count(DecOffset)?d->at(DecOffset):0;
    return offset+member_size(m,struct_id,last,t.back());
}
// pack_shaders.uniform_fields
bool uniform_fields(const Module &m,uint32_t type,uint32_t offset,const std::string &name,uint32_t count,uint32_t array_stride,
                    uint32_t matrix_stride,uint32_t block_bytes,std::vector<UniformField> &out,std::string &error,
                    const std::set<uint32_t> *members=nullptr) {
    const auto &t=m.type(type);
    if(t.empty()){error="unknown uniform type";return false;}
    if(t[0]==OpTypeStruct) {
        if(count!=1) {
            for(uint32_t i=0;i<count;++i)
                if(!uniform_fields(m,type,offset+i*array_stride,name+"["+std::to_string(i)+"]",1,0,0,block_bytes,out,error))return false;
            return true;
        }
        for(uint32_t i=0;i+1<t.size();++i) {
            if(members&&!members->count(i))continue; // a block member no code reads
            const auto *d=m.member(type,i);
            if(d&&d->count(DecRowMajor)){error="Row-major uniform storage requires an explicit pack layout";return false;}
            uint32_t member=t[i+1],n=1,stride=0;
            const auto &mt=m.type(member);
            if(!mt.empty()&&(mt[0]==OpTypeArray||mt[0]==OpTypeRuntimeArray)) {
                if(mt[0]==OpTypeRuntimeArray||(n=array_length(m,mt))<1){error="Unsupported runtime-sized uniform array";return false;}
                stride=m.decoration(member,DecArrayStride);member=mt[1];
                const auto &inner=m.type(member);
                if(!inner.empty()&&(inner[0]==OpTypeArray||inner[0]==OpTypeRuntimeArray)){error="Unsupported multidimensional uniform array";return false;}
            }
            const auto names=m.member_names.find(type);
            const std::string child=names!=m.member_names.end()&&names->second.count(i)?names->second.at(i):"_m"+std::to_string(i);
            if(!uniform_fields(m,member,offset+(d&&d->count(DecOffset)?d->at(DecOffset):0),name.empty()?child:name+"."+child,
                               n,stride,d&&d->count(DecMatrixStride)?d->at(DecMatrixStride):0,block_bytes,out,error))return false;
        }
        return true;
    }
    uint32_t scalar=0,columns=1,rows=1;
    const auto &base=t[0]==OpTypeMatrix?m.type(m.type(t[1])[1]):t[0]==OpTypeVector?m.type(t[1]):t;
    if(t[0]==OpTypeMatrix){columns=t[2];rows=m.type(t[1])[2];}
    else if(t[0]==OpTypeVector)rows=t[2];
    else if(t[0]!=OpTypeFloat&&t[0]!=OpTypeInt&&t[0]!=OpTypeBool){error="Unsupported uniform scalar type "+type_name(m,type);return false;}
    if(base.empty()){error="unknown uniform component type";return false;}
    if(base[0]==OpTypeInt)scalar=base[2]?1:2;
    else if(base[0]==OpTypeBool)scalar=1;
    else if(base[0]!=OpTypeFloat||base[1]!=32){error="Unsupported uniform scalar type "+type_name(m,type);return false;}
    const uint32_t end=offset+(count-1)*array_stride+(columns-1)*matrix_stride+rows*4;
    if(end>block_bytes){error="Uniform field "+name+" escapes reflected block";return false;}
    out.push_back({name,offset,scalar,columns,rows,count,array_stride,matrix_stride});
    return true;
}

// glslang keeps a declared attribute that no code reads in the vertex entry
// point's interface; the offline modules are optimized, so theirs is gone, and
// records list only live inputs. A pipeline must give every interface input a
// vertex attribute, so the unread ones are removed (OpVariable, its names and
// decorations, and its OpEntryPoint slot), as the optimizer does.
bool strip_unread_inputs(std::vector<uint32_t> &words, std::string &error) {
    Module m;if(!parse(words,m,error))return false;
    std::unordered_set<uint32_t> unread;
    for(const auto &v:m.variables)
        if(v.storage==StorageInput&&!m.used.count(v.id)&&m.decorated(v.id,DecLocation)&&!m.decorated(v.id,DecBuiltIn))unread.insert(v.id);
    if(unread.empty())return true;
    std::vector<uint32_t> out(words.begin(),words.begin()+5);out.reserve(words.size());
    for(size_t at=5;at<words.size();) {
        const uint32_t count=words[at]>>16,op=words[at]&0xffffu;
        const uint32_t *w=&words[at];at+=count;
        if((op==OpName||op==OpDecorate)&&count>1&&unread.count(w[1]))continue;
        if(op==OpVariable&&count>2&&unread.count(w[2]))continue;
        if(op==OpEntryPoint&&count>3) {
            size_t ids=3;while(ids<count){const uint32_t word=w[ids++];if(!(word&0xff000000u)||!(word&0xff0000u)||!(word&0xff00u)||!(word&0xffu))break;}
            const size_t start=out.size();out.insert(out.end(),w,w+ids);
            for(size_t i=ids;i<count;++i)if(!unread.count(w[i]))out.push_back(w[i]);
            out[start]=uint32_t(out.size()-start)<<16|op;
            continue;
        }
        out.insert(out.end(),w,w+count);
    }
    words.swap(out);
    return true;
}

} // namespace

bool reflect_spirv(const std::vector<uint32_t> &module, SpirvReflection &out, std::string &error) {
    Module m;if(!parse(module,m,error))return false;
    out={};
    for(const auto &v:m.variables) {
        if(!m.used.count(v.id))continue;
        const auto &pointer=m.type(v.pointer);if(pointer.size()<3)continue;
        uint32_t type=pointer[2];
        const auto name=m.names.count(v.id)?m.names.at(v.id):std::string();
        if(v.storage==StorageInput) {
            if(m.decorated(v.id,DecBuiltIn)||!m.decorated(v.id,DecLocation))continue;
            out.inputs.push_back({m.decoration(v.id,DecLocation),name,type_name(m,type)});
        } else if(v.storage==StorageUniform&&m.decorated(type,DecBlock)) {
            SpirvReflection::Block block{m.decoration(v.id,DecDescriptorSet),m.decoration(v.id,DecBinding),declared_size(m,type),{}};
            const auto used=m.members_used.find(v.id);
            const std::set<uint32_t> *members=m.whole_used.count(v.id)||used==m.members_used.end()?nullptr:&used->second;
            if(!uniform_fields(m,type,0,"",1,0,0,block.bytes,block.fields,error,members))return false;
            out.blocks.push_back(std::move(block));
        } else if(v.storage==StorageUniformConstant) {
            uint32_t count=1;const auto &t=m.type(type);
            if(!t.empty()&&t[0]==OpTypeArray){count=array_length(m,t);type=t[1];}
            const auto &inner=m.type(type);
            if(inner.empty()||inner[0]!=OpTypeSampledImage)continue;
            out.samplers.push_back({m.decoration(v.id,DecDescriptorSet),m.decoration(v.id,DecBinding),count,name,type_name(m,type)});
        }
    }
    return true;
}

std::string runtime_shader_generation() {
    const auto d=source_digest(std::string("lindbergh-runtime-glsl-2\n")+lvk_legacy_glsl+"\n"+lvk_sampling_glsl);
    static const char hex[]="0123456789abcdef";std::string s;
    for(int i=0;i<6;++i){s+=hex[d[i]>>4];s+=hex[d[i]&15];}
    return s;
}

bool compile_guest_program(const std::string &vertex, const std::string &fragment,
                           const std::vector<AttributeBinding> &bindings, ShaderRecord &out, std::string &error) {
    static std::mutex mutex; // glslang's process-wide state
    std::lock_guard lock(mutex);
    if(!initialize()){error="glslang did not initialize";return false;}
    std::string sources[2];
    if(!preprocess(vertex,EShLangVertex,sources[0],error)||!preprocess(fragment,EShLangFragment,sources[1],error))return false;
    Interface interface;if(!varyings(sources[0],sources[1],interface,error))return false;
    std::map<std::string,uint32_t> attributes,bound;
    for(const auto &b:bindings)if(b.name.rfind("gl_",0)!=0)attributes[b.name]=bound[b.name]=b.location;
    // Inactive declarations are not returned by GL_ACTIVE_ATTRIBUTES: they
    // get collision-free locations, and reflection must find them unused.
    std::set<uint32_t> occupied;for(const auto &[n,l]:attributes)occupied.insert(l);
    for(const auto &b:builtin_inputs())
        if(has_word(sources[0],b.name)||(std::strcmp(b.name,"gl_Vertex")==0&&sources[0].find("ftransform")!=std::string::npos))occupied.insert(b.location);
    for(const auto &[name,decl]:declarations(sources[0],"attribute")) {
        if(attributes.count(name))continue;
        uint32_t free=0;while(free<32&&occupied.count(free))++free;
        if(free==32){error="no free attribute location for "+name;return false;}
        attributes[name]=free;occupied.insert(free);
    }
    std::set<std::string> sampler_names;
    for(const auto &s:sources)
        for(std::sregex_iterator it(s.begin(),s.end(),sampler_regex()),end;it!=end;++it)sampler_names.insert((*it)[2]);
    std::map<std::string,uint32_t> samplers;
    {uint32_t i=0;for(const auto &n:sampler_names)samplers[n]=i++;}
    std::string converted[2];
    if(!convert(sources[0],"vert",interface,attributes,samplers,converted[0],error)||
       !convert(sources[1],"frag",interface,attributes,samplers,converted[1],error))return false;
    ShaderRecord record{};
    if(!compile(converted[0],converted[1],record.vertex,record.fragment,error))return false;
    if(!strip_unread_inputs(record.vertex,error))return false;
    SpirvReflection reflection[2];
    if(!reflect_spirv(record.vertex,reflection[0],error)||!reflect_spirv(record.fragment,reflection[1],error))return false;
    for(const auto &input:reflection[0].inputs)
        if(input.name.rfind("legacy_input_",0)!=0&&!bound.count(input.name)){error="Missing explicit guest binding for live vertex attribute: "+input.name;return false;}
    // pack(): attributes, inputs by location, blocks merged by set/binding, samplers by binding
    for(const auto &[name,location]:bound)record.attributes.push_back({name,location});
    auto inputs=reflection[0].inputs;
    std::sort(inputs.begin(),inputs.end(),[](const auto &a,const auto &b){return a.location<b.location;});
    for(const auto &input:inputs)record.inputs.push_back({input.location,input.type});
    std::map<std::pair<uint32_t,uint32_t>,std::pair<uint32_t,std::map<std::string,UniformField>>> blocks;
    std::map<uint32_t,ShaderSampler> sampler_records;
    for(const auto &stage:reflection) {
        for(const auto &block:stage.blocks) {
            auto &[bytes,fields]=blocks[{block.set,block.binding}];
            for(const auto &f:block.fields) {
                const auto found=fields.find(f.name);
                const auto same=[](const UniformField &a,const UniformField &b){
                    return std::tie(a.offset,a.scalar,a.columns,a.rows,a.count,a.array_stride,a.matrix_stride)==
                           std::tie(b.offset,b.scalar,b.columns,b.rows,b.count,b.array_stride,b.matrix_stride);};
                if(found!=fields.end()&&!same(found->second,f)) {
                    error="Uniform layout mismatch: "+f.name;return false;
                }
                fields[f.name]=f;
            }
            bytes=std::max(bytes,block.bytes);
        }
        for(const auto &s:stage.samplers) {
            if(s.set!=2){error="Unexpected sampler descriptor set";return false;}
            sampler_records[s.binding]={s.binding,s.name,s.type,s.count};
        }
    }
    for(auto &[key,value]:blocks) {
        UniformLayout layout{key.first,key.second,value.first,{}};
        for(auto &[name,f]:value.second)layout.fields.push_back(f);
        std::sort(layout.fields.begin(),layout.fields.end(),[](const auto &a,const auto &b){return std::tie(a.offset,a.name)<std::tie(b.offset,b.name);});
        record.uniforms.push_back(std::move(layout));
    }
    for(auto &[binding,s]:sampler_records)record.samplers.push_back(s);
    record.vertex_source=shader_source_digest(vertex);record.fragment_source=shader_source_digest(fragment);
    std::string identity="lindbergh-runtime-glsl-1";identity+='\0';identity+=vertex;identity+='\0';identity+=fragment;
    for(const auto &a:record.attributes){identity+='\0';identity+=a.name+"="+std::to_string(a.location);}
    record.key=source_digest(identity);
    out=std::move(record);
    return true;
}

} // namespace lindbergh::vk

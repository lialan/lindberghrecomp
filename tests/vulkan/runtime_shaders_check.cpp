// runtime_shaders_check <programs.jsonl> <pack.lvsp> [spirv-val]
// Every program of a game's harvest manifest compiled on the fly
// (compile_guest_program) and compared with the offline pack's record:
// attributes, inputs and samplers equal; every uniform field the pack has at
// the same place (the runtime modules are not optimized, so they may keep
// more); the modules valid SPIR-V. Game shaders: run locally, not in CI.
#include "runtime_shaders.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>
#include <tuple>

using namespace lindbergh::vk;
static std::string read(const std::filesystem::path &p) {std::ifstream in(p,std::ios::binary);std::stringstream s;s<<in.rdbuf();return s.str();}
static std::string field(const std::string &row,const char *key) {
    std::smatch m;std::regex r(std::string("\"")+key+R"re("\s*:\s*"([^"]*)")re");
    return std::regex_search(row,m,r)?std::string(m[1]):std::string();
}
int main(int argc,char **argv) {
    if(argc<3){std::fprintf(stderr,"usage: %s <programs.jsonl> <pack.lvsp> [spirv-val]\n",argv[0]);return 2;}
    const std::filesystem::path manifest=argv[1],root=manifest.parent_path();
    ShaderPack pack(argv[2]);
    const std::string validator=argc>3?argv[3]:"";
    std::ifstream rows(manifest);std::string row;
    std::set<std::string> seen;unsigned checked=0,failed=0,missing=0,extra_fields=0;double slowest=0,total=0;
    while(std::getline(rows,row)) {
        if(row.find('{')==std::string::npos)continue;
        const auto vpath=field(row,"vertex"),fpath=field(row,"fragment");
        const auto vertex=read(root/(field(row,"original_vertex").empty()?vpath:field(row,"original_vertex")));
        const auto fragment=read(root/(field(row,"original_fragment").empty()?fpath:field(row,"original_fragment")));
        std::vector<AttributeBinding> bindings;
        static const std::regex attribute(R"re(\{\s*"name"\s*:\s*"([^"]+)"\s*,\s*"location"\s*:\s*(\d+))re");
        for(std::sregex_iterator it(row.begin(),row.end(),attribute),end;it!=end;++it)
            bindings.push_back({(*it)[1],uint32_t(std::stoul((*it)[2]))});
        std::string identity=vertex+'\0'+fragment;for(const auto &b:bindings)identity+='\0'+b.name+std::to_string(b.location);
        if(!seen.insert(identity).second)continue;
        const auto vs=shader_source_digest(vertex),fs=shader_source_digest(fragment);
        const ShaderRecord *offline=nullptr;
        for(const auto &r:pack.programs())if(r.vertex_source==vs&&r.fragment_source==fs){offline=&r;break;}
        if(!offline){++missing;continue;}
        ShaderRecord runtime;std::string error;
        const auto start=std::chrono::steady_clock::now();
        const bool ok=compile_guest_program(vertex,fragment,bindings,runtime,error);
        const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        total+=ms;slowest=std::max(slowest,ms);++checked;
        std::vector<std::string> problems;
        if(!ok)problems.push_back("compile: "+error.substr(0,600));
        else {
            auto attrs=[](const ShaderRecord &r){std::string s;for(const auto &a:r.attributes)s+=a.name+"="+std::to_string(a.location)+" ";return s;};
            if(attrs(runtime)!=attrs(*offline))problems.push_back("attributes: runtime "+attrs(runtime)+" pack "+attrs(*offline));
            auto inputs=[](const ShaderRecord &r){std::string s;for(const auto &i:r.inputs)s+=std::to_string(i.location)+":"+i.type+" ";return s;};
            if(inputs(runtime)!=inputs(*offline))problems.push_back("inputs: runtime "+inputs(runtime)+" pack "+inputs(*offline));
            auto samplers=[](const ShaderRecord &r){std::string s;for(const auto &x:r.samplers)s+=std::to_string(x.binding)+":"+x.name+":"+x.type+":"+std::to_string(x.count)+" ";return s;};
            if(samplers(runtime)!=samplers(*offline))problems.push_back("samplers: runtime "+samplers(runtime)+" pack "+samplers(*offline));
            for(const auto &layout:offline->uniforms) {
                const UniformLayout *mine=nullptr;
                for(const auto &l:runtime.uniforms)if(l.set==layout.set&&l.binding==layout.binding)mine=&l;
                if(!mine){problems.push_back("uniform block "+std::to_string(layout.set)+"/"+std::to_string(layout.binding)+" missing");continue;}
                if(mine->bytes<layout.bytes)problems.push_back("uniform block smaller: "+std::to_string(mine->bytes)+" < "+std::to_string(layout.bytes));
                for(const auto &f:layout.fields) {
                    const UniformField *m=nullptr;for(const auto &g:mine->fields)if(g.name==f.name)m=&g;
                    if(!m){problems.push_back("uniform field missing: "+f.name);continue;}
                    if(std::tie(m->offset,m->scalar,m->columns,m->rows,m->count,m->array_stride,m->matrix_stride)!=
                       std::tie(f.offset,f.scalar,f.columns,f.rows,f.count,f.array_stride,f.matrix_stride))
                        problems.push_back("uniform field differs: "+f.name+" offset "+std::to_string(m->offset)+" vs "+std::to_string(f.offset));
                }
                extra_fields+=unsigned(mine->fields.size()-std::min(mine->fields.size(),layout.fields.size()));
            }
            for(const auto &l:runtime.uniforms) {
                bool known=false;for(const auto &o:offline->uniforms)known|=o.set==l.set&&o.binding==l.binding;
                if(!known)problems.push_back("extra uniform block "+std::to_string(l.set)+"/"+std::to_string(l.binding));
            }
            if(!validator.empty())
                for(auto [stage,module]:{std::pair{"vert",&runtime.vertex},std::pair{"frag",&runtime.fragment}}) {
                    const auto path=std::filesystem::temp_directory_path()/(std::string("lvk-runtime-check.")+stage+".spv");
                    std::ofstream(path,std::ios::binary).write(reinterpret_cast<const char *>(module->data()),std::streamsize(module->size()*4));
                    if(std::system((validator+" --target-env vulkan1.1 "+path.string()+" > /dev/null 2>&1").c_str())!=0)
                        problems.push_back(std::string("spirv-val rejects the ")+stage+" module");
                }
        }
        if(!problems.empty()) {
            ++failed;
            if(failed<=5) {std::fprintf(stderr,"FAIL %s + %s\n",vpath.c_str(),fpath.c_str());for(const auto &p:problems)std::fprintf(stderr,"   %s\n",p.c_str());}
        }
    }
    std::printf("runtime shaders: %u programs compiled and compared, %u differ, %u not in the pack; %u extra unoptimized uniform fields; %.1f ms mean, %.1f ms slowest\n",
                checked,failed,missing,extra_fields,checked?total/checked:0.0,slowest);
    return failed?1:0;
}

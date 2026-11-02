#include "guest_pixels.hpp"
// Guest GL ABI -> shared Vulkan state. No host GL entry is resolved or called.
#include "../renderer/vulkan/guest_arrays.hpp"
#include "../renderer/vulkan/guest_state.hpp"
#include "../renderer/vulkan/stereo_scene.hpp"
#include "../renderer/vulkan/controller_input.hpp"
#include "../renderer/vulkan/overlay.hpp"
#include "../renderer/vulkan/settings_panel.hpp"
#include "../renderer/vulkan/shader_pack.hpp"
#include "../renderer/vulkan/pipeline_recipes.hpp"
#include "../renderer/vulkan/guest_indices.hpp"
#include "../renderer/vulkan/window.hpp"
#include "../renderer/vulkan/resources.hpp"
#include "../renderer/vulkan/uniforms.hpp"
#include "vulkan_abi.h"
#include "fixed_fragment_source.h"
#include "game_states.h"
#include "vulkan_host.h"
#include <SDL.h>
#ifdef LINDBERGH_VULKAN_XR
#include "../renderer/vulkan/openxr.hpp"
#include <SDL_system.h>
#ifdef __ANDROID__
#include <android/asset_manager_jni.h>
#else
#include <cerrno>
#include <unistd.h>
#endif
#endif
extern "C" {
#include "game_profile.h"
#include "display_cadence.h"
#include "xr_settings.h"
#include "xr_sight.h"
}
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <cmath>
#include <sstream>
#include <set>
#include <chrono>

using namespace lindbergh::vk;
extern "C" unsigned char *g_guest_base;
extern "C" unsigned g_frame;
namespace {
[[noreturn]] void gap(const char *what) {
  std::fprintf(stderr, "[vulkan] %s\n", what);
  std::abort();
}
bool index_reuse_enabled() {
  static const bool enabled=[] {const auto *v=std::getenv("LINDBERGH_VULKAN_INDEX_REUSE");return !v||*v!='0';}();return enabled;
}
std::shared_ptr<GeometryBuffer> native_buffer(GLuint id,bool dedicated_staging=false);
struct Shader {
  GLenum type;
  std::string source;
  bool compiled = false, delete_pending = false;
  uint32_t attachments = 0;
};
struct UniformLocation {
  uint32_t block, field, first;
  int sampler;
};
struct TextureLevel {
  GLint internal;
  GLsizei width, height;
  GLenum format, type;
  std::vector<uint8_t> bytes;
  bool compressed;
};
struct ImageVersion {
  std::shared_ptr<Texture> image;
  uint64_t revision = 0, use_frame = 0;
};
struct BufferVersion {
  std::shared_ptr<GeometryBuffer> buffer;
  size_t bytes = 0;
  uint64_t revision = 0, use_frame = 0;
};
struct TextureObject {
  uint32_t layers = 1,render_width=0,render_height=0;
  bool picture_view=false,movie=false,gpu_target=false;
  // Sampling its framebuffer's drawable directly (GL's render-to-texture).
  std::shared_ptr<Texture> alias; Target *alias_target=nullptr;
  uint64_t revision = 1;
  bool dynamic = false;
  ImageVersion stable;
  std::array<ImageVersion,3> slots;

  std::map<int, TextureLevel> levels; // a cube map's +X face
  bool cube=false, rect=false; // rect: bound as GL_TEXTURE_RECTANGLE (clamped, no mipmaps)
  std::array<std::map<int, TextureLevel>,5> cube_faces; // -X, +Y, -Y, +Z, -Z
  std::unordered_map<GLenum, GLint> parameters;
  std::unordered_map<GLenum, std::array<float, 4>> float_parameters;
};
struct NativeProgram {
  std::array<std::unique_ptr<UniformSet>,2> uniform_sets;
  std::unique_ptr<SampledSet> empty_images;
  std::vector<VkDescriptorSetLayout> layouts;
  uint64_t legacy_version=0; uint32_t legacy_height=0;
  std::array<uint64_t,2> uniform_version{}, uniform_frame{};
  std::array<uint32_t,2> uniform_offset{};
  std::map<std::string,std::shared_ptr<Renderer>> renderers;
};
struct Program {
  bool delete_pending = false;
  bool fixed_fragment = false; // no guest fragment shader: fixed.frag follows its vertex program
  bool skipped = false;        // not in the pack: drawn as nothing (see lvk_glLinkProgram)
  std::shared_ptr<NativeProgram> native;

  std::vector<GLuint> shaders;
  std::vector<AttributeBinding> attributes;
  const ShaderRecord *record = nullptr;
  std::vector<std::unique_ptr<UniformData>> uniforms;
  std::vector<UniformLocation> locations;
  std::unordered_map<std::string, GLint> location_names;
  std::unordered_map<int, GLint> sampler_units;
};
struct Buffer {
  uint64_t revision = 1;
  bool dynamic = false;
  BufferVersion stable;
  std::array<BufferVersion,3> slots;

  std::vector<unsigned char> bytes;
  GuestIndexRanges index_ranges;
  GLenum usage = GL_STATIC_DRAW;
};
struct Attributes {
  GLbitfield mask;
  std::unordered_map<GLenum, std::vector<GLushort>> pixel_maps;
  std::unordered_map<GLenum, GLint> integers;
  std::unordered_map<GLenum, std::array<float, 4>> floats;
  std::unordered_map<GLenum, bool> enabled;
  RasterState raster;
  std::array<GLint, 4> viewport, scissor;
  GLenum matrix_mode, active_texture;
  std::array<GLuint, 8> textures;
};
struct DrawPacket {
  Renderer *renderer;
  Target *target;
  std::vector<Upload> streams;
  IndexStream index{};
  uint32_t count = 0;
  Matrices matrices{};
  RasterState raster;
  std::array<VkDescriptorSet,3> descriptors{};
  std::array<std::vector<uint32_t>,2> offsets;
  std::array<float,4> viewport;
  VkRect2D scissor;
  VkImageAspectFlags clear_aspects = 0;
  std::array<float,4> color{};
  float clear_depth = 1;
  uint32_t clear_stencil = 0, stencil_ref=0, stencil_compare_mask=~0u, stencil_write_mask=~0u;
  float depth_bias_factor=0, depth_bias_units=0;
  std::array<float,4> blend_constants{};
};
// A guest framebuffer object draws into a drawable (a Target sized by the same
// stereo rules as a pbuffer). Its attached textures take the drawable's colour
// or depth by GPU copy the next time a draw samples them after it was drawn.
struct Framebuffer {
  GLuint color=0,depth=0,depth_renderbuffer=0;
  bool rect=false,drawn=false,depthless=false; // depthless: how its drawable was made
  unsigned drawable=0;
  std::array<int,2> size{};
};
struct Material {
  std::shared_ptr<SampledSet> descriptors;
  std::vector<std::shared_ptr<Texture>> images;
  std::vector<std::shared_ptr<Sampler>> samplers;
};
#if defined(LINDBERGH_VULKAN_XR) && defined(__ANDROID__)
// SDL supplies a local Activity reference. Keep a global reference until the
// XR session and instance have been destroyed, including failure teardown.
struct ActivityRef {
  JavaVM *vm = nullptr;
  jobject activity = nullptr;
  ActivityRef() {
    auto *env = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
    if (!env || env->GetJavaVM(&vm) != JNI_OK) throw std::runtime_error("SDL JavaVM unavailable");
    auto local = static_cast<jobject>(SDL_AndroidGetActivity());
    if (local) { activity=env->NewGlobalRef(local); env->DeleteLocalRef(local); }
    if (!activity) throw std::runtime_error("SDL Activity unavailable");
  }
  ~ActivityRef() {
    auto *env = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
    if (env && activity) env->DeleteGlobalRef(activity);
  }
  void restart() {
    auto *env=static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());if(!env)return;
    const auto cls=env->GetObjectClass(activity);const auto method=env->GetMethodID(cls,"restartGame","()V");
    if(method)env->CallVoidMethod(activity,method);env->DeleteLocalRef(cls);
    if(env->ExceptionCheck()){env->ExceptionClear();std::fprintf(stderr,"[vulkan-xr] game restart failed\n");}
  }
  void shader_defaults() {
    auto *env=static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
    if (env->PushLocalFrame(8)<0) throw std::runtime_error("Cannot create shader asset JNI frame");
    struct LocalFrame {JNIEnv *env; ~LocalFrame(){env->PopLocalFrame(nullptr);}} locals{env};
    auto cls=env->GetObjectClass(activity);
    auto get=env->GetMethodID(cls,"getAssets","()Landroid/content/res/AssetManager;");
    auto assets=env->CallObjectMethod(activity,get);
    auto *manager=assets?AAssetManager_fromJava(env,assets):nullptr;
    if (env->ExceptionCheck()) {env->ExceptionClear();throw std::runtime_error("Cannot access Vulkan shader assets");}
    const char *storage=SDL_AndroidGetInternalStoragePath();
    if (!manager || !storage) throw std::runtime_error("Vulkan shader asset storage unavailable");
    const auto directory=std::filesystem::path(storage)/"vulkan";
    std::filesystem::create_directories(directory);
    for (auto entry : {std::pair{"LINDBERGH_VULKAN_SHADERS","game.lvsp"},
                       std::pair{"LINDBERGH_VULKAN_FIXED_SHADERS","fixed.lvsp"},
                       std::pair{"LINDBERGH_VULKAN_RECIPES_SEED","pipelines.lvpr"},
                       std::pair{"LINDBERGH_VULKAN_CACHE_SEED","pipelines.bin"}}) {
      const char *override=std::getenv(entry.first);
      if (override && *override) continue;
      const auto source=std::string("vulkan/")+entry.second;
      std::unique_ptr<AAsset,decltype(&AAsset_close)> asset(AAssetManager_open(manager,source.c_str(),AASSET_MODE_STREAMING),AAsset_close);
      if (!asset) {
        if(std::strcmp(entry.first,"LINDBERGH_VULKAN_RECIPES_SEED")==0||
           std::strcmp(entry.first,"LINDBERGH_VULKAN_CACHE_SEED")==0)continue;   // optional
        throw std::runtime_error("Missing packaged shader archive: "+source);
      }
      const auto path=directory/entry.second;
      const auto temporary=path.string()+".tmp";
      try {
        std::ofstream out(temporary,std::ios::binary|std::ios::trunc);
        std::array<char,65536> bytes;
        int n;
        while ((n=AAsset_read(asset.get(),bytes.data(),bytes.size()))>0) out.write(bytes.data(),n);
        out.close();
        if (n<0 || !out || std::rename(temporary.c_str(),path.c_str()))
          throw std::runtime_error("Cannot extract Vulkan shader archive: "+source);
      } catch (...) {std::remove(temporary.c_str());throw;}
      SDL_setenv(entry.first,path.c_str(),1);
    }
  }
  OpenXRPlatform platform() const { return {vm,activity}; }
};
#elif defined(LINDBERGH_VULKAN_XR)
// Desktop OpenXR (native Linux, e.g. Steam Frame): no Activity. The shader
// archives ship next to the executable, in vulkan/ (or LINDBERGH_VULKAN_DIR).
struct ActivityRef {
  void restart() {
    // Same process image, same arguments: what the Quest's restartGame does.
    std::fprintf(stderr,"[vulkan-xr] restarting the game\n");std::fflush(nullptr);
    char exe[4096];const auto n=readlink("/proc/self/exe",exe,sizeof exe-1);
    if(n<=0){std::fprintf(stderr,"[vulkan-xr] game restart failed: no /proc/self/exe\n");return;}
    exe[n]=0;
    std::vector<std::string> args;
    if(std::ifstream cmdline{"/proc/self/cmdline",std::ios::binary}) {
      std::string arg;while(std::getline(cmdline,arg,'\0'))args.push_back(arg);
    }
    if(args.empty())args.push_back(exe);
    std::vector<char *> argv;for(auto &a:args)argv.push_back(a.data());argv.push_back(nullptr);
    execv(exe,argv.data());
    std::fprintf(stderr,"[vulkan-xr] game restart failed: %s\n",std::strerror(errno));
  }
  void shader_defaults() {
    std::filesystem::path directory;
    if(const char *v=std::getenv("LINDBERGH_VULKAN_DIR");v&&*v)directory=v;
    else if(char *base=SDL_GetBasePath()){directory=std::filesystem::path(base)/"vulkan";SDL_free(base);}
    else directory="vulkan";
    for (auto entry : {std::pair{"LINDBERGH_VULKAN_SHADERS","game.lvsp"},
                       std::pair{"LINDBERGH_VULKAN_FIXED_SHADERS","fixed.lvsp"},
                       std::pair{"LINDBERGH_VULKAN_RECIPES_SEED","pipelines.lvpr"},
                       std::pair{"LINDBERGH_VULKAN_CACHE_SEED","pipelines.bin"}}) {
      const char *override=std::getenv(entry.first);
      if (override && *override) continue;
      const auto path=directory/entry.second;
      std::error_code ignored;
      if(!std::filesystem::is_regular_file(path,ignored)) {
        if(std::strcmp(entry.first,"LINDBERGH_VULKAN_RECIPES_SEED")==0||
           std::strcmp(entry.first,"LINDBERGH_VULKAN_CACHE_SEED")==0)continue;   // optional
        throw std::runtime_error("Missing packaged shader archive: "+path.string());
      }
      SDL_setenv(entry.first,path.c_str(),1);
    }
  }
  OpenXRPlatform platform() const { return {}; }
};
#endif
struct State {
  std::unique_ptr<Device> device;
#ifndef LINDBERGH_VULKAN_OFFSCREEN
  std::unique_ptr<Window> window;
#endif
#ifdef LINDBERGH_VULKAN_OFFSCREEN
  std::unique_ptr<Target> offscreen;
  std::unique_ptr<ConstantDensityMap> offscreen_density;
#endif
  std::unique_ptr<StereoScene> stereo;
  std::unique_ptr<Overlay> overlay;
  std::unique_ptr<SettingsPanel> panel;
  void panel_input(ControllerSample sample,int64_t time,bool focused);
  std::unique_ptr<ControllerInput> controllers;ControllerSnapshot game_input;
  LgXrSight sight{};
  int fake_guns=0;
  void fake_controllers();
  LgXrSettings settings=LG_XR_SETTINGS_DEFAULT;
  std::array<StereoPose,2> fake_eyes;
  std::unordered_map<unsigned,std::array<int,2>> drawable_sizes;
  unsigned world_draws=0,hud_sequence=0;
  // LINDBERGH_VULKAN_PASS_TIMINGS=file and LINDBERGH_VULKAN_PASS_FRAMES=a-b:
  // GPU time of each render pass and copy on those frames. Timestamps sit
  // between passes only (inside a multiview pass one would take a query per
  // view). Profiled frames wait for their own GPU work before reading it back.
  struct {
    VkQueryPool pool=VK_NULL_HANDLE;std::FILE *out=nullptr;unsigned from=1,to=0,n=0,draws=0;bool active=false;
    std::vector<std::string> labels;
  } pass_profile;
  void pass_stamp(std::string label) {
    if(!pass_profile.active||pass_profile.n>=1024)return;
    vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,pass_profile.pool,pass_profile.n++);
    pass_profile.labels.push_back(std::move(label));
  }
  void pass_profile_begin() {
    static const bool configured=[this] {
      const auto *path=std::getenv("LINDBERGH_VULKAN_PASS_TIMINGS"),*frames=std::getenv("LINDBERGH_VULKAN_PASS_FRAMES");
      if(!path||!frames)return false;
      pass_profile.from=unsigned(std::strtoul(frames,nullptr,10));pass_profile.to=pass_profile.from;
      if(const auto *end=std::strchr(frames,'-'))pass_profile.to=unsigned(std::strtoul(end+1,nullptr,10));
      pass_profile.out=std::fopen(path,"w");if(!pass_profile.out)return false;
      std::fprintf(pass_profile.out,"frame,index,gpu_ms,what\n");
      VkQueryPoolCreateInfo q{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};q.queryType=VK_QUERY_TYPE_TIMESTAMP;q.queryCount=1024;
      return vkCreateQueryPool(device->handles.device,&q,nullptr,&pass_profile.pool)==VK_SUCCESS;
    }();
    pass_profile.active=configured&&g_frame>=pass_profile.from&&g_frame<=pass_profile.to;
    if(!pass_profile.active)return;
    vkCmdResetQueryPool(cmd,pass_profile.pool,0,1024);pass_profile.n=0;pass_profile.labels.clear();pass_profile.draws=0;
    pass_stamp("frame start");
  }
  void pass_profile_end(Submission submitted) {
    if(!pass_profile.active)return;
    pass_profile.active=false;ring->wait(submitted);
    std::vector<uint64_t> t(pass_profile.n);
    if(!pass_profile.n||vkGetQueryPoolResults(device->handles.device,pass_profile.pool,0,pass_profile.n,t.size()*8,t.data(),8,
       VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT)!=VK_SUCCESS)return;
    const double period=device->capabilities.properties.limits.timestampPeriod;
    for(unsigned i=1;i<pass_profile.n;++i)
      std::fprintf(pass_profile.out,"%u,%u,%.3f,%s\n",g_frame,i,double(t[i]-t[i-1])*period/1e6,pass_profile.labels[i].c_str());
    std::fflush(pass_profile.out);
  }
  unsigned hud_target=~0u,hud_target_next=~0u;
  bool picture_size(int w,int h) const {
    const auto *p=game_profile();
    return (w==game_width&&h==game_height) || (p->vr.world_w>0&&w==p->vr.world_w&&h==p->vr.world_h);
  }
  // As in the GL host, where a pbuffer in the headset is drawn as the eyes:
  // a pbuffer holding the game's frame - its size, or rounded up from it (The
  // House of the Dead 4 draws 640x480 in a 640x512 one) - is the eye picture.
  bool eye_pbuffer(int w,int h) const {
    return picture_size(w,h) || (w>=game_width&&h>=game_height&&w<2*game_width&&h<2*game_height);
  }
  std::array<int,2> logical_size() const {return drawable?drawable_sizes.at(drawable):std::array<int,2>{game_width,game_height};}
  void scene_end() {
    if(stereo)stereo->finish_frame();
    world_draws=hud_sequence=0;hud_target=hud_target_next;hud_target_next=~0u;
  }
#ifdef LINDBERGH_VULKAN_XR
  std::unique_ptr<ActivityRef> activity;
  std::unique_ptr<OpenXR> xr;
  std::unique_ptr<Target> unfocused;
  std::unique_ptr<ConstantDensityMap> unfocused_density;
  bool xr_rendering = false;
#endif
  std::unique_ptr<PipelineCache> cache;
  std::unique_ptr<PipelineRecipes> recipes;
  std::unique_ptr<FrameRing> ring;
  std::unique_ptr<ShaderPack> pack, fixed_pack;
  Program fixed_program;
  std::array<bool,8> texture_enabled{}, rect_enabled{}; // fixed-function GL_TEXTURE_2D / GL_TEXTURE_RECTANGLE
  bool unit_textured(unsigned i) const {return texture_enabled[i]||rect_enabled[i];}
  std::array<GLenum,8> texture_env{GL_MODULATE,GL_MODULATE,GL_MODULATE,GL_MODULATE,GL_MODULATE,GL_MODULATE,GL_MODULATE,GL_MODULATE};
  std::array<std::array<float,4>,8> env_color{};
  std::array<std::unordered_map<GLenum,float>,8> env_params; // each unit's GL_TEXTURE_ENV parameters
  std::unordered_map<unsigned, std::unique_ptr<Target>> drawables;
  std::unordered_set<unsigned> deleted_drawables;
  std::unordered_map<Target *, std::unique_ptr<Renderer>> renderers;
  std::vector<DrawPacket> packets;
  std::unordered_set<Target *> initialized;
  // Stencil bits that may be non-zero in each target (absent: unknown). A
  // masked stencil clear is an ordinary full clear when the masked-out bits
  // are known to be zero; on a tiler that also lets the pass begin cleared.
  std::unordered_map<Target *,uint32_t> stencil_bits;
  uint32_t stencil_known(Target *t) const {
    auto it=stencil_bits.find(t);uint32_t bits=it==stencil_bits.end()?255u:it->second;
    for(const auto &q:packets)if(q.target==t&&q.count&&q.raster.stencil)bits|=q.stencil_write_mask&255;
    return bits;
  }
  // A pass that would only clear the whole target is deferred: the next pass
  // on that target begins with this clear (or drops it, when it clears
  // anyway). Anything that reads the target first applies it (materialize).
  // done: the part already cleared (a pass limited to the region it drew);
  // empty when nothing of the clear has been applied yet.
  struct PendingClear {std::array<float,4> color;float depth;uint32_t stencil;VkRect2D done{};};
  std::unordered_map<Target *,PendingClear> pending_clears;
  static bool contains(const VkRect2D &outer,const VkRect2D &inner) {
    return outer.extent.width&&inner.offset.x>=outer.offset.x&&inner.offset.y>=outer.offset.y&&
           int64_t(inner.offset.x)+inner.extent.width<=int64_t(outer.offset.x)+outer.extent.width&&
           int64_t(inner.offset.y)+inner.extent.height<=int64_t(outer.offset.y)+outer.extent.height;
  }
  // Begin a pass on t, applying its pending clear: a clear begin when none of
  // it has been applied, else a load with the remaining area cleared.
  std::string pass_note; // profile label: what the open pass skips
  // Frame-local depth. A render target whose depth every frame begins by
  // clearing needs none kept after the frame's last pass that uses it.
  // Within a frame that is known exactly: the frame's final flush holds every
  // pass left, and only a pass recorded there with no later depth use of its
  // target qualifies. Across frames it is learned, not proven (GL keeps a
  // framebuffer's depth from frame to frame): 120 frames in a row must start
  // by clearing, and a pass that loads depth which was not kept - or a copy
  // of it - ends it for that target for good (that one frame read stale depth).
  struct DepthFrames {
    unsigned streak=0;
    bool touched=false,first_clear=false,lost=false,off=false;
  };
  std::unordered_map<Target *,DepthFrames> depth_frames;
  // A pass on t that uses depth begins (cleared: with its depth cleared);
  // frame_last: nothing later in the frame uses t's depth. Returns whether
  // the pass may leave its depth unstored.
  bool depth_pass(Target *t,bool cleared,bool frame_last=false) {
    if(t->info.frame_depth||t->info.transient_depth)return false; // never kept anyway
    auto &d=depth_frames[t];
    if(!d.touched){d.touched=true;d.first_clear=cleared;}
    if(cleared)d.lost=false;
    else if(d.lost)depth_lost(d);
    const bool discard=frame_last&&!d.off&&d.streak>=120;
    if(discard)d.lost=true;
    return discard;
  }
  void depth_lost(DepthFrames &d) {
    if(!d.off)std::fprintf(stderr,"[vulkan] frame %u: a render target's depth was read after it was not kept; it is kept from now on\n",g_frame);
    d.off=true;d.lost=false;
  }
  // A copy reads t's stored depth: never leave it unstored.
  void depth_read(Target *t) {
    auto &d=depth_frames[t];
    if(d.lost)depth_lost(d);
    d.off=true;
  }
  void depth_frame_end() {
    for(auto &[t,d]:depth_frames) {
      if(!d.touched)continue;
      d.streak=d.first_clear?d.streak+1:0;
      d.touched=false;
    }
  }
  void begin_pending(Renderer &r,Target *t,const PendingClear &c,bool last=false,bool frame_last=false) {
    pass_note=last?" last":"";
    if(!c.done.extent.width) {
      const bool discard=depth_pass(t,true,frame_last);if(discard)pass_note+=" no-depth-kept";
      r.begin(cmd,c.color,false,c.depth,c.stencil,nullptr,false,last,discard);stencil_bits[t]=c.stencil;return;
    }
    depth_pass(t,false);
    r.begin(cmd,c.color,true,1,0,nullptr,false,last);
    const int W=int(t->info.width),H=int(t->info.height),x0=c.done.offset.x,y0=c.done.offset.y;
    const int x1=x0+int(c.done.extent.width),y1=y0+int(c.done.extent.height);
    for(auto rect:{VkRect2D{{0,0},{uint32_t(W),uint32_t(y0)}},VkRect2D{{0,y1},{uint32_t(W),uint32_t(H-y1)}},
                   VkRect2D{{0,y0},{uint32_t(x0),uint32_t(y1-y0)}},VkRect2D{{x1,y0},{uint32_t(W-x1),uint32_t(y1-y0)}}})
      r.clear(VK_IMAGE_ASPECT_COLOR_BIT|VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,c.color,c.depth,c.stencil,rect);
  }
  void materialize(Target *t,const VkRect2D *read=nullptr) {
    auto it=pending_clears.find(t);if(it==pending_clears.end())return;
    if(read&&contains(it->second.done,*read))return; // the part read is already cleared
    const auto c=it->second;pending_clears.erase(it);
    end_pass();auto &r=renderers[t];if(!r)r=std::make_unique<Renderer>(*device,*t,*cache);
    begin_pending(*r,t,c);r->end();
  }
  static bool covers(const VkRect2D &r,const Target *t) {
    return r.offset.x<=0&&r.offset.y<=0&&int64_t(r.offset.x)+r.extent.width>=t->info.width&&int64_t(r.offset.y)+r.extent.height>=t->info.height;
  }
  std::array<std::unordered_map<std::string,std::shared_ptr<Material>>,3> materials;
  std::unordered_map<std::string,std::shared_ptr<Sampler>> samplers;
  std::shared_ptr<Texture> fallback, cube_fallback;
  std::unordered_map<Target *,std::shared_ptr<Texture>> target_views; // a drawable's colour, as a texture
  Target *active_target = nullptr;
  int game_width = 640, game_height = 480;
  GuestState matrices;
  GuestArrays arrays,legacy_bounds_arrays;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  std::optional<Submission> last_submission;
  Target *screen = nullptr;
  Renderer *active = nullptr;
  unsigned drawable = 0, next_drawable = 1, next_object = 1;
  GLuint array_buffer = 0, element_buffer = 0, pack_buffer = 0,
         unpack_buffer = 0, program = 0;
  GLenum matrix_mode = GL_MODELVIEW, active_texture = GL_TEXTURE0,
         client_texture = GL_TEXTURE0;
  std::unordered_map<GLuint, Buffer> buffers;
  std::unordered_map<GLuint, Shader> shaders;
  std::unordered_map<GLuint, Program> programs;
  std::unordered_map<GLenum, GLint> integers;
  std::unordered_map<GLenum, std::array<float, 4>> floats;
  std::unordered_map<GLenum, bool> enabled;
  std::array<GLuint, 8> textures{}, rect_textures{}, cube_textures{};
  std::unordered_map<GLuint, TextureObject> texture_objects;
  std::unordered_map<GLuint, Framebuffer> framebuffers;
  std::unordered_map<GLuint, std::array<int,2>> renderbuffers;
  GLuint framebuffer = 0, renderbuffer = 0;
  Framebuffer *bound_framebuffer = nullptr;
  unsigned glx_drawable = 0; // the GLX drawable, under any bound framebuffer object
  // Textures whose framebuffer was drawn since they last took its contents.
  std::unordered_map<GLuint, unsigned> stale_textures;
  std::unordered_set<GLuint> render_textures; // attached to a framebuffer object
  std::unordered_map<GLenum, std::vector<GLushort>> pixel_maps;
  std::array<GLint, 4> viewport{0, 0, 640, 480}, scissor{0, 0, 640, 480};
  std::array<float, 4> clear{0, 0, 0, 0};
  RasterState raster;
  bool ready = false;
  bool index_stats=false;
  uint64_t direct_index_draws=0,uploaded_index_draws=0,uploaded_index_bytes=0;
  uint64_t legacy_version=1;
  bool in_begin=false; GLenum immediate_mode=GL_TRIANGLES;
  std::vector<std::array<float,64>> immediate;
  std::vector<Attributes> attributes;
  void shutdown() {
    if (!device) {
#ifdef LINDBERGH_VULKAN_XR
      xr.reset(); activity.reset();
#endif
      return;
    }
    ready = false;
    ring.reset(); // completes buffered commands before referenced resources
    cmd = VK_NULL_HANDLE;
    active = nullptr; active_target = nullptr; screen = nullptr;
    packets.clear();
    for (auto &slot : materials) slot.clear();
    programs.clear();
    fixed_program.native.reset();
    if(recipes) {
      try {recipes->save();}
      catch(const std::exception &e){std::fprintf(stderr,"[vulkan-recipes] save: %s\n",e.what());}
      std::fprintf(stderr,"[vulkan-recipes] %zu retained before target use, %zu first-use pipelines; %zu catalogue entries\n",
                   recipes->prepared_count(),recipes->first_use_count(),recipes->recipe_count());
      recipes.reset();
    }
    renderers.clear();overlay.reset();panel.reset();
    if(index_stats) {
      uint64_t hits=0,scans=0;for(const auto &b:buffers){hits+=b.second.index_ranges.hits();scans+=b.second.index_ranges.scans();}
      std::fprintf(stderr,"[vulkan-indices] %llu native draws, %llu expanded draws, %llu expanded bytes; live EBO range cache %llu hits, %llu scans\n",
        (unsigned long long)direct_index_draws,(unsigned long long)uploaded_index_draws,(unsigned long long)uploaded_index_bytes,
        (unsigned long long)hits,(unsigned long long)scans);
    }
    if(Renderer::command_stats_enabled()) {
      const auto counts=Renderer::global_command_stats();
      for(size_t i=0;i<Renderer::command_count;++i)
        std::fprintf(stderr,"[vulkan-commands] %s: %llu issued, %llu filtered\n",
          Renderer::command_name(Renderer::Command(i)),(unsigned long long)counts.issued[i],(unsigned long long)counts.filtered[i]);
    }
    texture_objects.clear();framebuffers.clear();renderbuffers.clear();stale_textures.clear();render_textures.clear();
    bound_framebuffer=nullptr;framebuffer=0;
    buffers.clear();
    samplers.clear();
    fallback.reset();cube_fallback.reset();target_views.clear();
    drawables.clear();drawable_sizes.clear();deleted_drawables.clear();stereo.reset();controllers.reset();game_input={};
    initialized.clear();
#ifndef LINDBERGH_VULKAN_OFFSCREEN
    window.reset();
#endif
#ifdef LINDBERGH_VULKAN_OFFSCREEN
    offscreen.reset();offscreen_density.reset();
#endif
#ifdef LINDBERGH_VULKAN_XR
    unfocused.reset();unfocused_density.reset();
    xr.reset();
    activity.reset();
    host_display_timing_reset();
#endif
    if (cache) {
      try { cache->save(); }
      catch (const std::exception &e) { std::fprintf(stderr,"[vulkan] pipeline cache save: %s\n",e.what()); }
    }
    cache.reset();
    pack.reset(); fixed_pack.reset();
    device.reset();
  }
  ~State() { shutdown(); }
  void frame() {
    if (!cmd) {
      ring->frame_id(g_frame);
      cmd = ring->begin();
      pass_profile_begin();
      if (!screen) {
#ifdef LINDBERGH_VULKAN_XR
        screen = xr->begin_frame();
        host_display_timing(xr->display_time(),xr->display_period());
        xr_rendering = screen != nullptr;
        if(screen) {
          std::array<StereoPose,2> eyes;
          for(unsigned i=0;i<2;++i) {
            const auto &v=xr->views()[i];
            eyes[i].orientation={v.pose.orientation.x,v.pose.orientation.y,v.pose.orientation.z,v.pose.orientation.w};
            eyes[i].position={v.pose.position.x,v.pose.position.y,v.pose.position.z};
            eyes[i].tangents={-std::tan(v.fov.angleLeft),std::tan(v.fov.angleRight),-std::tan(v.fov.angleDown),std::tan(v.fov.angleUp)};
          }
          stereo->publish(eyes);
          LgXrActionSample input{};ControllerSample sample;
          const bool focused=xr->input(input);
          for(unsigned h=0;h<2;++h) {
            sample.valid[h]=input.valid[h];
            sample.pose[h].position={input.pose[h].position.x,input.pose[h].position.y,input.pose[h].position.z};
            sample.pose[h].orientation={input.pose[h].orientation.x,input.pose[h].orientation.y,input.pose[h].orientation.z,input.pose[h].orientation.w};
            sample.buttons.trigger[h]=input.trigger[h]>.55f;sample.buttons.grip[h]=input.grip[h]>.6f;
            sample.buttons.lower[h]=input.lower[h];sample.buttons.upper[h]=input.upper[h];sample.buttons.stick[h]=input.stick[h];
          }
          sample.menu=input.menu;
          panel_input(sample,xr->display_time(),focused);
          const auto &haptics=panel->haptics();
          for(int h=0;h<2;++h)if(haptics[h].milliseconds)xr->rumble(h,haptics[h].amplitude,haptics[h].milliseconds);
          for(auto &entry:drawables)if(entry.second->info.density_map)
            entry.second->select_density_map(screen->info.density_map);
        } else {
          stereo->invalidate();panel_input({},xr->display_time(),false);
          for(auto &entry:drawables)if(entry.second->info.density_map)
            entry.second->select_density_map(unfocused_density->view());
        }
        if (screen) {
          static bool first=true;
          if (first) {first=false;std::fprintf(stderr,"[vulkan-xr] first game target %ux%u, %u layers; foveation %s; session %d\n",screen->info.width,screen->info.height,screen->info.layers,xr->foveation_active()?"active":"disabled",int(xr->session_state()));}
        }
        if (!screen) {
          // Keep guest simulation/resources valid while the headset is not
          // rendering. Never hand this scratch target to the compositor.
          screen=unfocused.get();
        }
#else
#ifdef LINDBERGH_VULKAN_OFFSCREEN
        screen=offscreen.get();
#else
        screen = &window->acquire(*ring);
#endif
        if(stereo) {stereo->publish(fake_eyes);if(fake_guns)fake_controllers();}
#endif
        // A swapchain image: a buffer swap leaves the guest's back buffer
        // undefined (GLX), and the image itself comes back frames later. The
        // frame's first pass on it begins cleared rather than loading it.
        if(screen->info.frame_depth){initialized.erase(screen);pending_clears.erase(screen);stencil_bits.erase(screen);}
      }
      if(panel&&overlay)panel->upload(*overlay,cmd);
      if (materials[ring->recording_slot()].size()>2048) materials[ring->recording_slot()].clear();
    }
  }
  bool active_color_only=false; // the open pass left depth/stencil untouched
  static bool uses_depth(const DrawPacket &p) {
    return (p.clear_aspects&(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT))||
           (p.count&&(p.raster.depth_test||p.raster.stencil));
  }
  void end_pass() {
    active_color_only=false;
    if (active) {
      active->end();
      if(pass_profile.active) {
        unsigned id=0;for(const auto &d:drawables)if(d.second.get()==active_target)id=d.first;
        pass_stamp("pass "+std::string(id?"pbuffer "+std::to_string(id):"eye")+" "+std::to_string(active_target->info.width)+"x"+
                   std::to_string(active_target->info.height)+" draws "+std::to_string(pass_profile.draws)+
                   (active_target->info.density_map?" foveated":"")+pass_note);
        pass_profile.draws=0;
      }
      active = nullptr; active_target = nullptr;
    }
  }
  Target *target() { frame(); return drawable ? drawables.at(drawable).get() : screen; }
  // limit: record only the first limit packets; later ones stay queued (a
  // copy can then go between a source's passes and its consumer's).
  void flush(bool keep_last=false,size_t limit=SIZE_MAX) {
    end_pass();
    const size_t n=std::min(limit,packets.size());
    if(pass_profile.active&&std::getenv("LINDBERGH_VULKAN_FLUSH_LOG")) {
      std::fprintf(stderr,"[vulkan-flush] frame %u: %zu of %zu packets:",g_frame,n,packets.size());
      for(size_t i=0;i<packets.size();++i){unsigned id=0;for(const auto &d:drawables)if(d.second.get()==packets[i].target)id=d.first;
        std::fprintf(stderr," %s%u%s%s",i==n?"| ":"",id,packets[i].count?"":"c",packets[i].clear_aspects?"C":"");}
      std::fputc(10,stderr);
    }
    static const bool profile_prepare=[] {const auto *v=std::getenv("LINDBERGH_VULKAN_PREPARE_TIMING");return v && *v=='1';}();
    if (profile_prepare) {
      const auto start=std::chrono::steady_clock::now();
      size_t created=0;
      for (size_t i=0;i<n;++i) if (auto &p=packets[i];p.count) {
        const auto before=p.renderer->pipeline_count();
        p.renderer->prepare(p.raster);
        created+=p.renderer->pipeline_count()-before;
      }
      const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
      if (created || ms>2) std::fprintf(stderr,"[vulkan-prepare] frame %u: %zu packets, %zu new pipelines, %.3f ms\n",g_frame,packets.size(),created,ms);
    } else for (size_t i=0;i<n;++i) if (packets[i].count) packets[i].renderer->prepare(packets[i].raster);
    constexpr VkImageAspectFlags color_depth=VK_IMAGE_ASPECT_COLOR_BIT|VK_IMAGE_ASPECT_DEPTH_BIT;
    auto full_clear=[&](const DrawPacket &q) {
      if(q.count||!covers(q.scissor,q.target)||(q.clear_aspects&color_depth)!=color_depth)return false;
      return (q.clear_aspects&VK_IMAGE_ASPECT_STENCIL_BIT)||(stencil_bits.count(q.target)&&!stencil_bits[q.target]);
    };
    for (size_t index=0;index<n;++index) {
      auto &p=packets[index];
      bool cleared_on_begin=false;
      if (active_target != p.target && full_clear(p)) {
        // A run on this target of whole-target clears only: defer it.
        size_t end=index;while(end<n&&packets[end].target==p.target&&full_clear(packets[end]))++end;
        if(end==packets.size()?!keep_last:packets[end].target!=p.target) {
          // Deferring records nothing: an open pass on another target goes on.
          const auto &last=packets[end-1];
          const uint32_t stencil=(last.clear_aspects&VK_IMAGE_ASPECT_STENCIL_BIT)?last.clear_stencil&255:0;
          pending_clears[p.target]={last.color,last.clear_depth,stencil};
          stencil_bits[p.target]=stencil;initialized.insert(p.target);
          index=end-1;continue;
        }
      }
      if (active_target==p.target&&active_color_only&&uses_depth(p))end_pass(); // depth is needed after all
      if (active_target != p.target) {
        end_pass();
        // The frame's last pass on a frame-depth target (the eye): every later
        // packet of the frame draws here, and the overlay ends this pass.
        bool last=keep_last&&n==packets.size()&&p.target->info.frame_depth;
        for(size_t k=index+1;last&&k<n;++k)last=packets[k].target==p.target;
        pass_note.clear();
        // No later packet of the frame uses this target's depth (the final
        // flush holds the rest of the frame). The pass runs on through this
        // target's packets and over other targets' clear-only runs, which are
        // deferred (as below) rather than recorded.
        bool frame_last=keep_last&&n==packets.size();
        if(frame_last) {
          size_t k=index+1;
          while(k<n) {
            if(packets[k].target==p.target){++k;continue;}
            size_t e=k;while(e<n&&packets[e].target==packets[k].target&&full_clear(packets[e]))++e;
            if(e>k&&(e==packets.size()?!keep_last:packets[e].target!=packets[k].target)){k=e;continue;}
            break;
          }
          for(;frame_last&&k<n;++k)frame_last=!(packets[k].target==p.target&&uses_depth(packets[k]));
        }
        bool preserve=initialized.count(p.target)!=0;
        float depth=1;uint32_t stencil=0;
        // A pass that starts by clearing everything begins cleared instead of
        // loading what it is about to overwrite. Without a stencil clear, the
        // stencil must already be known to be all zero (the begin clear value).
        const bool stencil_zero=stencil_bits.count(p.target)&&!stencil_bits[p.target];
        if(preserve&&covers(p.scissor,p.target)&&(p.clear_aspects&color_depth)==color_depth&&
           ((p.clear_aspects&VK_IMAGE_ASPECT_STENCIL_BIT)||stencil_zero)) {
          preserve=false;depth=p.clear_depth;cleared_on_begin=true;
          stencil=(p.clear_aspects&VK_IMAGE_ASPECT_STENCIL_BIT)?p.clear_stencil&255:0;
        }
        bool begun=false;
        if(auto pending=pending_clears.find(p.target);pending!=pending_clears.end()) {
          // This pass clears everything itself, or begins with the deferred clear.
          const auto c=pending->second;pending_clears.erase(pending);
          if(!cleared_on_begin) {
            if(!c.done.extent.width) {
              depth=c.depth;stencil=c.stencil;stencil_bits[p.target]=stencil;
              const bool discard=depth_pass(p.target,true,frame_last);
              p.renderer->begin(cmd,c.color,false,depth,stencil,nullptr,false,last,discard);
              pass_note=std::string(last?" last":"")+(discard?" no-depth-kept":"");
            } else begin_pending(*p.renderer,p.target,c,last,frame_last);
            begun=true;
          }
        }
        if(!begun&&cleared_on_begin) {
          // Begins with a whole-target clear but draws only part of it: limit
          // the pass to that part; the rest of the clear stays pending.
          VkRect2D area{};
          for(size_t k=index+1;k<n&&packets[k].target==p.target;++k) {
            const auto &q=packets[k];VkRect2D r=q.scissor;
            if(q.count) {
              const int vx=int(std::floor(q.viewport[0])),vx1=int(std::ceil(q.viewport[0]+q.viewport[2]));
              const float top=q.viewport[1]+q.viewport[3];
              const int vy0=p.target->info.guest_bottom_origin?int(std::floor(q.viewport[1])):int(p.target->info.height)-int(std::ceil(top));
              const int vy1=p.target->info.guest_bottom_origin?int(std::ceil(top)):int(p.target->info.height)-int(std::floor(q.viewport[1]));
              const int x0=std::max(r.offset.x,vx),y0=std::max(r.offset.y,vy0);
              const int x1=int(std::min<int64_t>(int64_t(r.offset.x)+r.extent.width,vx1)),y1=int(std::min<int64_t>(int64_t(r.offset.y)+r.extent.height,vy1));
              if(x1<=x0||y1<=y0)continue;
              r={{x0,y0},{uint32_t(x1-x0),uint32_t(y1-y0)}};
            }
            if(!r.extent.width||!r.extent.height)continue;
            if(!area.extent.width)area=r;
            else {
              const int x0=std::min(area.offset.x,r.offset.x),y0=std::min(area.offset.y,r.offset.y);
              const int x1=int(std::max<int64_t>(int64_t(area.offset.x)+area.extent.width,int64_t(r.offset.x)+r.extent.width));
              const int y1=int(std::max<int64_t>(int64_t(area.offset.y)+area.extent.height,int64_t(r.offset.y)+r.extent.height));
              area={{x0,y0},{uint32_t(x1-x0),uint32_t(y1-y0)}};
            }
          }
          if(area.extent.width&&uint64_t(area.extent.width)*area.extent.height*4<=uint64_t(p.target->info.width)*p.target->info.height) {
            // Pad to whole 32-pixel tiles, inside the target.
            const int x0=std::max(0,area.offset.x/32*32-32),y0=std::max(0,area.offset.y/32*32-32);
            const int x1=std::min<int>(int(p.target->info.width),(int(area.offset.x+area.extent.width)+63)/32*32);
            const int y1=std::min<int>(int(p.target->info.height),(int(area.offset.y+area.extent.height)+63)/32*32);
            area={{x0,y0},{uint32_t(x1-x0),uint32_t(y1-y0)}};
            stencil_bits[p.target]=stencil;
            depth_pass(p.target,false); // only part is cleared: not a frame-local start
            p.renderer->begin(cmd,p.color,false,depth,stencil,&area);
            pending_clears[p.target]={p.color,depth,stencil,area};
            begun=true;
          }
        }
        if(!begun) {
          if(!preserve)stencil_bits[p.target]=stencil;
          // A preserving pass whose queued draws never touch depth or stencil
          // leaves them in memory untouched (a full-target composite loads and
          // stores only colour). A depth user arriving later restarts the pass.
          bool color_only=preserve&&p.target->color_pass;
          for(size_t k=index;color_only&&k<n&&packets[k].target==p.target;++k)color_only=!uses_depth(packets[k]);
          const bool discard=!color_only&&depth_pass(p.target,!preserve,frame_last);
          p.renderer->begin(cmd,p.color,preserve,depth,stencil,nullptr,color_only,last,discard);
          active_color_only=color_only;
          pass_note=color_only?" colour-only":std::string(last?" last":"")+(discard?" no-depth-kept":"");
        }
        initialized.insert(p.target); active=p.renderer; active_target=p.target;
      } else if (active != p.renderer) {
        active->detach(); p.renderer->attach(cmd); active=p.renderer;
      }
      active->viewport(p.viewport[0],p.viewport[1],p.viewport[2],p.viewport[3],p.scissor);
      if (p.clear_aspects&&!cleared_on_begin) {
        active->clear(p.clear_aspects,p.color,p.clear_depth,p.clear_stencil,p.scissor);
        if(p.clear_aspects&VK_IMAGE_ASPECT_STENCIL_BIT) {
          auto &bits=stencil_bits[p.target];
          bits=covers(p.scissor,p.target)&&(p.stencil_write_mask&255)==255?(p.clear_stencil&255):(bits|(p.clear_stencil&255));
        }
      }
      if (p.count&&p.raster.stencil) stencil_bits[p.target]|=p.stencil_write_mask&255;
      if (p.count) {
        ++pass_profile.draws;
        active->dynamic_state(p.stencil_ref,p.stencil_compare_mask,p.stencil_write_mask,p.depth_bias_factor,p.depth_bias_units,p.blend_constants.data());
        for (unsigned set=0;set<3;++set) if (p.descriptors[set])
          active->bind_descriptors(set,1,&p.descriptors[set],set<2?uint32_t(p.offsets[set].size()):0,set<2?p.offsets[set].data():nullptr);
        active->draw_buffers(p.streams.data(),uint32_t(p.streams.size()),p.count,p.matrices,p.raster,p.index.buffer?&p.index:nullptr);
      }
    }
    packets.erase(packets.begin(),packets.begin()+std::ptrdiff_t(n));if(!keep_last)end_pass();
  }
  Renderer &renderer() {
    frame();
    Target *target = drawable ? drawables.at(drawable).get() : screen;
    auto &renderer = renderers[target];
    if (!renderer)
      renderer = std::make_unique<Renderer>(*device, *target, *cache);
    if (active != renderer.get()) {
      end_pass();
      renderer->prepare(raster);
      if(auto pending=pending_clears.find(target);pending!=pending_clears.end()) {
        const auto c=pending->second;pending_clears.erase(pending);begin_pending(*renderer,target,c);
      } else {
        const bool preserve=initialized.count(target)!=0;
        depth_pass(target,!preserve);
        renderer->begin(cmd, clear, preserve);
      }
      initialized.insert(target);
      active = renderer.get(); active_target = target;
    }
    return *renderer;
  }
  GLuint &binding(GLenum target) {
    switch (target) {
    case GL_ARRAY_BUFFER:
      return array_buffer;
    case GL_ELEMENT_ARRAY_BUFFER:
      return element_buffer;
    case GL_PIXEL_PACK_BUFFER:
      return pack_buffer;
    case GL_PIXEL_UNPACK_BUFFER:
      return unpack_buffer;
    default:
      gap("unsupported guest buffer target");
    }
  }
};
State state;
void watch_game_states(); // the menu-state watch (below)
void State::fake_controllers()
{
  // In the device-free panel demonstration, turn toward the placed panel after
  // opening it. The menu remains fixed in LOCAL space, as on the headset.
  static const bool look_at_panel=[] {const auto *v=std::getenv("LINDBERGH_VULKAN_FAKE_MENU_LOOK");return v&&*v=='1';}();
  if(look_at_panel&&g_frame>120) {
    auto eyes=fake_eyes;const float delta=.55f,q[]={0,std::sin(delta/2),0,std::cos(delta/2)};
    for(auto &eye:eyes){const auto old=eye.position;lg_stereo_rotate(q,old.data(),eye.position.data());
      const float a=2*std::atan2(eye.orientation[1],eye.orientation[3])+delta;eye.orientation={0,std::sin(a/2),0,std::cos(a/2)};}
    stereo->publish(eyes);
  }
  ControllerSample sample;const int64_t time=1000000000+int64_t(g_frame)*1000000000/60;
  const double seconds=double(g_frame)/60;
  for(int h=0;h<2;++h) {
    sample.valid[h]=1;const bool reload=std::fmod(seconds,6.)>=5.;
    const float yaw=.15f*std::sin(float(seconds)*.7f)*(h?1:-1),pitch=reload?-.9f:.03f;
    const float cy=std::cos(yaw/2),sy=std::sin(yaw/2),cp=std::cos(pitch/2),sp=std::sin(pitch/2);
    sample.pose[h].orientation={sp*cy,cp*sy,-sp*sy,cp*cy};sample.pose[h].position={h?.16f:-.16f,-.22f,-.28f};
    sample.buttons.trigger[h]=(h==controllers->gun_hand()||fake_guns>1)&&(reload||std::fmod(seconds,1.)<.5);
    sample.buttons.stick[h]=std::fmod(seconds+(h==controllers->gun_hand()?0:5),10.)<.2;
  }
  panel_input(sample,time,true);
}
void State::panel_input(ControllerSample sample,int64_t time,bool focused)
{
  static const std::vector<float> no_rates;
#ifdef LINDBERGH_VULKAN_XR
  const auto &rates=xr->refresh_rates();
#else
  const auto &rates=no_rates;
#endif
  // Fake input uses a deterministic simulation clock. Its FPS readout must
  // measure elapsed host time rather than reporting that clock's fixed 60 Hz.
  int64_t readout_time=0;
#ifndef LINDBERGH_VULKAN_XR
  if(settings.show_stats)readout_time=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
  const unsigned effects=panel->update(settings,*stereo,*controllers,sample,time,focused,rates,readout_time);
  if(effects&XP_SAVE)xr_settings_save(&settings);
#ifdef LINDBERGH_VULKAN_XR
  if(effects&XP_RATE_CHANGED)xr->request_refresh(float(settings.refresh));
  if(effects&XP_FOVEATION_CHANGED)xr->set_foveation(settings.foveation);
  if(effects&XP_RECENTER)xr->recenter();
  if(effects&XP_RESTART)activity->restart();
#endif
}
ControllerSettings controller_settings(const LgXrSettings &settings,int width,int height)
{
  const auto *profile=game_profile();bool hud=profile->vr.aim_on_hud;
  if(const auto *v=std::getenv("LINDBERGH_XR_AIM_HUD"))hud=*v=='1';
  ControllerSettings result{profile->vr.width?profile->vr.width:width,profile->vr.height?profile->vr.height:height,
          bool(settings.dual),bool(settings.auto_coins),bool(settings.left_handed),hud,settings.aim};
  result.show_gun=settings.show_gun;result.laser=settings.laser;result.sight=settings.sight;result.gun_model=profile->vr.gun_model;
  return result;
}

void shutdown_vulkan() { state.shutdown(); }
void queue_clear(GLbitfield mask);
GLuint object() { return state.next_object++; }
void check_range(size_t size, GLintptr offset, GLsizeiptr bytes) {
  if (offset < 0 || bytes < 0 || size_t(offset) > size ||
      size_t(bytes) > size - size_t(offset))
    gap("guest buffer range exceeds storage");
}
} // namespace
extern "C" void host_vr_input(int,int,unsigned,int);
extern "C" void host_vr_input2(int,int,int);
extern "C" void lvk_host_input(void)
{
  if(!state.controllers||!state.controllers->consume(state.game_input))return;
  const auto &input=state.game_input;
  host_vr_input(input.ray[0].x,input.ray[0].y,input.held,input.coins);
  host_vr_input2(input.dual,input.ray[1].x,input.ray[1].y);
}
extern "C" int stereo_gun_ray(int player,float origin[3],float direction[3])
{
  if(player<0||player>1||!state.game_input.valid[player])return 0;
  std::copy(state.game_input.ray[player].origin.begin(),state.game_input.ray[player].origin.end(),origin);
  std::copy(state.game_input.ray[player].direction.begin(),state.game_input.ray[player].direction.end(),direction);return 1;
}
extern "C" int lvk_host_can_thread(void) { return state.ready; }
extern "C" int lvk_host_thread_enter(void) {
#ifdef LINDBERGH_VULKAN_XR
#ifdef __ANDROID__
  // SDL attaches this pthread once and detaches it through its TLS destructor.
  if(!SDL_AndroidGetJNIEnv())return 0;
#endif
  if(state.xr)state.xr->renderer_thread();
  return 1;
#else
  return 1;
#endif
}
#ifdef LINDBERGH_VULKAN_XR
// One headset frame during startup work: a "Compiling shaders" panel ahead of
// the player. The frame loop has not started; keep the session's frames going.
static void loading_frame(const char *line) {
  Target *target=state.xr->begin_frame();
  if(!target) {if(state.xr->frame_open())state.xr->end_frame(false);return;}
  std::array<StereoPose,2> eyes;
  for(unsigned i=0;i<2;++i) {
    const auto &v=state.xr->views()[i];
    eyes[i].orientation={v.pose.orientation.x,v.pose.orientation.y,v.pose.orientation.z,v.pose.orientation.w};
    eyes[i].position={v.pose.position.x,v.pose.position.y,v.pose.position.z};
    eyes[i].tangents={-std::tan(v.fov.angleLeft),std::tan(v.fov.angleRight),-std::tan(v.fov.angleDown),std::tan(v.fov.angleUp)};
  }
  state.stereo->publish(eyes);
  static std::vector<unsigned char> pixels(size_t(XM_W)*XM_H*4);
  xm_paint_message(pixels.data(),"Compiling shaders",line);
  auto cmd=state.ring->begin();
  state.overlay->upload_panel(cmd,0,pixels.data(),pixels.size());
  XpPanel label{};label.hw=.55f;label.hh=label.hw*XM_H/XM_W;
  const float x[]={1,0,0},y[]={0,1,0},z[]={0,0,1},at[]={0,0,-1.5f};float offset[3];
  const auto &q=state.stereo->head_orientation();
  lg_stereo_rotate(q.data(),x,label.right);lg_stereo_rotate(q.data(),y,label.up);lg_stereo_rotate(q.data(),z,label.n);lg_stereo_rotate(q.data(),at,offset);
  for(int i=0;i<3;++i)label.c[i]=(eyes[0].position[i]+eyes[1].position[i])*.5f+offset[i];
  std::array<PanelDraw,2> draws{};draws[0].visible=true;xp_matrix(&label,draws[0].model.data());
  state.overlay->draw(cmd,*target,state.stereo->overlay_matrices(),state.stereo->overlay_eyes(),{},nullptr,false,draws);
  state.ring->submit();
  state.xr->end_frame(true);
}
#endif
// Compile every catalogued pipeline before the game draws, so play never
// waits on the driver's compiler. Later launches find them in the saved cache.
static void precompile_pipelines() {
  std::vector<Target *> ready;VkImageView density=VK_NULL_HANDLE;
#ifdef LINDBERGH_VULKAN_XR
  for(const auto &target:state.xr->render_targets())ready.push_back(target.get());
  ready.push_back(state.unfocused.get());
  if(!state.xr->density_maps().empty())density=state.xr->density_maps().front();
#elif defined(LINDBERGH_VULKAN_OFFSCREEN)
  ready.push_back(state.offscreen.get());density=state.offscreen->info.density_map;
#else
  for(const auto &target:state.window->render_targets())ready.push_back(target.get());
#endif
  const auto started=std::chrono::steady_clock::now();auto last=started;
  std::function<void(size_t,size_t)> progress;
#ifdef LINDBERGH_VULKAN_XR
  if(state.overlay&&state.stereo) {
    progress=[&](size_t done,size_t total) {
      const auto now=std::chrono::steady_clock::now();
      // A warm cache finishes in milliseconds: say nothing unless it is slow.
      if(now-started<std::chrono::milliseconds(250))return;
      if(now-last<std::chrono::milliseconds(40))return; // a frame while the compiler works
      last=now;char text[48];std::snprintf(text,sizeof text,"%zu%%  -  slow only on the first launch",total?done*100/total:100);loading_frame(text);
    };
  }
#endif
  (void)last;
  state.recipes->precompile(ready,density,progress);
  // Keep the work even if the app is killed before a clean exit.
  try {state.cache->save();}
  catch(const std::exception &e){std::fprintf(stderr,"[vulkan] pipeline cache save: %s\n",e.what());}
  try {state.recipes->save();}
  catch(const std::exception &e){std::fprintf(stderr,"[vulkan-recipes] save: %s\n",e.what());}
}
extern "C" int lvk_host_init(SDL_Window *window, int w, int h) {
  if (state.ready)
    return 1;
  try {
#ifdef LINDBERGH_VULKAN_XR
    state.activity=std::make_unique<ActivityRef>();
    state.activity->shader_defaults();
    xr_settings_load(&state.settings);
    {
      // Same vr.cfg lookup (and LINDBERGH_XR_SET override) as the GL host.
      std::error_code ignored;
      const char *source=std::filesystem::exists("vr.cfg",ignored)?"vr.cfg":std::filesystem::exists("../vr.cfg",ignored)?"../vr.cfg":"defaults";
      const auto &c=state.settings;
      std::fprintf(stderr,"[vulkan-xr] settings from %s%s (cwd %s): %.2f game units/m, HUD screen %.1f m, hud pitch %d, aim %.0f m, %dx%d @ %d Hz\n",
        source,std::getenv("LINDBERGH_XR_SET")?" + LINDBERGH_XR_SET":"",std::filesystem::current_path(ignored).string().c_str(),
        c.scale,c.screen,c.hud_pitch,c.aim,c.width,c.resolution,c.refresh);
    }
    const auto *profile=game_profile();
    bool hud_lock=profile->vr.hud_head_locked;
    if(const auto *v=std::getenv("LINDBERGH_XR_HUD_LOCK"))hud_lock=*v=='1';
    StereoSettings stereo{state.settings.scale,state.settings.screen,profile->vr.hud_lens,float(state.settings.hud_pitch),8,hud_lock};
    if(const auto *v=std::getenv("LINDBERGH_STEREO_2D_LENS"))stereo.hud_lens=std::atof(v);
    state.stereo=std::make_unique<StereoScene>(stereo);
    state.controllers=std::make_unique<ControllerInput>(controller_settings(state.settings,w,h));
    OpenXRConfig config{true,true,true,true,uint32_t(state.settings.width),uint32_t(state.settings.resolution),float(state.settings.refresh)};
    config.enable_input=true;config.foveation_level=state.settings.foveation;
    if(const auto *v=std::getenv("LINDBERGH_CPU_BOOST"))config.cpu_boost=*v=='1';
    state.xr=std::make_unique<OpenXR>(config,state.activity->platform());
    state.device=Device::create(state.xr->device_options());
#else
#ifdef LINDBERGH_VULKAN_OFFSCREEN
    DeviceOptions options;options.require_foveation=std::getenv("LINDBERGH_VULKAN_OFFSCREEN_DENSITY")!=nullptr;
    // A CPU rasterizer (lavapipe) only on request: correctness runs without a GPU
    if(const auto *v=std::getenv("LINDBERGH_VULKAN_ALLOW_SOFTWARE"))options.allow_software=*v=='1';
    state.device=Device::create(options);
#else
    state.device = Device::create(desktop_options(window));
#endif
#endif
    // The loader/layer C++ globals are initialized during device creation.
    // Retire our resources before their atexit destructors, not afterward
    // when the process-global guest state would normally be destroyed.
    static const bool cleanup_registered = std::atexit(shutdown_vulkan) == 0;
    if (!cleanup_registered) gap("cannot register Vulkan shutdown");
#ifdef LINDBERGH_VULKAN_XR
    host_display_timing_reset();
    state.xr->attach(*state.device);
#else
#ifndef LINDBERGH_VULKAN_OFFSCREEN
    state.window = std::make_unique<Window>(*state.device, window, 3);
#else
    if(!std::getenv("LINDBERGH_VULKAN_FAKE_YAW"))SDL_setenv("LINDBERGH_VULKAN_FAKE_YAW","0",1);
#endif
    if(const auto *yaw=std::getenv("LINDBERGH_VULKAN_FAKE_YAW")) {
      const auto *profile=game_profile();
      StereoSettings settings;settings.scale=profile->vr.scale>0?profile->vr.scale:1;settings.hud_lens=profile->vr.hud_lens;settings.head_locked=profile->vr.hud_head_locked;
      if(const auto *v=std::getenv("LINDBERGH_VULKAN_FAKE_GUN"))state.fake_guns=std::atoi(v);
#ifdef LINDBERGH_VULKAN_OFFSCREEN
      if(!state.fake_guns)state.fake_guns=1;
#endif
      if(state.fake_guns) {
        xr_settings_load(&state.settings);state.settings.dual=state.fake_guns>1;
        settings.scale=state.settings.scale;settings.screen=state.settings.screen;settings.hud_pitch=float(state.settings.hud_pitch);
        state.controllers=std::make_unique<ControllerInput>(controller_settings(state.settings,w,h));
      }
      state.stereo=std::make_unique<StereoScene>(settings);
      const float angle=std::atof(yaw)*.017453292519943295f;
      const std::array<float,4> q{0,std::sin(angle/2),0,std::cos(angle/2)};
      for(unsigned eye=0;eye<2;++eye) {
        state.fake_eyes[eye].orientation=q;
        const float offset[]={eye?.032f:-.032f,0,0};
        lg_stereo_rotate(q.data(),offset,state.fake_eyes[eye].position.data());
      }
      std::fprintf(stderr,"[vulkan] simulated eye poses at yaw %.1f degrees; no headset/display proof\n",std::atof(yaw));
    }
#endif
    const char *cache = std::getenv("LINDBERGH_VULKAN_CACHE");
    std::string cache_path;
    if (cache) cache_path=cache; // an explicitly empty override disables persistence
    else {
      // GLX games can create a generic-titled SDL window before naming it.
      // The applied game profile is the stable per-title cache namespace.
      const auto *profile=game_profile();
      const char *title=profile->title && *profile->title ? profile->title : SDL_GetWindowTitle(window);
      if (auto *directory=SDL_GetPrefPath("lindberghrecomp",title)) {
        cache_path=std::string(directory)+"vulkan-pipelines.bin";
        SDL_free(directory);
      }
    }
    // A fresh install starts from the driver cache packaged for its headset
    // (vulkan/pipelines.bin, harvested on that GPU and driver): the startup
    // precompile then finds every pipeline already built. PipelineCache checks
    // vendor, device and cache UUID, so on any other GPU or driver it is
    // ignored and the precompile builds them as before.
    if(const char *seed=std::getenv("LINDBERGH_VULKAN_CACHE_SEED");seed&&*seed&&!cache_path.empty()) {
      std::error_code ec;
      if(!std::filesystem::exists(cache_path,ec)&&std::filesystem::copy_file(seed,cache_path,ec))
        std::fprintf(stderr,"[vulkan] pipeline cache started from the packaged one: %s\n",seed);
    }
    state.cache=std::make_unique<PipelineCache>(*state.device,cache_path);
    if (!cache_path.empty()) std::fprintf(stderr,"[vulkan] pipeline cache %s: %s\n",
                                         state.cache->loaded()?"loaded":"cold",cache_path.c_str());
    state.ring = std::make_unique<FrameRing>(*state.device, 3, 64u << 20);
    state.index_stats=Renderer::command_stats_enabled();
    if(const auto *path=std::getenv("LINDBERGH_VULKAN_SUBMISSION_TIMINGS"))state.ring->profile(path);
#ifdef LINDBERGH_VULKAN_XR
    const auto density=state.xr->density_extent();
    state.unfocused_density=std::make_unique<ConstantDensityMap>(*state.device,*state.ring,density[0],density[1],2);
    state.unfocused=std::make_unique<Target>(*state.device,TargetInfo{uint32_t(w),uint32_t(h),2});
#endif
#ifdef LINDBERGH_VULKAN_OFFSCREEN
    TargetInfo offscreen{uint32_t(state.settings.width),uint32_t(state.settings.resolution),2};
    if(!offscreen.width)offscreen.width=offscreen.height; // no runtime recommendation in this diagnostic
    if(const auto *value=std::getenv("LINDBERGH_VULKAN_OFFSCREEN_DENSITY")) {
      const float density=std::strtof(value,nullptr);
      if(!std::isfinite(density)||density<=0||density>1)throw std::runtime_error("Invalid diagnostic density");
      if(!state.device->capabilities.density_map_non_subsampled)throw std::runtime_error("Offscreen density needs non-subsampled FDM");
      VkPhysicalDeviceFragmentDensityMapPropertiesEXT density_properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_PROPERTIES_EXT};
      VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};properties.pNext=&density_properties;
      vkGetPhysicalDeviceProperties2(state.device->handles.physical,&properties);
      const auto extent=density_properties.maxFragmentDensityTexelSize;
      if(!extent.width||!extent.height)throw std::runtime_error("Invalid density-map texel extent");
      state.offscreen_density=std::make_unique<ConstantDensityMap>(*state.device,*state.ring,
          (offscreen.width+extent.width-1)/extent.width,(offscreen.height+extent.height-1)/extent.height,2,density);
      offscreen.density_map=state.offscreen_density->view();
    }
    state.offscreen=std::make_unique<Target>(*state.device,offscreen);
    std::fprintf(stderr,"[vulkan-offscreen] diagnostic %ux%u x2; constant density map %s; no OpenXR/compositor/display proof\n",
      offscreen.width,offscreen.height,offscreen.density_map?"enabled":"disabled");
#endif
    const auto *recipes_path=std::getenv("LINDBERGH_VULKAN_RECIPES");
    const auto *seed=std::getenv("LINDBERGH_VULKAN_RECIPES_SEED");
    const std::string recipe_file=recipes_path?recipes_path:(cache_path.empty()?"":cache_path+".recipes");
    if(!recipe_file.empty()||(seed&&*seed)) {
      const auto *game=std::getenv("LINDBERGH_VULKAN_SHADERS");
      if(game&&*game) {
        state.pack=std::make_unique<ShaderPack>(game);
        const auto *fixed=std::getenv("LINDBERGH_VULKAN_FIXED_SHADERS");
        state.fixed_pack=std::make_unique<ShaderPack>(fixed&&*fixed?fixed:LINDBERGH_VULKAN_FIXED_ARCHIVE);
        state.recipes=std::make_unique<PipelineRecipes>(*state.device,*state.cache,
          std::vector<const ShaderPack *>{state.pack.get(),state.fixed_pack.get()},recipe_file,seed?seed:"");
      }
    }
    if(state.controllers) {
      const auto *fake=std::getenv("LINDBERGH_VULKAN_FAKE_MENU");
      state.panel=std::make_unique<SettingsPanel>(state.settings.width,state.settings.resolution,game_profile()->title,fake&&*fake=='1');
      state.overlay=std::make_unique<Overlay>(*state.device,*state.ring,*state.cache);
#ifdef LINDBERGH_VULKAN_XR
      for(const auto &target:state.xr->render_targets())state.overlay->prepare(*target);
#else
#ifdef LINDBERGH_VULKAN_OFFSCREEN
      state.overlay->prepare(*state.offscreen);
#else
      for(const auto &target:state.window->render_targets())state.overlay->prepare(*target);
#endif
#endif
    }
    if(state.recipes)precompile_pipelines();
    state.game_width=w; state.game_height=h;
    if(state.stereo)watch_game_states();
    state.viewport = state.scissor = {0, 0, w, h};
    state.arrays.current(3,{1,1,1,1});state.arrays.current(2,{0,0,1,0});
    state.raster.depth_test = false;
    state.raster.depth_write = true;
    state.ready = true;
    std::fprintf(stderr, "[vulkan] guest graphics routed directly to Vulkan; "
#ifdef LINDBERGH_VULKAN_OFFSCREEN
                         "offscreen diagnostic target ready; no OpenXR session\n");
#elif defined(LINDBERGH_VULKAN_XR)
                         "foveated OpenXR swapchain ready; world/input integration in progress\n");
#else
                         "desktop swapchain ready\n");
#endif
    return 1;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "[vulkan] initialization: %s\n", e.what());
    state.shutdown();
    return 0;
  }
}

namespace {
struct FrameCapture {
 Device &device;VkBuffer buffer=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;void *mapped=nullptr;uint32_t layers=1;
 FrameCapture(Device &d,VkCommandBuffer cmd,Target &target):device(d) {
#ifdef LINDBERGH_VULKAN_OFFSCREEN
  layers=std::min(2u,target.info.layers);
#endif
  const auto bytes=VkDeviceSize(target.info.width)*target.info.height*4*layers;
  VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};create.size=bytes;create.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  check(vkCreateBuffer(d.handles.device,&create,nullptr,&buffer),"capture buffer");
  VkMemoryRequirements req;vkGetBufferMemoryRequirements(d.handles.device,buffer,&req);
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};alloc.allocationSize=req.size;
  alloc.memoryTypeIndex=d.memory_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  check(vkAllocateMemory(d.handles.device,&alloc,nullptr,&memory),"capture memory");check(vkBindBufferMemory(d.handles.device,buffer,memory,0),"bind capture");
  check(vkMapMemory(d.handles.device,memory,0,bytes,0,&mapped),"map capture");
  VkImageMemoryBarrier image{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};image.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;image.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
  image.oldLayout=target.info.final_layout;image.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  image.srcQueueFamilyIndex=image.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;image.image=target.color;image.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,layers};
  vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&image);
  VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,layers};copy.imageExtent={target.info.width,target.info.height,1};
  vkCmdCopyImageToBuffer(cmd,target.color,image.newLayout,buffer,1,&copy);
  image.oldLayout=image.newLayout;image.newLayout=target.info.final_layout;image.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;image.dstAccessMask=0;
  vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&image);
  VkBufferMemoryBarrier ready{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};ready.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;ready.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
  ready.srcQueueFamilyIndex=ready.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;ready.buffer=buffer;ready.size=VK_WHOLE_SIZE;
  vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&ready,0,nullptr);
 }
 void write(const char *path,const Target &target) {
  for(uint32_t layer=0;layer<layers;++layer) {
    std::string name=path;if(layer){const auto suffix=name.rfind(".ppm");if(suffix!=std::string::npos&&suffix+4==name.size())name.insert(suffix,".right");else name+=".right.ppm";}
    std::ofstream out(name,std::ios::binary);out<<"P6\n"<<target.info.width<<' '<<target.info.height<<"\n255\n";
    const auto p=static_cast<const uint8_t *>(mapped)+size_t(layer)*target.info.width*target.info.height*4;
    const bool bgra=target.info.color_format==VK_FORMAT_B8G8R8A8_UNORM||target.info.color_format==VK_FORMAT_B8G8R8A8_SRGB;
    for(size_t i=0;i<size_t(target.info.width)*target.info.height;++i) {const char rgb[]={char(p[4*i+(bgra?2:0)]),char(p[4*i+1]),char(p[4*i+(bgra?0:2)])};out.write(rgb,3);}
    if(!out)std::fprintf(stderr,"[vulkan] frame capture write failed: %s\n",name.c_str());
    else if(layers==1)std::fprintf(stderr,"[vulkan] frame capture %s\n",name.c_str());
    else std::fprintf(stderr,"[vulkan] frame capture eye %u %s\n",layer,name.c_str());
  }
 }
 ~FrameCapture() {if(mapped)vkUnmapMemory(device.handles.device,memory);if(buffer)vkDestroyBuffer(device.handles.device,buffer,nullptr);if(memory)vkFreeMemory(device.handles.device,memory,nullptr);}
};
}
extern "C" unsigned g_frame;
extern "C" void lvk_host_present(void) {
  state.frame();
  bool show_overlay=state.overlay&&state.stereo&&state.stereo->tracking_valid();
#ifdef LINDBERGH_VULKAN_XR
  show_overlay=show_overlay&&state.xr_rendering;
#endif
  state.flush(show_overlay);
  state.depth_frame_end(); // every pass of this frame has begun
  if(state.active_target!=state.screen)state.materialize(state.screen);
  if (!state.initialized.count(state.screen)) {
    const auto old=state.drawable;state.drawable=0;state.renderer();state.end_pass();state.drawable=old;
  }
  if(show_overlay) {
    Renderer *previous=state.active_target==state.screen?state.active:nullptr;
    if(state.active&&!previous)state.end_pass();
    state.overlay->draw(state.cmd,*state.screen,state.stereo->overlay_matrices(),state.stereo->overlay_eyes(),state.panel->guns(),previous,true,state.panel->draws());
    state.pass_stamp("pass eye "+std::to_string(state.screen->info.width)+"x"+std::to_string(state.screen->info.height)+
                     " draws "+std::to_string(state.pass_profile.draws)+" + overlay"+(previous?state.pass_note:std::string(" own pass")));
    state.pass_profile.draws=0;
    state.active=nullptr;state.active_target=nullptr;
    static const bool overlay_log=std::getenv("LINDBERGH_VULKAN_OVERLAY_LOG")!=nullptr;
    if(overlay_log&&g_frame%600==0)std::fprintf(stderr,"[vulkan-overlay] frame %u: %zu retained meshes, %zu prepared targets, %llu multiview draws, %llu additional passes\n",
        g_frame,state.overlay->mesh_buffer_count(),state.overlay->target_count(),(unsigned long long)state.overlay->draw_count(),(unsigned long long)state.overlay->render_pass_starts());
  }
  const char *shot=std::getenv("LINDBERGH_SHOT");const char *at=std::getenv("LINDBERGH_SHOT_FRAME");
  const char *period=std::getenv("LINDBERGH_SHOT_EVERY"), *from=std::getenv("LINDBERGH_SHOT_FROM");
  const unsigned every=period?unsigned(std::strtoul(period,nullptr,10)):0;
  std::string capture_path;
  if (shot && *shot && (every ? (g_frame && g_frame%every==0 && g_frame>=unsigned(from?std::strtoul(from,nullptr,10):0))
                             : g_frame==unsigned(at?std::strtoul(at,nullptr,10):600))) {
    capture_path=shot;
    if(every) { char suffix[32];std::snprintf(suffix,sizeof suffix,".%05u.ppm",g_frame);capture_path+=suffix; }
  }
  std::unique_ptr<FrameCapture> capture;
#ifdef LINDBERGH_VULKAN_XR
  if (!state.xr_rendering) capture_path.clear(); // never report a scratch frame as XR output
#endif
  Target *capture_target=state.screen;
  if(!capture_path.empty()) {
    if(const auto *v=std::getenv("LINDBERGH_VULKAN_SHOT_DRAWABLE")) {
      const auto id=unsigned(std::strtoul(v,nullptr,10));
      if(id) {
        capture_target=state.drawables.at(id).get();
        if(!state.initialized.count(capture_target))gap("capture drawable has not been initialized");
        std::fprintf(stderr,"[vulkan] diagnostic capture of guest drawable %u; not final display output\n",id);
      }
    }
    state.materialize(capture_target);
    capture=std::make_unique<FrameCapture>(*state.device,state.cmd,*capture_target);
  }
#ifdef LINDBERGH_VULKAN_XR
  auto submitted=state.ring->submit();
  state.xr->end_frame(state.xr_rendering);
  if (state.xr->exiting()) {SDL_Event quit{};quit.type=SDL_QUIT;SDL_PushEvent(&quit);}
#else
#ifdef LINDBERGH_VULKAN_OFFSCREEN
  auto submitted=state.ring->submit();
#else
  auto submitted=state.window->present(*state.ring);
#endif
#endif
  state.last_submission=submitted;
  state.pass_profile_end(submitted);
  if(capture) {state.ring->wait(submitted);capture->write(capture_path.c_str(),*capture_target);}
  state.cmd=VK_NULL_HANDLE;
  state.screen=nullptr;
  state.scene_end();
}
extern "C" void lvk_glFlush(void) {
  if (!state.cmd) return;
  state.flush();state.pass_stamp("glFlush");
#ifdef LINDBERGH_VULKAN_XR
  state.last_submission=state.ring->submit();
#else
#ifdef LINDBERGH_VULKAN_OFFSCREEN
  state.last_submission=state.ring->submit();
#else
  state.last_submission=state.window->flush(*state.ring);
#endif
#endif
  state.cmd=VK_NULL_HANDLE; // next draw continues on the same acquired image
}
extern "C" void lvk_glFinish(void) {
  lvk_glFlush();
  if (state.last_submission) state.ring->wait(*state.last_submission);
}
namespace {
unsigned create_drawable(int w, int h, bool rect, bool depthless = false);
void changed(TextureObject &o);
std::shared_ptr<Texture> native_image(TextureObject &o, int overwritten_level, bool gpu_copy);
}
extern "C" unsigned lvk_create_drawable(int w, int h) { return create_drawable(w,h,false); }
namespace {
// rect: a rectangle texture's target, sampled in pixels, keeps the eyes' scale.
// depthless: a framebuffer object without depth or stencil attachments.
// A framebuffer object's colour texture samples its drawable's own image (GL's
// render-to-texture) instead of a copy.
unsigned create_drawable(int w, int h, bool rect, bool depthless) {
  if (w <= 0 || h <= 0)
    gap("invalid guest drawable dimensions");
  const auto id = state.next_drawable++;
  // A rounded-up eye pbuffer (HOTD4: 640x480 drawn in 640x512) becomes exactly
  // the eye frame, like an exact one: the swapchain's size, so its foveation
  // and copies match. What lies beyond the frame is clipped (GL also keeps
  // side passes out of the eyes).
  const bool clipped_eye=state.stereo&&state.eye_pbuffer(w,h)&&!state.picture_size(w,h);
  if(clipped_eye) {
    std::fprintf(stderr,"[vulkan] pbuffer %u: %dx%d holds the %dx%d frame; drawn as the eye frame, the rest clipped\n",id,w,h,state.game_width,state.game_height);
    w=state.game_width;h=state.game_height;
  }
  TargetInfo info{uint32_t(w),uint32_t(h),state.stereo?2u:1u};
#ifdef LINDBERGH_VULKAN_XR
  const auto *profile=game_profile();
  const LgStereoTargetSize sizes={state.game_width,state.game_height,profile->vr.world_w,profile->vr.world_h,int(state.xr->width()),int(state.xr->height())};
  float sx,sy;lg_stereo_target_scale(&sizes,w,h,rect,&sx,&sy);
  info.width=uint32_t(std::lround(w*sx));info.height=uint32_t(std::lround(h*sy));
  std::fprintf(stderr,"[vulkan-xr] pbuffer %u: %dx%d -> %ux%u, %u layers\n",id,w,h,info.width,info.height,info.layers);
  const bool foveated_world=state.picture_size(w,h) && info.width==state.xr->width() && info.height==state.xr->height();
  if(foveated_world) {
    if(state.xr->density_maps().empty())gap("World pbuffer requires runtime density maps");
    info.density_map=state.unfocused_density->view();
  }
#endif
#ifdef LINDBERGH_VULKAN_OFFSCREEN
  const auto *profile=game_profile();
  const LgStereoTargetSize sizes={state.game_width,state.game_height,profile->vr.world_w,profile->vr.world_h,
      int(state.offscreen->info.width),int(state.offscreen->info.height)};
  float sx,sy;lg_stereo_target_scale(&sizes,w,h,rect,&sx,&sy);
  info.width=uint32_t(std::lround(w*sx));info.height=uint32_t(std::lround(h*sy));
  if(state.picture_size(w,h)&&info.width==state.offscreen->info.width&&info.height==state.offscreen->info.height)
    info.density_map=state.offscreen->info.density_map;
  std::fprintf(stderr,"[vulkan-offscreen] pbuffer %u %dx%d -> %ux%u x%u, density map %s\n",
      id,w,h,info.width,info.height,info.layers,info.density_map?"enabled":"disabled");
#endif
  state.drawable_sizes[id]={w,h};
  info.guest_bottom_origin=true;info.transient_depth=depthless;
  // A drawable's colour is only ever read back (copied, or sampled by its
  // framebuffer object's texture): its passes end ready to be sampled.
  info.final_layout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  state.drawables[id] = std::make_unique<Target>(*state.device, info);
  if(state.recipes)state.recipes->prepare(*state.drawables[id]);
#ifdef LINDBERGH_VULKAN_XR
  if(foveated_world) {
    auto &target=*state.drawables.at(id);
    target.prepare_density_maps(state.xr->density_maps());
    if(state.xr_rendering)target.select_density_map(state.screen->info.density_map);
    std::fprintf(stderr,"[vulkan-xr] world pbuffer %u uses acquired-image foveation; %zu framebuffers prepared\n",id,target.density_framebuffer_count());
  }
#endif
  return id;
}
void retire_drawable(unsigned id) {
  auto found=state.drawables.find(id);
  if(found==state.drawables.end())return;
  // Bake queued draws/clears before detaching their caches. This closes the
  // render pass but does not submit or wait for GPU work.
  if(state.cmd)state.flush();
  auto retired=std::make_shared<RetiredTarget>();
  retired->target=std::move(found->second);
  auto *target=retired->target.get();
  state.target_views.erase(target);
  for(auto &o:state.texture_objects)if(o.second.alias_target==target){o.second.alias.reset();o.second.alias_target=nullptr;}
  if(state.recipes)retired->renderers=state.recipes->retire(*target);
  auto detach=[&](Program &program) {
    if(!program.native)return;
    auto &renderers=program.native->renderers;
    for(auto entry=renderers.begin();entry!=renderers.end();) {
      if(&entry->second->target()==target) {
        retired->renderers.push_back(std::move(entry->second));
        entry=renderers.erase(entry);
      } else ++entry;
    }
  };
  for(auto &program:state.programs)detach(program.second);
  detach(state.fixed_program);
  if(auto clear=state.renderers.find(target);clear!=state.renderers.end()) {
    retired->renderers.push_back(std::move(clear->second));state.renderers.erase(clear);
  }
  state.initialized.erase(target);state.stencil_bits.erase(target);state.pending_clears.erase(target);state.depth_frames.erase(target);
  state.drawables.erase(found);state.drawable_sizes.erase(id);state.deleted_drawables.erase(id);
  if(state.hud_target==id)state.hud_target=~0u;
  if(state.hud_target_next==id)state.hud_target_next=~0u;
  state.ring->retire(std::move(retired));
}
}
extern "C" void lvk_destroy_drawable(unsigned id) {
  if(!state.drawables.count(id))return;
  // GLX keeps a destroyed current pbuffer alive until it is unbound.
  if(state.glx_drawable==id)state.deleted_drawables.insert(id);
  else retire_drawable(id);
}
extern "C" void lvk_bind_drawable(unsigned id) {
  if (id && !state.drawables.count(id))
    gap("unknown guest Vulkan drawable");
  const auto previous=state.glx_drawable;
  state.glx_drawable = id;
  if(!state.framebuffer)state.drawable = id;
  if(previous!=id && state.deleted_drawables.count(previous))retire_drawable(previous);
}

namespace {
TextureObject *texture_object(GLuint id) {
  auto it=state.texture_objects.find(id);return it==state.texture_objects.end()?nullptr:&it->second;
}
// Leaving a drawn framebuffer: its textures take its contents when sampled.
void framebuffer_finished(Framebuffer &f) {
  if(!f.drawn||!f.drawable)return;
  f.drawn=false;
  for(auto id:{f.color,f.depth})if(id)state.stale_textures[id]=f.drawable;
}
void release_framebuffer_drawable(Framebuffer &f) {
  if(!f.drawable)return;
  for(auto it=state.stale_textures.begin();it!=state.stale_textures.end();)
    it=it->second==f.drawable?state.stale_textures.erase(it):std::next(it);
  if(state.drawable==f.drawable)state.drawable=state.glx_drawable;
  retire_drawable(f.drawable);f.drawable=0;f.size={};
}
// The drawable follows the attachments' size: the colour texture's, else the
// depth texture's or renderbuffer's.
void sync_framebuffer(Framebuffer &f) {
  std::array<int,2> size{};
  for(auto id:{f.color,f.depth})if(!size[0]&&id)if(auto *o=texture_object(id);o&&o->levels.count(0))
    size={o->levels.at(0).width,o->levels.at(0).height};
  if(!size[0]&&f.depth_renderbuffer)if(auto it=state.renderbuffers.find(f.depth_renderbuffer);it!=state.renderbuffers.end())size=it->second;
  const bool depthless=!f.depth&&!f.depth_renderbuffer;
  if(size!=f.size||depthless!=f.depthless)release_framebuffer_drawable(f);
  if(!f.drawable&&size[0]>0&&size[1]>0) {
    f.drawable=create_drawable(size[0],size[1],f.rect,depthless);f.size=size;f.depthless=depthless;
    std::fprintf(stderr,"[vulkan] framebuffer drawable %u: %dx%d%s, colour texture %u, depth %s %u\n",f.drawable,size[0],size[1],
      f.rect?" (rect)":"",f.color,f.depth?"texture":"renderbuffer",f.depth?f.depth:f.depth_renderbuffer);
  }
  if(&f==state.bound_framebuffer)state.drawable=f.drawable?f.drawable:state.glx_drawable;
}
void mark_drawn() {if(state.bound_framebuffer)state.bound_framebuffer->drawn=true;}
// A draw sampling texture id: bring in its framebuffer's contents first.
void resolve_texture(GLuint id) {
  if(!id||state.stale_textures.empty())return;
  if(auto *f=state.bound_framebuffer;f&&f->drawn&&(f->color==id||f->depth==id))framebuffer_finished(*f);
  const auto found=state.stale_textures.find(id);
  if(found==state.stale_textures.end())return;
  const unsigned drawable=found->second;state.stale_textures.erase(found);
  const auto source_it=state.drawables.find(drawable);auto *o=texture_object(id);
  if(source_it==state.drawables.end()||!o||!o->levels.count(0))return;
  Target *source=source_it->second.get();
  const bool depth=o->levels.at(0).format==GL_DEPTH_COMPONENT||o->levels.at(0).format==GL_DEPTH_STENCIL;
  const VkRect2D all{{0,0},{source->info.width,source->info.height}};
  // The drawing target cannot also be sampled: a draw into its own source copies.
  Target *drawing=state.drawable?state.drawables.at(state.drawable).get():state.screen;
  const bool in_place=!depth&&source->info.final_layout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL&&source!=drawing;
  size_t upto=0;for(size_t i=0;i<state.packets.size();++i)if(state.packets[i].target==source)upto=i+1;
  // Sampled in place, the source's queued passes are recorded ahead of its
  // consumer's anyway: nothing is flushed, so a frame is recorded whole and a
  // pass can know it is the frame's last on its target. A copy, or a pending
  // clear to apply, records what was queued up to the source's last packet;
  // packets after it (the consumer's) stay queued, so the copy does not split
  // their pass.
  // Only when its last queued packet draws: a trailing clear-only run is
  // deferred at the flush, so nothing would render it before it is sampled.
  const bool defer=in_place&&!state.pending_clears.count(source)&&(upto?state.packets[upto-1].count!=0:state.initialized.count(source)!=0);
  if(!defer) {
    state.frame();state.flush(false,upto);
    state.pass_stamp("resolve texture "+std::to_string(id)+" from drawable "+std::to_string(drawable)+", "+std::to_string(upto)+" packets recorded");
    if(!state.initialized.count(source))return;
  }
  if(in_place) {
    if(!defer){state.materialize(source,&all);state.end_pass();}
    else if(state.active_target==source)state.end_pass();
    auto &view=state.target_views[source];
    if(!view)view=std::make_shared<Texture>(*state.device,TextureInfo{source->info.width,source->info.height,1,source->info.layers,source->info.color_format},source->color);
    const bool first=o->alias_target!=source;
    o->layers=source->info.layers;o->gpu_target=true;o->picture_view=true;
    o->render_width=source->info.width;o->render_height=source->info.height;
    if(first){changed(*o);o->alias=view;o->alias_target=source;}
    state.pass_stamp("framebuffer texture "+std::to_string(source->info.width)+"x"+std::to_string(source->info.height)+" sampled in place");
    return;
  }
  o->layers=source->info.layers;o->gpu_target=true;o->picture_view=true;
  o->render_width=source->info.width;o->render_height=source->info.height;
  changed(*o);auto gpu=native_image(*o,0,true);
  state.materialize(source,&all);
  if(gpu->info.format==Target::depth_format)state.depth_read(source);
  gpu->copy_from(state.cmd,*source,0,0,0,0,0,source->info.width,source->info.height,0,0,o->layers);
  state.pass_stamp("framebuffer copy "+std::to_string(source->info.width)+"x"+std::to_string(source->info.height)+
                   (o->levels.at(0).format==GL_DEPTH_COMPONENT||o->levels.at(0).format==GL_DEPTH_STENCIL?" depth":" colour"));
}
void check_framebuffer_target(GLenum target) {
  if(target!=GL_FRAMEBUFFER&&target!=GL_DRAW_FRAMEBUFFER&&target!=GL_READ_FRAMEBUFFER)gap("unsupported guest framebuffer target");
}
Framebuffer &attachment_framebuffer(GLenum target) {
  check_framebuffer_target(target);
  if(!state.bound_framebuffer)gap("framebuffer attachment on the default framebuffer");
  return *state.bound_framebuffer;
}
}
extern "C" void lvk_glGenFramebuffers(GLsizei n, GLuint *ids) {
  for(GLsizei i=0;ids&&i<n;++i){ids[i]=object();state.framebuffers[ids[i]];}
}
extern "C" GLboolean lvk_glIsFramebuffer(GLuint id) {return state.framebuffers.count(id)?GL_TRUE:GL_FALSE;}
extern "C" void lvk_glBindFramebuffer(GLenum target, GLuint id) {
  check_framebuffer_target(target);
  if(target==GL_READ_FRAMEBUFFER)return; // reads follow the draw framebuffer here
  if(id==state.framebuffer)return;
  if(state.bound_framebuffer)framebuffer_finished(*state.bound_framebuffer);
  state.framebuffer=id;state.bound_framebuffer=id?&state.framebuffers[id]:nullptr;
  if(state.bound_framebuffer)sync_framebuffer(*state.bound_framebuffer);
  else state.drawable=state.glx_drawable;
}
extern "C" void lvk_glDeleteFramebuffers(GLsizei n, const GLuint *ids) {
  for(GLsizei i=0;ids&&i<n;++i) {
    auto it=state.framebuffers.find(ids[i]);if(!ids[i]||it==state.framebuffers.end())continue;
    if(state.framebuffer==ids[i])lvk_glBindFramebuffer(GL_FRAMEBUFFER,0);
    release_framebuffer_drawable(it->second);state.framebuffers.erase(it);
  }
}
extern "C" void lvk_glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {
  auto &f=attachment_framebuffer(target);
  if(texture&&level)gap("framebuffer attachment of a texture mip level");
  if(textarget!=GL_TEXTURE_2D&&textarget!=GL_TEXTURE_RECTANGLE_ARB)gap("unsupported framebuffer texture target");
  framebuffer_finished(f);
  if(attachment==GL_COLOR_ATTACHMENT0) {f.color=texture;f.rect=textarget==GL_TEXTURE_RECTANGLE_ARB;}
  else if(attachment==GL_DEPTH_ATTACHMENT||attachment==GL_DEPTH_STENCIL_ATTACHMENT)f.depth=texture;
  else if(attachment!=GL_STENCIL_ATTACHMENT)gap("unsupported framebuffer attachment");
  if(texture)state.render_textures.insert(texture);
  sync_framebuffer(f);
}
extern "C" void lvk_glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum, GLuint renderbuffer) {
  auto &f=attachment_framebuffer(target);
  if(attachment==GL_DEPTH_ATTACHMENT||attachment==GL_DEPTH_STENCIL_ATTACHMENT||attachment==GL_STENCIL_ATTACHMENT)f.depth_renderbuffer=renderbuffer;
  else gap("colour renderbuffer attachments are not implemented");
  sync_framebuffer(f);
}
extern "C" GLenum lvk_glCheckFramebufferStatus(GLenum target) {
  check_framebuffer_target(target);
  if(!state.bound_framebuffer)return GL_FRAMEBUFFER_COMPLETE;
  sync_framebuffer(*state.bound_framebuffer);
  return state.bound_framebuffer->drawable?GL_FRAMEBUFFER_COMPLETE:GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;
}
extern "C" void lvk_glGenRenderbuffers(GLsizei n, GLuint *ids) {
  for(GLsizei i=0;ids&&i<n;++i){ids[i]=object();state.renderbuffers[ids[i]]={};}
}
extern "C" GLboolean lvk_glIsRenderbuffer(GLuint id) {return state.renderbuffers.count(id)?GL_TRUE:GL_FALSE;}
extern "C" void lvk_glBindRenderbuffer(GLenum, GLuint id) {state.renderbuffer=id;}
extern "C" void lvk_glRenderbufferStorage(GLenum, GLenum, GLsizei w, GLsizei h) {
  if(!state.renderbuffer)gap("renderbuffer storage without a bound renderbuffer");
  state.renderbuffers[state.renderbuffer]={w,h};
}
extern "C" void lvk_glDeleteRenderbuffers(GLsizei n, const GLuint *ids) {
  for(GLsizei i=0;ids&&i<n;++i){state.renderbuffers.erase(ids[i]);if(state.renderbuffer==ids[i])state.renderbuffer=0;}
}
extern "C" void lvk_glDrawBuffer(GLenum) {}
extern "C" void lvk_glReadBuffer(GLenum) {}
// Menus: the game's state functions (game_states.h), as in the GL host. The
// dispatcher reports guest calls inside [stereo_state_lo, stereo_state_hi]
// (globals of the generated stereo bridge); a menu state having run this
// frame or the last makes the frame's 3D screen content (facts.menu).
extern "C" uint32_t stereo_state_lo, stereo_state_hi;
extern "C" uint32_t guest_symbol(const char *name);
namespace {
struct {std::vector<uint32_t> va;size_t menus=0;unsigned frame=~0u;} g_game_states;
void watch_game_states() {
  auto &g=g_game_states;if(!g.va.empty())return;
  for(int pass=0;pass<2;++pass) {
    const auto *names=pass?lg_play_states:lg_menu_states;
    const size_t n=pass?sizeof lg_play_states/sizeof *lg_play_states:sizeof lg_menu_states/sizeof *lg_menu_states;
    for(size_t i=0;i<n;++i) {
      const std::string name="_Z"+std::to_string(std::strlen(names[i])+4)+"Game"+names[i]+"P8GAMEWORK";
      if(const uint32_t va=guest_symbol(name.c_str())) {
        g.va.push_back(va);stereo_state_lo=std::min(stereo_state_lo,va);stereo_state_hi=std::max(stereo_state_hi,va);
      }
    }
    if(!pass)g.menus=g.va.size();
  }
  std::fprintf(stderr,"[vulkan] %zu menu states of %zu found\n",g.menus,g.va.size());
}
bool menu_state() {return g_game_states.frame!=~0u&&g_frame-g_game_states.frame<=1;}
}
extern "C" void stereo_guest_state(uint32_t va) {
  auto &g=g_game_states;
  for(size_t i=0;i<g.va.size();++i)if(g.va[i]==va){g.frame=i<g.menus?g_frame:~0u;return;}
}
// arbprog.c's rectangle lookups: render targets drawn larger are scaled to the
// eyes' resolution (what a rectangle target keeps, lg_stereo_target_scale).
// Vertex programs that do their own transform multiply by u_reproj (GL's
// per-eye reprojection). Here the shader wrapper's eye transform does that.
extern "C" void stereo_reproject(float out[16]) {for(int i=0;i<16;++i)out[i]=i%5==0?1.f:0.f;}
extern "C" int stereo_is_render_target(unsigned texture) {return state.stereo&&state.render_textures.count(texture);}
extern "C" void stereo_rect_scale(float *x, float *y) {
  *x=*y=1;
  if(!state.stereo)return;
  int width=state.game_width,height=state.game_height;
#ifdef LINDBERGH_VULKAN_XR
  width=int(state.xr->width());height=int(state.xr->height());
#elif defined(LINDBERGH_VULKAN_OFFSCREEN)
  width=int(state.offscreen->info.width);height=int(state.offscreen->info.height);
#endif
  const auto *profile=game_profile();
  const LgStereoTargetSize sizes={state.game_width,state.game_height,profile->vr.world_w,profile->vr.world_h,width,height};
  lg_stereo_target_scale(&sizes,state.game_width,state.game_height,1,x,y);
}

extern "C" GLenum lvk_glGetError(void) { return state.matrices.error(); }
extern "C" const GLubyte *lvk_glGetString(GLenum what) {
  const char *value = "";
  switch (what) {
  case GL_VENDOR:
    value = "Lindbergh direct Vulkan";
    break;
  case GL_RENDERER:
    value = "Vulkan guest HLE";
    break;
  case GL_VERSION:
    value = "2.1 guest ABI (Vulkan backend in development)";
    break;
  case GL_SHADING_LANGUAGE_VERSION:
    value = "1.20 offline SPIR-V";
    break;
  case GL_EXTENSIONS:
    value =
        "GL_ARB_shader_objects GL_ARB_vertex_shader GL_ARB_fragment_shader "
        "GL_ARB_multitexture GL_ARB_vertex_buffer_object "
        "GL_ARB_texture_compression GL_EXT_texture_compression_s3tc "
        "GL_ARB_depth_texture GL_ARB_shadow GL_ARB_texture_non_power_of_two";
    break;
  default:
    gap("unsupported guest glGetString query");
  }
  return reinterpret_cast<const GLubyte *>(value);
}
extern "C" void lvk_glGetIntegerv(GLenum what, GLint *out) {
  if (!out)
    gap("null guest integer query");
  if (what == GL_VIEWPORT || what == GL_SCISSOR_BOX) {
    auto &v = what == GL_VIEWPORT ? state.viewport : state.scissor;
    std::copy(v.begin(), v.end(), out);
    return;
  }
  if (state.integers.count(what)) {
    *out = state.integers.at(what);
    return;
  }
  switch (what) {
  case GL_MATRIX_MODE:
    *out = state.matrix_mode;
    break;
  case GL_ACTIVE_TEXTURE:
    *out = state.active_texture;
    break;
  case GL_CLIENT_ACTIVE_TEXTURE:
    *out = state.client_texture;
    break;
  case GL_ARRAY_BUFFER_BINDING:
    *out = state.array_buffer;
    break;
  case GL_ELEMENT_ARRAY_BUFFER_BINDING:
    *out = state.element_buffer;
    break;
  case GL_TEXTURE_BINDING_2D:
    *out = state.textures[state.active_texture - GL_TEXTURE0];
    break;
  case GL_CURRENT_PROGRAM:
    *out = state.program;
    break;
  case GL_MAX_TEXTURE_SIZE:
    *out = state.device->capabilities.properties.limits.maxImageDimension2D;
    break;
  case GL_MAX_TEXTURE_UNITS:
  case GL_MAX_TEXTURE_IMAGE_UNITS:
  case GL_MAX_TEXTURE_COORDS:
    *out = 8;
    break;
  case GL_MAX_VERTEX_ATTRIBS:
    *out = 16;
    break;
  case GL_MAX_VERTEX_UNIFORM_COMPONENTS:
  case GL_MAX_FRAGMENT_UNIFORM_COMPONENTS:
    *out = 4096;
    break;
  case GL_MAX_LIGHTS:
    *out = 8;
    break;
  case GL_MAX_MODELVIEW_STACK_DEPTH:
  case GL_MAX_PROJECTION_STACK_DEPTH:
  case GL_MAX_TEXTURE_STACK_DEPTH:
    *out = 32;
    break;
  case GL_RED_BITS:
  case GL_GREEN_BITS:
  case GL_BLUE_BITS:
  case GL_ALPHA_BITS:
    *out = 8;
    break;
  case GL_DEPTH_BITS:
    *out = 32;
    break;
  case GL_STENCIL_BITS:
    *out = 0;
    break;
  case GL_UNPACK_ALIGNMENT:
  case GL_PACK_ALIGNMENT:
    *out = 4;
    break;
  case GL_UNPACK_ROW_LENGTH:
  case GL_UNPACK_SKIP_PIXELS:
  case GL_UNPACK_SKIP_ROWS:
    *out = 0;
    break;
  case GL_FRAMEBUFFER_BINDING:
    *out = GLint(state.framebuffer);
    break;
  case GL_RENDERBUFFER_BINDING:
    *out = GLint(state.renderbuffer);
    break;
  case GL_DEPTH_WRITEMASK:
    *out = 1;
    break;
  case GL_BLEND_SRC:
    *out = GL_ONE;
    break;
  case GL_BLEND_DST:
    *out = GL_ZERO;
    break;
  case GL_DEPTH_FUNC:
    *out = GL_LEQUAL;
    break;
  case GL_SHADE_MODEL:
    *out = GL_SMOOTH;
    break;
  // GL's initial values for state the game has not set yet (once set, the
  // value stored in state.integers above answers instead).
  case GL_ALPHA_TEST_FUNC: case GL_STENCIL_FUNC: *out = GL_ALWAYS; break;
  case GL_CULL_FACE_MODE: *out = GL_BACK; break;
  case GL_LIGHT_MODEL_COLOR_CONTROL: *out = GL_SINGLE_COLOR; break;
  case GL_FRONT_FACE: *out = GL_CCW; break;
  case GL_STENCIL_FAIL: case GL_STENCIL_PASS_DEPTH_FAIL: case GL_STENCIL_PASS_DEPTH_PASS: *out = GL_KEEP; break;
  case GL_STENCIL_REF: *out = 0; break;
  case GL_STENCIL_VALUE_MASK: case GL_STENCIL_WRITEMASK: *out = 0xff; break;
  case GL_BLEND_EQUATION: *out = GL_FUNC_ADD; break;
  case GL_LOGIC_OP_MODE: *out = GL_COPY; break;
  case GL_FOG_MODE: *out = GL_EXP; break;
  case GL_TEXTURE_BINDING_CUBE_MAP: *out = GLint(state.cube_textures[state.active_texture - GL_TEXTURE0]); break;
  case GL_TEXTURE_BINDING_RECTANGLE_ARB: *out = GLint(state.rect_textures[state.active_texture - GL_TEXTURE0]); break;
  default: {
    // More of GL's initial state (values once set are stored and answered
    // above), and state the game set through a float call.
    static const std::pair<GLenum,GLint> initial[]={
      {GL_LIGHT_MODEL_LOCAL_VIEWER,0},{GL_LIGHT_MODEL_TWO_SIDE,0},{GL_COLOR_MATERIAL_FACE,GL_FRONT_AND_BACK},
      {GL_COLOR_MATERIAL_PARAMETER,GL_AMBIENT_AND_DIFFUSE},{GL_PERSPECTIVE_CORRECTION_HINT,GL_DONT_CARE},
      {GL_POINT_SMOOTH_HINT,GL_DONT_CARE},{GL_LINE_SMOOTH_HINT,GL_DONT_CARE},{GL_POLYGON_SMOOTH_HINT,GL_DONT_CARE},
      {GL_FOG_HINT,GL_DONT_CARE},{GL_GENERATE_MIPMAP_HINT,GL_DONT_CARE},{GL_BLEND_SRC_ALPHA,GL_ONE},{GL_BLEND_DST_ALPHA,GL_ZERO},
      {GL_BLEND_EQUATION_ALPHA,GL_FUNC_ADD},{GL_DEPTH_WRITEMASK,1},{GL_MAX_LIGHTS,8},{GL_MAX_CLIP_PLANES,6},
      {GL_MAX_MODELVIEW_STACK_DEPTH,32},{GL_MAX_PROJECTION_STACK_DEPTH,32},{GL_MAX_TEXTURE_STACK_DEPTH,32},
      {GL_UNPACK_ROW_LENGTH,0},{GL_UNPACK_SKIP_ROWS,0},{GL_UNPACK_SKIP_PIXELS,0},{GL_PACK_ROW_LENGTH,0},
      {GL_PACK_SKIP_ROWS,0},{GL_PACK_SKIP_PIXELS,0},{GL_SUBPIXEL_BITS,4},{GL_AUX_BUFFERS,0},{GL_DOUBLEBUFFER,1},
      {GL_RENDER_MODE,GL_RENDER},{GL_LIST_BASE,0},{GL_FOG_COORD_SRC,GL_FRAGMENT_DEPTH},{GL_TEXTURE_BINDING_3D,0},
      {GL_DRAW_BUFFER,GL_BACK},{GL_READ_BUFFER,GL_BACK},{GL_SAMPLE_BUFFERS,0},
      {GL_SAMPLES,0},{GL_MAX_PROGRAM_MATRICES_ARB,8},{GL_MAX_3D_TEXTURE_SIZE,2048},{GL_MAX_CUBE_MAP_TEXTURE_SIZE,4096},
      {GL_MAX_ELEMENTS_VERTICES,65536},{GL_MAX_ELEMENTS_INDICES,65536},{GL_ACCUM_RED_BITS,0},{GL_ACCUM_GREEN_BITS,0},
      {GL_ACCUM_BLUE_BITS,0},{GL_ACCUM_ALPHA_BITS,0},{GL_INDEX_BITS,0},{GL_STEREO,0}};
    for(const auto &[name,v]:initial)if(name==what){*out=v;return;}
    if(state.floats.count(what)){*out=GLint(std::lround(state.floats.at(what)[0]));return;}
    std::fprintf(stderr, "[vulkan] glGetIntegerv unsupported enum %#x\n", what);
    gap("guest query requires implementation");
  }
  }
}
extern "C" void lvk_glGetBooleanv(GLenum what, GLboolean *out) {
  if (what == GL_COLOR_WRITEMASK) {
    auto it = state.floats.find(what);
    for (unsigned i = 0; i < 4; ++i)
      out[i] = it == state.floats.end() ? GL_TRUE : it->second[i] != 0;
    return;
  }
  GLint values[4]{};
  lvk_glGetIntegerv(what, values);
  const auto count = what == GL_VIEWPORT || what == GL_SCISSOR_BOX ? 4 : 1;
  for (int i = 0; i < count; ++i)
    out[i] = values[i] != 0;
}
extern "C" void lvk_glGetFloatv(GLenum what, GLfloat *out) {
  if (what == GL_MODELVIEW_MATRIX || what == GL_PROJECTION_MATRIX ||
      what == GL_TEXTURE_MATRIX) {
    const auto mode = state.matrix_mode;
    state.matrices.matrix_mode(what == GL_MODELVIEW_MATRIX    ? GL_MODELVIEW
                               : what == GL_PROJECTION_MATRIX ? GL_PROJECTION
                                                              : GL_TEXTURE);
    std::copy(state.matrices.current_matrix().begin(),
              state.matrices.current_matrix().end(), out);
    state.matrices.matrix_mode(mode);
    return;
  }
  if (state.floats.count(what)) {
    auto &v = state.floats.at(what);
    const auto n = (what == GL_FOG_COLOR || what == GL_COLOR_CLEAR_VALUE ||
                    what == GL_CURRENT_COLOR || what == GL_COLOR_WRITEMASK ||
                    what == GL_LIGHT_MODEL_AMBIENT || what == GL_BLEND_COLOR)
                       ? 4
                       : what == GL_DEPTH_RANGE ? 2 : 1;
    std::copy(v.begin(), v.begin() + n, out);
    return;
  }
  { // GL's initial values for state the game has not set yet
    static const std::pair<GLenum,std::array<float,4>> initial[]={
      {GL_ALPHA_TEST_REF,{0}},{GL_DEPTH_CLEAR_VALUE,{1}},{GL_FOG_DENSITY,{1}},{GL_FOG_START,{0}},{GL_FOG_END,{1}},
      {GL_LINE_WIDTH,{1}},{GL_POINT_SIZE,{1}},{GL_POLYGON_OFFSET_FACTOR,{0}},{GL_POLYGON_OFFSET_UNITS,{0}},
      {GL_CURRENT_COLOR,{1,1,1,1}},{GL_COLOR_CLEAR_VALUE,{0,0,0,0}},{GL_FOG_COLOR,{0,0,0,0}},{GL_DEPTH_RANGE,{0,1,0,0}},
      {GL_LIGHT_MODEL_AMBIENT,{.2f,.2f,.2f,1}},{GL_BLEND_COLOR,{0,0,0,0}},{GL_FOG_INDEX,{0}},{GL_POINT_SIZE_MIN,{0}},
      {GL_POINT_SIZE_MAX,{1}},{GL_POINT_FADE_THRESHOLD_SIZE,{1}},{GL_CURRENT_RASTER_POSITION,{0,0,0,1}}};
    for(const auto &[name,v]:initial)if(name==what) {
      const unsigned n=(what==GL_CURRENT_COLOR||what==GL_COLOR_CLEAR_VALUE||what==GL_FOG_COLOR||what==GL_LIGHT_MODEL_AMBIENT||
                        what==GL_BLEND_COLOR||what==GL_CURRENT_RASTER_POSITION)?4:what==GL_DEPTH_RANGE?2:1;
      std::copy(v.begin(),v.begin()+n,out);return;
    }
  }
  if (state.integers.count(what)) { *out = float(state.integers.at(what)); return; }
  if (what == GL_CURRENT_MATRIX_ARB) { // the current stack, e.g. GL_MATRIXi_ARB
    std::copy(state.matrices.current_matrix().begin(),state.matrices.current_matrix().end(),out);
    return;
  }
  std::fprintf(stderr, "[vulkan] glGetFloatv unsupported enum %#x\n", what);
  gap("unsupported guest float query");
}
extern "C" void lvk_glMatrixMode(GLenum mode) {
  state.matrices.matrix_mode(mode);
  state.matrix_mode = mode;
}
extern "C" void lvk_glLoadIdentity(void) { ++state.legacy_version; state.matrices.load_identity(); }
extern "C" void lvk_glLoadMatrixf(const GLfloat *m) { ++state.legacy_version;
  state.matrices.load_matrix(m);
}
extern "C" void lvk_glMultMatrixf(const GLfloat *m) { ++state.legacy_version;
  state.matrices.mult_matrix(m);
}
extern "C" void lvk_glPushMatrix(void) { ++state.legacy_version; state.matrices.push_matrix(); }
extern "C" void lvk_glPopMatrix(void) { ++state.legacy_version; state.matrices.pop_matrix(); }
extern "C" void lvk_glTranslatef(GLfloat x, GLfloat y, GLfloat z) { ++state.legacy_version;
  state.matrices.translate(x, y, z);
}
extern "C" void lvk_glScalef(GLfloat x, GLfloat y, GLfloat z) { ++state.legacy_version;
  state.matrices.scale(x, y, z);
}
extern "C" void lvk_glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t,
                            GLdouble n, GLdouble f) { ++state.legacy_version;
  state.matrices.ortho(l, r, b, t, n, f);
}
extern "C" void lvk_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
  state.viewport = {x, y, w, h};
}
extern "C" void lvk_glScissor(GLint x, GLint y, GLsizei w, GLsizei h) {
  state.scissor = {x, y, w, h};
}
extern "C" void lvk_glClearColor(GLclampf r, GLclampf g, GLclampf b,
                                 GLclampf a) {
  state.clear = {r, g, b, a};
}
extern "C" void lvk_glClearDepth(GLclampd d) {
  // GLclampd: GL clamps the clear depth to [0,1] (HOTD4 passes 10000/32768).
  state.floats[GL_DEPTH_CLEAR_VALUE] = {float(std::clamp(d,0.,1.)), 0, 0, 0};
}
extern "C" void lvk_glClearStencil(GLint s) {
  state.integers[GL_STENCIL_CLEAR_VALUE] = s;
}
extern "C" void lvk_glClear(GLbitfield mask) { queue_clear(mask); }
extern "C" void lvk_glEnable(GLenum cap) {
  state.enabled[cap] = true;
  if(cap==GL_TEXTURE_2D)state.texture_enabled[state.active_texture-GL_TEXTURE0]=true;
  if(cap==GL_TEXTURE_RECTANGLE_ARB)state.rect_enabled[state.active_texture-GL_TEXTURE0]=true;
  if (cap == GL_DEPTH_TEST)
    state.raster.depth_test = true;
  if (cap == GL_BLEND)
    state.raster.blend = true;
}
extern "C" void lvk_glDisable(GLenum cap) {
  state.enabled[cap] = false;
  if(cap==GL_TEXTURE_2D)state.texture_enabled[state.active_texture-GL_TEXTURE0]=false;
  if(cap==GL_TEXTURE_RECTANGLE_ARB)state.rect_enabled[state.active_texture-GL_TEXTURE0]=false;
  if (cap == GL_DEPTH_TEST)
    state.raster.depth_test = false;
  if (cap == GL_BLEND)
    state.raster.blend = false;
}
extern "C" GLboolean lvk_glIsEnabled(GLenum cap) { return state.enabled[cap]; }
extern "C" void lvk_glDepthMask(GLboolean on) {
  state.raster.depth_write = on;
  state.integers[GL_DEPTH_WRITEMASK] = on;
}
extern "C" void lvk_glDepthFunc(GLenum function) {
  if (function < GL_NEVER || function > GL_ALWAYS)
    gap("invalid guest depth function");
  state.raster.depth_compare = static_cast<VkCompareOp>(function - GL_NEVER);
  state.integers[GL_DEPTH_FUNC] = function;
}
extern "C" void lvk_glPixelStorei(GLenum p, GLint v) { state.integers[p] = v; }
extern "C" void lvk_glActiveTexture(GLenum unit) { ++state.legacy_version;
  if (unit < GL_TEXTURE0 || unit >= GL_TEXTURE0 + 8)
    gap("guest texture unit out of range");
  state.active_texture = unit;
  state.matrices.active_texture(unit - GL_TEXTURE0);
}
extern "C" void lvk_glClientActiveTexture(GLenum unit) {
  if (unit < GL_TEXTURE0 || unit >= GL_TEXTURE0 + 8)
    gap("guest client texture unit out of range");
  state.client_texture = unit;
}
extern "C" void lvk_glGenBuffers(GLsizei n, GLuint *out) {
  for (GLsizei i = 0; i < n; ++i) {
    out[i] = object();
    state.buffers[out[i]] = {};
  }
}
extern "C" void lvk_glBindBuffer(GLenum target, GLuint name) {
  state.binding(target) = name;
  if (target == GL_ARRAY_BUFFER) {
    state.arrays.bind_array_buffer(name);
    state.legacy_bounds_arrays.bind_array_buffer(name);
  }
}
extern "C" void lvk_glBufferData(GLenum target, GLsizeiptr n,
                                 const GLvoid *data, GLenum usage) {
  if (n < 0)
    gap("negative guest buffer size");
  auto &b = state.buffers[state.binding(target)];
  ++b.revision; b.dynamic=b.dynamic || bool(b.stable.buffer);
  b.bytes.resize(size_t(n));
  if (data && n)
    std::memcpy(b.bytes.data(), data, size_t(n));
  b.usage = usage;
  // Match static GL buffer upload timing: populate immutable EBOs while the
  // guest loads them, rather than allocating/copying them at the first combat draw.
  if(state.ready&&target==GL_ELEMENT_ARRAY_BUFFER&&state.binding(target)&&data&&n&&
     usage==GL_STATIC_DRAW&&!b.dynamic&&index_reuse_enabled())native_buffer(state.binding(target),true);
}
extern "C" void lvk_glBufferSubData(GLenum target, GLintptr offset,
                                    GLsizeiptr n, const GLvoid *data) {
  auto &b = state.buffers.at(state.binding(target));
  check_range(b.bytes.size(), offset, n);
  ++b.revision; b.dynamic=b.dynamic || bool(b.stable.buffer);
  if (n)
    std::memcpy(b.bytes.data() + offset, data, size_t(n));
}
extern "C" void lvk_glGetBufferSubData(GLenum target, GLintptr offset,
                                       GLsizeiptr n, GLvoid *out) {
  auto &b = state.buffers.at(state.binding(target));
  check_range(b.bytes.size(), offset, n);
  if (n)
    std::memcpy(out, b.bytes.data() + offset, size_t(n));
}
extern "C" void lvk_glGetBufferParameteriv(GLenum target, GLenum p,
                                           GLint *out) {
  const auto &b = state.buffers.at(state.binding(target));
  if (p == GL_BUFFER_SIZE)
    *out = GLint(b.bytes.size());
  else if (p == GL_BUFFER_USAGE)
    *out = b.usage;
  else
    gap("unknown guest buffer query");
}
extern "C" void lvk_glDeleteBuffers(GLsizei n, const GLuint *ids) {
  for (GLsizei i = 0; i < n; ++i)
    state.buffers.erase(ids[i]);
}
extern "C" void lvk_glGenTextures(GLsizei n, GLuint *out) {
  for (GLsizei i = 0; i < n; ++i)
    out[i] = object();
}
extern "C" void lvk_glBindTexture(GLenum target, GLuint texture) {
  auto &slots=target==GL_TEXTURE_2D?state.textures:target==GL_TEXTURE_RECTANGLE_ARB?state.rect_textures:
              target==GL_TEXTURE_CUBE_MAP?state.cube_textures:(std::fprintf(stderr,"[vulkan] texture target %#x\n",target),gap("unsupported guest texture target"),state.textures);
  slots[state.active_texture - GL_TEXTURE0] = texture;
  if (texture && target == GL_TEXTURE_RECTANGLE_ARB) state.texture_objects[texture].rect = true;
}
extern "C" GLuint lvk_glCreateShader(GLenum type) {
  auto id = object();
  state.shaders[id] = {type, {}};
  return id;
}
extern "C" GLuint lvk_glCreateProgram(void) {
  auto id = object();
  state.programs[id] = {};
  return id;
}
extern "C" GLboolean lvk_glIsProgram(GLuint id) {
  return state.programs.count(id) != 0;
}
extern "C" void lvk_glShaderSource(GLuint id, GLsizei n,
                                   const GLchar *const *strings,
                                   const GLint *lengths) {
  auto &s = state.shaders.at(id);
  s.source.clear();
  for (GLsizei i = 0; i < n; ++i)
    s.source.append(strings[i], lengths && lengths[i] >= 0
                                    ? size_t(lengths[i])
                                    : std::strlen(strings[i]));
  std::replace(s.source.begin(), s.source.end(), '\r', ' ');
  s.compiled = false;
}
extern "C" void lvk_glCompileShader(GLuint id) {
  state.shaders.at(id).compiled = true;
} // lookup is validated at guest link
extern "C" void lvk_glAttachShader(GLuint p, GLuint s) {
  auto &attached = state.programs.at(p).shaders;
  if (std::find(attached.begin(), attached.end(), s) != attached.end())
    gap("guest attached a shader twice");
  ++state.shaders.at(s).attachments;
  attached.push_back(s);
}
namespace {
void release_shader(GLuint id) {
  auto &shader = state.shaders.at(id);
  if (!--shader.attachments && shader.delete_pending) state.shaders.erase(id);
}
void release_program(GLuint id) {
  for (auto shader : state.programs.at(id).shaders) release_shader(shader);
  state.programs.erase(id);
}
}
extern "C" void lvk_glDetachShader(GLuint p, GLuint s) {
  auto &attached = state.programs.at(p).shaders;
  const auto it = std::find(attached.begin(), attached.end(), s);
  if (it == attached.end()) gap("guest detached an unattached shader");
  attached.erase(it);
  release_shader(s);
}
extern "C" void lvk_glBindAttribLocation(GLuint p, GLuint index,
                                         const GLchar *name) {
  auto &a = state.programs.at(p).attributes;
  for (auto &b : a)
    if (b.name == name) {
      b.location = index;
      return;
    }
  a.push_back({name, index});
}
namespace {
// GL runs a program without a vertex shader through fixed-function vertex
// processing. Its stand-in is this generated GLSL stage, derived only from the
// fragment source, so harvested packs and the runtime lookup agree on it.
std::string fixed_vertex_stage(const std::string &fragment) {
  auto uses=[&](const std::string &name){return fragment.find(name)!=std::string::npos;};
  std::string v="#version 120\nvoid main() {\n  gl_Position = ftransform();\n  gl_FrontColor = gl_Color;\n  gl_BackColor = gl_Color;\n";
  if(uses("gl_SecondaryColor"))v+="  gl_FrontSecondaryColor = gl_SecondaryColor;\n  gl_BackSecondaryColor = gl_SecondaryColor;\n";
  for(int i=0;i<8;++i)if(uses("gl_TexCoord["+std::to_string(i)+"]"))
    v+="  gl_TexCoord["+std::to_string(i)+"] = gl_TextureMatrix["+std::to_string(i)+"] * gl_MultiTexCoord"+std::to_string(i)+";\n";
  if(uses("gl_FogFragCoord"))v+="  gl_FogFragCoord = abs((gl_ModelViewMatrix * gl_Vertex).z);\n";
  return v+"}\n";
}
// LINDBERGH_VULKAN_SHADER_HARVEST=dir: programs missing from the pack are
// written there as a compile_shaders.py manifest (programs.jsonl), and draws
// with them are skipped, so one run collects every missing program.
const char *harvest_directory() {static const char *d=[]{const auto *v=std::getenv("LINDBERGH_VULKAN_SHADER_HARVEST");return v&&*v?v:nullptr;}();return d;}
void harvest_program(GLuint id,const Program &p,const std::string &vertex,const std::string &fragment) {
  static std::set<std::string> seen;
  std::string key=vertex+'\0'+fragment;for(const auto &a:p.attributes)key+=a.name+'='+std::to_string(a.location)+';';
  if(!seen.insert(key).second)return;
  const std::filesystem::path directory=harvest_directory();std::filesystem::create_directories(directory);
  // Named by content (FNV-1a), so runs appending to one directory agree.
  uint32_t hash=2166136261u;for(unsigned char c:key)hash=(hash^c)*16777619u;
  if(std::filesystem::exists(directory/("fs_h"+std::to_string(hash)+".glsl")))return;
  const auto n=std::to_string(hash);
  for(auto [name,text]:{std::pair{"vs_h"+n+".glsl",&vertex},std::pair{"fs_h"+n+".glsl",&fragment}}) {
    std::ofstream out(directory/name,std::ios::binary);out<<*text;
  }
  std::ofstream rows(directory/"programs.jsonl",std::ios::app);
  rows<<"{\"program\":"<<hash<<",\"vertex\":\"vs_h"<<n<<".glsl\",\"fragment\":\"fs_h"<<n<<".glsl\",\"attributes\":[";
  for(size_t i=0;i<p.attributes.size();++i)rows<<(i?",":"")<<"{\"name\":\""<<p.attributes[i].name<<"\",\"location\":"<<p.attributes[i].location<<"}";
  rows<<"]}\n";
  std::fprintf(stderr,"[vulkan-harvest] program %u recorded as %s/vs_h%s.glsl + fs_h%s.glsl\n",id,directory.c_str(),n.c_str(),n.c_str());
}
}
extern "C" void lvk_glLinkProgram(GLuint id) {
  if (!state.pack) {
    const auto path = std::getenv("LINDBERGH_VULKAN_SHADERS");
    if (!path)
      gap("set LINDBERGH_VULKAN_SHADERS to the offline .lvsp archive");
    state.pack = std::make_unique<ShaderPack>(path);
  }
  auto &p = state.programs.at(id);
  p.record = nullptr; p.skipped = false;
  p.native.reset(); // submitted frames retain their previous native program
  SourceDigest vs{}, fs{};
  std::string vertex_source, fragment_source;
  for (const auto shader : p.shaders) {
    const auto &s = state.shaders.at(shader);
    if (s.type == GL_VERTEX_SHADER)
      vertex_source = s.source;
    if (s.type == GL_FRAGMENT_SHADER)
      fragment_source = s.source;
  }
  if (vertex_source.empty() && !fragment_source.empty())
    vertex_source = fixed_vertex_stage(fragment_source);
  p.fixed_fragment = fragment_source.empty() && !vertex_source.empty();
  if (p.fixed_fragment) fragment_source = lvk_fixed_fragment_source;
  if (!vertex_source.empty()) vs = shader_source_digest(vertex_source);
  if (!fragment_source.empty()) fs = shader_source_digest(fragment_source);
  // Unbound active inputs may use the archive's deterministic assignment;
  // explicit guest bindings must match. GetAttribLocation returns that slot.
  for (const auto &record : state.pack->programs()) {
    if (record.vertex_source != vs || record.fragment_source != fs)
      continue;
    auto bindings = p.attributes;
    bool ok = true;
    for (const auto &a : record.attributes) {
      const auto b =
          std::find_if(bindings.begin(), bindings.end(),
                       [&](const auto &v) { return v.name == a.name; });
      if (b == bindings.end())
        bindings.push_back(a);
      else if (b->location != a.location)
        ok = false;
    }
    if (ok) {
      p.record = state.pack->find(vs, fs, bindings);
      break;
    }
  }
  if (p.record) {
    static unsigned linked = 0;
    if (!linked++)
      std::fprintf(stderr, "[vulkan] first guest program selected from offline "
                           "SPIR-V archive\n");
    p.uniforms.clear();
    p.locations.clear();
    p.location_names.clear();
    p.sampler_units.clear(); // successful GL relink resets all guest uniforms
    for (const auto &layout : p.record->uniforms)
      p.uniforms.push_back(std::make_unique<UniformData>(layout));
    if (p.fixed_fragment)
      for (const auto &sampler : p.record->samplers)
        if (sampler.name.rfind("fixed_texture", 0) == 0)
          p.sampler_units[int(sampler.binding)] = GLint(std::strtoul(sampler.name.c_str() + 13, nullptr, 10));
  }
  if (!p.record) {
    if (const auto directory = std::getenv("LINDBERGH_VULKAN_SHADER_MISS")) {
      std::filesystem::create_directories(directory);
      for (const auto shader : p.shaders) {
        const auto &s = state.shaders.at(shader);
        const auto path =
            std::filesystem::path(directory) /
            (s.type == GL_VERTEX_SHADER ? "guest.vert" : "guest.frag");
        std::ofstream out(path, std::ios::binary);
        out.write(s.source.data(), s.source.size());
      }
    }
    if (harvest_directory()) {
      harvest_program(id, p, vertex_source, fragment_source);
      return; // its draws are skipped
    }
    // A player's game keeps running without this program's draws; diagnostics
    // can insist on a complete pack with LINDBERGH_VULKAN_STRICT_SHADERS=1.
    static const bool strict=[]{const auto *v=std::getenv("LINDBERGH_VULKAN_STRICT_SHADERS");return v&&*v=='1';}();
    if (!strict) {
      std::fprintf(stderr, "[vulkan] MISSING SHADER: guest program %u is not in the offline pack; its draws are skipped "
                           "(harvest it with LINDBERGH_VULKAN_SHADER_HARVEST)\n", id);
      p.skipped = true;
      return;
    }
    std::fprintf(stderr,
                 "[vulkan] missing offline shader pair for guest program %u\n",
                 id);
    gap("guest shader identity/binding variant not present in archive");
  }
}
extern "C" void lvk_glUseProgram(GLuint id) {
  if (id && !state.programs.at(id).record && !harvest_directory() && !state.programs.at(id).skipped)
    gap("guest selected an unlinked Vulkan program");
  const auto previous = state.program;
  state.program = id;
  if (previous && previous != id && state.programs.at(previous).delete_pending)
    release_program(previous);
}
extern "C" void lvk_glGetShaderiv(GLuint id, GLenum p, GLint *out) {
  const auto &s = state.shaders.at(id);
  if (p == GL_SHADER_TYPE)
    *out = s.type;
  else if (p == GL_DELETE_STATUS)
    *out = s.delete_pending;
  else if (p == GL_COMPILE_STATUS)
    *out = s.compiled;
  else if (p == GL_SHADER_SOURCE_LENGTH)
    *out = GLint(s.source.size() + 1);
  else if (p == GL_INFO_LOG_LENGTH)
    *out = 1;
  else
    gap("unknown guest shader query");
}
extern "C" void lvk_glGetProgramiv(GLuint id, GLenum p, GLint *out) {
  const auto &v = state.programs.at(id);
  if (p == GL_LINK_STATUS || p == GL_VALIDATE_STATUS)
    *out = v.record != nullptr || v.skipped || harvest_directory() != nullptr;
  else if (p == GL_INFO_LOG_LENGTH)
    *out = 1;
  else if (p == GL_DELETE_STATUS)
    *out = v.delete_pending;
  else if (p == GL_ATTACHED_SHADERS)
    *out = GLint(v.shaders.size());
  // Offline uniform blocks keep every array at its declared length; nothing
  // is enumerable, so arbprog.c's array_len() keeps the length it declared.
  else if (p == GL_ACTIVE_UNIFORMS || p == GL_ACTIVE_UNIFORM_MAX_LENGTH ||
           p == GL_ACTIVE_ATTRIBUTES || p == GL_ACTIVE_ATTRIBUTE_MAX_LENGTH)
    *out = 0;
  else
    {std::fprintf(stderr,"[vulkan] program query %#x\n",p);gap("unknown guest program query");}
}
extern "C" void lvk_glGetShaderInfoLog(GLuint, GLsizei n, GLsizei *length,
                                       GLchar *out) {
  if (length)
    *length = 0;
  if (n > 0)
    *out = 0;
}
extern "C" void lvk_glGetProgramInfoLog(GLuint, GLsizei n, GLsizei *length,
                                        GLchar *out) {
  if (length)
    *length = 0;
  if (n > 0)
    *out = 0;
}
extern "C" void lvk_glDeleteShader(GLuint id) {
  const auto it = state.shaders.find(id);
  if (it == state.shaders.end()) return;
  if (it->second.attachments) it->second.delete_pending = true;
  else state.shaders.erase(it);
}
extern "C" void lvk_glDeleteProgram(GLuint id) {
  if (!state.programs.count(id)) return;
  if (state.program == id) state.programs.at(id).delete_pending = true;
  else release_program(id);
}
extern "C" GLint lvk_glGetAttribLocation(GLuint id, const GLchar *name) {
  const auto &p = state.programs.at(id);
  if (!p.record)
    return -1;
  for (const auto &a : p.record->attributes)
    if (a.name == name)
      return GLint(a.location);
  return -1;
}

namespace {
bool cube_face(GLenum target) {return target>=GL_TEXTURE_CUBE_MAP_POSITIVE_X&&target<=GL_TEXTURE_CUBE_MAP_NEGATIVE_Z;}
TextureObject &texture(GLenum target) {
  const auto unit = state.active_texture - GL_TEXTURE0;
  if (target == GL_TEXTURE_2D) return state.texture_objects[state.textures[unit]];
  if (target == GL_TEXTURE_RECTANGLE_ARB) return state.texture_objects[state.rect_textures[unit]];
  if (target == GL_TEXTURE_CUBE_MAP || cube_face(target)) return state.texture_objects[state.cube_textures[unit]];
  std::fprintf(stderr,"[vulkan] texture target %#x\n",target);gap("unsupported guest texture target");
}
// The image levels a TexImage call addresses: a cube face's own, else the texture's.
std::map<int, TextureLevel> &target_levels(TextureObject &o, GLenum target) {
  if (!cube_face(target)) return o.levels;
  o.cube = true;
  const auto face = target - GL_TEXTURE_CUBE_MAP_POSITIVE_X;
  return face ? o.cube_faces[face - 1] : o.levels;
}
int components(GLenum format) {
  switch (format) {
  case GL_DEPTH_COMPONENT:
  case GL_DEPTH_STENCIL:
  case GL_COLOR_INDEX:
  case GL_LUMINANCE:
  case GL_ALPHA:
  case GL_RED:
    return 1;
  case GL_LUMINANCE_ALPHA:
    return 2;
  case GL_RGB:
    return 3;
  case GL_RGBA:
  case GL_BGRA:
    return 4;
  default:
    std::fprintf(stderr, "[vulkan] texture pixel format %#x\n", format);
    gap("unsupported guest texture pixel format");
  }
}
void canonical_color_storage(TextureLevel &v) {
  if (!v.compressed)
    v.format=guest::canonical_color_storage(v.internal,v.format,v.type,uint32_t(v.width),uint32_t(v.height),v.bytes);
}
int integer(GLenum name, int fallback) {
  auto it = state.integers.find(name);
  return it == state.integers.end() ? fallback : it->second;
}
size_t pixel_size(GLenum format, GLenum type) {
  if (type == GL_UNSIGNED_BYTE) return size_t(components(format));
  if (format == GL_DEPTH_COMPONENT && (type == GL_UNSIGNED_INT || type == GL_FLOAT)) return 4;
  if (format == GL_DEPTH_STENCIL && type == GL_UNSIGNED_INT_24_8) return 4;
  if ((format == GL_RGBA || format == GL_BGRA) && type == GL_UNSIGNED_INT_8_8_8_8_REV) return 4;
  if ((format == GL_RGB && type == GL_UNSIGNED_SHORT_5_6_5) ||
      (format == GL_BGRA && type == GL_UNSIGNED_SHORT_4_4_4_4_REV)) return 2;
  return 0;
}
struct UnpackRows { size_t stride, skip, bytes; };
UnpackRows unpack_rows(GLsizei w, GLsizei h, size_t pixel_bytes, GLenum type) {
  const auto alignment = integer(GL_UNPACK_ALIGNMENT, 4);
  const auto width = integer(GL_UNPACK_ROW_LENGTH, 0);
  const auto rows = integer(GL_UNPACK_SKIP_ROWS, 0), columns = integer(GL_UNPACK_SKIP_PIXELS, 0);
  if ((alignment != 1 && alignment != 2 && alignment != 4 && alignment != 8) || width < 0 || rows < 0 || columns < 0)
    gap("invalid guest pixel unpack layout");
  if (integer(GL_UNPACK_SWAP_BYTES, 0) && type != GL_UNSIGNED_BYTE)
    gap("guest pixel byte swapping needs implementation");
  const size_t raw = size_t(width ? width : w) * pixel_bytes;
  const size_t stride = (raw + alignment - 1) & ~size_t(alignment - 1);
  const size_t skip = size_t(rows) * stride + size_t(columns) * pixel_bytes;
  return {stride, skip, skip + (h ? size_t(h - 1) * stride + size_t(w) * pixel_bytes : 0)};
}
const uint8_t *pixels(const void *pointer, size_t count) {
  if (!state.unpack_buffer)
    return static_cast<const uint8_t *>(pointer);
  auto &b = state.buffers.at(state.unpack_buffer);
  auto offset = reinterpret_cast<uintptr_t>(pointer);
  if (offset > b.bytes.size() || count > b.bytes.size() - offset)
    gap("pixel unpack buffer overflow");
  return b.bytes.data() + offset;
}
uint32_t guest_pointer(const void *p) {
  const auto value = reinterpret_cast<uintptr_t>(p),
             base = reinterpret_cast<uintptr_t>(g_guest_base);
  if (state.array_buffer) {
    if (value > UINT32_MAX)
      gap("guest VBO offset overflow");
    return uint32_t(value);
  }
  if (!value) return 0; // a null client-array pointer is legal until used
  if (value < base || value - base > UINT32_MAX) {
    std::fprintf(stderr,"[vulkan] array pointer %p outside guest window %p (VBO %u)\n",p,g_guest_base,state.array_buffer);
    gap("host pointer has no guest vertex-array address");
  }
  return uint32_t(value - base);
}
void uniform(GLint location, const void *data, size_t bytes, GLsizei count = 1,
             bool transpose = false) {
  if (location == -1)
    return;
  if (!state.program || count < 0)
    gap("invalid guest uniform write");
  auto &p = state.programs.at(state.program);
  if (location < 0 || size_t(location) >= p.locations.size())
    gap("unknown guest uniform location");
  const auto &where = p.locations[size_t(location)];
  if (where.sampler >= 0) {
    if (bytes != 4 || count != 1)
      gap("invalid guest sampler uniform");
    p.sampler_units[where.sampler] = *static_cast<const GLint *>(data);
    return;
  }
  if (count)
    p.uniforms[where.block]->write(where.field, data, bytes, where.first,
                                   uint32_t(count), transpose);
}
} // namespace
extern "C" void lvk_glTexParameteri(GLenum target, GLenum p, GLint v) {
  texture(target).parameters[p] = v;
}
extern "C" void lvk_glTexParameterf(GLenum target, GLenum p, GLfloat v) {
  texture(target).parameters[p] = GLint(v);
}
extern "C" void lvk_glTexImage2D(GLenum target, GLint level, GLint internal,
                                 GLsizei w, GLsizei h, GLint border,
                                 GLenum format, GLenum type,
                                 const GLvoid *pointer) {
  // Packed guest storage maps directly to native formats and view swizzles.
  const auto pixel_bytes = pixel_size(format, type);
  if (w < 0 || h < 0 || level < 0 || border || !pixel_bytes) {
    std::fprintf(
        stderr,
        "[vulkan] texture internal %#x format %#x type %#x size %dx%d\n",
        internal, format, type, w, h);
    gap("guest texture format/dimensions need conversion support");
  }
  auto &object=texture(target); ++object.revision; object.dynamic=object.dynamic || bool(object.stable.image);
  if(level==0) {object.gpu_target=false;object.layers=1;object.render_width=object.render_height=0;object.picture_view=false;}
  auto &v = target_levels(object, target)[level];
  v = {internal, w, h, format, type, {}, false};
  const auto row = size_t(w) * pixel_bytes;
  v.bytes.resize(row * size_t(h));
  if (!pointer && !state.unpack_buffer) {
    canonical_color_storage(v);
    return;
  }
  const auto layout = unpack_rows(w, h, pixel_bytes, type);
  const auto stride = layout.stride, skip = layout.skip;
  const auto source = pixels(pointer, layout.bytes);
  if (format == GL_COLOR_INDEX) {
    if (integer(GL_INDEX_SHIFT, 0) || integer(GL_INDEX_OFFSET, 0))
      gap("indexed texture transfer shift/offset needs implementation");
    v.format = GL_RGBA;
    v.internal = GL_RGBA8;
    v.bytes.resize(size_t(w) * h * 4);
    const GLenum maps[] = {GL_PIXEL_MAP_I_TO_R, GL_PIXEL_MAP_I_TO_G,
                           GL_PIXEL_MAP_I_TO_B, GL_PIXEL_MAP_I_TO_A};
    for (GLsizei y = 0; y < h; ++y)
      for (GLsizei x = 0; x < w; ++x)
        for (unsigned c = 0; c < 4; ++c) {
          const auto index = source[skip + size_t(y) * stride + x];
          const auto &map = state.pixel_maps[maps[c]];
          const uint32_t value =
              map.empty() ? (c == 3 ? 65535 : 0) : map[index % map.size()];
          v.bytes[(size_t(y) * w + x) * 4 + c] =
              uint8_t((value * 255 + 32767) / 65535);
        }
  } else
    for (GLsizei y = 0; y < h; ++y)
      std::memcpy(v.bytes.data() + size_t(y) * row,
                  source + skip + size_t(y) * stride, row);
  canonical_color_storage(v);
}
extern "C" void lvk_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y,
                                    GLsizei w, GLsizei h, GLenum format, GLenum type,
                                    const GLvoid *pointer) {
  auto &object = texture(target);
  if(format==GL_LUMINANCE && int64_t(w)*h>=256*256)object.movie=true;
  auto &levels = target_levels(object, target);
  const auto it = levels.find(level);
  if (it == levels.end()) gap("guest texture subimage has no allocated level");
  auto &v = it->second;
  if (x < 0 || y < 0 || w < 0 || h < 0 || x > v.width || w > v.width - x ||
      y > v.height || h > v.height - y || v.compressed)
    gap("guest texture subimage exceeds level storage");
  if (!w || !h) return;
  const auto pixel_bytes = pixel_size(format, type);
  if (!pixel_bytes || format != v.format || type != v.type || format == GL_COLOR_INDEX)
    gap("guest texture subimage requires pixel conversion");
  if (!pointer && !state.unpack_buffer) gap("null guest texture subimage source");
  const auto layout = unpack_rows(w, h, pixel_bytes, type);
  const auto source = pixels(pointer, layout.bytes);
  for (GLsizei row = 0; row < h; ++row)
    std::memcpy(v.bytes.data() + (size_t(y + row) * v.width + x) * pixel_bytes,
                source + layout.skip + size_t(row) * layout.stride, size_t(w) * pixel_bytes);
  ++object.revision;
  object.dynamic = object.dynamic || bool(object.stable.image);
}
extern "C" void lvk_glCompressedTexImage2D(GLenum target, GLint level,
                                           GLenum internal, GLsizei w,
                                           GLsizei h, GLint border,
                                           GLsizei bytes, const GLvoid *data) {
  if (w < 0 || h < 0 || level < 0 || border || bytes < 0)
    gap("invalid guest compressed texture dimensions");
  auto &object=texture(target); ++object.revision; object.dynamic=object.dynamic || bool(object.stable.image);
  auto &v = target_levels(object, target)[level];
  v = {GLint(internal), w, h, 0, 0, {}, true};
  v.bytes.resize(size_t(bytes));
  if (data || state.unpack_buffer)
    std::memcpy(v.bytes.data(), pixels(data, size_t(bytes)), size_t(bytes));
}
extern "C" void lvk_glDeleteTextures(GLsizei n, const GLuint *ids) {
  for (GLsizei i = 0; i < n; ++i) {
    state.texture_objects.erase(ids[i]);state.stale_textures.erase(ids[i]);state.render_textures.erase(ids[i]);
  }
}
extern "C" void lvk_glTexEnvf(GLenum target, GLenum name, GLfloat value) {
  const unsigned unit=state.active_texture-GL_TEXTURE0;
  if(target==GL_TEXTURE_ENV)state.env_params[unit][name]=value; // per texture unit, as in GL
  else state.floats[(target << 16) | name] = {value, 0, 0, 0};
  if(target==GL_TEXTURE_ENV&&name==GL_TEXTURE_ENV_MODE)state.texture_env[unit]=GLenum(value);
}
extern "C" void lvk_glHint(GLenum target, GLenum mode) {
  state.integers[target] = mode;
} // advisory quality hint
extern "C" void lvk_glShadeModel(GLenum model) {
  state.integers[GL_SHADE_MODEL] = model;
}
extern "C" void lvk_glColorMask(GLboolean r, GLboolean g, GLboolean b,
                                GLboolean a) {
  state.floats[GL_COLOR_WRITEMASK] = {float(r), float(g), float(b), float(a)};
}
extern "C" void lvk_glBlendFunc(GLenum src, GLenum dst) {
  state.integers[GL_BLEND_SRC] = src;
  state.integers[GL_BLEND_DST] = dst;
}
extern "C" void lvk_glCullFace(GLenum face) {
  state.integers[GL_CULL_FACE_MODE] = face;
}
extern "C" void lvk_glFrontFace(GLenum face) {
  state.integers[GL_FRONT_FACE] = face;
}
extern "C" void lvk_glLightf(GLenum light, GLenum name, GLfloat v) { ++state.legacy_version;
  state.floats[(light << 16) | name] = {v, 0, 0, 0};
}
extern "C" void lvk_glLightfv(GLenum light, GLenum name, const GLfloat *v) { ++state.legacy_version;
  auto &out = state.floats[(light << 16) | name];
  auto n = name == GL_SPOT_DIRECTION ? 3
           : name == GL_AMBIENT || name == GL_DIFFUSE || name == GL_SPECULAR ||
                   name == GL_POSITION
               ? 4
               : 1;
  std::copy(v, v + n, out.begin());
  // GL keeps a light's position and spot direction in eye coordinates: they
  // are transformed by the modelview matrix current when they are specified.
  if (name == GL_POSITION || name == GL_SPOT_DIRECTION) {
    const auto &m = state.matrices.matrix(GL_MODELVIEW);
    const std::array<float,4> in{out[0], out[1], out[2], name == GL_POSITION ? out[3] : 0.f};
    for (unsigned i = 0; i < (name == GL_POSITION ? 4u : 3u); ++i)
      out[i] = m[i] * in[0] + m[4 + i] * in[1] + m[8 + i] * in[2] + m[12 + i] * in[3];
  }
}
extern "C" void lvk_glMaterialfv(GLenum face, GLenum name, const GLfloat *v) { ++state.legacy_version;
  if (face == GL_FRONT_AND_BACK) {
    auto n = name == GL_SHININESS ? 1 : 4;
    std::copy(v, v + n, state.floats[(GL_FRONT << 16) | name].begin());
    std::copy(v, v + n, state.floats[(GL_BACK << 16) | name].begin());
  } else
    std::copy(v, v + (name == GL_SHININESS ? 1 : 4),
              state.floats[(face << 16) | name].begin());
}
extern "C" void lvk_glFogf(GLenum name, GLfloat value) { ++state.legacy_version;
  state.floats[name] = {value, 0, 0, 0};
}
extern "C" void lvk_glFogi(GLenum name, GLint value) { ++state.legacy_version;
  state.floats[name] = {float(value), 0, 0, 0};
}
extern "C" void lvk_glFogfv(GLenum name, const GLfloat *v) { ++state.legacy_version;
  std::copy(v, v + (name == GL_FOG_COLOR ? 4 : 1), state.floats[name].begin());
}
extern "C" void lvk_glVertexPointer(GLint size, GLenum type, GLsizei stride,
                                    const GLvoid *pointer) {
  if (stride < 0)
    gap("negative guest vertex stride");
  state.arrays.pointer(0, size, type, false, stride, guest_pointer(pointer));
  state.legacy_bounds_arrays.pointer(0,size,type,false,stride,guest_pointer(pointer));
}
extern "C" void lvk_glNormalPointer(GLenum type, GLsizei stride,
                                    const GLvoid *pointer) {
  if (stride < 0)
    gap("negative guest normal stride");
  state.arrays.pointer(2, 3, type, type != GL_FLOAT, stride,
                       guest_pointer(pointer));
}
extern "C" void lvk_glColorPointer(GLint size, GLenum type, GLsizei stride,
                                   const GLvoid *pointer) {
  if (stride < 0)
    gap("negative guest color stride");
  state.arrays.pointer(3, size, type, type != GL_FLOAT, stride,
                       guest_pointer(pointer));
}
extern "C" void lvk_glSecondaryColorPointer(GLint size, GLenum type, GLsizei stride,
                                            const GLvoid *pointer) {
  if (stride < 0) gap("negative guest secondary color stride");
  state.arrays.pointer(4, size, type, type != GL_FLOAT, stride, guest_pointer(pointer));
}
extern "C" void lvk_glTexCoordPointer(GLint size, GLenum type, GLsizei stride,
                                      const GLvoid *pointer) {
  if (stride < 0)
    gap("negative guest texcoord stride");
  state.arrays.pointer(8 + state.client_texture - GL_TEXTURE0, size, type,
                       false, stride, guest_pointer(pointer));
}
extern "C" void lvk_glVertexAttribPointer(GLuint index, GLint size, GLenum type,
                                          GLboolean norm, GLsizei stride,
                                          const GLvoid *pointer) {
  if (stride < 0)
    gap("negative guest attrib stride");
  state.arrays.pointer(index, size, type, norm, stride, guest_pointer(pointer));
}
extern "C" void lvk_glEnableVertexAttribArray(GLuint index) {
  state.arrays.enable(index, true);
}
extern "C" void lvk_glDisableVertexAttribArray(GLuint index) {
  state.arrays.enable(index, false);
}
extern "C" void lvk_glEnableClientState(GLenum cap) {
  // Colour-index and edge-flag arrays do nothing in RGBA mode with filled
  // polygons; only their enable state is kept.
  if(cap==GL_INDEX_ARRAY||cap==GL_EDGE_FLAG_ARRAY) {state.enabled[cap]=true;return;}
  if(cap==GL_VERTEX_ARRAY)state.legacy_bounds_arrays.enable(0,true);
  unsigned index;
  if (cap == GL_VERTEX_ARRAY)
    index = 0;
  else if (cap == GL_NORMAL_ARRAY)
    index = 2;
  else if (cap == GL_COLOR_ARRAY)
    index = 3;
  else if (cap == GL_SECONDARY_COLOR_ARRAY)
    index = 4;
  else if (cap == GL_TEXTURE_COORD_ARRAY)
    index = 8 + state.client_texture - GL_TEXTURE0;
  else {
    std::fprintf(stderr, "[vulkan] unknown client array %#x\n", cap);
    gap("unsupported guest client array");
  }
  state.arrays.enable(index, true);
  state.enabled[cap] = true;
}
extern "C" void lvk_glDisableClientState(GLenum cap) {
  if(cap==GL_INDEX_ARRAY||cap==GL_EDGE_FLAG_ARRAY||cap==GL_FOG_COORD_ARRAY) {state.enabled[cap]=false;return;}
  if(cap==GL_VERTEX_ARRAY)state.legacy_bounds_arrays.enable(0,false);
  unsigned index;
  if (cap == GL_VERTEX_ARRAY)
    index = 0;
  else if (cap == GL_NORMAL_ARRAY)
    index = 2;
  else if (cap == GL_COLOR_ARRAY)
    index = 3;
  else if (cap == GL_SECONDARY_COLOR_ARRAY)
    index = 4;
  else if (cap == GL_TEXTURE_COORD_ARRAY)
    index = 8 + state.client_texture - GL_TEXTURE0;
  else {
    std::fprintf(stderr, "[vulkan] unknown client array %#x\n", cap);
    gap("unsupported guest client array");
  }
  state.arrays.enable(index, false);
  state.enabled[cap] = false;
}
extern "C" GLint lvk_glGetUniformLocation(GLuint id, const GLchar *name) {
  auto &p = state.programs.at(id);
  if (!p.record)
    return -1;
  const auto cached = p.location_names.find(name);
  if (cached != p.location_names.end())
    return cached->second;
  UniformLocation location{};
  bool found = false;
  for (const auto &sampler : p.record->samplers)
    if (sampler.name == name) {
      location.sampler = int(sampler.binding);
      found = true;
      break;
    }
  if (!found)
    for (size_t block = 0; block < p.record->uniforms.size() && !found;
         ++block) {
      if (p.record->uniforms[block].set != 1)
        continue;
      auto base = std::string(name);
      uint32_t first = 0;
      auto field = p.uniforms[block]->find(base);
      if (field < 0 && base.size() > 3 && base.back() == ']') {
        const auto at = base.rfind('[');
        if (at != std::string::npos) {
          first = uint32_t(std::strtoul(base.c_str() + at + 1, nullptr, 10));
          base.resize(at);
          field = p.uniforms[block]->find(base);
        }
      }
      if (field >= 0 &&
          first < p.record->uniforms[block].fields[size_t(field)].count) {
        location = {uint32_t(block), uint32_t(field), first, -1};
        found = true;
      }
    }
  if (!found)
    return -1;
  const auto result = GLint(p.locations.size());
  p.locations.push_back(location);
  p.location_names[name] = result;
  return result;
}
extern "C" void lvk_glUniform1f(GLint l, GLfloat v) { uniform(l, &v, 4); }
extern "C" void lvk_glUniform1i(GLint l, GLint v) { uniform(l, &v, 4); }
extern "C" void lvk_glUniform2f(GLint l, GLfloat a, GLfloat b) {
  float v[] = {a, b};
  uniform(l, v, sizeof v);
}
extern "C" void lvk_glUniform3f(GLint l, GLfloat a, GLfloat b, GLfloat c) {
  float v[] = {a, b, c};
  uniform(l, v, sizeof v);
}
extern "C" void lvk_glUniform4f(GLint l, GLfloat a, GLfloat b, GLfloat c,
                                GLfloat d) {
  float v[] = {a, b, c, d};
  uniform(l, v, sizeof v);
}
extern "C" void lvk_glUniform1fv(GLint l, GLsizei n, const GLfloat *v) {
  uniform(l, v, size_t(n) * 4, n);
}
extern "C" void lvk_glUniform2fv(GLint l, GLsizei n, const GLfloat *v) {
  uniform(l, v, size_t(n) * 8, n);
}
extern "C" void lvk_glUniform3fv(GLint l, GLsizei n, const GLfloat *v) {
  uniform(l, v, size_t(n) * 12, n);
}
extern "C" void lvk_glUniform4fv(GLint l, GLsizei n, const GLfloat *v) {
  uniform(l, v, size_t(n) * 16, n);
}
extern "C" void lvk_glUniformMatrix2fv(GLint l, GLsizei n, GLboolean transpose, const GLfloat *v) {
  uniform(l, v, size_t(n) * 16, n, transpose);
}
extern "C" void lvk_glUniformMatrix3fv(GLint l, GLsizei n, GLboolean transpose, const GLfloat *v) {
  uniform(l, v, size_t(n) * 36, n, transpose);
}
extern "C" void lvk_glUniformMatrix4fv(GLint l, GLsizei n, GLboolean transpose,
                                       const GLfloat *v) {
  uniform(l, v, size_t(n) * 64, n, transpose);
}

extern "C" void lvk_glPixelMapusv(GLenum name, GLsizei n,
                                  const GLushort *data) {
  if (n < 0)
    gap("negative guest pixel map size");
  state.pixel_maps[name] = std::vector<GLushort>(data, data + n);
}
extern "C" void lvk_glPixelTransferi(GLenum name, GLint value) {
  state.integers[name] = value;
}
extern "C" void lvk_glPixelTransferf(GLenum name, GLfloat value) {
  state.floats[name] = {value, 0, 0, 0};
}

extern "C" void lvk_glPushAttrib(GLbitfield mask) {
  if (mask != GL_ALL_ATTRIB_BITS && mask != GL_ENABLE_BIT &&
      mask != GL_VIEWPORT_BIT && mask != GL_SCISSOR_BIT &&
      mask != GL_PIXEL_MODE_BIT) {
    std::fprintf(stderr, "[vulkan] guest attribute mask %#x\n", mask);
    gap("guest attribute group requires selective restoration");
  }
  if (state.attributes.size() >= 16)
    gap("guest attribute stack overflow");
  state.attributes.push_back({mask, state.pixel_maps, state.integers,
                              state.floats, state.enabled, state.raster,
                              state.viewport, state.scissor, state.matrix_mode,
                              state.active_texture, state.textures});
}
extern "C" void lvk_glPopAttrib(void) {
  if (state.attributes.empty())
    gap("guest attribute stack underflow");
  auto saved = std::move(state.attributes.back());
  state.attributes.pop_back();
  if (saved.mask == GL_ALL_ATTRIB_BITS) {
    state.pixel_maps = std::move(saved.pixel_maps);
    state.integers = std::move(saved.integers);
    state.floats = std::move(saved.floats);
    state.raster = saved.raster;
    state.matrix_mode = saved.matrix_mode;
    state.matrices.matrix_mode(saved.matrix_mode);
    state.active_texture = saved.active_texture;
    state.matrices.active_texture(saved.active_texture - GL_TEXTURE0);
    state.textures = saved.textures;
  }
  if (saved.mask == GL_PIXEL_MODE_BIT) {
    state.pixel_maps = std::move(saved.pixel_maps);
    for (auto key :
         {GL_MAP_COLOR, GL_MAP_STENCIL, GL_INDEX_SHIFT, GL_INDEX_OFFSET}) {
      if (saved.integers.count(key))
        state.integers[key] = saved.integers[key];
      else
        state.integers.erase(key);
    }
    for (auto key :
         {GL_RED_SCALE, GL_GREEN_SCALE, GL_BLUE_SCALE, GL_ALPHA_SCALE,
          GL_DEPTH_SCALE, GL_RED_BIAS, GL_GREEN_BIAS, GL_BLUE_BIAS,
          GL_ALPHA_BIAS, GL_DEPTH_BIAS, GL_ZOOM_X, GL_ZOOM_Y}) {
      if (saved.floats.count(key))
        state.floats[key] = saved.floats[key];
      else
        state.floats.erase(key);
    }
  }
  if (saved.mask & GL_ENABLE_BIT) {
    state.enabled = std::move(saved.enabled);
    state.raster.depth_test = state.enabled[GL_DEPTH_TEST];
    state.raster.blend = state.enabled[GL_BLEND];
  }
  if (saved.mask & GL_VIEWPORT_BIT)
    state.viewport = saved.viewport;
  if (saved.mask & GL_SCISSOR_BIT)
    state.scissor = saved.scissor;
}

extern "C" void lvk_glTexParameterfv(GLenum target, GLenum name,
                                     const GLfloat *values) {
  auto &t = texture(target);
  std::copy(values, values + (name == GL_TEXTURE_BORDER_COLOR ? 4 : 1),
            t.float_parameters[name].begin());
  if (name != GL_TEXTURE_BORDER_COLOR)
    t.parameters[name] = GLint(values[0]);
}
// ARB assembly program names: the programs themselves live in arbprog.c,
// which keys them by name below its table size (4096).
extern "C" void lvk_glGenProgramsARB(GLsizei n, GLuint *ids) {
  static GLuint next=1;
  for(GLsizei i=0;ids&&i<n;++i){if(next>=4096)gap("too many guest assembly programs");ids[i]=next++;}
}
extern "C" GLboolean lvk_glIsProgramARB(GLuint id) {return id&&id<4096?GL_TRUE:GL_FALSE;}
// Occlusion queries: provisional. Every query reports all of its samples
// visible until GPU queries are integrated; the call pattern is logged so
// the readback strategy can follow the game's use.
namespace { std::unordered_map<GLuint,uint32_t> g_queries; unsigned g_query_log=0; }
static void query_log(const char *what,GLuint id) {
  if(g_query_log<40){++g_query_log;std::fprintf(stderr,"[vulkan-query] frame %u %s %u (provisional: reports visible)\n",g_frame,what,id);}
}
extern "C" void lvk_glGenQueries(GLsizei n, GLuint *ids) {for(GLsizei i=0;ids&&i<n;++i){ids[i]=object();g_queries[ids[i]]=0;query_log("gen",ids[i]);}}
extern "C" void lvk_glDeleteQueries(GLsizei n, const GLuint *ids) {for(GLsizei i=0;ids&&i<n;++i)g_queries.erase(ids[i]);}
extern "C" GLboolean lvk_glIsQuery(GLuint id) {return g_queries.count(id)?GL_TRUE:GL_FALSE;}
extern "C" void lvk_glBeginQuery(GLenum target, GLuint id) {g_queries[id]=0;query_log("begin",id);(void)target;}
extern "C" void lvk_glEndQuery(GLenum target) {(void)target;}
extern "C" void lvk_glGetQueryObjectuiv(GLuint id, GLenum name, GLuint *v) {
  if(!v)return;query_log(name==GL_QUERY_RESULT_AVAILABLE?"available?":"result",id);
  *v=name==GL_QUERY_RESULT_AVAILABLE?1u:0x7fffffffu;
}
extern "C" void lvk_glGetQueryObjectiv(GLuint id, GLenum name, GLint *v) {GLuint u=0;lvk_glGetQueryObjectuiv(id,name,&u);if(v)*v=GLint(u);}
extern "C" void lvk_glGetQueryiv(GLenum target, GLenum name, GLint *v) {if(v)*v=name==GL_QUERY_COUNTER_BITS?32:0;(void)target;}
extern "C" void lvk_glGetTexParameterfv(GLenum target, GLenum name, GLfloat *v) {
  if (!v) return;
  auto &t = texture(target);
  if (name == GL_TEXTURE_BORDER_COLOR) {
    auto it = t.float_parameters.find(name);
    for (unsigned i = 0; i < 4; ++i) v[i] = it == t.float_parameters.end() ? 0 : it->second[i];
    return;
  }
  if (auto it = t.float_parameters.find(name); it != t.float_parameters.end()) { v[0] = it->second[0]; return; }
  if (auto it = t.parameters.find(name); it != t.parameters.end()) { v[0] = GLfloat(it->second); return; }
  switch (name) { // GL's initial texture parameters
  case GL_TEXTURE_MIN_FILTER: v[0] = GL_NEAREST_MIPMAP_LINEAR; return;
  case GL_TEXTURE_MAG_FILTER: v[0] = GL_LINEAR; return;
  case GL_TEXTURE_WRAP_S: case GL_TEXTURE_WRAP_T: case GL_TEXTURE_WRAP_R: v[0] = GL_REPEAT; return;
  case GL_TEXTURE_BASE_LEVEL: case GL_GENERATE_MIPMAP: v[0] = 0; return;
  case GL_TEXTURE_MAX_LEVEL: v[0] = 1000; return;
  case GL_TEXTURE_MIN_LOD: v[0] = -1000; return;
  case GL_TEXTURE_MAX_LOD: v[0] = 1000; return;
  case GL_TEXTURE_COMPARE_MODE: v[0] = GL_NONE; return;
  case GL_TEXTURE_COMPARE_FUNC: v[0] = GL_LEQUAL; return;
  case GL_DEPTH_TEXTURE_MODE: v[0] = GL_LUMINANCE; return;
  case GL_TEXTURE_PRIORITY: case GL_TEXTURE_RESIDENT: case GL_TEXTURE_MAX_ANISOTROPY_EXT: v[0] = 1; return;
  }
  std::fprintf(stderr, "[vulkan] glGetTexParameter unsupported %#x\n", name);
  gap("texture parameter query requires implementation");
}
extern "C" void lvk_glGetTexParameteriv(GLenum target, GLenum name, GLint *v) {
  GLfloat f[4]{};lvk_glGetTexParameterfv(target, name, f);
  for (unsigned i = 0; i < (name == GL_TEXTURE_BORDER_COLOR ? 4u : 1u); ++i)
    v[i] = name == GL_TEXTURE_BORDER_COLOR ? GLint(f[i] * 2147483647.f) : GLint(f[i]);
}
extern "C" void lvk_glGetTexLevelParameteriv(GLenum target, GLint level, GLenum name, GLint *v) {
  if (!v) return;
  auto &t = texture(target);
  auto it = t.levels.find(level);
  if (it == t.levels.end()) { *v = 0; return; } // an undefined level reports zero size
  const auto &l = it->second;
  switch (name) {
  case GL_TEXTURE_WIDTH: *v = l.width; return;
  case GL_TEXTURE_HEIGHT: *v = l.height; return;
  case GL_TEXTURE_DEPTH: *v = 1; return;
  case GL_TEXTURE_INTERNAL_FORMAT: *v = l.internal; return;
  case GL_TEXTURE_BORDER: *v = 0; return;
  case GL_TEXTURE_COMPRESSED: *v = l.compressed; return;
  case GL_TEXTURE_COMPRESSED_IMAGE_SIZE: *v = GLint(l.bytes.size()); return;
  }
  std::fprintf(stderr, "[vulkan] glGetTexLevelParameter unsupported %#x\n", name);
  gap("texture level query requires implementation");
}
extern "C" void lvk_glGetTexLevelParameterfv(GLenum target, GLint level, GLenum name, GLfloat *v) {
  GLint i = 0;lvk_glGetTexLevelParameteriv(target, level, name, &i);if (v) *v = GLfloat(i);
}
extern "C" void lvk_glAlphaFunc(GLenum compare, GLclampf reference) { ++state.legacy_version;
  state.integers[GL_ALPHA_TEST_FUNC] = compare;
  state.floats[GL_ALPHA_TEST_REF] = {std::clamp(reference, 0.f, 1.f), 0, 0, 0};
}
extern "C" void lvk_glPointSize(GLfloat size) {
  state.floats[GL_POINT_SIZE] = {size, 0, 0, 0};
}
// Point size attenuation and fade are fixed-function point state; programs
// that write gl_PointSize (GSE's particle vertex programs) set the size. Kept
// for queries.
extern "C" void lvk_glPointParameterfv(GLenum name, const GLfloat *v) {
  if(!v)return;
  auto &value=state.floats[name];value={v[0],0,0,0};
  if(name==GL_POINT_DISTANCE_ATTENUATION)value={v[0],v[1],v[2],0};
}
extern "C" void lvk_glPointParameterf(GLenum name, GLfloat v) {const GLfloat a[3]={v,0,0};lvk_glPointParameterfv(name,a);}
extern "C" void lvk_glPointParameteri(GLenum name, GLint v) {lvk_glPointParameterf(name,GLfloat(v));}
extern "C" void lvk_glPointParameteriv(GLenum name, const GLint *v) {if(v){const GLfloat a[3]={GLfloat(v[0]),GLfloat(v[1]),GLfloat(v[2])};lvk_glPointParameterfv(name,a);}}
extern "C" void lvk_glLineWidth(GLfloat width) {
  state.floats[GL_LINE_WIDTH] = {width, 0, 0, 0};
}
extern "C" void lvk_glPolygonOffset(GLfloat factor, GLfloat units) {
  state.floats[GL_POLYGON_OFFSET_FACTOR] = {factor, 0, 0, 0};
  state.floats[GL_POLYGON_OFFSET_UNITS] = {units, 0, 0, 0};
}
extern "C" void lvk_glStencilMask(GLuint mask) {
  state.integers[GL_STENCIL_WRITEMASK] = GLint(mask);
}
extern "C" void lvk_glStencilFunc(GLenum compare, GLint reference,
                                  GLuint mask) {
  state.integers[GL_STENCIL_FUNC] = compare;
  state.integers[GL_STENCIL_REF] = reference;
  state.integers[GL_STENCIL_VALUE_MASK] = GLint(mask);
}
extern "C" void lvk_glStencilOp(GLenum fail, GLenum depth_fail, GLenum pass) {
  state.integers[GL_STENCIL_FAIL] = fail;
  state.integers[GL_STENCIL_PASS_DEPTH_FAIL] = depth_fail;
  state.integers[GL_STENCIL_PASS_DEPTH_PASS] = pass;
}

namespace {
void key_word(std::string &key,uint64_t word) { key.append(reinterpret_cast<const char *>(&word),sizeof word); }
void changed(TextureObject &o) { o.alias.reset();o.alias_target=nullptr;++o.revision; o.dynamic=o.dynamic || bool(o.stable.image); }
VkFormat texture_format(const TextureLevel &v) {
  if(v.compressed) switch(v.internal) {
    case 0x83f0:return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
    case 0x83f1:return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case 0x83f2:return VK_FORMAT_BC2_UNORM_BLOCK;
    case 0x83f3:return VK_FORMAT_BC3_UNORM_BLOCK;
    default:gap("unknown guest compressed texture format");
  }
  if (v.format == GL_BGRA && v.type == GL_UNSIGNED_SHORT_4_4_4_4_REV)
    return VK_FORMAT_R4G4B4A4_UNORM_PACK16;
  if (v.format == GL_RGB && v.type == GL_UNSIGNED_SHORT_5_6_5)
    return VK_FORMAT_R5G6B5_UNORM_PACK16;
  switch(v.format) {
    case GL_DEPTH_COMPONENT:case GL_DEPTH_STENCIL:return Target::depth_format;
    case GL_LUMINANCE:case GL_ALPHA:case GL_RED:return VK_FORMAT_R8_UNORM;
    case GL_LUMINANCE_ALPHA:return VK_FORMAT_R8G8_UNORM;
    case GL_BGRA:return VK_FORMAT_B8G8R8A8_UNORM;
    case GL_RGB:case GL_RGBA:return VK_FORMAT_R8G8B8A8_UNORM;
    default:gap("native texture format not implemented");
  }
}
std::shared_ptr<Texture> native_image(TextureObject &o, int overwritten_level = -1, bool gpu_copy = false) {
  state.frame();state.end_pass();
  if(o.levels.empty() || !o.levels.count(0)) {
    if(!state.fallback) {
      state.fallback=std::make_shared<Texture>(*state.device,TextureInfo{1,1});
      const uint8_t black[]={0,0,0,255}; state.fallback->upload(state.cmd,*state.ring,0,0,black,4);
    }
    return state.fallback;
  }
  const auto &base=o.levels.at(0);
  TextureInfo info{o.render_width?o.render_width:uint32_t(base.width),o.render_height?o.render_height:uint32_t(base.height),uint32_t(o.levels.rbegin()->first+1),o.layers,texture_format(base)};
  if(o.cube) {info.layers=6;info.cube=true;}
  if(base.format==GL_BGRA && base.type==GL_UNSIGNED_SHORT_4_4_4_4_REV)
    info.components={VK_COMPONENT_SWIZZLE_G,VK_COMPONENT_SWIZZLE_B,VK_COMPONENT_SWIZZLE_A,VK_COMPONENT_SWIZZLE_R};
  if(base.format==GL_LUMINANCE)info.components={VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_ONE};
  if(base.format==GL_LUMINANCE_ALPHA)info.components={VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_G};
  if(base.format==GL_ALPHA)info.components={VK_COMPONENT_SWIZZLE_ZERO,VK_COMPONENT_SWIZZLE_ZERO,VK_COMPONENT_SWIZZLE_ZERO,VK_COMPONENT_SWIZZLE_R};
  // A framebuffer copy drains preceding packets first. Its persistent image
  // can be updated in queue order after prior shader reads, including reads
  // from older in-flight frames. CPU uploads retain snapshot/version storage.
  auto &version=o.dynamic && !o.gpu_target?o.slots[state.ring->recording_slot()]:o.stable;
  const auto frame=state.ring->submissions()+1;
  if(version.revision!=o.revision) {
    const bool allocate=!version.image || (version.use_frame==frame && !gpu_copy) || version.image->info.width!=info.width || version.image->info.height!=info.height ||
        version.image->info.levels!=info.levels || version.image->info.layers!=info.layers || version.image->info.format!=info.format ||
        std::memcmp(&version.image->info.components,&info.components,sizeof info.components);
    if(allocate)version.image=std::make_shared<Texture>(*state.device,info);
    for(uint32_t face=0;face<(o.cube?6u:1u);++face)
    for(const auto &[level,v]:face?o.cube_faces[face-1]:o.levels) {
      if(gpu_copy && !allocate)continue; // preserve the last GPU contents for partial copies

      if(texture_format(v)!=info.format)gap("mip texture formats disagree");
      // A full GPU color/depth copy initializes this level directly. Never
      // stage the obsolete CPU backing pixels before overwriting them.
      if (level == overwritten_level) continue;
      if(info.width!=uint32_t(base.width) || info.height!=uint32_t(base.height))
        gap("scaled partial/mip texture initialization needs a preserving GPU resample");
      const void *data=v.bytes.data();size_t bytes=v.bytes.size();std::vector<uint8_t> expanded;
      if(v.format==GL_RGB&&v.type==GL_UNSIGNED_BYTE&&!v.compressed) {
        expanded.resize(size_t(v.width)*v.height*4);
        for(size_t i=0;i<size_t(v.width)*v.height;++i) {std::memcpy(expanded.data()+4*i,v.bytes.data()+3*i,3);expanded[4*i+3]=255;}
        data=expanded.data();bytes=expanded.size();
      } else if((v.format==GL_DEPTH_COMPONENT&&v.type==GL_UNSIGNED_INT)||(v.format==GL_DEPTH_STENCIL&&v.type==GL_UNSIGNED_INT_24_8)) {
        expanded.resize(bytes);const bool packed=v.format==GL_DEPTH_STENCIL;
        for(size_t i=0;i<bytes;i+=4) {uint32_t d;std::memcpy(&d,v.bytes.data()+i,4);
          const float f=packed?float(double(d>>8)/16777215.0):float(double(d)/4294967295.0);std::memcpy(expanded.data()+i,&f,4);}
        data=expanded.data();
      }
      if(o.cube)version.image->upload(state.cmd,*state.ring,uint32_t(level),face,data,bytes);
      else for(uint32_t layer=0;layer<info.layers;++layer)
        version.image->upload(state.cmd,*state.ring,uint32_t(level),layer,data,bytes);
    }
    version.revision=o.revision;
  }
  version.use_frame=frame;state.ring->keep_alive(version.image);return version.image;
}
std::shared_ptr<GeometryBuffer> native_buffer(GLuint id,bool dedicated_staging) {
  auto &o=state.buffers.at(id);state.frame();state.end_pass();
  auto &version=o.dynamic?o.slots[state.ring->recording_slot()]:o.stable;
  const auto frame=state.ring->submissions()+1;
  const auto bytes=(o.bytes.size()+3)&~size_t(3);
  if(!bytes)gap("empty guest geometry buffer used for drawing");
  if(version.revision!=o.revision) {
    if(!version.buffer||version.use_frame==frame||version.bytes!=bytes) {
      version.buffer=std::make_shared<GeometryBuffer>(*state.device,bytes);version.bytes=bytes;
    }
    if(bytes==o.bytes.size())version.buffer->upload(state.cmd,*state.ring,o.bytes.data(),bytes,0,dedicated_staging);
    else {auto copy=o.bytes;copy.resize(bytes);version.buffer->upload(state.cmd,*state.ring,copy.data(),bytes,0,dedicated_staging);}
    version.revision=o.revision;
  }
  version.use_frame=frame;state.ring->keep_alive(version.buffer);return version.buffer;
}
VkSamplerAddressMode wrap_mode(GLint value) {
  switch(value) {case GL_REPEAT:return VK_SAMPLER_ADDRESS_MODE_REPEAT;case GL_MIRRORED_REPEAT:return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
  case GL_CLAMP_TO_EDGE:return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;case GL_CLAMP:case GL_CLAMP_TO_BORDER:return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  default:gap("guest texture wrap mode not implemented");}
}
std::shared_ptr<Sampler> native_sampler(TextureObject &o,uint32_t levels,bool shadow) {
  auto param=[&](GLenum name,GLint fallback){const auto it=o.parameters.find(name);return it==o.parameters.end()?fallback:it->second;};
  SamplerInfo info;
  // Rectangle textures default to GL_LINEAR and GL_CLAMP_TO_EDGE and cannot repeat.
  const auto min=param(GL_TEXTURE_MIN_FILTER,o.rect?GL_LINEAR:GL_NEAREST_MIPMAP_LINEAR),mag=param(GL_TEXTURE_MAG_FILTER,GL_LINEAR);
  info.mag_filter=mag==GL_NEAREST?VK_FILTER_NEAREST:VK_FILTER_LINEAR;
  info.min_filter=min==GL_NEAREST||min==GL_NEAREST_MIPMAP_NEAREST||min==GL_NEAREST_MIPMAP_LINEAR?VK_FILTER_NEAREST:VK_FILTER_LINEAR;
  info.mip_filter=min==GL_NEAREST_MIPMAP_LINEAR||min==GL_LINEAR_MIPMAP_LINEAR?VK_SAMPLER_MIPMAP_MODE_LINEAR:VK_SAMPLER_MIPMAP_MODE_NEAREST;
  info.max_lod=min==GL_NEAREST||min==GL_LINEAR?0:float(levels-1);
  const GLint wrap=o.rect?GL_CLAMP_TO_EDGE:GL_REPEAT;
  info.u=wrap_mode(param(GL_TEXTURE_WRAP_S,wrap));info.v=wrap_mode(param(GL_TEXTURE_WRAP_T,wrap));
  info.compare=shadow;info.compare_op=VkCompareOp(param(GL_TEXTURE_COMPARE_FUNC,GL_LEQUAL)-GL_NEVER);
  const auto color=o.float_parameters.find(GL_TEXTURE_BORDER_COLOR);
  if(color!=o.float_parameters.end()) {
    if(color->second==std::array<float,4>{1,1,1,1})info.border=VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    else if(color->second==std::array<float,4>{0,0,0,1})info.border=VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    else if(color->second!=std::array<float,4>{0,0,0,0})gap("custom sampler border color requires device feature support");
  }
  std::string key;
  for(auto value:{int(info.min_filter),int(info.mag_filter),int(info.mip_filter),int(info.u),int(info.v),int(info.max_lod),int(info.compare),int(info.compare_op),int(info.border)})key_word(key,uint64_t(value));
  auto &sampler=state.samplers[key];if(!sampler)sampler=std::make_shared<Sampler>(*state.device,info);return sampler;
}
DrawPacket packet_for(Target *target,Renderer *renderer) {
  DrawPacket p{};p.target=target;p.renderer=renderer;p.color=state.clear;
  const auto logical=state.logical_size();
  const float sx=float(target->info.width)/logical[0],sy=float(target->info.height)/logical[1];
  p.viewport={state.viewport[0]*sx,state.viewport[1]*sy,state.viewport[2]*sx,state.viewport[3]*sy};
  auto rect=state.enabled[GL_SCISSOR_TEST]?state.scissor:std::array<GLint,4>{0,0,int(target->info.width/sx),int(target->info.height/sy)};
  int x0=std::clamp(int(std::floor(rect[0]*sx)),0,int(target->info.width));
  int x1=std::clamp(int(std::ceil((rect[0]+rect[2])*sx)),x0,int(target->info.width));
  int y0=std::clamp(int(std::floor(rect[1]*sy)),0,int(target->info.height));
  int y1=std::clamp(int(std::ceil((rect[1]+rect[3])*sy)),y0,int(target->info.height));
  p.scissor={{x0,target->info.guest_bottom_origin?y0:int(target->info.height)-y1},{uint32_t(x1-x0),uint32_t(y1-y0)}};
  p.stencil_ref=uint32_t(integer(GL_STENCIL_REF,0));p.stencil_compare_mask=uint32_t(integer(GL_STENCIL_VALUE_MASK,~0));
  p.stencil_write_mask=uint32_t(integer(GL_STENCIL_WRITEMASK,~0));
  p.depth_bias_factor=state.floats[GL_POLYGON_OFFSET_FACTOR][0];p.depth_bias_units=state.floats[GL_POLYGON_OFFSET_UNITS][0];
  p.blend_constants=state.floats[GL_BLEND_COLOR];
  return p;
}
void queue_clear(GLbitfield mask) {
  if(mask&~(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT))gap("invalid guest clear mask");
  auto *target=state.target();auto &renderer=state.renderers[target];mark_drawn();
  if(!renderer)renderer=std::make_unique<Renderer>(*state.device,*target,*state.cache);
  auto p=packet_for(target,renderer.get());
  const auto write=state.floats.find(GL_COLOR_WRITEMASK);
  bool full=true,none=false;
  if(write!=state.floats.end()) {full=std::all_of(write->second.begin(),write->second.end(),[](float v){return v!=0;});none=std::all_of(write->second.begin(),write->second.end(),[](float v){return v==0;});}
  if(mask&GL_COLOR_BUFFER_BIT) {if(!none)p.clear_aspects|=VK_IMAGE_ASPECT_COLOR_BIT;}
  if((mask&GL_DEPTH_BUFFER_BIT)&&state.raster.depth_write)p.clear_aspects|=VK_IMAGE_ASPECT_DEPTH_BIT;
  if((mask&GL_STENCIL_BUFFER_BIT)&&(p.stencil_write_mask&255)) {
    p.clear_aspects|=VK_IMAGE_ASPECT_STENCIL_BIT;
    // Bits outside the write mask known to be zero and cleared to zero: the
    // masked clear equals a full one, so no clear quad is needed.
    const uint32_t masked=p.stencil_write_mask&255;
    if(masked!=255&&p.clear_stencil==(p.clear_stencil&masked)&&!(state.stencil_known(target)&~masked&255))p.stencil_write_mask|=255;
  }
  p.clear_depth=state.floats.count(GL_DEPTH_CLEAR_VALUE)?state.floats[GL_DEPTH_CLEAR_VALUE][0]:1;
  p.clear_stencil=uint32_t(integer(GL_STENCIL_CLEAR_VALUE,0));
  if(p.clear_aspects && ((!full&&!none) || ((p.clear_aspects&VK_IMAGE_ASPECT_STENCIL_BIT)&&((p.stencil_write_mask&255)!=255)))) {
    p.count=6;p.clear_aspects=0;p.raster={};p.raster.depth_test=bool(mask&GL_DEPTH_BUFFER_BIT)&&state.raster.depth_write;
    p.raster.depth_write=p.raster.depth_test;p.raster.depth_compare=VK_COMPARE_OP_ALWAYS;
    p.raster.color_mask=0;if(mask&GL_COLOR_BUFFER_BIT)for(unsigned i=0;i<4;++i)if(write==state.floats.end()||write->second[i])p.raster.color_mask|=1u<<i;
    p.raster.stencil=(mask&GL_STENCIL_BUFFER_BIT)&&(p.stencil_write_mask&255);p.raster.stencil_compare=VK_COMPARE_OP_ALWAYS;
    if(target->info.transient_depth)p.raster.depth_test=p.raster.depth_write=p.raster.stencil=false;
    p.raster.stencil_pass=VK_STENCIL_OP_REPLACE;p.stencil_ref=p.clear_stencil;
    const auto z=std::clamp(p.clear_depth,0.f,1.f)*2-1;
    Vertex quad[]={{{-1,-1,z,1},{}},{{1,-1,z,1},{}},{{1,1,z,1},{}},{{-1,-1,z,1},{}},{{1,1,z,1},{}},{{-1,1,z,1},{}}};
    for(auto &v:quad)std::copy(p.color.begin(),p.color.end(),v.color);
    p.streams.push_back(state.ring->upload(quad,sizeof quad));for(auto &m:p.matrices.view_projection)for(unsigned i=0;i<4;++i)m[i*5]=1;
    p.viewport={0,0,float(target->info.width),float(target->info.height)};
  }
  static const auto clear_log=[] {std::array<unsigned,2> r{};if(const auto *v=std::getenv("LINDBERGH_VULKAN_CLASSIFY")){r[0]=r[1]=unsigned(std::strtoul(v,nullptr,10));if(const auto *end=std::strchr(v,'-'))r[1]=unsigned(std::strtoul(end+1,nullptr,10));}return r;}();
  if(clear_log[1]&&g_frame>=clear_log[0]&&g_frame<=clear_log[1])
    std::fprintf(stderr,"[vulkan-clear] frame %u target %u mask %#x color-mask full %d none %d depth-write %d -> aspects %#x quad %d stencil-write %#x clear-stencil %u depth %g color %g %g %g %g scissor %d %d %u %u\n",
      g_frame,state.drawable,mask,full,none,state.raster.depth_write,p.clear_aspects,p.count!=0,p.stencil_write_mask,p.clear_stencil,p.clear_depth,p.color[0],p.color[1],p.color[2],p.color[3],p.scissor.offset.x,p.scissor.offset.y,p.scissor.extent.width,p.scissor.extent.height);
  if(p.clear_aspects||p.count)state.packets.push_back(std::move(p));
}
}
extern "C" void lvk_glCopyTexSubImage2D(GLenum target,GLint level,GLint dx,GLint dy,GLint x,GLint y,GLsizei w,GLsizei h) {
  auto &o=texture(target);if(!o.levels.count(level)||w<0||h<0)gap("invalid guest texture copy");
  auto *source=state.target();state.flush();
  if(!state.initialized.count(source)) {state.renderer();state.end_pass();}
  const auto &v=o.levels.at(level);
  const auto logical=state.logical_size();
  const bool full_level=dx==0 && dy==0 && w==v.width && h==v.height;
  // Clip the source to the drawable (a clipped eye pbuffer ends at the frame).
  if(x<0){dx-=x;w+=x;x=0;} if(y<0){dy-=y;h+=y;y=0;}
  w=std::min(w,logical[0]-x);h=std::min(h,logical[1]-y);
  if(w<=0||h<=0)return;
  o.layers=source->info.layers;o.gpu_target=true;
  const double sx=double(source->info.width)/logical[0],sy=double(source->info.height)/logical[1];
  const auto &base=o.levels.at(0);
  o.render_width=uint32_t(std::lround(base.width*sx));o.render_height=uint32_t(std::lround(base.height*sy));
  if(full_level)o.picture_view=true; // copied images play the role of legacy rectangle/view textures
  changed(o);auto gpu=native_image(o,full_level?level:-1,true);
  const auto px=int32_t(std::lround(x*sx)),py=int32_t(std::lround(y*sy));
  const auto pw=uint32_t(std::lround((x+w)*sx)-px),ph=uint32_t(std::lround((y+h)*sy)-py);
  const VkRect2D read{{px,py},{pw,ph}};state.materialize(source,&read);
  if(gpu->info.format==Target::depth_format)state.depth_read(source);
  gpu->copy_from(state.cmd,*source,uint32_t(level),int32_t(std::lround(dx*sx)),int32_t(std::lround(dy*sy)),px,py,pw,ph,0,0,o.layers);
  state.pass_stamp("copy "+std::to_string(pw)+"x"+std::to_string(ph)+" x"+std::to_string(o.layers));
  static const auto copy_log=[] {std::array<unsigned,2> r{};if(const auto *v=std::getenv("LINDBERGH_VULKAN_CLASSIFY")){r[0]=r[1]=unsigned(std::strtoul(v,nullptr,10));if(const auto *end=std::strchr(v,'-'))r[1]=unsigned(std::strtoul(end+1,nullptr,10));}return r;}();
  if(copy_log[1]&&g_frame>=copy_log[0]&&g_frame<=copy_log[1])
    std::fprintf(stderr,"[vulkan-copy] frame %u from target %u %ux%u region %d %d %d %d -> texture %u %dx%d level %d\n",g_frame,state.drawable,source->info.width,source->info.height,x,y,w,h,state.textures[state.active_texture-GL_TEXTURE0],v.width,v.height,level);
}

namespace {
std::array<float,4> value(GLenum name,std::array<float,4> fallback={}) {
  const auto it=state.floats.find(name);return it==state.floats.end()?fallback:it->second;
}
VkBlendFactor blend_factor(GLenum v) {
  switch(v) {
  case GL_ZERO:return VK_BLEND_FACTOR_ZERO;case GL_ONE:return VK_BLEND_FACTOR_ONE;
  case GL_SRC_COLOR:return VK_BLEND_FACTOR_SRC_COLOR;case GL_ONE_MINUS_SRC_COLOR:return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
  case GL_DST_COLOR:return VK_BLEND_FACTOR_DST_COLOR;case GL_ONE_MINUS_DST_COLOR:return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
  case GL_SRC_ALPHA:return VK_BLEND_FACTOR_SRC_ALPHA;case GL_ONE_MINUS_SRC_ALPHA:return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  case GL_DST_ALPHA:return VK_BLEND_FACTOR_DST_ALPHA;case GL_ONE_MINUS_DST_ALPHA:return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
  case GL_CONSTANT_COLOR:return VK_BLEND_FACTOR_CONSTANT_COLOR;case GL_ONE_MINUS_CONSTANT_COLOR:return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
  case GL_CONSTANT_ALPHA:return VK_BLEND_FACTOR_CONSTANT_ALPHA;case GL_ONE_MINUS_CONSTANT_ALPHA:return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
  case GL_SRC_ALPHA_SATURATE:return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;default:gap("unsupported guest blend factor");
  }
}
VkStencilOp stencil_op(GLenum v) {
  switch(v) {case GL_KEEP:return VK_STENCIL_OP_KEEP;case GL_ZERO:return VK_STENCIL_OP_ZERO;case GL_REPLACE:return VK_STENCIL_OP_REPLACE;
  case GL_INCR:return VK_STENCIL_OP_INCREMENT_AND_CLAMP;case GL_DECR:return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
  case GL_INVERT:return VK_STENCIL_OP_INVERT;case GL_INCR_WRAP:return VK_STENCIL_OP_INCREMENT_AND_WRAP;case GL_DECR_WRAP:return VK_STENCIL_OP_DECREMENT_AND_WRAP;
  default:gap("unsupported guest stencil operation");}
}
VkBlendOp blend_op(GLenum v) {
  switch(v) {case GL_FUNC_ADD:return VK_BLEND_OP_ADD;case GL_FUNC_SUBTRACT:return VK_BLEND_OP_SUBTRACT;
  case GL_FUNC_REVERSE_SUBTRACT:return VK_BLEND_OP_REVERSE_SUBTRACT;case GL_MIN:return VK_BLEND_OP_MIN;case GL_MAX:return VK_BLEND_OP_MAX;
  default:gap("unsupported guest blend equation");}
}
// A unit's GL_COMBINE parameter, or GL's initial value.
GLenum env_param(unsigned unit,GLenum name) {
  if(auto it=state.env_params[unit].find(name);it!=state.env_params[unit].end())return GLenum(it->second);
  switch(name) {
  case GL_COMBINE_RGB:case GL_COMBINE_ALPHA:return GL_MODULATE;
  case GL_SOURCE0_RGB:case GL_SOURCE0_ALPHA:return GL_TEXTURE;
  case GL_SOURCE1_RGB:case GL_SOURCE1_ALPHA:return GL_PREVIOUS;
  case GL_SOURCE2_RGB:case GL_SOURCE2_ALPHA:return GL_CONSTANT;
  case GL_OPERAND0_RGB:case GL_OPERAND1_RGB:return GL_SRC_COLOR;
  default:return GL_SRC_ALPHA; // the other operands
  }
}
// fixed.frag's packed GL_COMBINE description; false when the unit reads a
// disabled unit's texture, which GL treats as texture blending disabled.
bool combine_words(unsigned unit,std::array<int32_t,4> &d) {
  auto function=[](GLenum f,bool rgb){
    switch(f){case GL_REPLACE:return 0;case GL_MODULATE:return 1;case GL_ADD:return 2;case GL_ADD_SIGNED:return 3;
    case GL_INTERPOLATE:return 4;case GL_SUBTRACT:return 5;case GL_DOT3_RGB:if(rgb)return 6;break;case GL_DOT3_RGBA:if(rgb)return 7;break;}
    gap("unsupported GL_COMBINE function");};
  bool usable=true;
  auto source=[&](GLenum s){
    switch(s){case GL_TEXTURE:return 0;case GL_CONSTANT:return 1;case GL_PRIMARY_COLOR:return 2;case GL_PREVIOUS:return 3;}
    if(s>=GL_TEXTURE0&&s<GL_TEXTURE0+8){if(!state.unit_textured(s-GL_TEXTURE0))usable=false;return int(8+s-GL_TEXTURE0);}
    gap("unsupported GL_COMBINE source");};
  auto operand=[](GLenum o){if(o<GL_SRC_COLOR||o>GL_ONE_MINUS_SRC_ALPHA)gap("unsupported GL_COMBINE operand");return int(o-GL_SRC_COLOR);};
  auto scale=[](float v){return v==2?2:v==4?4:1;};
  d={};
  d[0]=function(env_param(unit,GL_COMBINE_RGB),true)|function(env_param(unit,GL_COMBINE_ALPHA),false)<<4;
  for(int i=0;i<3;++i) {
    d[1]|=source(env_param(unit,GL_SOURCE0_RGB+i))<<(4*i)|source(env_param(unit,GL_SOURCE0_ALPHA+i))<<(12+4*i);
    d[2]|=operand(env_param(unit,GL_OPERAND0_RGB+i))<<(2*i)|operand(env_param(unit,GL_OPERAND0_ALPHA+i))<<(6+2*i);
  }
  auto stored=[&](GLenum name){auto it=state.env_params[unit].find(name);return it==state.env_params[unit].end()?1.f:it->second;};
  d[3]=scale(stored(GL_RGB_SCALE))|scale(stored(GL_ALPHA_SCALE))<<4;
  return usable;
}
// The classic environment mode (0 GL_MODULATE, 1 GL_REPLACE) a GL_COMBINE
// unit computes exactly, else -1. Those run fixed.frag's one-line paths
// instead of its general combiner. first: no enabled unit before this one, so
// the primary colour is also the previous unit's output. in_range: that
// output lies in [0,1], so the combiner's clamp of a product changes nothing.
int classic_combine(unsigned unit,const std::array<int32_t,4> &d,bool first,bool in_range) {
  if(d[3]!=(1|1<<4))return -1; // RGB and alpha scales 1
  auto texture=[&](int s){return s==0||s==int(8+unit);};
  auto previous=[&](int s){return s==3||(first&&s==2);};
  auto src=[&](int word,int i){return (d[1]>>(word+4*i))&15;};
  auto op=[&](int shift,int i){return (d[2]>>(shift+2*i))&3;};
  const int frgb=d[0]&15,falpha=(d[0]>>4)&15;
  // MODULATE: texture x previous, colours and alphas as they are (either order).
  if(in_range&&frgb==1&&falpha==1&&op(0,0)==0&&op(0,1)==0&&op(6,0)==2&&op(6,1)==2&&
     ((texture(src(0,0))&&previous(src(0,1)))||(previous(src(0,0))&&texture(src(0,1))))&&
     ((texture(src(12,0))&&previous(src(12,1)))||(previous(src(12,0))&&texture(src(12,1)))))return 0;
  // REPLACE: the texture's colour and alpha.
  if(frgb==0&&falpha==0&&op(0,0)==0&&op(6,0)==2&&texture(src(0,0))&&texture(src(12,0)))return 1;
  return -1;
}
RasterState raster_state(bool fixed) {
  auto r=state.raster;
  if(state.enabled[GL_CULL_FACE]) {
    auto face=integer(GL_CULL_FACE_MODE,GL_BACK);r.cull=face==GL_FRONT?VK_CULL_MODE_FRONT_BIT:face==GL_BACK?VK_CULL_MODE_BACK_BIT:VK_CULL_MODE_FRONT_AND_BACK;
  }else r.cull=VK_CULL_MODE_NONE;
  r.clockwise=integer(GL_FRONT_FACE,GL_CCW)==GL_CW;
  r.src_rgb=blend_factor(integer(GL_BLEND_SRC,GL_ONE));r.dst_rgb=blend_factor(integer(GL_BLEND_DST,GL_ZERO));
  r.src_alpha=blend_factor(integer(GL_BLEND_SRC_ALPHA,integer(GL_BLEND_SRC,GL_ONE)));
  r.dst_alpha=blend_factor(integer(GL_BLEND_DST_ALPHA,integer(GL_BLEND_DST,GL_ZERO)));
  r.blend_rgb=blend_op(integer(GL_BLEND_EQUATION_RGB,GL_FUNC_ADD));r.blend_alpha=blend_op(integer(GL_BLEND_EQUATION_ALPHA,GL_FUNC_ADD));
  auto mask=value(GL_COLOR_WRITEMASK,{1,1,1,1});r.color_mask=0;for(unsigned i=0;i<4;++i)if(mask[i])r.color_mask|=1u<<i;
  r.alpha_func=state.enabled[GL_ALPHA_TEST]?uint32_t(integer(GL_ALPHA_TEST_FUNC,GL_ALWAYS)-GL_NEVER+1):0;
  r.stencil=state.enabled[GL_STENCIL_TEST];r.stencil_compare=VkCompareOp(integer(GL_STENCIL_FUNC,GL_ALWAYS)-GL_NEVER);
  r.stencil_fail=stencil_op(integer(GL_STENCIL_FAIL,GL_KEEP));r.stencil_depth_fail=stencil_op(integer(GL_STENCIL_PASS_DEPTH_FAIL,GL_KEEP));r.stencil_pass=stencil_op(integer(GL_STENCIL_PASS_DEPTH_PASS,GL_KEEP));
  r.depth_bias=state.enabled[GL_POLYGON_OFFSET_FILL];
  if(fixed) {
    // The colour each unit starts from lies in [0,1]: fixed-function vertex
    // colours (lit ones are clamped), then each unit's output in turn.
    bool in_range=!state.program&&(state.enabled[GL_LIGHTING]||state.arrays.unit_range(3));
    for(unsigned i=0;i<8;++i) {
      if(!state.unit_textured(i))continue; // a disabled unit's environment has no effect
      const bool first=!r.fixed_texture;
      r.fixed_texture|=1u<<i;
      unsigned mode=0;switch(state.texture_env[i]) {case GL_MODULATE:break;case GL_REPLACE:mode=1;break;case GL_ADD:mode=2;break;case GL_DECAL:mode=3;break;case GL_BLEND:mode=4;break;
      case GL_COMBINE: {
        std::array<int32_t,4> d;if(!combine_words(i,d)){r.fixed_texture&=~(1u<<i);continue;}
        const int classic=classic_combine(i,d,first,in_range);mode=classic<0?5:unsigned(classic);break;
      }
      default:gap("unsupported fixed-function texture environment mode");}
      r.fixed_env|=mode<<(3*i);
      in_range=mode==1||mode==5||(mode!=2&&in_range); // REPLACE and GL_COMBINE end in range; GL_ADD may not
    }
    if(state.enabled[GL_FOG]) {auto mode=GLenum(value(GL_FOG_MODE,{GL_EXP,0,0,0})[0]);r.fog_mode=mode==GL_LINEAR?1:mode==GL_EXP?2:3;}
  }
  return r;
}
Program &current_program() {
  if(state.program)return state.programs.at(state.program);
  if(!state.fixed_program.record) {
    const auto *fixed=std::getenv("LINDBERGH_VULKAN_FIXED_SHADERS");
    if(!state.fixed_pack)state.fixed_pack=std::make_unique<ShaderPack>(fixed && *fixed ? fixed : LINDBERGH_VULKAN_FIXED_ARCHIVE);
    auto &p=state.fixed_program;p.record=&state.fixed_pack->programs().at(0);
    for(const auto &u:p.record->uniforms)p.uniforms.push_back(std::make_unique<UniformData>(u));
    for(const auto &s:p.record->samplers) {
      const auto unit=std::strtoul(s.name.c_str()+std::strlen("fixed_texture"),nullptr,10);p.sampler_units[int(s.binding)]=GLint(unit);
    }
  }
  return state.fixed_program;
}
void legacy_uniforms(Program &p,Target &target) {
  if(p.native->legacy_version==state.legacy_version&&p.native->legacy_height==target.info.height)return;
  const auto &mv=state.matrices.matrix(GL_MODELVIEW),&projection=state.matrices.matrix(GL_PROJECTION);
  float normal[9]{};
  const float a=mv[0],b=mv[4],c=mv[8],d=mv[1],e=mv[5],f=mv[9],g=mv[2],h=mv[6],i=mv[10];
  const float det=a*(e*i-f*h)-b*(d*i-f*g)+c*(d*h-e*g);
  if(det) {float cofactors[]={e*i-f*h,c*h-b*i,b*f-c*e,f*g-d*i,a*i-c*g,c*d-a*f,d*h-e*g,b*g-a*h,a*e-b*d};for(unsigned j=0;j<9;++j)normal[j]=cofactors[j]/det;}
  for(size_t block=0;block<p.record->uniforms.size();++block) {
    const auto &layout=p.record->uniforms[block];if(layout.set!=0)continue;
    for(uint32_t field=0;field<layout.fields.size();++field) {
      const auto &schema=layout.fields[field];const auto &name=schema.name;
      std::array<float,4> v{};
      auto write=[&](const float *data,size_t size,uint32_t count=1){p.uniforms[block]->write(field,data,size,0,count);};
      if(name=="modelview") {write(mv.data(),64);continue;}
      if(name=="projection") {write(projection.data(),64);continue;}
      if(name=="normal_matrix") {write(normal,sizeof normal);continue;}
      if(name=="texture_matrix") {float matrices[8][16];for(unsigned t=0;t<8;++t)std::copy(state.matrices.matrix(GL_TEXTURE,t).begin(),state.matrices.matrix(GL_TEXTURE,t).end(),matrices[t]);write(&matrices[0][0],schema.count*64,schema.count);continue;}
      if(name=="normal_scale") {const auto length=std::sqrt(normal[2]*normal[2]+normal[5]*normal[5]+normal[8]*normal[8]);v[0]=length?1/length:1;}
      else if(name=="viewport_height")v[0]=float(target.info.height);
      else if(name=="alpha_ref")v=value(GL_ALPHA_TEST_REF);
      else if(name=="fog.color")v=value(GL_FOG_COLOR);
      else if(name=="fog.start")v=value(GL_FOG_START);
      else if(name=="fog.end")v=value(GL_FOG_END,{1,0,0,0});
      else if(name=="fog.density")v=value(GL_FOG_DENSITY,{1,0,0,0});
      else if(name=="fog.scale") {auto range=value(GL_FOG_END,{1,0,0,0})[0]-value(GL_FOG_START)[0];v[0]=range?1/range:0;}
      else if(name=="depth_range.near")v[0]=value(GL_DEPTH_RANGE,{0,1,0,0})[0];
      else if(name=="depth_range.far")v[0]=value(GL_DEPTH_RANGE,{0,1,0,0})[1];
      else if(name=="depth_range.diff") {auto range=value(GL_DEPTH_RANGE,{0,1,0,0});v[0]=range[1]-range[0];}
      else if(name=="front_light_model.sceneColor") {
        auto emission=value((GL_FRONT<<16)|GL_EMISSION,{0,0,0,1}),ambient=value((GL_FRONT<<16)|GL_AMBIENT,{.2f,.2f,.2f,1});
        auto global=value(GL_LIGHT_MODEL_AMBIENT,{.2f,.2f,.2f,1});for(unsigned j=0;j<3;++j)v[j]=emission[j]+ambient[j]*global[j];v[3]=value((GL_FRONT<<16)|GL_DIFFUSE,{.8f,.8f,.8f,1})[3];
      } else if(name.rfind("front_material.",0)==0) {
        const auto leaf=name.substr(15);
        if(leaf=="emission")v=value((GL_FRONT<<16)|GL_EMISSION,{0,0,0,1});
        else if(leaf=="ambient")v=value((GL_FRONT<<16)|GL_AMBIENT,{.2f,.2f,.2f,1});
        else if(leaf=="diffuse")v=value((GL_FRONT<<16)|GL_DIFFUSE,{.8f,.8f,.8f,1});
        else if(leaf=="specular")v=value((GL_FRONT<<16)|GL_SPECULAR,{0,0,0,1});
        else if(leaf=="shininess")v=value((GL_FRONT<<16)|GL_SHININESS);
        else gap("unknown legacy material uniform");
      } else if(name.rfind("light_source[",0)==0) {
        const auto at=name.find(']');const unsigned light=unsigned(std::strtoul(name.c_str()+13,nullptr,10));const auto leaf=name.substr(at+2);
        const GLenum key=(GL_LIGHT0+light)<<16;
        if(leaf=="ambient")v=value(key|GL_AMBIENT,{0,0,0,1});
        else if(leaf=="diffuse")v=value(key|GL_DIFFUSE,light?std::array<float,4>{0,0,0,1}:std::array<float,4>{1,1,1,1});
        else if(leaf=="specular")v=value(key|GL_SPECULAR,light?std::array<float,4>{0,0,0,1}:std::array<float,4>{1,1,1,1});
        else if(leaf=="position")v=value(key|GL_POSITION,{0,0,1,0});
        else if(leaf=="spotDirection")v=value(key|GL_SPOT_DIRECTION,{0,0,-1,0});
        else if(leaf=="spotExponent")v=value(key|GL_SPOT_EXPONENT);
        else if(leaf=="spotCutoff")v=value(key|GL_SPOT_CUTOFF,{180,0,0,0});
        else if(leaf=="spotCosCutoff")v[0]=std::cos(value(key|GL_SPOT_CUTOFF,{180,0,0,0})[0]*3.14159265358979323846f/180);
        else if(leaf=="constantAttenuation")v=value(key|GL_CONSTANT_ATTENUATION,{1,0,0,0});
        else if(leaf=="linearAttenuation")v=value(key|GL_LINEAR_ATTENUATION);
        else if(leaf=="quadraticAttenuation")v=value(key|GL_QUADRATIC_ATTENUATION);
        else if(leaf=="halfVector") {v=value(key|GL_POSITION,{0,0,1,0});float n=std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);if(n)for(unsigned j=0;j<3;++j)v[j]/=n;v[2]+=1;n=std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);if(n)for(unsigned j=0;j<3;++j)v[j]/=n;v[3]=0;}
        else gap("unknown legacy light uniform");
      } else gap("unknown legacy uniform");
      write(v.data(),schema.rows*schema.columns*4);
    }
  }
  p.native->legacy_version=state.legacy_version;p.native->legacy_height=target.info.height;
}
void native_program(Program &p) {
  if(p.native)return;
  p.native=std::make_shared<NativeProgram>();
  for(unsigned set=0;set<2;++set) {
    std::vector<UniformBlock> blocks;for(const auto &u:p.record->uniforms)if(u.set==set)blocks.push_back({u.binding,u.bytes});
    if(blocks.size()>1)gap("multiple UBOs per guest descriptor set not implemented");
    p.native->uniform_sets[set]=std::make_unique<UniformSet>(*state.device,*state.ring,blocks);
    p.native->layouts.push_back(p.native->uniform_sets[set]->layout());
  }
  p.native->empty_images=std::make_unique<SampledSet>(*state.device,std::vector<SampledImage>{});
  p.native->layouts.push_back(p.native->empty_images->layout());
}
std::shared_ptr<Material> material(Program &p) {
  std::vector<std::shared_ptr<Texture>> images;std::vector<std::shared_ptr<Sampler>> samplers;std::vector<SampledImage> bindings;
  std::string key(reinterpret_cast<const char *>(p.record->key.data()),p.record->key.size());
  for(const auto &s:p.record->samplers) {
    if(s.count!=1)gap("sampler arrays need descriptor-array binding");
    const auto unit=p.sampler_units[int(s.binding)];if(unit<0||unit>=8)gap("guest sampler texture unit outside supported range");
    // arbprog.c names its rectangle samplers sr_<unit>: those read the unit's rectangle binding.
    const bool cube=s.type.find("Cube")!=std::string::npos;
    // fixed_texture<N>: a fixed-function unit, whose rectangle target wins over 2D when enabled.
    const bool rect=s.name.rfind("sr_",0)==0||(s.name.rfind("fixed_texture",0)==0&&state.rect_enabled[unsigned(unit)]);
    const GLuint id=(cube?state.cube_textures:rect?state.rect_textures:state.textures)[unsigned(unit)];
    resolve_texture(id);
    auto &object=state.texture_objects[id];
    auto image=object.alias?object.alias:native_image(object);
    if(cube&&image==state.fallback) {
      if(!state.cube_fallback) {
        TextureInfo info{1,1,1,6};info.cube=true;state.cube_fallback=std::make_shared<Texture>(*state.device,info);
        const uint8_t black[]={0,0,0,255};for(uint32_t face=0;face<6;++face)state.cube_fallback->upload(state.cmd,*state.ring,0,face,black,4);
      }
      image=state.cube_fallback;
    }
    auto sampler=native_sampler(object,image->info.levels,s.type.find("Shadow")!=std::string::npos);
    const bool array=s.type.find("2DArray")!=std::string::npos;
    if(cube&&!image->info.cube)gap("cube sampler bound to a 2D texture");
    if(image->info.layers>1 && !array && !cube)gap("stereo texture requires a rebuilt offline shader archive with array samplers");
    const auto view=array&&!image->info.cube?image->array_view():image->view();
    bindings.push_back({s.binding,view,sampler->handle()});key_word(key,(uint64_t)(uintptr_t)view);key_word(key,(uint64_t)(uintptr_t)sampler->handle());
    images.push_back(image);samplers.push_back(sampler);
  }
  auto &entry=state.materials[state.ring->recording_slot()][key];
  if(!entry) {entry=std::make_shared<Material>();entry->descriptors=std::make_shared<SampledSet>(*state.device,bindings);entry->images=std::move(images);entry->samplers=std::move(samplers);}
  state.ring->keep_alive(entry);return entry;
}
bool stereo_bounds(const Matrix &p,GLint first,GLsizei count,const std::vector<uint32_t> &elements,
                   const void *immediate,uint32_t stride,bool guest_indexed,std::array<float,4> &box)
{
  if(count<=0 || count>64 || (!immediate&&guest_indexed&&state.element_buffer))return false;
  box={1e9f,1e9f,-1e9f,-1e9f};
  for(uint32_t i=0;i<uint32_t(count);++i) {
    const auto index=uint32_t(first)+(elements.empty()?i:elements.at(i));
    std::array<float,4> v;
    if(immediate)std::memcpy(v.data(),static_cast<const uint8_t *>(immediate)+size_t(index)*stride,16);
    else if(!state.legacy_bounds_arrays.position(index,{g_guest_base,uint64_t(1)<<32},[](uint32_t id){
      const auto &b=state.buffers.at(id);return GuestMemoryView{b.bytes.data(),b.bytes.size()};},v))return false;
    float xy[2];if(!lg_stereo_project_xy(p.data(),v.data(),xy))return false;
    box[0]=std::min(box[0],xy[0]);box[1]=std::min(box[1],xy[1]);box[2]=std::max(box[2],xy[0]);box[3]=std::max(box[3],xy[1]);
  }
  return true;
}
// A full-frame copied picture already contains each eye projection. Its
// fullscreen composition keeps guest clip space, including its depth, instead
// of projecting that rasterized image a second time as world geometry.
bool picture_composite(GLint first,GLsizei count,const std::vector<uint32_t> &elements,
                       const void *immediate,uint32_t stride)
{
  if(count<3 || count>6)return false;
  const auto m=state.matrices.modelview_projection();
  std::array<float,4> box{1e9f,1e9f,-1e9f,-1e9f};
  for(uint32_t i=0;i<uint32_t(count);++i) {
    const auto index=uint32_t(first)+(elements.empty()?i:elements.at(i));
    std::array<float,4> v;
    if(immediate)std::memcpy(v.data(),static_cast<const uint8_t *>(immediate)+size_t(index)*stride,16);
    else if(!state.arrays.position(index,{g_guest_base,uint64_t(1)<<32},[](uint32_t id){
      const auto &b=state.buffers.at(id);return GuestMemoryView{b.bytes.data(),b.bytes.size()};},v))return false;
    float xy[2];if(!lg_stereo_project_xy(m.data(),v.data(),xy))return false;
    box[0]=std::min(box[0],xy[0]);box[1]=std::min(box[1],xy[1]);box[2]=std::max(box[2],xy[0]);box[3]=std::max(box[3],xy[1]);
  }
  return box[0]<=-.98f&&box[1]<=-.98f&&box[2]>=.98f&&box[3]>=.98f;
}
bool stereo_packet(DrawPacket &packet,GLint first,GLsizei count,const std::vector<uint32_t> &elements,
                   const void *immediate,uint32_t stride,bool guest_indexed)
{
  if(!state.stereo) {
    for(auto &m:packet.matrices.view_projection)for(unsigned i=0;i<4;++i)m[i*5]=1;
    return false;
  }
  const auto &p=state.matrices.matrix(GL_PROJECTION);
  LgStereoDrawFacts facts{};facts.perspective=state.stereo->world_camera(p);
  const auto size=state.logical_size();
  facts.offscreen=state.drawable && !state.eye_pbuffer(size[0],size[1]);
  facts.hud_target=state.drawable==state.hud_target;facts.multiview=1;
  facts.zoom=state.stereo->zoomed(p);
  facts.menu=facts.perspective&&menu_state();
  const auto texture=state.texture_objects.find(!state.program&&state.rect_enabled[0]?state.rect_textures[0]:state.textures[0]);
  if(texture!=state.texture_objects.end())facts.texture_view=texture->second.picture_view;
  const auto *profile=game_profile();
  facts.picture_target=profile->vr.world_w>0 && size[0]==profile->vr.world_w && size[1]==profile->vr.world_h;
  facts.covers_viewport=1;
  std::array<float,4> box{};bool bounded=false;
  if(!facts.perspective) {
    bounded=stereo_bounds(p,first,count,elements,immediate,stride,guest_indexed,box);
    if(bounded) {
      facts.covers_viewport=box[0]<=-.95f&&box[1]<=-.95f&&box[2]>=.95f&&box[3]>=.95f;
      facts.fills_screen=box[0]<=-.98f&&box[1]<=-.98f&&box[2]>=.98f&&box[3]>=.98f;
    }
  }
  bool composite=false;
  if(facts.texture_view && texture->second.gpu_target && !facts.offscreen) {
    const auto &base=texture->second.levels.at(0);
    composite=state.picture_size(base.width,base.height)&&picture_composite(first,count,elements,immediate,stride);
  }
  int kind=composite?LG_STEREO_PASS:lg_stereo_classify_draw(&facts);
  // Full-screen 2D into a framebuffer object with no world drawn yet this
  // frame is a 2D screen (GSE's menus and briefings: a frame layer with
  // windows over its panels): it goes on the HUD's screen with the rest of
  // the 2D. Over a world it stays a full-eye fill (a fade, a flash).
  if(kind==LG_STEREO_FILL&&!facts.perspective&&!state.world_draws&&state.bound_framebuffer)kind=LG_STEREO_HUD;
  const bool raw_perspective=lg_stereo_is_perspective(p.data());
  const bool movie=texture!=state.texture_objects.end()&&texture->second.movie;
  // Movie/HUD layers hide their black backing with depth, so they take the
  // HUD transform (Z/W and XY): the 4:3 picture sits on the virtual screen at
  // the same size as the HUD drawn over it. Stretching it across the whole eye
  // magnified it about twice.
  const bool full_hud=kind==LG_STEREO_FILL&&raw_perspective&&(!state.world_draws||movie);
  packet.matrices=state.stereo->transforms(p,full_hud?LG_STEREO_HUD:kind);
  state.stereo->observe(p,kind);
  if(kind==LG_STEREO_WORLD && facts.perspective)++state.world_draws;
  if(kind==LG_STEREO_HUD||full_hud) {
    if(!raw_perspective)state.hud_target_next=state.drawable;
    if(!raw_perspective&&bounded&&state.controllers) {
      float gun[2][2]={{-100,-100},{-100,-100}};const auto &aim=state.controllers->aims();const auto &settings=state.controllers->settings();
      for(int pl=0;pl<2;++pl)if(aim.valid[pl]&&aim.ray[pl].x>=0&&aim.ray[pl].y>=0) {
        gun[pl][0]=2.f*aim.ray[pl].x/settings.width-1;gun[pl][1]=1-2.f*aim.ray[pl].y/settings.height;
      }
      const unsigned primary=state.sight.primary,secondary=state.sight.secondary;
      const bool hide=lg_xr_sight_draw(&state.sight,g_frame,state.textures[0],state.texture_enabled[0],box.data(),gun[0],gun[1],state.settings.crosshair);
      if(primary!=state.sight.primary||secondary!=state.sight.secondary)
        std::fprintf(stderr,"[vulkan-sight] frame %u learned primary %u secondary %u\n",g_frame,state.sight.primary,state.sight.secondary);
      if(hide)return true;
    }
    if(!raw_perspective || (state.world_draws && !movie))packet.raster.depth_write=false;
    if(!facts.perspective && raw_perspective && (!state.world_draws || movie)) {
      packet.raster.depth_bias=true;packet.depth_bias_factor=0;packet.depth_bias_units=-4.f*++state.hud_sequence;
    }
  }
  static const auto log_range=[] {std::array<unsigned,2> r{};if(const auto *v=std::getenv("LINDBERGH_VULKAN_CLASSIFY")){r[0]=r[1]=unsigned(std::strtoul(v,nullptr,10));if(const auto *end=std::strchr(v,'-'))r[1]=unsigned(std::strtoul(end+1,nullptr,10));}return r;}();
  if(log_range[1] && g_frame>=log_range[0] && g_frame<=log_range[1] && (state.world_draws==1 || facts.texture_view || kind==LG_STEREO_FILL)) {
    std::fprintf(stderr,"[vulkan-camera] P");for(float v:p)std::fprintf(stderr," %.6g",v);
    std::fprintf(stderr," reprojection");for(float v:packet.matrices.view_projection[0])std::fprintf(stderr," %.6g",v);std::fputc(10,stderr);
    std::fprintf(stderr,"[vulkan-camera] modelview");for(float v:state.matrices.matrix(GL_MODELVIEW))std::fprintf(stderr," %.6g",v);
    std::fprintf(stderr,"; depth test %d write %d func %d\n",packet.raster.depth_test,packet.raster.depth_write,int(packet.raster.depth_compare));
    if(kind==LG_STEREO_FILL) {
      std::fprintf(stderr,"[vulkan-fill] frame %u program %u texture %u movie %d full-hud %d; depth bias %d %.3f %.3f blend %d src %u dst %u color-mask %u; bounds %d %.4f %.4f %.4f %.4f\n",
        g_frame,state.program,state.textures[0],texture!=state.texture_objects.end()&&texture->second.movie,full_hud,
        packet.raster.depth_bias,packet.depth_bias_factor,packet.depth_bias_units,packet.raster.blend,unsigned(packet.raster.src_rgb),unsigned(packet.raster.dst_rgb),packet.raster.color_mask,
        bounded,box[0],box[1],box[2],box[3]);
      auto &program=current_program();
      for(const auto &sampler:program.record->samplers) {
        const int unit=program.sampler_units[int(sampler.binding)];
        if(unit<0||unit>=8)continue;
        auto image=state.texture_objects.find(state.textures[unit]);
        if(image==state.texture_objects.end()||!image->second.levels.count(0))continue;
        const auto &level=image->second.levels.at(0);const auto &bytes=level.bytes;
        const auto range=std::minmax_element(bytes.begin(),bytes.end());uint64_t sum=0;for(auto value:bytes)sum+=value;
        std::fprintf(stderr,"[vulkan-fill-image] sampler %s binding %u unit %d texture %u %dx%d format %#x type %#x movie %d gpu-target %d; CPU shadow %zu bytes min %u max %u mean %.3f\n",
          sampler.name.c_str(),sampler.binding,unit,state.textures[unit],level.width,level.height,level.format,level.type,image->second.movie,image->second.gpu_target,
          bytes.size(),bytes.empty()?0u:unsigned(*range.first),bytes.empty()?0u:unsigned(*range.second),bytes.empty()?0:double(sum)/bytes.size());
      }
      if(const auto *directory=std::getenv("LINDBERGH_VULKAN_CLASSIFY_DIR");directory&&*directory) {
        static std::set<std::pair<GLuint,GLuint>> dumped;
        for(auto id:program.shaders)if(dumped.emplace(state.program,id).second) {
          const auto path=std::filesystem::path(directory)/("program-"+std::to_string(state.program)+"-shader-"+std::to_string(id)+".glsl");
          std::ofstream file(path);file<<state.shaders.at(id).source;
          if(!file)std::fprintf(stderr,"[vulkan-fill] cannot write diagnostic shader %s\n",path.string().c_str());
        }
      }
    }
  }
  if(log_range[1] && g_frame>=log_range[0] && g_frame<=log_range[1])
    std::fprintf(stderr,"[vulkan-class] frame %u target %u %dx%d class %d lens %.4f %.4f texture-view %d offscreen %d; cache builds %zu; program %u texture %u gpu-target %d world draws %u; viewport %d %d %d %d\n",g_frame,state.drawable,size[0],size[1],kind,p[0],p[5],facts.texture_view,facts.offscreen,state.stereo->transform_builds(),state.program,state.textures[0],texture!=state.texture_objects.end()&&texture->second.gpu_target,state.world_draws,state.viewport[0],state.viewport[1],state.viewport[2],state.viewport[3]);
  return false;
}
void draw_geometry(GLenum mode,GLint first,GLsizei count,const void *indices,GLenum index_type,bool indexed,
                   const void *immediate=nullptr,uint32_t immediate_stride=0) {
  if(count<=0)return;if(first<0)gap("negative guest first vertex");
  if(state.program&&!state.programs.at(state.program).record)return; // harvesting or missing from the pack
  // LINDBERGH_VULKAN_DEBUG_DRAWS=drawable:k keeps the first k draws into that
  // drawable each frame (bisecting a diagnostic capture); the rest are dropped.
  static const auto debug_draws=[]{std::array<unsigned,2> r{~0u,~0u};if(const auto *v=std::getenv("LINDBERGH_VULKAN_DEBUG_DRAWS"))std::sscanf(v,"%u:%u",&r[0],&r[1]);return r;}();
  if(debug_draws[0]==state.drawable) {
    static unsigned frame=~0u,n=0;if(frame!=g_frame){frame=g_frame;n=0;}
    if(n++>=debug_draws[1])return;
    if(g_frame==unsigned(std::atoi(std::getenv("LINDBERGH_SHOT_FRAME")?std::getenv("LINDBERGH_SHOT_FRAME"):"0")))
      std::fprintf(stderr,"[vulkan-debug-draw] %u: program %u textures %u %u %u %u fixed %d mode %#x count %d\n",n,state.program,state.textures[0],state.textures[1],state.textures[2],state.textures[3],!state.program,mode,count);
  }
  {static unsigned dn;if(std::getenv("LINDBERGH_DIAG_GLOWZ")&&state.drawable==2&&!state.program&&mode==GL_QUADS&&count==4&&state.rect_textures[0]&&g_frame>=1200&&dn++<3) { // DIAGNOSTIC ONLY
    std::fprintf(stderr,"[diag-glowz] frame %u depth test %d write %d compare %d blend %d immediate %d\n",g_frame,state.raster.depth_test,state.raster.depth_write,int(state.raster.depth_compare),int(state.raster.blend),immediate!=nullptr);
    for(int i=0;i<4;++i){std::array<float,4> v{};bool ok=true;if(immediate)std::memcpy(v.data(),static_cast<const uint8_t *>(immediate)+size_t(first+i)*immediate_stride,16);
      else ok=state.arrays.position(uint32_t(first+i),{g_guest_base,uint64_t(1)<<32},[](uint32_t id){const auto &b=state.buffers.at(id);return GuestMemoryView{b.bytes.data(),b.bytes.size()};},v);
      std::fprintf(stderr,"  v%d ok %d %.4f %.4f %.6f %.4f\n",i,ok,v[0],v[1],v[2],v[3]);}}}
  {static unsigned sn;if(std::getenv("LINDBERGH_DIAG_GLOWZ")&&state.drawable==0&&mode==GL_QUADS&&count==4&&g_frame>=1200&&sn++<2) { // DIAGNOSTIC ONLY
    const auto c=state.arrays.current_value(3);
    std::fprintf(stderr,"[diag-screen] frame %u program %u rect %u tex %u env %#x unit1 rect %u tex %u env %#x color %.3f %.3f %.3f %.3f blend %d depth %d/%d stencil %d textured0 %d textured1 %d\n",g_frame,state.program,state.rect_textures[0],state.textures[0],state.texture_env[0],state.rect_textures[1],state.textures[1],state.texture_env[1],c[0],c[1],c[2],c[3],int(state.raster.blend),state.raster.depth_test,state.raster.depth_write,int(state.raster.stencil),int(state.unit_textured(0)),int(state.unit_textured(1)));
    for(auto &e:state.env_params[0])std::fprintf(stderr,"   env0 %#x=%g\n",e.first,e.second);
    for(int i=0;i<4;++i){std::array<float,4> v{};bool ok=true;if(immediate)std::memcpy(v.data(),static_cast<const uint8_t *>(immediate)+size_t(first+i)*immediate_stride,16);
      else ok=state.arrays.position(uint32_t(first+i),{g_guest_base,uint64_t(1)<<32},[](uint32_t id){const auto &b=state.buffers.at(id);return GuestMemoryView{b.bytes.data(),b.bytes.size()};},v);
      std::fprintf(stderr,"  v%d ok %d %.4f %.4f %.6f %.4f\n",i,ok,v[0],v[1],v[2],v[3]);}}}
  const bool guest_indexed=indexed;mark_drawn();
  auto *target=state.target();auto &p=current_program();native_program(p);legacy_uniforms(p,*target);
  const bool fixed=!state.program||p.fixed_fragment; // fixed-function fragment state applies
  if(fixed)for(size_t block=0;block<p.record->uniforms.size();++block) {
    auto field=p.uniforms[block]->find("fixed_env_color");if(field>=0)p.uniforms[block]->write(uint32_t(field),state.env_color.data(),sizeof state.env_color,0,8);
    if(const auto lighting=p.uniforms[block]->find("fixed_lighting");lighting>=0) {
      std::array<int32_t,4> l{};
      if(state.enabled[GL_LIGHTING]) {
        for(unsigned i=0;i<8;++i)if(state.enabled[GL_LIGHT0+i])l[0]|=1<<i;
        if(state.enabled[GL_COLOR_MATERIAL])switch(integer(GL_COLOR_MATERIAL_PARAMETER,GL_AMBIENT_AND_DIFFUSE)) {
          case GL_AMBIENT:l[1]=1;break;case GL_DIFFUSE:l[1]=2;break;case GL_AMBIENT_AND_DIFFUSE:l[1]=3;break;
          case GL_SPECULAR:l[1]=4;break;case GL_EMISSION:l[1]=5;break;}
        l[2]=(integer(GL_LIGHT_MODEL_LOCAL_VIEWER,0)!=0)|(integer(GL_LIGHT_MODEL_TWO_SIDE,0)!=0)<<1;l[3]=state.enabled[GL_NORMALIZE]||state.enabled[GL_RESCALE_NORMAL];
      }
      p.uniforms[block]->write(uint32_t(lighting),l.data(),sizeof l,0,1);
    }
    if(const auto ambient=p.uniforms[block]->find("fixed_light_model_ambient");ambient>=0) {
      const auto a=value(GL_LIGHT_MODEL_AMBIENT,{.2f,.2f,.2f,1});p.uniforms[block]->write(uint32_t(ambient),a.data(),sizeof a,0,1);
    }
    if(const auto rect=p.uniforms[block]->find("fixed_rect");rect>=0) {
      // A rectangle unit's texel coordinates, normalized by its logical size
      // (a render target drawn larger keeps the same normalized lookup).
      std::array<std::array<float,4>,8> scale{};
      for(unsigned i=0;i<8;++i) {
        scale[i]={1,1,0,0};
        if(!state.rect_enabled[i])continue;
        auto it=state.texture_objects.find(state.rect_textures[i]);
        if(it!=state.texture_objects.end()&&it->second.levels.count(0)&&it->second.levels.at(0).width>0&&it->second.levels.at(0).height>0)
          scale[i]={1.f/it->second.levels.at(0).width,1.f/it->second.levels.at(0).height,0,0};
      }
      p.uniforms[block]->write(uint32_t(rect),scale.data(),sizeof scale,0,8);
    }
    field=p.uniforms[block]->find("fixed_combine");
    if(field>=0) {
      std::array<std::array<int32_t,4>,8> words{};
      for(unsigned i=0;i<8;++i)if(state.unit_textured(i)&&state.texture_env[i]==GL_COMBINE)combine_words(i,words[i]);
      p.uniforms[block]->write(uint32_t(field),words.data(),sizeof words,0,8);
    }
  }
  auto images=material(p);p.native->layouts[2]=images->descriptors->layout();
  std::vector<uint32_t> locations;for(const auto &input:p.record->inputs)locations.push_back(input.location);
  uint32_t vertices=uint32_t(count);std::vector<uint32_t> elements;
  bool direct_indices=false;VkDeviceSize index_offset=0;
  if(indexed) {
    const size_t bytes=index_type==GL_UNSIGNED_BYTE?1:index_type==GL_UNSIGNED_SHORT?2:index_type==GL_UNSIGNED_INT?4:0;
    if(!bytes)gap("unsupported guest index type");
    const uint8_t *data=static_cast<const uint8_t *>(indices);
    if(state.element_buffer) {
      auto &b=state.buffers.at(state.element_buffer);const auto offset=(uintptr_t)indices;
      check_range(b.bytes.size(),GLintptr(offset),GLsizeiptr(size_t(count)*bytes));data=b.bytes.data()+offset;
      // Rewritten/streaming EBOs keep the efficient frame-upload path instead
      // of allocating a new device-local buffer for each same-frame revision.
      direct_indices=index_reuse_enabled()&&!b.dynamic&&b.usage==GL_STATIC_DRAW&&
          GuestIndexRanges::native(mode,index_type,offset);index_offset=offset;
      if(direct_indices) {
        vertices=b.index_ranges.vertices(b.bytes.data(),b.bytes.size(),b.revision,offset,uint32_t(count),index_type);
        // The bounded HUD/composite classifiers still inspect small CPU lists.
        if(count<=64)GuestIndexRanges::decode(data,uint32_t(count),index_type,elements);
      }
    }
    if(!direct_indices) {
      elements.resize(size_t(count));vertices=0;
      for(size_t i=0;i<size_t(count);++i) {uint32_t v=0;std::memcpy(&v,data+i*bytes,bytes);elements[i]=v;if(v==UINT32_MAX)gap("guest index range overflow");vertices=std::max(vertices,v+1);}
    }
  }
  // Mesa KosmicKrisp (Mac) cannot build a layered Metal pipeline (our shaders
  // select the eye layer) for points or for the fans it emulates
  // ("inputPrimitiveTopology is not specified"). There, fans become triangle
  // lists and points are skipped; other drivers (Quest) draw both natively.
  static const bool metal_layers=std::strncmp(state.device->capabilities.properties.deviceName,"Apple",5)==0;
  if(metal_layers&&mode==GL_TRIANGLE_FAN)mode=GL_POLYGON; // the same triangles
  auto raster=raster_state(fixed);raster.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  if(target->info.transient_depth)raster.depth_test=raster.depth_write=raster.stencil=false; // no depth buffer in GL
  if(mode==GL_TRIANGLE_STRIP)raster.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  else if(mode==GL_TRIANGLE_FAN)raster.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
  else if(mode==GL_POINTS) {
    if(metal_layers) {
      static bool warned=false;
      if(!warned){warned=true;std::fprintf(stderr,"[vulkan] point draws skipped on this Apple GPU driver (KosmicKrisp layered points); particles missing in Mac runs only\n");}
      return;
    }
    raster.topology=VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
  }
  else if(mode==GL_LINES) {raster.topology=VK_PRIMITIVE_TOPOLOGY_LINE_LIST;count-=count%2;}
  else if(mode==GL_LINE_STRIP)raster.topology=VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
  else if(mode==GL_TRIANGLES)count-=count%3;
  else if(mode==GL_QUADS||mode==GL_QUAD_STRIP||mode==GL_POLYGON||mode==GL_LINE_LOOP) {
    auto original=std::move(elements);auto at=[&](uint32_t i){return original.empty()?i:original[i];};
    auto tri=[&](uint32_t a,uint32_t b,uint32_t c){elements.push_back(at(a));elements.push_back(at(b));elements.push_back(at(c));};
    if(mode==GL_QUADS)for(uint32_t i=0;i+3<uint32_t(count);i+=4){tri(i,i+1,i+2);tri(i,i+2,i+3);}
    else if(mode==GL_QUAD_STRIP)for(uint32_t i=0;i+3<uint32_t(count);i+=2){tri(i,i+1,i+3);tri(i,i+3,i+2);}
    else if(mode==GL_POLYGON)for(uint32_t i=2;i<uint32_t(count);++i)tri(0,i-1,i);
    else {raster.topology=VK_PRIMITIVE_TOPOLOGY_LINE_LIST;for(uint32_t i=0;i<uint32_t(count);++i){elements.push_back(at(i));elements.push_back(at((i+1)%uint32_t(count)));}}
    count=GLsizei(elements.size());indexed=true;
  } else gap("unsupported guest primitive topology");
  if(!count)return;
  ArrayPlan plan;
  if(immediate) {
    auto stream=state.ring->upload(immediate,size_t(vertices)*immediate_stride);
    for(uint32_t i=0;i<locations.size();++i) {plan.bindings.push_back({i,immediate_stride,VK_VERTEX_INPUT_RATE_VERTEX});plan.attributes.push_back({locations[i],i,VK_FORMAT_R32G32B32A32_SFLOAT,locations[i]*16});plan.streams.push_back(stream);}
  } else {
    state.arrays.inputs(locations);
    const auto &prepared=state.arrays.prepare(*state.device,*state.ring,uint32_t(first),vertices,{g_guest_base,uint64_t(1)<<32},
      [&](uint32_t id){auto gpu=native_buffer(id);return GuestBufferView{gpu->handle(),state.buffers.at(id).bytes.size()};});
    plan=prepared;
  }
  std::string key;key_word(key,(uint64_t)(uintptr_t)target);
  for(const auto &b:plan.bindings){key_word(key,b.binding);key_word(key,b.stride);}
  for(const auto &a:plan.attributes){key_word(key,a.location);key_word(key,a.binding);key_word(key,a.format);key_word(key,a.offset);}
  auto &renderer=p.native->renderers[key];
  if(!renderer) {
    if(state.recipes)renderer=state.recipes->renderer(*target,*p.record,plan.bindings,plan.attributes);
    else {
      ProgramSource source{p.record->vertex.data(),p.record->fragment.data(),p.record->vertex.size()*4,p.record->fragment.size()*4,plan.bindings,plan.attributes,p.native->layouts};
      renderer=std::make_shared<Renderer>(*state.device,*target,*state.cache,source);
    }
  }
  auto packet=packet_for(target,renderer.get());packet.raster=raster;packet.streams=std::move(plan.streams);packet.count=uint32_t(count);
  if(stereo_packet(packet,first,count,elements,immediate,immediate_stride,guest_indexed))return;
  if(direct_indices) {
    auto gpu=native_buffer(state.element_buffer,true);
    packet.index={gpu->handle(),index_offset,index_type==GL_UNSIGNED_SHORT?VK_INDEX_TYPE_UINT16:VK_INDEX_TYPE_UINT32};
    if(state.index_stats)++state.direct_index_draws;
  } else if(indexed) {
    auto upload=state.ring->upload(elements.data(),size_t(count)*4,4);packet.index={upload.buffer,upload.offset,VK_INDEX_TYPE_UINT32};
    if(state.index_stats){++state.uploaded_index_draws;state.uploaded_index_bytes+=size_t(count)*4;}
  }
  for(size_t block=0;block<p.record->uniforms.size();++block) {
    const auto set=p.record->uniforms[block].set;if(set>=2)gap("unexpected uniform descriptor set");
    const auto version=p.uniforms[block]->version(),frame=state.ring->submissions()+1;
    if(p.native->uniform_frame[set]!=frame||p.native->uniform_version[set]!=version) {
      const auto &bytes=p.uniforms[block]->bytes();p.native->uniform_offset[set]=p.native->uniform_sets[set]->upload(0,bytes.data(),bytes.size());
      p.native->uniform_version[set]=version;p.native->uniform_frame[set]=frame;
    }
    packet.descriptors[set]=p.native->uniform_sets[set]->current_set();packet.offsets[set].push_back(p.native->uniform_offset[set]);
  }
  if(!p.record->samplers.empty())packet.descriptors[2]=images->descriptors->handle();
  state.ring->keep_alive(p.native);state.packets.push_back(std::move(packet));
}
}
extern "C" void lvk_glDrawArrays(GLenum mode,GLint first,GLsizei count) {draw_geometry(mode,first,count,nullptr,0,false);}
extern "C" void lvk_glDrawElements(GLenum mode,GLsizei count,GLenum type,const GLvoid *indices) {draw_geometry(mode,0,count,indices,type,true);}
extern "C" void lvk_glDrawRangeElements(GLenum mode,GLuint,GLuint,GLsizei count,GLenum type,const GLvoid *indices) {draw_geometry(mode,0,count,indices,type,true);}

extern "C" void lvk_glLightModeli(GLenum name,GLint v) {++state.legacy_version;state.integers[name]=v;}
extern "C" void lvk_glLightModelf(GLenum name,GLfloat v) {++state.legacy_version;state.floats[name]={v,0,0,0};}
extern "C" void lvk_glLightModelfv(GLenum name,const GLfloat *v) {++state.legacy_version;std::copy(v,v+(name==GL_LIGHT_MODEL_AMBIENT?4:1),state.floats[name].begin());}
extern "C" void lvk_glMaterialf(GLenum face,GLenum name,GLfloat v) {++state.legacy_version;if(name!=GL_SHININESS)gap("scalar material parameter is not shininess");state.floats[(face<<16)|name]={v,0,0,0};if(face==GL_FRONT_AND_BACK)state.floats[(GL_FRONT<<16)|name]=state.floats[(GL_BACK<<16)|name]={v,0,0,0};}
extern "C" void lvk_glColorMaterial(GLenum face,GLenum mode) {state.integers[GL_COLOR_MATERIAL_FACE]=face;state.integers[GL_COLOR_MATERIAL_PARAMETER]=mode;}
extern "C" void lvk_glBlendFuncSeparate(GLenum sr,GLenum dr,GLenum sa,GLenum da) {state.integers[GL_BLEND_SRC]=sr;state.integers[GL_BLEND_DST]=dr;state.integers[GL_BLEND_SRC_ALPHA]=sa;state.integers[GL_BLEND_DST_ALPHA]=da;}
extern "C" void lvk_glBlendEquation(GLenum op) {state.integers[GL_BLEND_EQUATION_RGB]=state.integers[GL_BLEND_EQUATION_ALPHA]=op;}
extern "C" void lvk_glBlendEquationSeparate(GLenum rgb,GLenum alpha) {state.integers[GL_BLEND_EQUATION_RGB]=rgb;state.integers[GL_BLEND_EQUATION_ALPHA]=alpha;}
extern "C" void lvk_glBlendColor(GLclampf r,GLclampf g,GLclampf b,GLclampf a) {state.floats[GL_BLEND_COLOR]={r,g,b,a};}
extern "C" void lvk_glTexEnvi(GLenum target,GLenum name,GLint v) {glTexEnvf(target,name,GLfloat(v));}
extern "C" void lvk_glGetTexEnvfv(GLenum target,GLenum name,GLfloat *v) {
  if(!v)return;
  const unsigned unit=state.active_texture-GL_TEXTURE0;
  if(target==GL_TEXTURE_ENV&&name==GL_TEXTURE_ENV_COLOR){std::copy(state.env_color[unit].begin(),state.env_color[unit].end(),v);return;}
  if(target==GL_TEXTURE_ENV&&name==GL_TEXTURE_ENV_MODE){v[0]=GLfloat(state.texture_env[unit]);return;}
  if(target==GL_TEXTURE_ENV){if(auto it=state.env_params[unit].find(name);it!=state.env_params[unit].end()){v[0]=it->second;return;}}
  else if(auto it=state.floats.find((target<<16)|name);it!=state.floats.end()){v[0]=it->second[0];return;}
  // GL's initial texture environment
  switch(name){
  case GL_COMBINE_RGB: case GL_COMBINE_ALPHA: v[0]=GL_MODULATE; return;
  case GL_SOURCE0_RGB: case GL_SOURCE0_ALPHA: v[0]=GL_TEXTURE; return;
  case GL_SOURCE1_RGB: case GL_SOURCE1_ALPHA: v[0]=GL_PREVIOUS; return;
  case GL_SOURCE2_RGB: case GL_SOURCE2_ALPHA: v[0]=GL_CONSTANT; return;
  case GL_OPERAND0_RGB: case GL_OPERAND1_RGB: v[0]=GL_SRC_COLOR; return;
  case GL_OPERAND2_RGB: case GL_OPERAND2_ALPHA: v[0]=GL_SRC_ALPHA; return;
  case GL_OPERAND0_ALPHA: case GL_OPERAND1_ALPHA: v[0]=GL_SRC_ALPHA; return;
  case GL_RGB_SCALE: case GL_ALPHA_SCALE: v[0]=1; return;
  case GL_TEXTURE_LOD_BIAS: v[0]=0; return;
  }
  std::fprintf(stderr,"[vulkan] glGetTexEnv unsupported %#x %#x\n",target,name);
  gap("texture environment query requires implementation");
}
extern "C" void lvk_glGetTexEnviv(GLenum target,GLenum name,GLint *v) {
  GLfloat f[4]{};lvk_glGetTexEnvfv(target,name,f);
  const unsigned n=name==GL_TEXTURE_ENV_COLOR?4:1;
  for(unsigned i=0;i<n;++i)v[i]=name==GL_TEXTURE_ENV_COLOR?GLint(f[i]*2147483647.f):GLint(f[i]);
}
extern "C" void lvk_glTexEnvfv(GLenum target,GLenum name,const GLfloat *v) {if(target!=GL_TEXTURE_ENV||name!=GL_TEXTURE_ENV_COLOR)gap("unsupported texture environment vector");state.env_color[state.active_texture-GL_TEXTURE0]={v[0],v[1],v[2],v[3]};}
extern "C" void lvk_glDepthRange(GLclampd n,GLclampd f) {++state.legacy_version;state.floats[GL_DEPTH_RANGE]={float(std::clamp(n,0.,1.)),float(std::clamp(f,0.,1.)),0,0};}
extern "C" void lvk_glPolygonMode(GLenum face,GLenum mode) {if(mode!=GL_FILL)gap("non-solid polygon mode requires Vulkan non-solid fill support");state.integers[(face<<16)|GL_POLYGON_MODE]=mode;}
extern "C" void lvk_glColor4f(GLfloat r,GLfloat g,GLfloat b,GLfloat a) {state.arrays.current(3,{r,g,b,a});state.matrices.color(r,g,b,a);state.floats[GL_CURRENT_COLOR]={r,g,b,a};}
extern "C" void lvk_glColor3f(GLfloat r,GLfloat g,GLfloat b) {glColor4f(r,g,b,1);}
extern "C" void lvk_glColor4ub(GLubyte r,GLubyte g,GLubyte b,GLubyte a) {glColor4f(r/255.f,g/255.f,b/255.f,a/255.f);}
extern "C" void lvk_glColor3ub(GLubyte r,GLubyte g,GLubyte b) {glColor4f(r/255.f,g/255.f,b/255.f,1);}
extern "C" void lvk_glNormal3f(GLfloat x,GLfloat y,GLfloat z) {state.arrays.current(2,{x,y,z,0});}
extern "C" void lvk_glTexCoord2f(GLfloat u,GLfloat v) {state.arrays.current(8,{u,v,0,1});}
extern "C" void lvk_glMultiTexCoord2f(GLenum unit,GLfloat u,GLfloat v) {state.arrays.current(8+unit-GL_TEXTURE0,{u,v,0,1});}
extern "C" void lvk_glTexCoord4f(GLfloat x,GLfloat y,GLfloat z,GLfloat w) {state.arrays.current(8,{x,y,z,w});}
extern "C" void lvk_glVertexAttrib4f(GLuint index,GLfloat x,GLfloat y,GLfloat z,GLfloat w) {state.arrays.current(index,{x,y,z,w});}
extern "C" void lvk_glVertexAttrib3f(GLuint index,GLfloat x,GLfloat y,GLfloat z) {state.arrays.current(index,{x,y,z,1});}
extern "C" void lvk_glVertexAttrib2f(GLuint index,GLfloat x,GLfloat y) {state.arrays.current(index,{x,y,0,1});}
extern "C" void lvk_glVertexAttrib1f(GLuint index,GLfloat x) {state.arrays.current(index,{x,0,0,1});}
extern "C" void lvk_glBegin(GLenum mode) {if(state.in_begin)gap("nested guest glBegin");state.in_begin=true;state.immediate_mode=mode;state.immediate.clear();state.matrices.begin(mode);}
namespace {
void immediate_vertex(float x,float y,float z,float w) {
 if(!state.in_begin)gap("immediate vertex outside glBegin");
 auto &v=state.immediate.emplace_back();for(unsigned a=0;a<16;++a)std::copy(state.arrays.current_value(a).begin(),state.arrays.current_value(a).end(),v.begin()+a*4);
 v[0]=x;v[1]=y;v[2]=z;v[3]=w;state.matrices.vertex(x,y,z,w);
}
}
extern "C" void lvk_glVertex4f(GLfloat x,GLfloat y,GLfloat z,GLfloat w) {immediate_vertex(x,y,z,w);}
extern "C" void lvk_glVertex3f(GLfloat x,GLfloat y,GLfloat z) {immediate_vertex(x,y,z,1);}
extern "C" void lvk_glVertex3fv(const GLfloat *v) {immediate_vertex(v[0],v[1],v[2],1);}
extern "C" void lvk_glVertex2f(GLfloat x,GLfloat y) {immediate_vertex(x,y,0,1);}
extern "C" void lvk_glVertex2i(GLint x,GLint y) {immediate_vertex(float(x),float(y),0,1);}
extern "C" void lvk_glEnd(void) {if(!state.in_begin)gap("guest glEnd outside glBegin");state.in_begin=false;state.matrices.end();if(!state.immediate.empty())draw_geometry(state.immediate_mode,0,GLsizei(state.immediate.size()),nullptr,0,false,state.immediate.data(),sizeof(state.immediate[0]));}

extern "C" void lvk_glInterleavedArrays(GLenum format,GLsizei stride,const GLvoid *pointer) {
  struct Attribute {unsigned size=0,offset=0;GLenum type=GL_FLOAT;};Attribute position,color,normal,uv;unsigned packed=0;
  switch(format) {
  case GL_V2F:position={2,0};packed=8;break;
  case GL_V3F:position={3,0};packed=12;break;
  case GL_C4UB_V2F:color={4,0,GL_UNSIGNED_BYTE};position={2,4};packed=12;break;
  case GL_C4UB_V3F:color={4,0,GL_UNSIGNED_BYTE};position={3,4};packed=16;break;
  case GL_C3F_V3F:color={3,0};position={3,12};packed=24;break;
  case GL_N3F_V3F:normal={3,0};position={3,12};packed=24;break;
  case GL_C4F_N3F_V3F:color={4,0};normal={3,16};position={3,28};packed=40;break;
  case GL_T2F_V3F:uv={2,0};position={3,8};packed=20;break;
  case GL_T4F_V4F:uv={4,0};position={4,16};packed=32;break;
  case GL_T2F_C4UB_V3F:uv={2,0};color={4,8,GL_UNSIGNED_BYTE};position={3,12};packed=24;break;
  case GL_T2F_C3F_V3F:uv={2,0};color={3,8};position={3,20};packed=32;break;
  case GL_T2F_N3F_V3F:uv={2,0};normal={3,8};position={3,20};packed=32;break;
  case GL_T2F_C4F_N3F_V3F:uv={2,0};color={4,8};normal={3,24};position={3,36};packed=48;break;
  case GL_T4F_C4F_N3F_V4F:uv={4,0};color={4,16};normal={3,32};position={4,44};packed=60;break;
  default:gap("unknown guest interleaved array format");
  }
  if(stride<0)gap("negative interleaved array stride");if(!stride)stride=GLsizei(packed);
  const auto address=guest_pointer(pointer);
  auto set=[&](unsigned slot,Attribute a,bool normalized) {
    state.arrays.enable(slot,a.size!=0);if(a.size)state.arrays.pointer(slot,a.size,a.type,normalized,uint32_t(stride),address+a.offset);
  };
  set(0,position,false);set(3,color,color.type!=GL_FLOAT);set(2,normal,false);set(8+state.client_texture-GL_TEXTURE0,uv,false);
}

// Vector entry points share the same current-attribute state as scalar calls.
extern "C" void lvk_glColor4fv(const GLfloat *v) {lvk_glColor4f(v[0],v[1],v[2],v[3]);}
extern "C" void lvk_glColor3fv(const GLfloat *v) {lvk_glColor3f(v[0],v[1],v[2]);}
extern "C" void lvk_glColor4ubv(const GLubyte *v) {lvk_glColor4ub(v[0],v[1],v[2],v[3]);}
extern "C" void lvk_glColor3ubv(const GLubyte *v) {lvk_glColor3ub(v[0],v[1],v[2]);}
extern "C" void lvk_glNormal3fv(const GLfloat *v) {lvk_glNormal3f(v[0],v[1],v[2]);}
extern "C" void lvk_glTexCoord2fv(const GLfloat *v) {lvk_glTexCoord2f(v[0],v[1]);}
extern "C" void lvk_glTexCoord4fv(const GLfloat *v) {lvk_glTexCoord4f(v[0],v[1],v[2],v[3]);}
extern "C" void lvk_glVertex2fv(const GLfloat *v) {immediate_vertex(v[0],v[1],0,1);}
extern "C" void lvk_glVertex4fv(const GLfloat *v) {immediate_vertex(v[0],v[1],v[2],v[3]);}
extern "C" void lvk_glVertexAttrib1fv(GLuint i,const GLfloat *v) {lvk_glVertexAttrib1f(i,v[0]);}
extern "C" void lvk_glVertexAttrib2fv(GLuint i,const GLfloat *v) {lvk_glVertexAttrib2f(i,v[0],v[1]);}
extern "C" void lvk_glVertexAttrib3fv(GLuint i,const GLfloat *v) {lvk_glVertexAttrib3f(i,v[0],v[1],v[2]);}
extern "C" void lvk_glVertexAttrib4fv(GLuint i,const GLfloat *v) {lvk_glVertexAttrib4f(i,v[0],v[1],v[2],v[3]);}

extern "C" void lvk_glSecondaryColor3f(GLfloat r,GLfloat g,GLfloat b) {state.arrays.current(4,{r,g,b,1});}
extern "C" void lvk_glSecondaryColor3fv(const GLfloat *v) {lvk_glSecondaryColor3f(v[0],v[1],v[2]);}
extern "C" void lvk_glSecondaryColor3ub(GLubyte r,GLubyte g,GLubyte b) {lvk_glSecondaryColor3f(r/255.f,g/255.f,b/255.f);}
extern "C" void lvk_glSecondaryColor3ubv(const GLubyte *v) {lvk_glSecondaryColor3ub(v[0],v[1],v[2]);}

// Expand only draw packets: all subdraws keep the enclosing pass/submission.
extern "C" void lvk_glMultiDrawElements(GLenum mode,const GLsizei *counts,GLenum type,
                                       const GLvoid *const *indices,GLsizei count) {
  if (count < 0) gap("negative guest multidraw count");
  for (GLsizei i=0;i<count;++i) lvk_glDrawElements(mode,counts[i],type,indices[i]);
}
extern "C" void lvk_glMultiDrawArrays(GLenum mode,const GLint *first,const GLsizei *counts,GLsizei count) {
  if (count < 0) gap("negative guest multidraw count");
  for (GLsizei i=0;i<count;++i) lvk_glDrawArrays(mode,first[i],counts[i]);
}

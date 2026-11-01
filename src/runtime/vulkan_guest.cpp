#include "guest_pixels.hpp"
// Guest GL ABI -> shared Vulkan state. No host GL entry is resolved or called.
#include "../renderer/vulkan/guest_arrays.hpp"
#include "../renderer/vulkan/guest_state.hpp"
#include "../renderer/vulkan/stereo_scene.hpp"
#include "../renderer/vulkan/controller_input.hpp"
#include "../renderer/vulkan/overlay.hpp"
#include "../renderer/vulkan/settings_panel.hpp"
#include "../renderer/vulkan/shader_pack.hpp"
#include "../renderer/vulkan/window.hpp"
#include "../renderer/vulkan/resources.hpp"
#include "../renderer/vulkan/uniforms.hpp"
#include "vulkan_abi.h"
#include "vulkan_host.h"
#include <SDL.h>
#ifdef LINDBERGH_VULKAN_XR
#include "../renderer/vulkan/openxr.hpp"
#include <SDL_system.h>
#include <android/asset_manager_jni.h>
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
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <cmath>
#include <sstream>
#include <chrono>

using namespace lindbergh::vk;
extern "C" unsigned char *g_guest_base;
extern "C" unsigned g_frame;
namespace {
[[noreturn]] void gap(const char *what) {
  std::fprintf(stderr, "[vulkan] %s\n", what);
  std::abort();
}
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
  uint64_t revision = 1;
  bool dynamic = false;
  ImageVersion stable;
  std::array<ImageVersion,3> slots;

  std::map<int, TextureLevel> levels;
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
  std::map<std::string,std::unique_ptr<Renderer>> renderers;
};
struct Program {
  bool delete_pending = false;
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
struct Material {
  std::shared_ptr<SampledSet> descriptors;
  std::vector<std::shared_ptr<Texture>> images;
  std::vector<std::shared_ptr<Sampler>> samplers;
};
#ifdef LINDBERGH_VULKAN_XR
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
                       std::pair{"LINDBERGH_VULKAN_FIXED_SHADERS","fixed.lvsp"}}) {
      const char *override=std::getenv(entry.first);
      if (override && *override) continue;
      const auto source=std::string("vulkan/")+entry.second;
      std::unique_ptr<AAsset,decltype(&AAsset_close)> asset(AAssetManager_open(manager,source.c_str(),AASSET_MODE_STREAMING),AAsset_close);
      if (!asset) throw std::runtime_error("Missing packaged shader archive: "+source);
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
#endif
struct State {
  std::unique_ptr<Device> device;
  std::unique_ptr<Window> window;
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
  unsigned hud_target=~0u,hud_target_next=~0u;
  bool picture_size(int w,int h) const {
    const auto *p=game_profile();
    return (w==game_width&&h==game_height) || (p->vr.world_w>0&&w==p->vr.world_w&&h==p->vr.world_h);
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
  std::unique_ptr<FrameRing> ring;
  std::unique_ptr<ShaderPack> pack, fixed_pack;
  Program fixed_program;
  std::array<bool,8> texture_enabled{};
  std::array<GLenum,8> texture_env{GL_MODULATE,GL_MODULATE,GL_MODULATE,GL_MODULATE,GL_MODULATE,GL_MODULATE,GL_MODULATE,GL_MODULATE};
  std::array<std::array<float,4>,8> env_color{};
  std::unordered_map<unsigned, std::unique_ptr<Target>> drawables;
  std::unordered_map<Target *, std::unique_ptr<Renderer>> renderers;
  std::vector<DrawPacket> packets;
  std::unordered_set<Target *> initialized;
  std::array<std::unordered_map<std::string,std::shared_ptr<Material>>,3> materials;
  std::unordered_map<std::string,std::shared_ptr<Sampler>> samplers;
  std::shared_ptr<Texture> fallback;
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
  std::array<GLuint, 8> textures{};
  std::unordered_map<GLuint, TextureObject> texture_objects;
  std::unordered_map<GLenum, std::vector<GLushort>> pixel_maps;
  std::array<GLint, 4> viewport{0, 0, 640, 480}, scissor{0, 0, 640, 480};
  std::array<float, 4> clear{0, 0, 0, 0};
  RasterState raster;
  bool ready = false;
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
    renderers.clear();overlay.reset();panel.reset();
    texture_objects.clear();
    buffers.clear();
    samplers.clear();
    fallback.reset();
    drawables.clear();drawable_sizes.clear();stereo.reset();controllers.reset();game_input={};
    initialized.clear();
    window.reset();
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
      cmd = ring->begin();
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
          if (!unfocused) unfocused=std::make_unique<Target>(*device,TargetInfo{uint32_t(game_width),uint32_t(game_height),2});
          screen=unfocused.get();
        }
#else
        screen = &window->acquire(*ring);
        if(stereo) {stereo->publish(fake_eyes);if(fake_guns)fake_controllers();}
#endif
      }
      if(panel&&overlay)panel->upload(*overlay,cmd);
      if (materials[ring->recording_slot()].size()>2048) materials[ring->recording_slot()].clear();
    }
  }
  void end_pass() {
    if (active) {
      active->end();
      active = nullptr; active_target = nullptr;
    }
  }
  Target *target() { frame(); return drawable ? drawables.at(drawable).get() : screen; }
  void flush(bool keep_last=false) {
    end_pass();
    static const bool profile_prepare=[] {const auto *v=std::getenv("LINDBERGH_VULKAN_PREPARE_TIMING");return v && *v=='1';}();
    if (profile_prepare) {
      const auto start=std::chrono::steady_clock::now();
      size_t created=0;
      for (auto &p : packets) if (p.count) {
        const auto before=p.renderer->pipeline_count();
        p.renderer->prepare(p.raster);
        created+=p.renderer->pipeline_count()-before;
      }
      const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
      if (created || ms>2) std::fprintf(stderr,"[vulkan-prepare] frame %u: %zu packets, %zu new pipelines, %.3f ms\n",g_frame,packets.size(),created,ms);
    } else for (auto &p : packets) if (p.count) p.renderer->prepare(p.raster);
    for (auto &p : packets) {
      if (active_target != p.target) {
        end_pass(); p.renderer->begin(cmd,p.color,initialized.count(p.target)!=0);
        initialized.insert(p.target); active=p.renderer; active_target=p.target;
      } else if (active != p.renderer) {
        active->detach(); p.renderer->attach(cmd); active=p.renderer;
      }
      active->viewport(p.viewport[0],p.viewport[1],p.viewport[2],p.viewport[3],p.scissor);
      if (p.clear_aspects) active->clear(p.clear_aspects,p.color,p.clear_depth,p.clear_stencil,p.scissor);
      if (p.count) {
        active->dynamic_state(p.stencil_ref,p.stencil_compare_mask,p.stencil_write_mask,p.depth_bias_factor,p.depth_bias_units,p.blend_constants.data());
        for (unsigned set=0;set<3;++set) if (p.descriptors[set])
          active->bind_descriptors(set,1,&p.descriptors[set],set<2?uint32_t(p.offsets[set].size()):0,set<2?p.offsets[set].data():nullptr);
        active->draw_buffers(p.streams.data(),uint32_t(p.streams.size()),p.count,p.matrices,p.raster,p.index.buffer?&p.index:nullptr);
      }
    }
    packets.clear();if(!keep_last)end_pass();
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
      renderer->begin(cmd, clear, initialized.count(target)!=0);
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
  const unsigned effects=panel->update(settings,*stereo,*controllers,sample,time,focused,rates);
  if(effects&XP_SAVE)xr_settings_save(&settings);
#ifdef LINDBERGH_VULKAN_XR
  if(effects&XP_RATE_CHANGED)xr->request_refresh(float(settings.refresh));
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
  // SDL attaches this pthread once and detaches it through its TLS destructor.
  if(!SDL_AndroidGetJNIEnv())return 0;
  if(state.xr)state.xr->renderer_thread();
  return 1;
#else
  return 1;
#endif
}
extern "C" int lvk_host_init(SDL_Window *window, int w, int h) {
  if (state.ready)
    return 1;
  try {
#ifdef LINDBERGH_VULKAN_XR
    state.activity=std::make_unique<ActivityRef>();
    state.activity->shader_defaults();
    xr_settings_load(&state.settings);
    const auto *profile=game_profile();
    bool hud_lock=profile->vr.hud_head_locked;
    if(const auto *v=std::getenv("LINDBERGH_XR_HUD_LOCK"))hud_lock=*v=='1';
    StereoSettings stereo{state.settings.scale,state.settings.screen,profile->vr.hud_lens,float(state.settings.hud_pitch),8,hud_lock};
    if(const auto *v=std::getenv("LINDBERGH_STEREO_2D_LENS"))stereo.hud_lens=std::atof(v);
    state.stereo=std::make_unique<StereoScene>(stereo);
    state.controllers=std::make_unique<ControllerInput>(controller_settings(state.settings,w,h));
    OpenXRConfig config{true,true,true,true,uint32_t(state.settings.width),uint32_t(state.settings.resolution),float(state.settings.refresh)};
    config.enable_input=true;
    if(const auto *v=std::getenv("LINDBERGH_CPU_BOOST"))config.cpu_boost=*v=='1';
    state.xr=std::make_unique<OpenXR>(config,state.activity->platform());
    state.device=Device::create(state.xr->device_options());
#else
    state.device = Device::create(desktop_options(window));
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
    state.window = std::make_unique<Window>(*state.device, window, 3);
    if(const auto *yaw=std::getenv("LINDBERGH_VULKAN_FAKE_YAW")) {
      const auto *profile=game_profile();
      StereoSettings settings;settings.scale=profile->vr.scale>0?profile->vr.scale:1;settings.hud_lens=profile->vr.hud_lens;settings.head_locked=profile->vr.hud_head_locked;
      if(const auto *v=std::getenv("LINDBERGH_VULKAN_FAKE_GUN"))state.fake_guns=std::atoi(v);
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
    state.cache=std::make_unique<PipelineCache>(*state.device,cache_path);
    if (!cache_path.empty()) std::fprintf(stderr,"[vulkan] pipeline cache %s: %s\n",
                                         state.cache->loaded()?"loaded":"cold",cache_path.c_str());
    state.ring = std::make_unique<FrameRing>(*state.device, 3, 64u << 20);
#ifdef LINDBERGH_VULKAN_XR
    const auto density=state.xr->density_extent();
    state.unfocused_density=std::make_unique<ConstantDensityMap>(*state.device,*state.ring,density[0],density[1],2);
#endif
    if(state.controllers) {
      const auto *fake=std::getenv("LINDBERGH_VULKAN_FAKE_MENU");
      state.panel=std::make_unique<SettingsPanel>(state.settings.width,state.settings.resolution,game_profile()->title,fake&&*fake=='1');
      state.overlay=std::make_unique<Overlay>(*state.device,*state.ring,*state.cache);
#ifdef LINDBERGH_VULKAN_XR
      for(const auto &target:state.xr->render_targets())state.overlay->prepare(*target);
#else
      for(const auto &target:state.window->render_targets())state.overlay->prepare(*target);
#endif
    }
    state.game_width=w; state.game_height=h;
    state.viewport = state.scissor = {0, 0, w, h};
    state.arrays.current(3,{1,1,1,1});state.arrays.current(2,{0,0,1,0});
    state.raster.depth_test = false;
    state.raster.depth_write = true;
    state.ready = true;
    std::fprintf(stderr, "[vulkan] guest graphics routed directly to Vulkan; "
#ifdef LINDBERGH_VULKAN_XR
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
 Device &device;VkBuffer buffer=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;void *mapped=nullptr;
 FrameCapture(Device &d,VkCommandBuffer cmd,Target &target):device(d) {
  const auto bytes=VkDeviceSize(target.info.width)*target.info.height*4;
  VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};create.size=bytes;create.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  check(vkCreateBuffer(d.handles.device,&create,nullptr,&buffer),"capture buffer");
  VkMemoryRequirements req;vkGetBufferMemoryRequirements(d.handles.device,buffer,&req);
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};alloc.allocationSize=req.size;
  alloc.memoryTypeIndex=d.memory_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  check(vkAllocateMemory(d.handles.device,&alloc,nullptr,&memory),"capture memory");check(vkBindBufferMemory(d.handles.device,buffer,memory,0),"bind capture");
  check(vkMapMemory(d.handles.device,memory,0,bytes,0,&mapped),"map capture");
  VkImageMemoryBarrier image{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};image.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;image.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
  image.oldLayout=target.info.final_layout;image.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  image.srcQueueFamilyIndex=image.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;image.image=target.color;image.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
  vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&image);
  VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={target.info.width,target.info.height,1};
  vkCmdCopyImageToBuffer(cmd,target.color,image.newLayout,buffer,1,&copy);
  image.oldLayout=image.newLayout;image.newLayout=target.info.final_layout;image.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;image.dstAccessMask=0;
  vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&image);
  VkBufferMemoryBarrier ready{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};ready.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;ready.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
  ready.srcQueueFamilyIndex=ready.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;ready.buffer=buffer;ready.size=VK_WHOLE_SIZE;
  vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&ready,0,nullptr);
 }
 void write(const char *path,const Target &target) {
  std::ofstream out(path,std::ios::binary);out<<"P6\n"<<target.info.width<<' '<<target.info.height<<"\n255\n";
  const auto p=static_cast<const uint8_t *>(mapped);const bool bgra=target.info.color_format==VK_FORMAT_B8G8R8A8_UNORM||target.info.color_format==VK_FORMAT_B8G8R8A8_SRGB;
  for(size_t i=0;i<size_t(target.info.width)*target.info.height;++i) {const char rgb[]={char(p[4*i+(bgra?2:0)]),char(p[4*i+1]),char(p[4*i+(bgra?0:2)])};out.write(rgb,3);}
  std::fprintf(stderr,"[vulkan] frame capture %s\n",path);
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
  if (!state.initialized.count(state.screen)) {
    const auto old=state.drawable;state.drawable=0;state.renderer();state.end_pass();state.drawable=old;
  }
  if(show_overlay) {
    Renderer *previous=state.active_target==state.screen?state.active:nullptr;
    if(state.active&&!previous)state.end_pass();
    state.overlay->draw(state.cmd,*state.screen,state.stereo->overlay_matrices(),state.stereo->overlay_eyes(),state.panel->guns(),previous,true,state.panel->draws());
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
    capture=std::make_unique<FrameCapture>(*state.device,state.cmd,*capture_target);
  }
#ifdef LINDBERGH_VULKAN_XR
  auto submitted=state.ring->submit();
  state.xr->end_frame(state.xr_rendering);
  if (state.xr->exiting()) {SDL_Event quit{};quit.type=SDL_QUIT;SDL_PushEvent(&quit);}
#else
  auto submitted=state.window->present(*state.ring);
#endif
  state.last_submission=submitted;
  if(capture) {state.ring->wait(submitted);capture->write(capture_path.c_str(),*capture_target);}
  state.cmd=VK_NULL_HANDLE;
  state.screen=nullptr;
  state.scene_end();
}
extern "C" void lvk_glFlush(void) {
  if (!state.cmd) return;
  state.flush();
#ifdef LINDBERGH_VULKAN_XR
  state.last_submission=state.ring->submit();
#else
  state.last_submission=state.window->flush(*state.ring);
#endif
  state.cmd=VK_NULL_HANDLE; // next draw continues on the same acquired image
}
extern "C" void lvk_glFinish(void) {
  lvk_glFlush();
  if (state.last_submission) state.ring->wait(*state.last_submission);
}
extern "C" unsigned lvk_create_drawable(int w, int h) {
  if (w <= 0 || h <= 0)
    gap("invalid guest drawable dimensions");
  const auto id = state.next_drawable++;
  TargetInfo info{uint32_t(w),uint32_t(h),state.stereo?2u:1u};
#ifdef LINDBERGH_VULKAN_XR
  const auto *profile=game_profile();
  const LgStereoTargetSize sizes={state.game_width,state.game_height,profile->vr.world_w,profile->vr.world_h,int(state.xr->width()),int(state.xr->height())};
  float sx,sy;lg_stereo_target_scale(&sizes,w,h,0,&sx,&sy);
  info.width=uint32_t(std::lround(w*sx));info.height=uint32_t(std::lround(h*sy));
  std::fprintf(stderr,"[vulkan-xr] pbuffer %u: %dx%d -> %ux%u, %u layers\n",id,w,h,info.width,info.height,info.layers);
  const bool foveated_world=state.picture_size(w,h) && info.width==state.xr->width() && info.height==state.xr->height();
  if(foveated_world) {
    if(state.xr->density_maps().empty())gap("World pbuffer requires runtime density maps");
    info.density_map=state.unfocused_density->view();
  }
#endif
  state.drawable_sizes[id]={w,h};
  info.guest_bottom_origin=true;
  state.drawables[id] = std::make_unique<Target>(*state.device, info);
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
extern "C" void lvk_destroy_drawable(unsigned id) {
  // Destruction of in-flight guest render targets needs deferred retirement.
  if (state.drawables.count(id))
    gap("guest drawable retirement is not implemented");
}
extern "C" void lvk_bind_drawable(unsigned id) {
  if (id && !state.drawables.count(id))
    gap("unknown guest Vulkan drawable");
  state.drawable = id;
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
  case GL_FRAMEBUFFER_BINDING:
    *out = 0;
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
  default:
    std::fprintf(stderr, "[vulkan] glGetIntegerv unsupported enum %#x\n", what);
    gap("guest query requires implementation");
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
                    what == GL_CURRENT_COLOR || what == GL_COLOR_WRITEMASK)
                       ? 4
                       : 1;
    std::copy(v.begin(), v.begin() + n, out);
    return;
  }
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
  state.floats[GL_DEPTH_CLEAR_VALUE] = {float(d), 0, 0, 0};
}
extern "C" void lvk_glClearStencil(GLint s) {
  state.integers[GL_STENCIL_CLEAR_VALUE] = s;
}
extern "C" void lvk_glClear(GLbitfield mask) { queue_clear(mask); }
extern "C" void lvk_glEnable(GLenum cap) {
  state.enabled[cap] = true;
  if(cap==GL_TEXTURE_2D)state.texture_enabled[state.active_texture-GL_TEXTURE0]=true;
  if (cap == GL_DEPTH_TEST)
    state.raster.depth_test = true;
  if (cap == GL_BLEND)
    state.raster.blend = true;
}
extern "C" void lvk_glDisable(GLenum cap) {
  state.enabled[cap] = false;
  if(cap==GL_TEXTURE_2D)state.texture_enabled[state.active_texture-GL_TEXTURE0]=false;
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
  if (target != GL_TEXTURE_2D)
    gap("unsupported guest texture target");
  state.textures[state.active_texture - GL_TEXTURE0] = texture;
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
extern "C" void lvk_glLinkProgram(GLuint id) {
  if (!state.pack) {
    const auto path = std::getenv("LINDBERGH_VULKAN_SHADERS");
    if (!path)
      gap("set LINDBERGH_VULKAN_SHADERS to the offline .lvsp archive");
    state.pack = std::make_unique<ShaderPack>(path);
  }
  auto &p = state.programs.at(id);
  p.record = nullptr;
  p.native.reset(); // submitted frames retain their previous native program
  SourceDigest vs{}, fs{};
  for (const auto shader : p.shaders) {
    const auto &s = state.shaders.at(shader);
    if (s.type == GL_VERTEX_SHADER)
      vs = shader_source_digest(s.source);
    if (s.type == GL_FRAGMENT_SHADER)
      fs = shader_source_digest(s.source);
  }
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
    std::fprintf(stderr,
                 "[vulkan] missing offline shader pair for guest program %u\n",
                 id);
    gap("guest shader identity/binding variant not present in archive");
  }
}
extern "C" void lvk_glUseProgram(GLuint id) {
  if (id && !state.programs.at(id).record)
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
    *out = v.record != nullptr;
  else if (p == GL_INFO_LOG_LENGTH)
    *out = 1;
  else if (p == GL_DELETE_STATUS)
    *out = v.delete_pending;
  else if (p == GL_ATTACHED_SHADERS)
    *out = GLint(v.shaders.size());
  else
    gap("unknown guest program query");
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
TextureObject &texture(GLenum target) {
  if (target != GL_TEXTURE_2D)
    gap("unsupported guest texture target");
  return state
      .texture_objects[state.textures[state.active_texture - GL_TEXTURE0]];
}
int components(GLenum format) {
  switch (format) {
  case GL_DEPTH_COMPONENT:
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
  auto &v = object.levels[level];
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
  const auto it = object.levels.find(level);
  if (it == object.levels.end()) gap("guest texture subimage has no allocated level");
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
  auto &v = object.levels[level];
  v = {GLint(internal), w, h, 0, 0, {}, true};
  v.bytes.resize(size_t(bytes));
  if (data || state.unpack_buffer)
    std::memcpy(v.bytes.data(), pixels(data, size_t(bytes)), size_t(bytes));
}
extern "C" void lvk_glDeleteTextures(GLsizei n, const GLuint *ids) {
  for (GLsizei i = 0; i < n; ++i)
    state.texture_objects.erase(ids[i]);
}
extern "C" void lvk_glTexEnvf(GLenum target, GLenum name, GLfloat value) {
  state.floats[(target << 16) | name] = {value, 0, 0, 0};
  if(target==GL_TEXTURE_ENV&&name==GL_TEXTURE_ENV_MODE)state.texture_env[state.active_texture-GL_TEXTURE0]=GLenum(value);
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
extern "C" void lvk_glAlphaFunc(GLenum compare, GLclampf reference) { ++state.legacy_version;
  state.integers[GL_ALPHA_TEST_FUNC] = compare;
  state.floats[GL_ALPHA_TEST_REF] = {std::clamp(reference, 0.f, 1.f), 0, 0, 0};
}
extern "C" void lvk_glPointSize(GLfloat size) {
  state.floats[GL_POINT_SIZE] = {size, 0, 0, 0};
}
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
void changed(TextureObject &o) { ++o.revision; o.dynamic=o.dynamic || bool(o.stable.image); }
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
    case GL_DEPTH_COMPONENT:return Target::depth_format;
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
    for(const auto &[level,v]:o.levels) {
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
      } else if(v.format==GL_DEPTH_COMPONENT&&v.type==GL_UNSIGNED_INT) {
        expanded.resize(bytes);
        for(size_t i=0;i<bytes;i+=4) {uint32_t d;std::memcpy(&d,v.bytes.data()+i,4);const float f=float(double(d)/4294967295.0);std::memcpy(expanded.data()+i,&f,4);}
        data=expanded.data();
      }
      for(uint32_t layer=0;layer<info.layers;++layer)
        version.image->upload(state.cmd,*state.ring,uint32_t(level),layer,data,bytes);
    }
    version.revision=o.revision;
  }
  version.use_frame=frame;state.ring->keep_alive(version.image);return version.image;
}
std::shared_ptr<GeometryBuffer> native_buffer(GLuint id) {
  auto &o=state.buffers.at(id);state.frame();state.end_pass();
  auto &version=o.dynamic?o.slots[state.ring->recording_slot()]:o.stable;
  const auto frame=state.ring->submissions()+1;
  const auto bytes=(o.bytes.size()+3)&~size_t(3);
  if(!bytes)gap("empty guest geometry buffer used for drawing");
  if(version.revision!=o.revision) {
    if(!version.buffer||version.use_frame==frame||version.bytes!=bytes) {
      version.buffer=std::make_shared<GeometryBuffer>(*state.device,bytes);version.bytes=bytes;
    }
    if(bytes==o.bytes.size())version.buffer->upload(state.cmd,*state.ring,o.bytes.data(),bytes);
    else {auto copy=o.bytes;copy.resize(bytes);version.buffer->upload(state.cmd,*state.ring,copy.data(),bytes);}
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
  const auto min=param(GL_TEXTURE_MIN_FILTER,GL_NEAREST_MIPMAP_LINEAR),mag=param(GL_TEXTURE_MAG_FILTER,GL_LINEAR);
  info.mag_filter=mag==GL_NEAREST?VK_FILTER_NEAREST:VK_FILTER_LINEAR;
  info.min_filter=min==GL_NEAREST||min==GL_NEAREST_MIPMAP_NEAREST||min==GL_NEAREST_MIPMAP_LINEAR?VK_FILTER_NEAREST:VK_FILTER_LINEAR;
  info.mip_filter=min==GL_NEAREST_MIPMAP_LINEAR||min==GL_LINEAR_MIPMAP_LINEAR?VK_SAMPLER_MIPMAP_MODE_LINEAR:VK_SAMPLER_MIPMAP_MODE_NEAREST;
  info.max_lod=min==GL_NEAREST||min==GL_LINEAR?0:float(levels-1);
  info.u=wrap_mode(param(GL_TEXTURE_WRAP_S,GL_REPEAT));info.v=wrap_mode(param(GL_TEXTURE_WRAP_T,GL_REPEAT));
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
  auto *target=state.target();auto &renderer=state.renderers[target];
  if(!renderer)renderer=std::make_unique<Renderer>(*state.device,*target,*state.cache);
  auto p=packet_for(target,renderer.get());
  const auto write=state.floats.find(GL_COLOR_WRITEMASK);
  bool full=true,none=false;
  if(write!=state.floats.end()) {full=std::all_of(write->second.begin(),write->second.end(),[](float v){return v!=0;});none=std::all_of(write->second.begin(),write->second.end(),[](float v){return v==0;});}
  if(mask&GL_COLOR_BUFFER_BIT) {if(!none)p.clear_aspects|=VK_IMAGE_ASPECT_COLOR_BIT;}
  if((mask&GL_DEPTH_BUFFER_BIT)&&state.raster.depth_write)p.clear_aspects|=VK_IMAGE_ASPECT_DEPTH_BIT;
  if((mask&GL_STENCIL_BUFFER_BIT)&&(p.stencil_write_mask&255)) {p.clear_aspects|=VK_IMAGE_ASPECT_STENCIL_BIT;}
  p.clear_depth=state.floats.count(GL_DEPTH_CLEAR_VALUE)?state.floats[GL_DEPTH_CLEAR_VALUE][0]:1;
  p.clear_stencil=uint32_t(integer(GL_STENCIL_CLEAR_VALUE,0));
  if(p.clear_aspects && ((!full&&!none) || ((p.clear_aspects&VK_IMAGE_ASPECT_STENCIL_BIT)&&((p.stencil_write_mask&255)!=255)))) {
    p.count=6;p.clear_aspects=0;p.raster={};p.raster.depth_test=bool(mask&GL_DEPTH_BUFFER_BIT)&&state.raster.depth_write;
    p.raster.depth_write=p.raster.depth_test;p.raster.depth_compare=VK_COMPARE_OP_ALWAYS;
    p.raster.color_mask=0;if(mask&GL_COLOR_BUFFER_BIT)for(unsigned i=0;i<4;++i)if(write==state.floats.end()||write->second[i])p.raster.color_mask|=1u<<i;
    p.raster.stencil=(mask&GL_STENCIL_BUFFER_BIT)&&(p.stencil_write_mask&255);p.raster.stencil_compare=VK_COMPARE_OP_ALWAYS;
    p.raster.stencil_pass=VK_STENCIL_OP_REPLACE;p.stencil_ref=p.clear_stencil;
    const auto z=std::clamp(p.clear_depth,0.f,1.f)*2-1;
    Vertex quad[]={{{-1,-1,z,1},{}},{{1,-1,z,1},{}},{{1,1,z,1},{}},{{-1,-1,z,1},{}},{{1,1,z,1},{}},{{-1,1,z,1},{}}};
    for(auto &v:quad)std::copy(p.color.begin(),p.color.end(),v.color);
    p.streams.push_back(state.ring->upload(quad,sizeof quad));for(auto &m:p.matrices.view_projection)for(unsigned i=0;i<4;++i)m[i*5]=1;
    p.viewport={0,0,float(target->info.width),float(target->info.height)};
  }
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
  o.layers=source->info.layers;o.gpu_target=true;
  const double sx=double(source->info.width)/logical[0],sy=double(source->info.height)/logical[1];
  const auto &base=o.levels.at(0);
  o.render_width=uint32_t(std::lround(base.width*sx));o.render_height=uint32_t(std::lround(base.height*sy));
  if(full_level)o.picture_view=true; // copied images play the role of legacy rectangle/view textures
  changed(o);auto gpu=native_image(o,full_level?level:-1,true);
  const auto px=int32_t(std::lround(x*sx)),py=int32_t(std::lround(y*sy));
  const auto pw=uint32_t(std::lround((x+w)*sx)-px),ph=uint32_t(std::lround((y+h)*sy)-py);
  gpu->copy_from(state.cmd,*source,uint32_t(level),int32_t(std::lround(dx*sx)),int32_t(std::lround(dy*sy)),px,py,pw,ph,0,0,o.layers);
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
    if(state.enabled[GL_LIGHTING])gap("fixed-function lighting draw needs implementation");
    for(unsigned i=0;i<8;++i) {
      if(state.texture_enabled[i])r.fixed_texture|=1u<<i;
      unsigned mode=0;switch(state.texture_env[i]) {case GL_MODULATE:break;case GL_REPLACE:mode=1;break;case GL_ADD:mode=2;break;case GL_DECAL:mode=3;break;case GL_BLEND:mode=4;break;default:gap("fixed-function texture combine mode needs implementation");}
      r.fixed_env|=mode<<(3*i);
    }
    if(state.enabled[GL_FOG]) {auto mode=GLenum(value(GL_FOG_MODE,{GL_EXP,0,0,0})[0]);r.fog_mode=mode==GL_LINEAR?1:mode==GL_EXP?2:3;}
  }
  return r;
}
Program &current_program() {
  if(state.program)return state.programs.at(state.program);
  if(!state.fixed_pack) {
    const auto *fixed=std::getenv("LINDBERGH_VULKAN_FIXED_SHADERS");
    state.fixed_pack=std::make_unique<ShaderPack>(fixed && *fixed ? fixed : LINDBERGH_VULKAN_FIXED_ARCHIVE);
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
    auto &object=state.texture_objects[state.textures[unsigned(unit)]];
    auto image=native_image(object);auto sampler=native_sampler(object,image->info.levels,s.type.find("Shadow")!=std::string::npos);
    const bool array=s.type.find("2DArray")!=std::string::npos;
    if(image->info.layers>1 && !array)gap("stereo texture requires a rebuilt offline shader archive with array samplers");
    const auto view=array?image->array_view():image->view();
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
    float x=p[0]*v[0]+p[4]*v[1]+p[12],y=p[1]*v[0]+p[5]*v[1]+p[13];
    if(immediate) { // legacy glBegin fill tracking also includes clip W/Z
      const float w=p[3]*v[0]+p[7]*v[1]+p[11]*v[2]+p[15];
      if(w<=1e-6f)return false;
      x=(x+p[8]*v[2])/w;y=(y+p[9]*v[2])/w;
    }
    box[0]=std::min(box[0],x);box[1]=std::min(box[1],y);box[2]=std::max(box[2],x);box[3]=std::max(box[3],y);
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
    const float w=m[3]*v[0]+m[7]*v[1]+m[11]*v[2]+m[15]*v[3];
    if(!std::isfinite(w)||w<=1e-6f)return false;
    const float x=(m[0]*v[0]+m[4]*v[1]+m[8]*v[2]+m[12]*v[3])/w;
    const float y=(m[1]*v[0]+m[5]*v[1]+m[9]*v[2]+m[13]*v[3])/w;
    if(!std::isfinite(x)||!std::isfinite(y))return false;
    box[0]=std::min(box[0],x);box[1]=std::min(box[1],y);box[2]=std::max(box[2],x);box[3]=std::max(box[3],y);
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
  facts.offscreen=state.drawable && !state.picture_size(size[0],size[1]);
  facts.hud_target=state.drawable==state.hud_target;facts.multiview=1;
  facts.zoom=state.stereo->zoomed(p);
  const auto texture=state.texture_objects.find(state.textures[0]);
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
  const int kind=composite?LG_STEREO_PASS:lg_stereo_classify_draw(&facts);
  packet.matrices=state.stereo->transforms(p,kind);
  state.stereo->observe(p,kind);
  if(kind==LG_STEREO_WORLD && facts.perspective)++state.world_draws;
  if(kind==LG_STEREO_HUD) {
    const bool raw_perspective=lg_stereo_is_perspective(p.data());
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
    const bool movie=texture!=state.texture_objects.end()&&texture->second.movie;
    if(!raw_perspective || (state.world_draws && !movie))packet.raster.depth_write=false;
    if(!facts.perspective && raw_perspective && (!state.world_draws || movie)) {
      packet.raster.depth_bias=true;packet.depth_bias_factor=0;packet.depth_bias_units=-4.f*++state.hud_sequence;
    }
  }
  static const auto log_range=[] {std::array<unsigned,2> r{};if(const auto *v=std::getenv("LINDBERGH_VULKAN_CLASSIFY")){r[0]=r[1]=unsigned(std::strtoul(v,nullptr,10));if(const auto *end=std::strchr(v,'-'))r[1]=unsigned(std::strtoul(end+1,nullptr,10));}return r;}();
  if(log_range[1] && g_frame>=log_range[0] && g_frame<=log_range[1] && (state.world_draws==1 || facts.texture_view)) {
    std::fprintf(stderr,"[vulkan-camera] P");for(float v:p)std::fprintf(stderr," %.6g",v);
    std::fprintf(stderr," reprojection");for(float v:packet.matrices.view_projection[0])std::fprintf(stderr," %.6g",v);std::fputc(10,stderr);
    std::fprintf(stderr,"[vulkan-camera] modelview");for(float v:state.matrices.matrix(GL_MODELVIEW))std::fprintf(stderr," %.6g",v);
    std::fprintf(stderr,"; depth test %d write %d func %d\n",packet.raster.depth_test,packet.raster.depth_write,int(packet.raster.depth_compare));
  }
  if(log_range[1] && g_frame>=log_range[0] && g_frame<=log_range[1])
    std::fprintf(stderr,"[vulkan-class] frame %u target %u %dx%d class %d lens %.4f %.4f texture-view %d offscreen %d; cache builds %zu; program %u texture %u gpu-target %d world draws %u\n",g_frame,state.drawable,size[0],size[1],kind,p[0],p[5],facts.texture_view,facts.offscreen,state.stereo->transform_builds(),state.program,state.textures[0],texture!=state.texture_objects.end()&&texture->second.gpu_target,state.world_draws);
  return false;
}
void draw_geometry(GLenum mode,GLint first,GLsizei count,const void *indices,GLenum index_type,bool indexed,
                   const void *immediate=nullptr,uint32_t immediate_stride=0) {
  if(count<=0)return;if(first<0)gap("negative guest first vertex");
  const bool guest_indexed=indexed;
  auto *target=state.target();auto &p=current_program();native_program(p);legacy_uniforms(p,*target);
  const bool fixed=!state.program;
  if(fixed)for(size_t block=0;block<p.record->uniforms.size();++block) {
    auto field=p.uniforms[block]->find("fixed_env_color");if(field>=0)p.uniforms[block]->write(uint32_t(field),state.env_color.data(),sizeof state.env_color,0,8);
  }
  auto images=material(p);p.native->layouts[2]=images->descriptors->layout();
  std::vector<uint32_t> locations;for(const auto &input:p.record->inputs)locations.push_back(input.location);
  uint32_t vertices=uint32_t(count);std::vector<uint32_t> elements;
  if(indexed) {
    const size_t bytes=index_type==GL_UNSIGNED_BYTE?1:index_type==GL_UNSIGNED_SHORT?2:index_type==GL_UNSIGNED_INT?4:0;
    if(!bytes)gap("unsupported guest index type");
    const uint8_t *data=static_cast<const uint8_t *>(indices);
    if(state.element_buffer) {auto &b=state.buffers.at(state.element_buffer);const auto offset=(uintptr_t)indices;check_range(b.bytes.size(),GLintptr(offset),GLsizeiptr(size_t(count)*bytes));data=b.bytes.data()+offset;}
    elements.resize(size_t(count));vertices=0;
    for(size_t i=0;i<size_t(count);++i) {uint32_t v=0;std::memcpy(&v,data+i*bytes,bytes);elements[i]=v;if(v==UINT32_MAX)gap("guest index range overflow");vertices=std::max(vertices,v+1);}
  }
  auto raster=raster_state(fixed);raster.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  if(mode==GL_TRIANGLE_STRIP)raster.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  else if(mode==GL_TRIANGLE_FAN)raster.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
  else if(mode==GL_POINTS)raster.topology=VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
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
    ProgramSource source{p.record->vertex.data(),p.record->fragment.data(),p.record->vertex.size()*4,p.record->fragment.size()*4,plan.bindings,plan.attributes,p.native->layouts};
    renderer=std::make_unique<Renderer>(*state.device,*target,*state.cache,source);
  }
  auto packet=packet_for(target,renderer.get());packet.raster=raster;packet.streams=std::move(plan.streams);packet.count=uint32_t(count);
  if(stereo_packet(packet,first,count,elements,immediate,immediate_stride,guest_indexed))return;
  if(indexed) {auto upload=state.ring->upload(elements.data(),size_t(count)*4,4);packet.index={upload.buffer,upload.offset,VK_INDEX_TYPE_UINT32};}
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

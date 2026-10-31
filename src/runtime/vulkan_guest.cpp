// Guest GL ABI -> shared Vulkan state. No host GL entry is resolved or called.
#include "../renderer/vulkan/guest_arrays.hpp"
#include "../renderer/vulkan/guest_state.hpp"
#include "../renderer/vulkan/shader_pack.hpp"
#include "../renderer/vulkan/window.hpp"
#include "vulkan_abi.h"
#include "vulkan_host.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <unordered_map>

using namespace lindbergh::vk;
extern "C" unsigned char *g_guest_base;
namespace {
[[noreturn]] void gap(const char *what) {
  std::fprintf(stderr, "[vulkan] %s\n", what);
  std::abort();
}
struct Shader {
  GLenum type;
  std::string source;
  bool compiled = false;
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
struct TextureObject {
  std::map<int, TextureLevel> levels;
  std::unordered_map<GLenum, GLint> parameters;
  std::unordered_map<GLenum, std::array<float, 4>> float_parameters;
};
struct Program {
  std::vector<GLuint> shaders;
  std::vector<AttributeBinding> attributes;
  const ShaderRecord *record = nullptr;
  std::vector<std::unique_ptr<UniformData>> uniforms;
  std::vector<UniformLocation> locations;
  std::unordered_map<std::string, GLint> location_names;
  std::unordered_map<int, GLint> sampler_units;
};
struct Buffer {
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
struct State {
  std::unique_ptr<Device> device;
  std::unique_ptr<Window> window;
  std::unique_ptr<PipelineCache> cache;
  std::unique_ptr<FrameRing> ring;
  std::unique_ptr<ShaderPack> pack;
  std::unordered_map<unsigned, std::unique_ptr<Target>> drawables;
  std::unordered_map<Target *, std::unique_ptr<Renderer>> renderers;
  GuestState matrices;
  GuestArrays arrays;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
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
  std::vector<Attributes> attributes;
  ~State() {
    ring.reset(); // completes buffered commands before referenced resources
    renderers.clear();
    drawables.clear();
    window.reset();
  }
  void frame() {
    if (!cmd) {
      cmd = ring->begin();
      screen = &window->acquire(*ring);
    }
  }
  void end_pass() {
    if (active) {
      active->end();
      active = nullptr;
    }
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
      renderer->begin(cmd, clear);
      active = renderer.get();
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
GLuint object() { return state.next_object++; }
void check_range(size_t size, GLintptr offset, GLsizeiptr bytes) {
  if (offset < 0 || bytes < 0 || size_t(offset) > size ||
      size_t(bytes) > size - size_t(offset))
    gap("guest buffer range exceeds storage");
}
} // namespace
extern "C" int lvk_host_init(SDL_Window *window, int w, int h) {
  if (state.ready)
    return 1;
  try {
    state.device = Device::create(desktop_options(window));
    state.window = std::make_unique<Window>(*state.device, window, 3);
    const char *cache = std::getenv("LINDBERGH_VULKAN_CACHE");
    state.cache =
        std::make_unique<PipelineCache>(*state.device, cache ? cache : "");
    state.ring = std::make_unique<FrameRing>(*state.device, 3, 32u << 20);
    state.viewport = state.scissor = {0, 0, w, h};
    state.raster.depth_test = false;
    state.raster.depth_write = true;
    state.ready = true;
    std::fprintf(stderr, "[vulkan] guest graphics routed directly to Vulkan; "
                         "desktop swapchain ready\n");
    return 1;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "[vulkan] initialization: %s\n", e.what());
    return 0;
  }
}
extern "C" void lvk_host_present(void) {
  if (state.drawable)
    gap("offscreen-to-window presentation requires render-target routing");
  state.frame();
  if (!state.active)
    state.renderer();
  state.end_pass();
  state.window->present(*state.ring);
  state.cmd = VK_NULL_HANDLE;
}
extern "C" unsigned lvk_create_drawable(int w, int h) {
  if (w <= 0 || h <= 0)
    gap("invalid guest drawable dimensions");
  const auto id = state.next_drawable++;
  TargetInfo info{uint32_t(w), uint32_t(h), 1};
  state.drawables[id] = std::make_unique<Target>(*state.device, info);
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
  if (state.drawable != id)
    state.end_pass();
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
extern "C" void lvk_glLoadIdentity(void) { state.matrices.load_identity(); }
extern "C" void lvk_glLoadMatrixf(const GLfloat *m) {
  state.matrices.load_matrix(m);
}
extern "C" void lvk_glMultMatrixf(const GLfloat *m) {
  state.matrices.mult_matrix(m);
}
extern "C" void lvk_glPushMatrix(void) { state.matrices.push_matrix(); }
extern "C" void lvk_glPopMatrix(void) { state.matrices.pop_matrix(); }
extern "C" void lvk_glTranslatef(GLfloat x, GLfloat y, GLfloat z) {
  state.matrices.translate(x, y, z);
}
extern "C" void lvk_glScalef(GLfloat x, GLfloat y, GLfloat z) {
  state.matrices.scale(x, y, z);
}
extern "C" void lvk_glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t,
                            GLdouble n, GLdouble f) {
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
extern "C" void lvk_glClear(GLbitfield mask) {
  if (mask &
      ~(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT))
    gap("unknown guest clear mask");
  if (state.active)
    gap("mid-pass guest clear is not implemented");
}
extern "C" void lvk_glEnable(GLenum cap) {
  state.enabled[cap] = true;
  if (cap == GL_DEPTH_TEST)
    state.raster.depth_test = true;
  if (cap == GL_BLEND)
    state.raster.blend = true;
}
extern "C" void lvk_glDisable(GLenum cap) {
  state.enabled[cap] = false;
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
extern "C" void lvk_glActiveTexture(GLenum unit) {
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
  if (target == GL_ARRAY_BUFFER)
    state.arrays.bind_array_buffer(name);
}
extern "C" void lvk_glBufferData(GLenum target, GLsizeiptr n,
                                 const GLvoid *data, GLenum usage) {
  if (n < 0)
    gap("negative guest buffer size");
  auto &b = state.buffers[state.binding(target)];
  b.bytes.resize(size_t(n));
  if (data && n)
    std::memcpy(b.bytes.data(), data, size_t(n));
  b.usage = usage;
}
extern "C" void lvk_glBufferSubData(GLenum target, GLintptr offset,
                                    GLsizeiptr n, const GLvoid *data) {
  auto &b = state.buffers.at(state.binding(target));
  check_range(b.bytes.size(), offset, n);
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
  state.programs.at(p).shaders.push_back(s);
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
  SourceDigest vs{}, fs{};
  for (const auto shader : p.shaders) {
    const auto &s = state.shaders.at(shader);
    if (s.type == GL_VERTEX_SHADER)
      vs = source_digest(s.source);
    if (s.type == GL_FRAGMENT_SHADER)
      fs = source_digest(s.source);
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
  state.program = id;
}
extern "C" void lvk_glGetShaderiv(GLuint id, GLenum p, GLint *out) {
  const auto &s = state.shaders.at(id);
  if (p == GL_SHADER_TYPE)
    *out = s.type;
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
extern "C" void lvk_glDeleteShader(GLuint) {
} // attached objects retain source until program retirement
extern "C" void lvk_glDeleteProgram(GLuint id) { state.programs.erase(id); }
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
int integer(GLenum name, int fallback) {
  auto it = state.integers.find(name);
  return it == state.integers.end() ? fallback : it->second;
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
  if (value < base || value - base > UINT32_MAX)
    gap("host pointer has no guest vertex-array address");
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
  if (w < 0 || h < 0 || level < 0 || border ||
      (type != GL_UNSIGNED_BYTE &&
       !(format == GL_DEPTH_COMPONENT &&
         (type == GL_UNSIGNED_INT || type == GL_FLOAT)))) {
    std::fprintf(
        stderr,
        "[vulkan] texture internal %#x format %#x type %#x size %dx%d\n",
        internal, format, type, w, h);
    gap("guest texture format/dimensions need conversion support");
  }
  auto &v = texture(target).levels[level];
  v = {internal, w, h, format, type, {}, false};
  const auto pixel_bytes =
      components(format) * (type == GL_UNSIGNED_BYTE ? 1 : 4);
  const auto row = size_t(w) * pixel_bytes;
  v.bytes.resize(row * size_t(h));
  if (!pointer && !state.unpack_buffer)
    return;
  const auto alignment = integer(GL_UNPACK_ALIGNMENT, 4);
  if (alignment != 1 && alignment != 2 && alignment != 4 && alignment != 8)
    gap("invalid guest pixel unpack alignment");
  const auto width = integer(GL_UNPACK_ROW_LENGTH, 0);
  const auto raw = size_t(width ? width : w) * pixel_bytes;
  const auto stride = (raw + alignment - 1) & ~size_t(alignment - 1);
  const auto skip = integer(GL_UNPACK_SKIP_ROWS, 0) * stride +
                    integer(GL_UNPACK_SKIP_PIXELS, 0) * pixel_bytes;
  const auto source =
      pixels(pointer, skip + (h ? size_t(h - 1) * stride + row : 0));
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
}
extern "C" void lvk_glCompressedTexImage2D(GLenum target, GLint level,
                                           GLenum internal, GLsizei w,
                                           GLsizei h, GLint border,
                                           GLsizei bytes, const GLvoid *data) {
  if (w < 0 || h < 0 || level < 0 || border || bytes < 0)
    gap("invalid guest compressed texture dimensions");
  auto &v = texture(target).levels[level];
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
extern "C" void lvk_glLightf(GLenum light, GLenum name, GLfloat v) {
  state.floats[(light << 16) | name] = {v, 0, 0, 0};
}
extern "C" void lvk_glLightfv(GLenum light, GLenum name, const GLfloat *v) {
  auto &out = state.floats[(light << 16) | name];
  auto n = name == GL_SPOT_DIRECTION ? 3
           : name == GL_AMBIENT || name == GL_DIFFUSE || name == GL_SPECULAR ||
                   name == GL_POSITION
               ? 4
               : 1;
  std::copy(v, v + n, out.begin());
}
extern "C" void lvk_glMaterialfv(GLenum face, GLenum name, const GLfloat *v) {
  if (face == GL_FRONT_AND_BACK) {
    auto n = name == GL_SHININESS ? 1 : 4;
    std::copy(v, v + n, state.floats[(GL_FRONT << 16) | name].begin());
    std::copy(v, v + n, state.floats[(GL_BACK << 16) | name].begin());
  } else
    std::copy(v, v + (name == GL_SHININESS ? 1 : 4),
              state.floats[(face << 16) | name].begin());
}
extern "C" void lvk_glFogf(GLenum name, GLfloat value) {
  state.floats[name] = {value, 0, 0, 0};
}
extern "C" void lvk_glFogi(GLenum name, GLint value) {
  state.floats[name] = {float(value), 0, 0, 0};
}
extern "C" void lvk_glFogfv(GLenum name, const GLfloat *v) {
  std::copy(v, v + (name == GL_FOG_COLOR ? 4 : 1), state.floats[name].begin());
}
extern "C" void lvk_glVertexPointer(GLint size, GLenum type, GLsizei stride,
                                    const GLvoid *pointer) {
  if (stride < 0)
    gap("negative guest vertex stride");
  state.arrays.pointer(0, size, type, false, stride, guest_pointer(pointer));
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
  unsigned index;
  if (cap == GL_VERTEX_ARRAY)
    index = 0;
  else if (cap == GL_NORMAL_ARRAY)
    index = 2;
  else if (cap == GL_COLOR_ARRAY)
    index = 3;
  else if (cap == GL_TEXTURE_COORD_ARRAY)
    index = 8 + state.client_texture - GL_TEXTURE0;
  else
    gap("unsupported guest client array");
  state.arrays.enable(index, true);
  state.enabled[cap] = true;
}
extern "C" void lvk_glDisableClientState(GLenum cap) {
  unsigned index;
  if (cap == GL_VERTEX_ARRAY)
    index = 0;
  else if (cap == GL_NORMAL_ARRAY)
    index = 2;
  else if (cap == GL_COLOR_ARRAY)
    index = 3;
  else if (cap == GL_TEXTURE_COORD_ARRAY)
    index = 8 + state.client_texture - GL_TEXTURE0;
  else
    gap("unsupported guest client array");
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
extern "C" void lvk_glAlphaFunc(GLenum compare, GLclampf reference) {
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

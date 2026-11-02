#include <array>
// Drive the real guest ABI without a game ELF, SDL surface, or OpenXR runtime.
// Only cabinet/profile/settings producers are replaced by inert test inputs.
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "vulkan_abi.h"
#include "vulkan_host.h"
extern "C" {
#include "game_profile.h"
#include "xr_settings.h"
unsigned char *g_guest_base=nullptr;
unsigned g_frame=0;
const LgGameProfile *game_profile() {
    static const auto profile=[] {LgGameProfile p{};p.title="Vulkan target test";p.width=p.height=64;p.vr.scale=1;p.vr.hud_lens=1.8107f;p.vr.hud_head_locked=1;return p;}();
    return &profile;
}
void xr_settings_load(LgXrSettings *p) {*p=LgXrSettings LG_XR_SETTINGS_DEFAULT;p->width=p->resolution=64;}
void xr_settings_save(const LgXrSettings *) {}
void host_vr_input(int,int,unsigned,int) {}
void host_vr_input2(int,int,int) {}
// No guest code: no state functions to watch.
uint32_t stereo_state_lo=~0u,stereo_state_hi=0;
uint32_t guest_symbol(const char *) {return 0;}
// gl_thread.c's glGen* on the game thread
uint32_t lvk_reserve_names(uint32_t n);
void lvk_adopt_buffer_names(uint32_t n,const uint32_t *names);
}
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

static std::string directory;
static void require(bool value,const char *message) {if(!value)throw std::runtime_error(message);}
static void cleanup() {std::error_code ec;std::filesystem::remove_all(directory,ec);}
static void pixel(const std::string &path,unsigned size) {
    std::ifstream in(path,std::ios::binary);std::string magic;unsigned w,h,maximum;
    in>>magic>>w>>h>>maximum;in.get();
    require(in&&magic=="P6"&&w==size&&h==size&&maximum==255,"missing or stale target readback");
    std::vector<unsigned char> rgb(size_t(w)*h*3);in.read(reinterpret_cast<char *>(rgb.data()),rgb.size());
    const auto at=(size_t(h/2)*w+w/2)*3;
    require(in&&rgb[at]==0&&rgb[at+1]==255&&rgb[at+2]==0,"retired/current target lost its green triangle");
}
static void hud_pixels(const std::string &path) {
    std::ifstream in(path,std::ios::binary);std::string magic;unsigned w,h,maximum;
    in>>magic>>w>>h>>maximum;in.get();
    require(in&&magic=="P6"&&w==64&&h==64&&maximum==255,"missing perspective HUD readback");
    std::vector<unsigned char> rgb(size_t(w)*h*3);in.read(reinterpret_cast<char *>(rgb.data()),rgb.size());
    require(bool(in),"truncated perspective HUD readback");
    for(unsigned x:{25u,32u,38u}) {
        const auto at=(32*w+x)*3;
        require(rgb[at]==0&&rgb[at+1]==(x==32?255:0)&&rgb[at+2]==0,
                "small perspective HUD array incorrectly classified as full-screen fill");
    }
}
static void layered_pixels(const std::string &path) {
    std::ifstream in(path,std::ios::binary);std::string magic;unsigned w,h,maximum;
    in>>magic>>w>>h>>maximum;in.get();
    require(in&&magic=="P6"&&w==64&&h==64&&maximum==255,"missing full-screen HUD readback");
    std::vector<unsigned char> rgb(size_t(w)*h*3);in.read(reinterpret_cast<char *>(rgb.data()),rgb.size());
    require(bool(in),"truncated full-screen HUD readback");
    // The movie sits on the virtual screen like the HUD, not stretched over
    // the eye: eye corners stay clear, the screen shows the movie in front of
    // its later black backing, and the finite text stays in front of both.
    auto pixel=[&](unsigned x,unsigned y){const auto *p=&rgb[3*(y*64+x)];return std::array<unsigned,3>{p[0],p[1],p[2]};};
    for(auto [x,y]:{std::pair{2u,2u},{61u,2u},{2u,61u},{61u,61u}})
        require(pixel(x,y)==std::array<unsigned,3>{0,0,0},"full-screen movie was stretched past its screen");
    for(auto [x,y]:{std::pair{32u,24u},{24u,32u},{40u,32u},{32u,40u}})
        require(pixel(x,y)==std::array<unsigned,3>{0,255,0},"full-screen movie lost its screen or layer depth");
    require(pixel(32,32)==std::array<unsigned,3>{255,255,255},"foreground HUD text fell behind the movie");
}
// Left and right quarter points of a 32x32 readback's middle row.
static std::array<std::array<unsigned,3>,2> sides(const std::string &path) {
    std::ifstream in(path,std::ios::binary);std::string magic;unsigned w,h,maximum;
    in>>magic>>w>>h>>maximum;in.get();
    require(in&&magic=="P6"&&w==32&&h==32&&maximum==255,"missing depth readback");
    std::vector<unsigned char> rgb(size_t(w)*h*3);in.read(reinterpret_cast<char *>(rgb.data()),rgb.size());
    auto at=[&](unsigned x){const auto *q=&rgb[3*(16*32+x)];return std::array<unsigned,3>{q[0],q[1],q[2]};};
    return {at(8),at(24)};
}
// The centre texel of a size x size readback.
static std::array<unsigned,3> sample(const std::string &path,unsigned size,unsigned x,unsigned y) {
    std::ifstream in(path,std::ios::binary);std::string magic;unsigned w,h,maximum;
    in>>magic>>w>>h>>maximum;in.get();
    require(in&&magic=="P6"&&w==size&&h==size&&maximum==255,"missing target readback");
    std::vector<unsigned char> rgb(size_t(w)*h*3);in.read(reinterpret_cast<char *>(rgb.data()),rgb.size());
    const auto *q=&rgb[(size_t(y)*w+x)*3];return {q[0],q[1],q[2]};
}
static std::array<unsigned,3> centre(const std::string &path,unsigned size) {return sample(path,size,size/2,size/2);}
static void triangle(unsigned size) {
    glViewport(0,0,size,size);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    glColor4f(0,1,0,1);glBegin(GL_TRIANGLES);
    glVertex3f(-1,-1,0);glVertex3f(1,-1,0);glVertex3f(0,1,0);glEnd();
}
int main() {
    try {
        directory=(std::filesystem::temp_directory_path()/
            ("lindbergh-target-abi-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))).string();
        std::filesystem::create_directories(directory);std::atexit(cleanup);
        SDL_SetMainReady();SDL_setenv("SDL_VIDEODRIVER","dummy",1);
        SDL_setenv("LINDBERGH_VULKAN_CACHE",(directory+"/cache.bin").c_str(),1);
        const auto *pack=std::getenv("LV_TEST_SHADER_PACK");
        SDL_setenv("LINDBERGH_VULKAN_SHADERS",pack?pack:LV_TEST_SHADER_PACK,1);
        require(lvk_host_init(nullptr,64,64),"guest Vulkan host initialization failed");
        glMatrixMode(GL_PROJECTION);glLoadIdentity();glMatrixMode(GL_MODELVIEW);glLoadIdentity();
        {
            // Names taken on the game thread (gl_thread.c's glGen*) are never
            // handed out again by the host's own glGen*, and an adopted buffer
            // name is the empty buffer glGenBuffers would have made.
            const auto first=lvk_reserve_names(3);
            GLuint buffer=0,texture=0;glGenBuffers(1,&buffer);glGenTextures(1,&texture);
            for(const GLuint name:{buffer,texture})require(name<first||name>=first+3,"a reserved name was handed out again");
            const std::array<uint32_t,3> names{first,first+1,first+2};lvk_adopt_buffer_names(3,names.data());
            GLint size=-1;glBindBuffer(GL_ARRAY_BUFFER,first+1);glGetBufferParameteriv(GL_ARRAY_BUFFER,GL_BUFFER_SIZE,&size);
            require(size==0,"an adopted buffer name is not an empty buffer");
            const float data[4]{};glBufferData(GL_ARRAY_BUFFER,sizeof data,data,GL_STATIC_DRAW);
            glGetBufferParameteriv(GL_ARRAY_BUFFER,GL_BUFFER_SIZE,&size);require(size==sizeof data,"an adopted buffer took no data");
            glBindBuffer(GL_ARRAY_BUFFER,0);GLuint gone[]={first,first+1,first+2,buffer};glDeleteBuffers(4,gone);glDeleteTextures(1,&texture);
        }
        for(unsigned cycle=0;cycle<18;++cycle) {
            const auto size=32+16*(cycle%3);const auto id=lvk_create_drawable(size,size);
            lvk_bind_drawable(id);
            if(cycle%3==0)lvk_destroy_drawable(id); // current deletion remains drawable
            triangle(size);
            if(cycle%3==1) {
                lvk_bind_drawable(0);lvk_destroy_drawable(id); // queued/unsubmitted use
                SDL_setenv("LINDBERGH_SHOT","",1);
            } else {
                const auto shot=directory+"/frame.ppm";
                std::filesystem::remove(shot);std::filesystem::remove(directory+"/frame.right.ppm");
                SDL_setenv("LINDBERGH_SHOT",shot.c_str(),1);
                SDL_setenv("LINDBERGH_SHOT_FRAME",std::to_string(g_frame).c_str(),1);
                SDL_setenv("LINDBERGH_VULKAN_SHOT_DRAWABLE",std::to_string(id).c_str(),1);
            }
            lvk_host_present();
            if(cycle%3!=1) {pixel(directory+"/frame.ppm",size);pixel(directory+"/frame.right.ppm",size);}
            lvk_bind_drawable(0);lvk_destroy_drawable(id);lvk_destroy_drawable(id);
            const auto unused=lvk_create_drawable(16,16);lvk_destroy_drawable(unused);
            ++g_frame;
        }
        // HOTD4 Special's HUD lens is perspective. A panel at z=-4 covers
        // only +/-0.294 NDC, although its pre-divide X extends past +/-1.
        // A fill classification skips its HUD screen-plane projection.
        const auto hud=lvk_create_drawable(64,64);lvk_bind_drawable(hud);
        glViewport(0,0,64,64);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        glMatrixMode(GL_PROJECTION);glLoadIdentity();
        float projection[16]{};projection[0]=projection[5]=1.8107f;
        projection[10]=-100.1f/99.9f;projection[11]=-1;projection[14]=-20.f/99.9f;
        glLoadMatrixf(projection);
        glMatrixMode(GL_MODELVIEW);glLoadIdentity();
        const float quad[][4]={{-.65f,-.65f,-4,1},{.65f,-.65f,-4,1},{-.65f,.65f,-4,1},{.65f,.65f,-4,1}};
        GLuint buffer;glGenBuffers(1,&buffer);glBindBuffer(GL_ARRAY_BUFFER,buffer);
        glBufferData(GL_ARRAY_BUFFER,sizeof quad,quad,GL_STATIC_DRAW);
        glEnableClientState(GL_VERTEX_ARRAY);glVertexPointer(4,GL_FLOAT,0,nullptr);
        glColor4f(0,1,0,1);glDrawArrays(GL_TRIANGLE_STRIP,0,4);
        glDisableClientState(GL_VERTEX_ARRAY);glBindBuffer(GL_ARRAY_BUFFER,0);glDeleteBuffers(1,&buffer);
        std::filesystem::remove(directory+"/frame.ppm");std::filesystem::remove(directory+"/frame.right.ppm");
        SDL_setenv("LINDBERGH_SHOT",(directory+"/frame.ppm").c_str(),1);
        SDL_setenv("LINDBERGH_SHOT_FRAME",std::to_string(g_frame).c_str(),1);
        SDL_setenv("LINDBERGH_VULKAN_SHOT_DRAWABLE",std::to_string(hud).c_str(),1);
        lvk_host_present();hud_pixels(directory+"/frame.ppm");hud_pixels(directory+"/frame.right.ppm");
        lvk_bind_drawable(0);lvk_destroy_drawable(hud);++g_frame;
        const auto movie=lvk_create_drawable(64,64);lvk_bind_drawable(movie);
        glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LEQUAL);glDepthMask(GL_TRUE);
        glViewport(0,0,64,64);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        auto layer=[](float depth,float span,float r,float g,float b) {
            const float half=depth*span/1.8107f;
            glColor4f(r,g,b,1);glBegin(GL_QUADS);
            glVertex3f(-half,-half,-depth);glVertex3f(half,-half,-depth);
            glVertex3f(half,half,-depth);glVertex3f(-half,half,-depth);glEnd();
        };
        layer(1.3f,1,0,1,0); // full movie: green through all eye corners
        layer(1.7f,1,0,0,0); // later black backing must stay behind it
        layer(1.1f,.25f,1,1,1); // finite HUD text must remain in front
        std::filesystem::remove(directory+"/frame.ppm");std::filesystem::remove(directory+"/frame.right.ppm");
        SDL_setenv("LINDBERGH_SHOT_FRAME",std::to_string(g_frame).c_str(),1);
        SDL_setenv("LINDBERGH_VULKAN_SHOT_DRAWABLE",std::to_string(movie).c_str(),1);
        lvk_host_present();layered_pixels(directory+"/frame.ppm");layered_pixels(directory+"/frame.right.ppm");
        lvk_bind_drawable(0);lvk_destroy_drawable(movie);++g_frame;
        glMatrixMode(GL_PROJECTION);glLoadIdentity();glMatrixMode(GL_MODELVIEW);glLoadIdentity();glDisable(GL_DEPTH_TEST);
        {
            // GL_COMBINE clamps each unit's result. A MODULATE combiner may take
            // the one-multiply path only when its inputs are already in [0,1]:
            // a colour of 4 x a half texel is 1 after unit 0, and half after a
            // second half texel (not 1, as an unclamped 2 x half would give).
            const auto target=lvk_create_drawable(32,32);lvk_bind_drawable(target);
            glViewport(0,0,32,32);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
            const unsigned char half[4]={128,128,128,255};GLuint tex[2];glGenTextures(2,tex);
            for(unsigned u=0;u<2;++u) {
                glActiveTexture(GL_TEXTURE0+u);glBindTexture(GL_TEXTURE_2D,tex[u]);glEnable(GL_TEXTURE_2D);
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
                glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,half);
                glTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,u==0?GL_COMBINE:GL_MODULATE);
            }
            glActiveTexture(GL_TEXTURE0);
            glTexEnvi(GL_TEXTURE_ENV,GL_COMBINE_RGB,GL_MODULATE);glTexEnvi(GL_TEXTURE_ENV,GL_COMBINE_ALPHA,GL_MODULATE);
            glColor4f(4,4,4,1);
            glBegin(GL_QUADS);glVertex3f(-1,-1,0);glVertex3f(1,-1,0);glVertex3f(1,1,0);glVertex3f(-1,1,0);glEnd();
            for(unsigned u=0;u<2;++u){glActiveTexture(GL_TEXTURE0+u);glDisable(GL_TEXTURE_2D);glTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,GL_MODULATE);}
            glActiveTexture(GL_TEXTURE0);glColor4f(1,1,1,1);
            std::filesystem::remove(directory+"/frame.ppm");std::filesystem::remove(directory+"/frame.right.ppm");
            SDL_setenv("LINDBERGH_SHOT_FRAME",std::to_string(g_frame).c_str(),1);
            SDL_setenv("LINDBERGH_VULKAN_SHOT_DRAWABLE",std::to_string(target).c_str(),1);
            lvk_host_present();
            for(const char *eye:{"/frame.ppm","/frame.right.ppm"}) {
                const auto c=centre(directory+eye,32);
                require(c[0]>=126&&c[0]<=130&&c[1]>=126&&c[1]<=130&&c[2]>=126&&c[2]<=130,"a GL_COMBINE unit's result was not clamped");
            }
            lvk_bind_drawable(0);lvk_destroy_drawable(target);glDeleteTextures(2,tex);++g_frame;
        }
        {
            // One program, two textures, two draws in one frame: each draw's
            // material (descriptor set) is its own. A cache that matched a
            // material on the program alone would paint both halves red.
            const auto target=lvk_create_drawable(32,32);lvk_bind_drawable(target);
            glViewport(0,0,32,32);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
            const unsigned char texels[2][4]={{255,0,0,255},{0,0,255,255}};GLuint tex[2];glGenTextures(2,tex);
            glActiveTexture(GL_TEXTURE0);glEnable(GL_TEXTURE_2D);glColor4f(1,1,1,1);
            for(unsigned i=0;i<2;++i) {
                glBindTexture(GL_TEXTURE_2D,tex[i]);
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
                glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,texels[i]);
            }
            for(unsigned frame=0;frame<2;++frame) { // the second frame reuses both cached materials
                glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
                for(unsigned i=0;i<2;++i) {
                    const float x0=i?0.f:-1.f,x1=i?1.f:0.f;glBindTexture(GL_TEXTURE_2D,tex[i]);
                    glBegin(GL_QUADS);glVertex3f(x0,-1,0);glVertex3f(x1,-1,0);glVertex3f(x1,1,0);glVertex3f(x0,1,0);glEnd();
                }
                std::filesystem::remove(directory+"/frame.ppm");std::filesystem::remove(directory+"/frame.right.ppm");
                SDL_setenv("LINDBERGH_SHOT_FRAME",std::to_string(g_frame).c_str(),1);
                SDL_setenv("LINDBERGH_VULKAN_SHOT_DRAWABLE",std::to_string(target).c_str(),1);
                lvk_host_present();
                for(const char *eye:{"/frame.ppm","/frame.right.ppm"}) {
                    require(sample(directory+eye,32,8,16)==std::array<unsigned,3>{255,0,0},"left draw's texture lost to another draw's material");
                    require(sample(directory+eye,32,24,16)==std::array<unsigned,3>{0,0,255},"right draw sampled another draw's texture");
                }
                ++g_frame;
            }
            glDisable(GL_TEXTURE_2D);lvk_bind_drawable(0);lvk_destroy_drawable(target);glDeleteTextures(2,tex);
        }
        {
            // Frame-local depth: a target that clears its depth at the start of
            // every frame keeps none after its frame's last depth pass - never
            // after an earlier one a later pass reads, whatever the frame's
            // shape. The near red half moves every frame, so depth left from an
            // earlier frame would show. A frame that loads its depth without
            // clearing it reads depth that was not kept: that frame alone may
            // be wrong, and from then on the depth is kept for good.
            const auto target=lvk_create_drawable(32,32),other=lvk_create_drawable(16,16);
            glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LEQUAL);glDepthMask(GL_TRUE);
            auto quad=[](float x0,float x1,float z){glBegin(GL_QUADS);glVertex3f(x0,-1,z);glVertex3f(x1,-1,z);glVertex3f(x1,1,z);glVertex3f(x0,1,z);glEnd();};
            auto pass_break=[&]{lvk_bind_drawable(other);glDisable(GL_DEPTH_TEST);triangle(16);glEnable(GL_DEPTH_TEST);lvk_bind_drawable(target);glViewport(0,0,32,32);};
            const std::array<unsigned,3> red{255,0,0},green{0,255,0},black{0,0,0};
            for(unsigned frame=0;frame<257;++frame) {
                const bool left=frame%2==0,third=frame>=125&&frame<128,unclear=frame==128||frame==256;
                lvk_bind_drawable(target);glViewport(0,0,32,32);glClearColor(0,0,0,1);glClearDepth(1);
                if(unclear) {
                    // Depth from the frame before: 0.2 or 0.5 everywhere, so a
                    // quad at 0.7 shows nowhere - if that depth was kept.
                    glClear(GL_COLOR_BUFFER_BIT);glColor4f(1,1,0,1);quad(-1,1,.4f);
                } else {
                    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
                    glColor4f(1,0,0,1);quad(left?-1:0,left?0:1,-.6f);         // window depth 0.2
                    pass_break();glColor4f(0,1,0,1);quad(-1,1,0);               // 0.5: behind the red half
                    if(third){pass_break();glColor4f(1,1,0,1);quad(-1,1,.4f);} // 0.7: behind both
                }
                const bool shot=frame>=120&&frame!=128;
                if(shot) {
                    std::filesystem::remove(directory+"/frame.ppm");std::filesystem::remove(directory+"/frame.right.ppm");
                    SDL_setenv("LINDBERGH_SHOT_FRAME",std::to_string(g_frame).c_str(),1);
                    SDL_setenv("LINDBERGH_VULKAN_SHOT_DRAWABLE",std::to_string(target).c_str(),1);
                } else SDL_setenv("LINDBERGH_SHOT_FRAME","0",1);
                lvk_host_present();
                if(shot) for(const char *eye:{"/frame.ppm","/frame.right.ppm"}) {
                    const auto got=sides(directory+eye);
                    if(unclear)require(got[0]==black&&got[1]==black,"depth was not kept again after a frame read it unkept");
                    else require(got[0]==(left?red:green)&&got[1]==(left?green:red),"a frame-local depth pass lost depth a later pass needed");
                }
                lvk_bind_drawable(0);++g_frame;
            }
            glDisable(GL_DEPTH_TEST);lvk_destroy_drawable(target);lvk_destroy_drawable(other);
        }
        const auto unfinished=lvk_create_drawable(32,32);lvk_bind_drawable(unfinished);
        triangle(32);lvk_destroy_drawable(unfinished);lvk_bind_drawable(0);
        std::puts("ok: real guest ABI pbuffer lifetime, perspective HUD bounds, on-screen layered movie/text depth, clamped GL_COMBINE results, frame-local depth, game-thread names, stereo readback and unfinished shutdown");
        return 0; // production atexit drain runs before fixture cleanup
    } catch(const std::exception &error) {std::cerr<<error.what()<<'\n';return 1;}
}

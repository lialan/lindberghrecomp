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
        glMatrixMode(GL_PROJECTION);glLoadIdentity();
        const auto unfinished=lvk_create_drawable(32,32);lvk_bind_drawable(unfinished);
        triangle(32);lvk_destroy_drawable(unfinished);lvk_bind_drawable(0);
        std::puts("ok: real guest ABI pbuffer lifetime, perspective HUD bounds, on-screen layered movie/text depth, stereo readback and unfinished shutdown");
        return 0; // production atexit drain runs before fixture cleanup
    } catch(const std::exception &error) {std::cerr<<error.what()<<'\n';return 1;}
}

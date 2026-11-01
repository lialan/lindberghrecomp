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
    static const auto profile=[] {LgGameProfile p{};p.title="Vulkan target test";p.width=p.height=64;p.vr.scale=1;return p;}();
    return &profile;
}
void xr_settings_load(LgXrSettings *p) {*p=LgXrSettings LG_XR_SETTINGS_DEFAULT;p->width=p->resolution=64;}
void xr_settings_save(const LgXrSettings *) {}
void host_vr_input(int,int,unsigned,int) {}
void host_vr_input2(int,int,int) {}
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
        const auto unfinished=lvk_create_drawable(32,32);lvk_bind_drawable(unfinished);
        triangle(32);lvk_destroy_drawable(unfinished);lvk_bind_drawable(0);
        std::puts("ok: real guest ABI current/unbound pbuffer destruction, queued draws, repeated resize/recreation, stereo readback and unfinished shutdown");
        return 0; // production atexit drain runs before fixture cleanup
    } catch(const std::exception &error) {std::cerr<<error.what()<<'\n';return 1;}
}

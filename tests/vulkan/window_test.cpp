#include "window.hpp"
#include <SDL.h>
#include <cstdio>
#include <unordered_map>
using namespace lindbergh::vk;
int main()
try {
    if(SDL_Init(SDL_INIT_VIDEO)) throw std::runtime_error(SDL_GetError());
    auto *window=SDL_CreateWindow("Vulkan direct presentation check",0,0,320,240,SDL_WINDOW_VULKAN|SDL_WINDOW_HIDDEN);
    if(!window) throw std::runtime_error(SDL_GetError());
    {
        auto device=Device::create(desktop_options(window));
        Window swapchain(*device,window,3);
        PipelineCache cache(*device,"");
        std::unordered_map<Target *,std::unique_ptr<Renderer>> renderers;
        FrameRing ring(*device);
        Matrices matrices{};
        for(auto &m:matrices.view_projection)for(unsigned i=0;i<4;++i)m[i*5]=1;
        Vertex triangle[]={{{-1,-1,0,1},{1,0,0,1}},{{1,-1,0,1},{0,1,0,1}},{{0,1,0,1},{0,0,1,1}}};
        RasterState raster;
        Submission last{};
        for(unsigned frame=0;frame<12;++frame) {
            SDL_PumpEvents();
            auto command=ring.begin(); auto &target=swapchain.acquire(ring);
            auto &renderer=renderers[&target];
            if(!renderer) {renderer=std::make_unique<Renderer>(*device,target,cache);renderer->prepare(raster);}
            renderer->begin(command,{0,0,0,1}); renderer->draw(ring,triangle,3,matrices,raster); renderer->end();
            last=swapchain.present(ring);
        }
        ring.wait(last);
        std::puts("ok: 12 direct Vulkan SDL swapchain presents with buffered acquire/render/present synchronization");
    }
    SDL_DestroyWindow(window);SDL_Quit();return 0;
} catch(const std::exception &e) {std::fprintf(stderr,"Vulkan presentation check failed: %s\n",e.what());return 1;}

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
        uint64_t expected_submissions=0;
        for(unsigned frame=0;frame<12;++frame) {
            SDL_PumpEvents();
            auto command=ring.begin(); auto &target=swapchain.acquire(ring);
            auto &renderer=renderers[&target];
            if(!renderer) {renderer=std::make_unique<Renderer>(*device,target,cache);renderer->prepare(raster);}
            renderer->begin(command,{0,0,0,1}); renderer->draw(ring,triangle,3,matrices,raster); renderer->end();
            // More partial submits than slots exercises acquire consumption,
            // image retention and slot reuse before a single final present.
            for (unsigned partial=0;partial<frame%5;++partial) {
                swapchain.flush(ring);++expected_submissions;
                command=ring.begin();
                renderer->begin(command,{0,0,0,1},true);
                renderer->draw(ring,triangle,3,matrices,raster);renderer->end();
            }
            last=swapchain.present(ring);++expected_submissions;
        }
        ring.wait(last);
        if (ring.submissions()!=expected_submissions) throw std::runtime_error("partial submit count changed");
        std::puts("ok: 12 Vulkan presents with asynchronous partial flushes and acquire semaphore reuse");
    }
    SDL_DestroyWindow(window);SDL_Quit();return 0;
} catch(const std::exception &e) {std::fprintf(stderr,"Vulkan presentation check failed: %s\n",e.what());return 1;}

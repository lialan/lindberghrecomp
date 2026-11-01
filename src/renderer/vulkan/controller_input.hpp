#pragma once
#include "stereo_scene.hpp"
#include "../../runtime/xr_controls.h"
#include "../../runtime/xr_overlay.h"
#include <mutex>
namespace lindbergh::vk {
struct ControllerSettings {
    int width=640,height=480;bool dual=false,auto_coins=true,left_handed=false,aim_on_hud=false;
    float aim_distance=15;
    bool show_gun=true;int laser=0,sight=XO_SIGHT_NONE,gun_model=XO_GUN_SMG;
};
struct ControllerSample {
    std::array<StereoPose,2> pose;std::array<int,2> valid{};LgXrControlFrame buttons{};
};
struct ControllerSnapshot {
    bool dual=false;std::array<StereoRay,2> ray;std::array<int,2> valid{};
    unsigned held=0;int coins=0;
};
struct ControllerHaptic {float amplitude=0;int milliseconds=0;};
// Renderer produces pose/button snapshots; the game thread consumes cabinet
// input and rays. No OpenXR/GL/SDL call, GPU wait or per-frame allocation.
class ControllerInput {
public:
    explicit ControllerInput(ControllerSettings settings);
    std::array<ControllerHaptic,2> update(const StereoScene &scene,const ControllerSample &sample,int64_t time,bool focused);
    bool consume(ControllerSnapshot &snapshot);
    const std::array<XoFrame,2> &overlay_frames() const {return overlay_;} // producer only
    int gun_hand() const { return controls_.gun_hand; } // producer only
private:
    std::array<XoFrame,2> overlay_{};
    ControllerSettings settings_;LgXrControls controls_=LG_XR_CONTROLS_DEFAULT;
    int64_t fire_start_[2]{};float fire_phase_[2]{1,1};
    std::mutex mutex_;ControllerSnapshot pending_;bool available_=false;
    unsigned pulses_=0,last_held_=0;
};
}

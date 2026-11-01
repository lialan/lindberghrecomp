#pragma once
#include "overlay.hpp"
#include "controller_input.hpp"
#include "../../runtime/xr_panel.h"
namespace lindbergh::vk {
// Render-worker UI state. Policy/painter are shared with GL; pictures allocate
// once and upload only on change, before any game render pass starts.
class SettingsPanel {
public:
    SettingsPanel(int width,int height,const char *title,bool fake=false);
    unsigned update(LgXrSettings &settings,StereoScene &scene,ControllerInput &input,
                    ControllerSample sample,int64_t time,bool focused,const std::vector<float> &rates,int64_t readout_time=0);
    void upload(Overlay &overlay,VkCommandBuffer cmd);
    const std::array<PanelDraw,2> &draws() const {return draws_;}
    const std::array<XoFrame,2> &guns() const {return guns_;}
    const std::array<ControllerHaptic,2> &haptics() const {return haptics_;}
    bool open() const {return panel_.open;}
private:
    XpPanel panel_{};int width_,height_,previous_menu_=0,previous_upper_[2]{},previous_trigger_[2]{};
    int64_t held_=0,fps_since_=0;unsigned fps_frames_=0;int fps_=-1;
    bool fake_,panel_dirty_=false,label_dirty_=false;
    std::string title_;std::vector<unsigned char> panel_pixels_,label_pixels_;
    std::array<PanelDraw,2> draws_{};std::array<XoFrame,2> guns_{};
    std::array<ControllerHaptic,2> haptics_{};
};
}

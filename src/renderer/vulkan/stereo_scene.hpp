#pragma once
#include "guest_state.hpp"
#include "../../runtime/stereo_projection.h"

namespace lindbergh::vk {
struct StereoPose {
    std::array<float,4> orientation{0,0,0,1};
    std::array<float,3> position{};
    std::array<float,4> tangents{1,1,1,1};
};
struct StereoSettings {
    float scale=1,screen=3,hud_lens=0,hud_pitch=0,zoom_lens=8;
    bool head_locked=false;
};
// Renderer-thread camera policy. Pose publication invalidates a bounded,
// reusable transform cache; identical draws never invert their camera again.
// No OpenXR/SDL/GL dependency: the host supplies anchored poses once per frame.
class StereoScene {
public:
    explicit StereoScene(StereoSettings settings);
    void publish(const std::array<StereoPose,2> &eyes);
    void invalidate();
    bool world_camera(const Matrix &projection) const;
    bool zoomed(const Matrix &projection) const;
    const Matrices &transforms(const Matrix &projection,int draw_class);
    void observe(const Matrix &projection,int draw_class);
    void finish_frame();
    size_t transform_builds() const { return builds_; }
    const std::array<float,2> &screen_lens() const { return lens_; }
private:
    StereoSettings settings_;
    bool valid_=false;
    std::array<Matrix,2> view_,hud_view_;
    std::array<std::array<float,4>,2> tangents_;
    std::array<float,2> lens_{1.732f,2.309f};
    struct Entry { Matrix projection;int kind;Matrices matrices; };
    std::vector<Entry> cache_;
    size_t last_=0,builds_=0;
    struct Lens {Matrix projection{};unsigned draws=0;};
    std::array<Lens,4> tally_{};
    float longest_=0;
    unsigned zoom_frames_=0;
    Matrices identity_{};
};
} // namespace lindbergh::vk

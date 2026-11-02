#pragma once
#include "guest_state.hpp"
#include "../../runtime/stereo_projection.h"

namespace lindbergh::vk {
struct StereoPose {
    std::array<float,4> orientation{0,0,0,1};
    std::array<float,3> position{};
    std::array<float,4> tangents{1,1,1,1};
};
struct StereoRay {std::array<float,3> origin{},direction{};int x=-100000,y=-100000;float reach=0;};
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
    void configure(StereoSettings settings);
    const std::array<float,4> &head_orientation() const {return eyes_[0].orientation;}
    const StereoSettings &settings() const {return settings_;}
    bool head_locked() const {return settings_.head_locked;}
    bool aim(const StereoPose &controller,int width,int height,bool on_hud,float aim_distance,StereoRay &ray) const;
    const Matrices &overlay_matrices() const {return overlay_;}
    const std::array<std::array<float,3>,2> &overlay_eyes() const {return eye_positions_;}
    bool tracking_valid() const { return valid_; }
    bool world_camera(const Matrix &projection) const;
    // A scope (or a close-up) held: a lens longer than the zoom limit for 6
    // frames running. The whole frame is then the arcade's picture on the
    // screen, as with stereo.c's flat frames.
    bool zoomed(const Matrix &projection) const;
    bool flat() const {return flat_;}
    // Ghost Squad Evolution's close-quarters event: its markers are 2D on the
    // screen, so the gun aims on the screen's plane.
    bool cqc() const {return cqc_>0;}
    const Matrices &transforms(const Matrix &projection,int draw_class);
    void observe(const Matrix &projection,int draw_class);
    // sniping/cqc: the game ran its sniping or close-quarters event this frame.
    void finish_frame(bool sniping=false,bool cqc=false);
    size_t transform_builds() const { return builds_; }
    // A flat frame is the arcade's whole monitor: the screen at the size it
    // has in play (lens 1.73), whatever lens the scope ramped through.
    const std::array<float,2> &screen_lens() const { return flat_?arcade_lens_:lens_; }
private:
    StereoSettings settings_;
    std::array<StereoPose,2> eyes_;
    Matrices overlay_{};std::array<std::array<float,3>,2> eye_positions_;
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
    // Frames left in which a sniping/close-quarters event counts as running.
    unsigned snipe_=0,cqc_=0;
    bool flat_=false;
    static constexpr std::array<float,2> arcade_lens_{1.732f,1.732f*4/3};
    // The sniping sections zoom in steps, some well below the zoom limit:
    // while one runs, any lens past 3.2 is its scope.
    float zoom_limit() const {return snipe_?3.2f:settings_.zoom_lens;}
    Matrices identity_{};
};
} // namespace lindbergh::vk

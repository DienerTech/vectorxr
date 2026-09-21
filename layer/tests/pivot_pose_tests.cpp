#include "depthxr/openxr_layer.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
constexpr double pi = 3.14159265358979323846;
constexpr XrSpaceLocationFlags valid_pose =
    XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
XrSpace Space(uintptr_t value) { return reinterpret_cast<XrSpace>(value); }
XrPosef Pose(double yaw = 0, XrVector3f position = {.3f, 1.6f, -.2f}) {
    return {{0, static_cast<float>(std::sin(yaw/2)), 0, static_cast<float>(std::cos(yaw/2))}, position};
}
XrVector3f Rotate(XrQuaternionf q, XrVector3f v) {
    // Independent vector rotation for expected values.
    const XrVector3f t{2*(q.y*v.z-q.z*v.y), 2*(q.z*v.x-q.x*v.z), 2*(q.x*v.y-q.y*v.x)};
    return {v.x+q.w*t.x+q.y*t.z-q.z*t.y,
            v.y+q.w*t.y+q.z*t.x-q.x*t.z,
            v.z+q.w*t.z+q.x*t.y-q.y*t.x};
}
XrQuaternionf Multiply(XrQuaternionf a, XrQuaternionf b) {
    return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,
            a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
            a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,
            a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};
}
XrPosef Compose(XrPosef a, XrPosef b) {
    auto p = Rotate(a.orientation,b.position);
    return {Multiply(a.orientation,b.orientation),{p.x+a.position.x,p.y+a.position.y,p.z+a.position.z}};
}
XrPosef Inverse(XrPosef p) {
    auto q = p.orientation; q.x=-q.x; q.y=-q.y; q.z=-q.z;
    return {q,Rotate(q,{-p.position.x,-p.position.y,-p.position.z})};
}
bool Close(XrVector3f a, XrVector3f b) {
    return std::abs(a.x-b.x)<1e-5 && std::abs(a.y-b.y)<1e-5 && std::abs(a.z-b.z)<1e-5;
}
bool Close(XrPosef a, XrPosef b) {
    const auto q=a.orientation, r=b.orientation;
    return Close(a.position,b.position) && std::abs(std::abs(q.x*r.x+q.y*r.y+q.z*r.z+q.w*r.w)-1)<1e-5;
}
void Expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

namespace depthxr {
class PivotPoseTestPeer {
public:
    enum class Mode { Snap, GlobalNudge, ProfileNudge, Continuous, Stepped };
    static void Prepare(Mode mode, double yaw, double pitch = 0) {
        auto& l=OpenXrLayer::Instance();
        l.ResetPivotActivationState();
        l.resolved_settings_={};
        l.resolved_settings_.pivotxr.enabled=true;
        PivotXrResolvedProfile profile;
        profile.behavior=mode==Mode::Snap ? PivotProfileBehavior::SnapViews : PivotProfileBehavior::EnhancedMotion;
        profile.response_mode=mode==Mode::Stepped ? PivotResponseMode::Stepped : PivotResponseMode::Continuous;
        l.resolved_settings_.pivotxr.profiles={profile};
        l.tracked_view_spaces_={Space(1)};
        l.next_locate_space_=nullptr;
        if (mode==Mode::Snap) {
            l.pivotxr_quick_view_active_=true;
            l.pivotxr_quick_view_transition_.current={yaw,pitch};
        } else if (mode==Mode::GlobalNudge) {
            l.pivotxr_manual_view_transition_.current={yaw,pitch};
        } else if (mode==Mode::ProfileNudge) {
            l.pivotxr_profile_view_transition_.current={yaw,pitch};
        } else {
            // Freeze the generated Continuous/Stepped angle on the release
            // envelope so both modes exercise their real shared pose path.
            l.pivotxr_activation_gain_=1;
            l.pivotxr_smoothed_extra_yaw_radians_=yaw;
            l.pivotxr_smoothed_extra_pitch_radians_=pitch;
        }
    }
    static XrPosef Apply(XrPosef raw, XrPosef* delta=nullptr, bool reverse=false,
                        XrSpace base=Space(2), XrSpaceLocationFlags flags=valid_pose, bool drive=true) {
        auto& l=OpenXrLayer::Instance();
        l.pivotxr_last_smoothing_wall_time_.reset(); // deterministic zero elapsed time
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        location.pose=reverse ? Inverse(raw) : raw; location.locationFlags=flags;
        l.ApplyPivotToLocatedSpace(reverse ? base : Space(1), reverse ? Space(1) : base,
            100, true, &location, nullptr, nullptr, delta, drive && !reverse);
        return location.pose;
    }
    static void Target(double yaw) {
        auto& l=OpenXrLayer::Instance();
        l.pivotxr_quick_view_pending_pose_={yaw};
        l.pivotxr_quick_view_pending_duration_seconds_=0;
        l.pivotxr_quick_view_retarget_pending_=true;
    }
    static void Origin(XrPosef pose) {
        auto& l=OpenXrLayer::Instance();
        l.pivotxr_origin_=OpenXrLayer::PivotOrigin{};
        l.pivotxr_origin_->pose=pose;
        l.pivotxr_translation_anchor_=OpenXrLayer::PivotTranslationAnchor{pose.position,Space(2)};
    }
    static void PositionOffset(XrVector3f meters) {
        auto& offset=OpenXrLayer::Instance().pivotxr_quick_view_transition_.current;
        offset.right_meters=meters.x; offset.up_meters=meters.y; offset.forward_meters=-meters.z;
    }
    static void Gain(double gain) { OpenXrLayer::Instance().pivotxr_activation_gain_=gain; }
    static bool HasAnchor() { return OpenXrLayer::Instance().pivotxr_translation_anchor_.has_value(); }
    static void FullyRelease(XrPosef pose) {
        auto& l=OpenXrLayer::Instance();
        l.pivotxr_activation_gain_=0;
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        location.pose=pose; location.locationFlags=valid_pose;
        l.ApplyPivotToLocatedSpace(Space(1),Space(2),100,false,&location,nullptr,nullptr,nullptr,false);
    }
    static void DriveMotion() {
        auto& l=OpenXrLayer::Instance();
        l.pivotxr_engaged_=true;
        auto& p=l.resolved_settings_.pivotxr.profiles[0];
        p.activation_ramp_seconds=0;
        p.smoothing=0;
        p.yaw_positive=p.yaw_negative={3,0,180};
        p.yaw_step_positive=p.yaw_step_negative={0,15,30,0,180};
        p.step_glide_mode=PivotStepGlideMode::Instant;
    }
    static void Transition(double from, double to, double elapsed) {
        auto& l=OpenXrLayer::Instance();
        l.pivotxr_quick_view_transition_.current.yaw_radians=from;
        RetargetPivotViewTransition({to},1,l.pivotxr_quick_view_transition_);
        UpdatePivotViewTransition(elapsed,l.pivotxr_quick_view_transition_);
    }
    inline static XrPosef relation=Pose(pi/3,{2,.4f,-3});
    inline static bool relation_valid=true;
    inline static bool runtime_failed=false;
    inline static int relation_calls=0;
    static XrResult XRAPI_CALL LocateRelation(XrSpace source, XrSpace target, XrTime, XrSpaceLocation* out) {
        ++relation_calls;
        Expect(source==Space(2) && target==Space(3),"Anchor relation used the wrong source/base");
        out->pose=relation;
        out->locationFlags=relation_valid ? valid_pose : 0;
        return runtime_failed ? XR_ERROR_RUNTIME_FAILURE : XR_SUCCESS;
    }
    static void AlternateSpace() { OpenXrLayer::Instance().next_locate_space_=&LocateRelation; }
    static XrPosef ApplyAlternateAttachment(XrPosef raw) {
        auto& l=OpenXrLayer::Instance();
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION}, reference{XR_TYPE_SPACE_LOCATION};
        location.pose=raw; location.locationFlags=valid_pose;
        reference.pose=relation; reference.locationFlags=valid_pose;
        l.ApplyPivotToLocatedSpace(Space(1),Space(3),100,true,&location,nullptr,nullptr,nullptr,false,&reference);
        return location.pose;
    }
};
}

int main() {
    using Peer=depthxr::PivotPoseTestPeer;
    try {
        for (auto mode : {Peer::Mode::Snap,Peer::Mode::GlobalNudge,Peer::Mode::ProfileNudge,
                          Peer::Mode::Continuous,Peer::Mode::Stepped}) {
            for (double yaw : {0.,pi/2,-pi/2,pi,-pi}) {
                Peer::Prepare(mode,yaw);
                auto center=Pose();
                auto selected=Peer::Apply(center);
                Expect(Close(selected,Pose(yaw,center.position)),"Pivot must turn in place at its initial anchor");
                for (XrVector3f lean : {XrVector3f{.01f,0,0},XrVector3f{0,.01f,0},XrVector3f{0,0,-.01f}}) {
                    auto raw=center;
                    raw.position={center.position.x+lean.x,center.position.y+lean.y,center.position.z+lean.z};
                    raw.orientation=Pose(.03).orientation;
                    XrPosef delta;
                    auto actual=Peer::Apply(raw,&delta);
                    auto expected_lean=Rotate(Pose(yaw).orientation,lean);
                    auto expected=Pose(yaw+.03,{center.position.x+expected_lean.x,center.position.y+expected_lean.y,center.position.z+expected_lean.z});
                    Expect(Close(actual,expected),"Lean must follow the rotated view in every Pivot mode");
                    Expect(Close(Compose(Inverse(delta),actual),raw),"Submission correction must undo rotation and translation");
                    Expect(Close(Peer::Apply(raw,nullptr,true),Inverse(actual)),"Reverse VIEW locate must agree with the forward pose");
                    Expect(Close(Peer::Apply(raw,nullptr,false,Space(2),valid_pose,false),actual),"Head-attached geometry must use the same anchor without advancing it");
                }
            }
        }
        for (double sign : {-1.,1.}) {
            Peer::Prepare(Peer::Mode::Snap,sign*pi);
            Peer::Target(-sign*pi/2);
            Expect(Close(Peer::Apply(Pose()),Pose(-sign*pi/2)),"Shortest-path snap yaw must not clamp at the 180-degree seam");
            for (double t : {0.,.25,.5,.75,1.}) {
                Peer::Prepare(Peer::Mode::Snap,0);
                Peer::Transition(sign*pi,sign*2*pi,t);
                const double eased=t*t*(3-2*t);
                Expect(Close(Peer::Apply(Pose()),Pose(sign*pi*(1+eased))),"Unwrapped return must rotate smoothly through the seam to identity");
            }
        }

        for (auto mode : {Peer::Mode::Snap,Peer::Mode::GlobalNudge,Peer::Mode::ProfileNudge,
                          Peer::Mode::Continuous,Peer::Mode::Stepped}) {
            for (double yaw : {0.,pi/2,pi}) {
                const double pitch=.7;
                Peer::Prepare(mode,yaw,pitch);
                const auto center=Pose(.4);
                const auto selected=Peer::Apply(center);
                auto raw=center; raw.position.x+=.02f; raw.position.y+=.01f; raw.position.z-=.03f;
                const auto actual=Peer::Apply(raw);
                auto relative=Compose(Inverse(selected),actual);
                const auto expected_local=Rotate(Inverse(center).orientation,{.02f,.01f,-.03f});
                Expect(Close(relative.position,expected_local),"Combined pitch/yaw must preserve view-relative lean");
            }
        }

        // Set Origin supplies the anchor even if the user leans before enabling.
        Peer::Prepare(Peer::Mode::Snap,pi);
        Peer::Origin(Pose());
        auto leaned=Pose(); leaned.position.x+=.02f;
        auto expected=Pose(pi); expected.position.x-=.02f;
        Expect(Close(Peer::Apply(leaned),expected),"Captured origin must win over first engaged head position");
        Peer::PositionOffset({.1f,.2f,-.3f});
        expected.position.x+=.1f; expected.position.y+=.2f; expected.position.z-=.3f;
        Expect(Close(Peer::Apply(leaned),expected),"Snap position offsets must remain additive after rotated lean");
        Peer::FullyRelease(leaned);
        Expect(Peer::HasAnchor(),"Explicit origin anchor must survive disengagement");

        Peer::Prepare(Peer::Mode::GlobalNudge,pi);
        Peer::Apply(Pose(),nullptr,false,Space(2),XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
        Expect(!Peer::HasAnchor(),"Invalid position must not seed the seated anchor");
        Peer::Apply(Pose(),nullptr,false,Space(2),0);
        Expect(!Peer::HasAnchor(),"Invalid orientation must not seed the seated anchor");
        Peer::Apply(Pose());
        Peer::Apply(leaned,nullptr,false,Space(2),XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
        expected=Pose(pi); expected.position.x-=.02f;
        Expect(Close(Peer::Apply(leaned),expected),"Tracking recovery must retain the last valid anchor");
        Peer::FullyRelease(leaned);
        Expect(!Peer::HasAnchor(),"Automatic anchor must reset after full release");
        Expect(Close(Peer::Apply(leaned).position,leaned.position),"Reengagement must capture the new seated position without orbiting");

        for (auto mode : {Peer::Mode::Continuous,Peer::Mode::Stepped}) {
            Peer::Prepare(mode,pi);
            Peer::Apply(Pose());
            Peer::Gain(.5);
            auto at_half=Peer::Apply(leaned);
            expected=Pose(pi/2); expected.position.z-=.02f;
            Expect(Close(at_half,expected),"Release envelope must ease translation and rotation together");
            Peer::Gain(0);
            Expect(Close(Peer::Apply(leaned),leaned),"Zero gain must leave no translation behind");

            Peer::Prepare(mode,0); Peer::DriveMotion(); Peer::Apply(Pose());
            auto turned=leaned; turned.orientation=Pose(pi/6+.001).orientation;
            auto driven=Peer::Apply(turned);
            auto local=Compose(Inverse(driven),Pose(0,Pose().position)).position;
            auto original_local=Compose(Inverse(turned),Pose(0,Pose().position)).position;
            Expect(Close(local,original_local),"Live Continuous/Stepped updates must rotate lean with their generated angle");
            Expect(!Close(driven,turned),"Motion fixture must actually generate extra rotation");
        }

        Peer::Prepare(Peer::Mode::GlobalNudge,pi/2);
        Peer::Apply(Pose());
        auto source_result=Peer::Apply(leaned);
        Peer::AlternateSpace();
        auto alternate_raw=Compose(Peer::relation,leaned);
        Expect(Close(Peer::Apply(alternate_raw,nullptr,false,Space(3)),Compose(Peer::relation,source_result)),
               "Anchor must be converted into translated/yawed reference spaces");
        const int calls=Peer::relation_calls;
        Expect(Close(Peer::ApplyAlternateAttachment(alternate_raw),Compose(Peer::relation,source_result)),
               "Attachment locate must accept the reference relation obtained outside the lock");
        Expect(Peer::relation_calls==calls,"Attachment application must not call the runtime under the lock");
        for (bool failed : {false,true}) {
            Peer::relation_valid=false; Peer::runtime_failed=failed;
            Expect(Close(Peer::Apply(alternate_raw,nullptr,false,Space(3)).position,alternate_raw.position),
                   "Failed reference conversion must not apply an anchor from the wrong space");
        }
        std::cout << "Pivot pose regression tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}

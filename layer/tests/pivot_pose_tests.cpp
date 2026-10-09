#include "depthxr/openxr_layer.h"

#include <cmath>
#include <cstring>
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
                        XrSpace base=Space(2), XrSpaceLocationFlags flags=valid_pose, bool drive=true,
                        XrTime time=100) {
        auto& l=OpenXrLayer::Instance();
        l.pivotxr_last_smoothing_wall_time_.reset(); // deterministic zero elapsed time
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        location.pose=reverse ? Inverse(raw) : raw; location.locationFlags=flags;
        l.ApplyPivotToLocatedSpace(reverse ? base : Space(1), reverse ? Space(1) : base,
            time, true, &location, nullptr, nullptr, delta, drive && !reverse);
        return location.pose;
    }
    inline static XrEventDataReferenceSpaceChangePending pending_change{XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING};
    static XrResult XRAPI_CALL PollChange(XrInstance, XrEventDataBuffer* buffer) {
        std::memcpy(buffer,&pending_change,sizeof(pending_change));
        return XR_SUCCESS;
    }
    static void TrackLocal(XrSpace space) {
        auto& l=OpenXrLayer::Instance();
        l.tracked_local_spaces_={space}; l.tracked_stage_spaces_.clear();
    }
    // Delivers a runtime recenter through the layer's xrPollEvent hook.
    static void Recenter(XrReferenceSpaceType type, XrTime change_time) {
        auto& l=OpenXrLayer::Instance();
        l.active_session_=reinterpret_cast<XrSession>(uintptr_t{0x99});
        pending_change.session=l.active_session_;
        pending_change.referenceSpaceType=type;
        pending_change.changeTime=change_time;
        l.next_poll_event_=&PollChange;
        XrEventDataBuffer buffer{XR_TYPE_EVENT_DATA_BUFFER};
        Expect(l.PollEvent(XR_NULL_HANDLE,&buffer)==XR_SUCCESS &&
               buffer.type==XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING,"Event hook must pass events through unchanged");
        l.next_poll_event_=nullptr;
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
    static void Expanded(bool motion_first=true,bool stepped=false,bool coexist=true) {
        Prepare(Mode::Continuous,0);
        auto& l=OpenXrLayer::Instance(); l.resolved_settings_.core.enabled=true;
        auto& motion=l.resolved_settings_.pivotxr.profiles[0];
        motion.name="Flight";motion.apply_motion_to_snap_views=coexist;motion.activation_ramp_seconds=0;motion.smoothing=0;
        motion.yaw_positive=motion.yaw_negative={2,0,180};motion.pitch_positive=motion.pitch_negative={2,0,180};
        motion.response_mode=stepped?PivotResponseMode::Stepped:PivotResponseMode::Continuous;
        motion.yaw_step_positive=motion.yaw_step_negative={0,10,10,0,180};motion.step_glide_mode=PivotStepGlideMode::Instant;
        InputBinding input;input.type=InputBindingType::Keyboard;input.chord={"F1"};
        motion.activation_bindings={{PivotActivationBehavior::Toggle,input}};
        PivotXrResolvedProfile snap;snap.name="Views";snap.behavior=PivotProfileBehavior::SnapViews;
        PivotQuickView view;view.name="Right";view.yaw_degrees=90;view.transition_seconds=0;
        input.chord={"F2"};view.activation_bindings={{PivotActivationBehavior::Hold,input}};
        snap.view_controls.quick_views={view};
        l.resolved_settings_.pivotxr.profiles.push_back(snap);
        auto exclusive=l.resolved_settings_.pivotxr.profiles[0];exclusive.name="Exclusive";exclusive.apply_motion_to_snap_views=false;
        exclusive.activation_bindings[0].binding.chord={"F3"};
        l.resolved_settings_.pivotxr.profiles.push_back(exclusive);
        // Prime all real input structures, then provide cached inputs without
        // calling hardware polling. This tests actual production arbitration.
        l.pivotxr_binding_last_poll_time_.reset();l.IsPivotXrActive();
        l.pivotxr_binding_last_poll_time_=std::chrono::steady_clock::now()+std::chrono::hours(1);
        if(motion_first) { l.EngagePivotMotionProfile(0);l.pivotxr_profile_input_states_[0].toggle_latched=true; }
    }
    static void Key(size_t owner,bool down) {
        auto& l=OpenXrLayer::Instance();
        if(owner==1)l.pivotxr_profile_input_states_[1].quick_views[0].activation[0].down_cached=down;
        else l.pivotxr_profile_input_states_[owner].activation[0].down_cached=down;
        l.IsPivotXrActive();
    }
    static void SnapDuration(double seconds) {OpenXrLayer::Instance().resolved_settings_.pivotxr.profiles[1].view_controls.quick_views[0].transition_seconds=seconds;}
    static void Snap() {OpenXrLayer::Instance().ActivatePivotQuickView(1,0);}
    static void Motion() {OpenXrLayer::Instance().EngagePivotMotionProfile(0);}
    static void Exit(double duration=0) {OpenXrLayer::Instance().ReleasePivotQuickView(duration);}
    static bool Engaged() {return OpenXrLayer::Instance().pivotxr_engaged_;}
    static size_t Owner() {return OpenXrLayer::Instance().pivotxr_active_profile_index_;}
    static bool SnapActive() {return OpenXrLayer::Instance().pivotxr_quick_view_active_;}
    static OsdSnapshot Status() {return OpenXrLayer::Instance().BuildOsdSnapshot();}
    static void AlwaysHold() {
        auto& l=OpenXrLayer::Instance();auto& p=l.resolved_settings_.pivotxr.profiles[0];
        p.always_active=true;p.activation_bindings[0].behavior=PivotActivationBehavior::Hold;
        l.pivotxr_profile_input_states_[0].toggle_latched=false;
    }
    static void Position() {auto& p=OpenXrLayer::Instance().resolved_settings_.pivotxr.profiles[0].position_assist;p.enabled=true;p.yaw={10,4,0,0,30};}
    static XrPosef Advance(XrPosef pose,double seconds) {
        auto& l=OpenXrLayer::Instance();
        l.pivotxr_last_smoothing_wall_time_=std::chrono::steady_clock::now()-std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds));
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};location.pose=pose;location.locationFlags=valid_pose;
        l.ApplyPivotToLocatedSpace(Space(1),Space(2),100,true,&location,nullptr,nullptr,nullptr,true);
        return location.pose;
    }
    static double Yaw(XrPosef pose) {return std::atan2(2*(pose.orientation.w*pose.orientation.y+pose.orientation.x*pose.orientation.z),1-2*(pose.orientation.y*pose.orientation.y+pose.orientation.x*pose.orientation.x));}
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

        // A runtime recenter moves LOCAL under the stored anchor: re-anchor at
        // the change time instead of orbiting around the old seated point.
        for (bool explicit_origin : {false,true}) {
            Peer::Prepare(Peer::Mode::GlobalNudge,pi/2);
            Peer::TrackLocal(Space(2));
            if (explicit_origin) Peer::Origin(Pose());
            Peer::Apply(Pose());
            const auto recentered=Pose(0,{0,1.6f,0});
            Peer::Recenter(XR_REFERENCE_SPACE_TYPE_STAGE,50);
            Expect(!Close(Peer::Apply(recentered).position,recentered.position),
                   "A STAGE change must not re-anchor a LOCAL seated position");
            Peer::Recenter(XR_REFERENCE_SPACE_TYPE_LOCAL,200);
            Expect(!Close(Peer::Apply(recentered,nullptr,false,Space(2),valid_pose,true,150).position,recentered.position),
                   "Frames before the change time still belong to the old origin");
            Expect(Close(Peer::Apply(recentered,nullptr,false,Space(2),valid_pose,true,200).position,recentered.position),
                   "A recenter must re-anchor the seated position at its change time");
            auto leaned_after=recentered; leaned_after.position.x+=.02f;
            Expect(Close(Peer::Apply(leaned_after,nullptr,false,Space(2),valid_pose,true,250).position,{0,1.6f,-.02f}),
                   "Lean after a recenter must rotate about the new seated position");
        }

        Peer::Prepare(Peer::Mode::GlobalNudge,pi/2);
        Peer::TrackLocal(XR_NULL_HANDLE);
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
        for(bool stepped:{false,true}) {
            Peer::Expanded(true,stepped);Peer::Apply(Pose());Peer::Snap();
            Expect(Close(Peer::Apply(Pose()),Pose(-pi/2)),"Stacked snap must land on named pose");
            auto output=Peer::Apply(Pose(11*pi/180));
            const double expected_yaw=-pi/2+(stepped?21:22)*pi/180;
            Expect(std::abs(Peer::Yaw(output)-expected_yaw)<1e-5,"Motion must persist inside Snap Views");
            auto status=Peer::Status();Expect(status.pivot.find("Active")!=std::string::npos && status.pivot_secondary.find("Stacked")!=std::string::npos,"OSD omits applied stacked state");
            Peer::Exit(.2);Peer::Advance(Pose(11*pi/180),.3);
            Expect(Peer::Engaged() && Peer::Owner()==0,"Snap release must retain motion owner");
            Expect(Peer::Status().pivot_secondary.find("Stacked")==std::string::npos,"OSD retains stale stack after return");
        }
        // Returning from a snap entered off-center must not abruptly swap
        // local assistance for normal-origin assistance, even with zero filtering.
        for(bool stepped:{false,true}) {
            Peer::Expanded(true,stepped);Peer::Apply(Pose(.25));Peer::Snap();Peer::Apply(Pose(.25));
            const auto displayed=Peer::Apply(Pose(.45));Peer::Exit(.2);
            Expect(Close(Peer::Apply(Pose(.45)),displayed),"Snap return must preserve its initial pose while rebasing motion");
        }
        Peer::Expanded(false);Peer::Snap();Peer::Apply(Pose(.2));
        const auto before=Peer::Apply(Pose(.2));Peer::Motion();
        Expect(Close(Peer::Apply(Pose(.2)),before),"Enabling motion inside snap must preserve current pose");
        Expect(std::abs(Peer::Yaw(Peer::Apply(Pose(.3)))-(-pi/2+.2))<1e-5,"Snap-first motion must amplify around captured physical reference");
        Peer::Expanded();Peer::Key(1,true);Peer::Apply(Pose());
        Peer::Key(0,true);Expect(!Peer::Engaged() && Peer::SnapActive(),"Disabling stacked motion must leave snap selected");
        Peer::Apply(Pose());Expect(Peer::Status().pivot_secondary.find("Stacked")==std::string::npos,"Off motion must not be labelled stacked");
        Peer::Key(0,false);Peer::Key(0,true);Peer::Apply(Pose());
        Peer::Key(2,true);Expect(Peer::Owner()==2 && !Peer::SnapActive(),"Incompatible profile must dismiss snap");
        Peer::Key(1,false);Expect(Peer::Owner()==2 && Peer::Engaged(),"Stale snap Hold release must not resurrect outgoing profile");
        Peer::Expanded();Peer::SnapDuration(1);Peer::Apply(Pose());Peer::Key(1,true);Peer::Apply(Pose());
        Peer::Advance(Pose(),.3);Peer::Key(0,true);Peer::Apply(Pose(.1));Peer::Key(0,false);
        const auto reenable_before=Peer::Apply(Pose(.1));Peer::Key(0,true);
        Expect(Close(Peer::Apply(Pose(.1)),reenable_before),"Re-enabling motion must preserve the in-progress snap pose");
        Expect(std::abs(Peer::Yaw(Peer::Advance(Pose(.1),1))-(-pi/2+.1))<1e-4,"Re-enabling motion must not cancel an entering snap transition");
        Peer::Expanded();Peer::AlwaysHold();Peer::Key(0,true);Peer::Key(2,true);Peer::Key(0,false);
        Expect(Peer::Owner()==2 && Peer::Engaged(),"Outgoing Always On Hold release must not override a newly selected profile");
        Peer::Expanded(true,false,false);Peer::Apply(Pose());Peer::Snap();Peer::Apply(Pose());
        Expect(std::abs(Peer::Yaw(Peer::Apply(Pose(.1)))-(-pi/2+.1))<1e-5,"Non-opt-in snap must preserve 1:1 behavior");
        Peer::Exit();Expect(Peer::Owner()==0 && Peer::Engaged(),"Legacy snap must restore previous motion");
        Peer::Expanded();Peer::Position();Peer::Apply(Pose());
        const auto translated=Peer::Advance(Pose(-pi/6),1);
        Expect(std::abs(translated.position.x-(Pose().position.x+.1))<1e-4 && std::abs(translated.position.y-(Pose().position.y+.04))<1e-4,"Live Position Assist must add expected origin-relative displacement");
        auto center_position=Peer::Advance(Pose(),1);
        Expect(Close(center_position.position,Pose().position),"Position Assist must return to neutral");
        std::cout << "Pivot pose regression tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}

#pragma once

// Stabilized mirror window (modules.mirror): the math of a desktop window
// that shows one eye of the frame the game submits with the head's shake
// smoothed out, for streaming and recording. The headset's frame is never
// changed: the window is drawn from the submitted image, re-aimed by a
// rotation (a homography in the view's tangent space).
//
// No graphics dependency: the window is mirror_window.h, the draw is
// SubmittedImageGpu::DrawMirror (submitted_image.h).

#include <array>
#include <cstdint>
#include <string_view>

#include <openxr/openxr.h>

#include "depthxr/settings.h"

namespace depthxr {

struct MirrorQuat {
    double x{0.0};
    double y{0.0};
    double z{0.0};
    double w{1.0};
};

MirrorQuat MirrorNormalize(const MirrorQuat& q);
MirrorQuat MirrorMultiply(const MirrorQuat& a, const MirrorQuat& b);
MirrorQuat MirrorConjugate(const MirrorQuat& q);
// Along the shorter arc; t = 0 gives a, 1 gives b.
MirrorQuat MirrorSlerp(const MirrorQuat& a, const MirrorQuat& b, double t);
// The angle in radians between two orientations, 0..pi.
double MirrorAngle(const MirrorQuat& a, const MirrorQuat& b);
// The orientation with the same heading and pitch and no roll about the view
// direction, the space's +Y being up. Looking straight up or down (within
// about 3 degrees) there is no level: the orientation is returned as it is.
MirrorQuat MirrorLevel(const MirrorQuat& q);

// A low-pass filter of the view's orientation whose cutoff rises with how
// fast the head turns (a one-euro filter on the rotation), so shake is
// smoothed and a deliberate turn is followed with little lag.
class MirrorOrientationFilter {
  public:
    // `strength` 0..100: 0 follows the head exactly, 100 smooths most.
    // `seconds` since the last Update; a first call, a step back in time or a
    // gap of more than half a second starts again at `actual`.
    MirrorQuat Update(const MirrorQuat& actual, double seconds, double strength);
    void Reset() { primed_ = false; }
    bool Primed() const { return primed_; }
    // The clamp's correction: the filter goes on from here.
    void Set(const MirrorQuat& smoothed) { smoothed_ = smoothed; }

    // The cutoff in Hz at rest for a strength (0 = none: no filtering).
    static double RestCutoff(double strength);

  private:
    MirrorQuat smoothed_;
    MirrorQuat last_actual_;
    double speed_{0.0}; // rad/s, low-passed
    bool primed_{false};
};

// The part of the view's image the window shows, in the view's tangent space
// (x right, y up, at distance 1): centred in the field of view, of the
// window's aspect, as large as fits and then smaller by the margin.
struct MirrorCrop {
    double center_x{0.0};
    double center_y{0.0};
    double half_width{0.0};
    double half_height{0.0};
};
// `aspect` is width over height; `margin_percent` 0..40 (clamped).
MirrorCrop MirrorCropFor(const XrFovf& fov, double aspect, double margin_percent);

// Row-major 3x3: (x, y, 1) with x right and y up over the window, each
// -1..1, to (u*w, v*w, w), u right and v down over the view's rectangle,
// each 0..1.
using MirrorHomography = std::array<float, 9>;
// `correction`: the rotation from the window's (smoothed) view to the frame's
// actual view, actual^-1 * smoothed.
MirrorHomography MirrorHomographyFor(const XrFovf& fov, const MirrorCrop& crop, const MirrorQuat& correction);
// Where a window point lands in the view's rectangle; false behind the view.
bool MirrorMap(const MirrorHomography& h, double x, double y, double& u, double& v);
// Whether the whole window is inside the view's image (its corners are).
bool MirrorFilled(const MirrorHomography& h);

// What shaders/mirror.hlsl reads (80 bytes; Direct3D 12 root constants).
struct MirrorShaderConstants {
    float rows[3][4];             // the homography's rows in xyz
    std::int32_t source_offset[2]; // the view's rectangle in its image
    std::int32_t source_size[2];
    float target_size[2]; // the window's picture in pixels
    std::uint32_t flags;  // kMirrorFlagEncodeSrgb
    // 0..1: how far apart the four samples of a window pixel are taken, a
    // quarter pixel at 1. 0 where the picture is not shrunk (one texel a
    // pixel or fewer: the texels as they are), 1 from two texels a pixel.
    float spread;
};
constexpr std::uint32_t kMirrorFlagEncodeSrgb = 1u;
// `encode_srgb`: the image's texels are linear (an sRGB format's view, half
// floats) and the window's are display-encoded.
MirrorShaderConstants PackMirrorConstants(const MirrorHomography& h, std::int32_t offset_x, std::int32_t offset_y,
                                          std::int32_t width, std::int32_t height, std::uint32_t target_width,
                                          std::uint32_t target_height, bool encode_srgb);

// The whole view at its own shape: the window's corners are the view's.
MirrorHomography MirrorWholeView();

// A quad views eye that was not composed into one image (the runtime
// composes its four views, or Frame Generation does): where the eye's focus
// view lies in the eye's own view, and the band inside its edge over which
// it is blended in, both in that view's uv (u right, v down, 0..1), as the
// quad views compositor and Frame Generation place and blend it
// (BuildFrameGenComposeEye).
struct MirrorFocusPlace {
    std::array<float, 4> rect{};    // left, top, right, bottom
    std::array<float, 2> feather{}; // across, down
};
// What the focus pixel shader of shaders/mirror.hlsl reads after
// MirrorShaderConstants (48 bytes; Direct3D 11's buffer holds both).
struct MirrorFocusShaderConstants {
    float rect[4];
    std::int32_t offset[2]; // the focus view's rectangle in its image
    std::int32_t size[2];
    float feather[2];
    float spread; // as MirrorShaderConstants::spread, for the focus view's texels
    float unused;
};
struct MirrorFocusedShaderConstants {
    MirrorShaderConstants eye;
    MirrorFocusShaderConstants focus;
};
MirrorFocusShaderConstants PackMirrorFocusConstants(const MirrorHomography& h, const MirrorFocusPlace& place,
                                                    std::int32_t offset_x, std::int32_t offset_y, std::int32_t width,
                                                    std::int32_t height, std::uint32_t target_width,
                                                    std::uint32_t target_height);

// One frame's aim, from the frame's actual orientation.
struct MirrorAim {
    MirrorHomography homography{};
    // How much of the wanted correction was applied, 1 = all: less when the
    // margin could not cover it (the picture then follows the head).
    double applied{1.0};
};

class MirrorStabilizer {
  public:
    // `display_time` is the frame's (ns). Smoothing 0 without the level
    // horizon gives the plain crop: no rotation at all. `aspect`: the
    // picture's width over height where it is not the settings' (a window
    // that fills a monitor); 0 for the settings'.
    MirrorAim Update(const XrPosef& pose, const XrFovf& fov, XrTime display_time, const MirrorSettings& settings,
                     double aspect = 0.0);
    void Reset();

  private:
    MirrorOrientationFilter filter_;
    XrTime last_time_{0};
};

// At most `frame_rate` window frames a second out of the game's frames.
class MirrorPacer {
  public:
    bool Due(XrTime display_time, int frame_rate);
    void Reset() { next_ = 0; }

  private:
    XrTime next_{0};
};

// A Vulkan game's picture on its way to the window: the game's queue copies
// the eye into one of two images both graphics APIs share, and the mirror's
// own Direct3D 12 queue draws the window from the one copied last. Neither
// queue is ever waited for: a copy is started only into an image nothing
// reads or writes, one copy at a time, and an image is shown only once its
// copy is done (the game's fence, polled) and until the draw from it is
// done (the mirror's fence, polled). This is the bookkeeping, with no
// graphics in it.
class MirrorRelay {
  public:
    static constexpr int kSlots = 2;
    enum class Slot {
        kFree,
        kCopying, // the game's queue is writing it
        kReady,   // holds the newest picture, not shown yet
        kShowing, // the mirror's queue is reading it
    };
    // The image the next copy goes into; -1 while a copy is under way or no
    // image is free.
    int ToCopy() const;
    void CopyStarted(int slot);
    // The image being copied into; -1 for none.
    int Copying() const;
    // Its copy is done: it holds the newest picture, and an older one that
    // was never shown is free again.
    void CopyDone(int slot);
    // Its copy never ran (the submission was refused): free again.
    void CopyFailed(int slot);
    // The image to show; -1 for none.
    int ToShow() const;
    void ShowStarted(int slot);
    // The mirror's queue is done with every draw so far.
    void ShowsDone();
    // Nothing reads or writes any image (they may be made anew).
    bool Idle() const;
    // Forgets pictures not shown yet (the images are to be made anew).
    void DropReady();
    void Reset();
    Slot State(int slot) const { return slots_[slot]; }

  private:
    Slot slots_[kSlots]{};
};

double MirrorAspectRatio(MirrorAspect aspect);
// The window's picture size for a width: the height follows the aspect.
void MirrorPictureSize(int width, MirrorAspect aspect, int& out_width, int& out_height);
// The index of the eye's view among `view_count` (2 stereo, 4 quad views
// with the stereo pair first); -1 with none.
int MirrorViewIndex(MirrorEye eye, std::uint32_t view_count);

// What the session's status says (the app's mirrorState).
enum class MirrorState {
    kOff,         // not on for this game, or the suite is off
    kPending,     // on, no frame shown yet
    kActive,      // showing frames
    kHidden,      // on, the window closed or hidden with the binding
    kUnsupported, // this session's images cannot be shown (the reason says why)
    kFailed,
};
std::string_view MirrorStateName(MirrorState state);

} // namespace depthxr

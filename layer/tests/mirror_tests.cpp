// The stabilized mirror window (mirror.h, mirror_window.h, submitted_image.h:
// DrawMirror). No argument: the CPU half (the orientation filter, the crop,
// the homography, the margin's clamp, the level horizon, pacing, the shader
// constants, the window's placement and the window itself, never put on the
// desktop). `d3d11` and `d3d12`: the draw on the software adapter (WARP),
// never a hardware one, against a CPU reference, and frames presented to a
// window that is never put on the desktop. `measure` (by hand, not a ctest
// test): the cost of a frame on the real adapter.

#include <windows.h>
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <dxgidebug.h>
#include <wrl/client.h>
#include <openxr/openxr_platform.h>

#include "depthxr/mirror.h"
#include "depthxr/mirror_window.h"
#include "depthxr/submitted_image.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace depthxr;
using Microsoft::WRL::ComPtr;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

int checks = 0;

void Expect(bool ok, const std::string& why) {
    ++checks;
    static const bool trace = std::getenv("VECTORXR_TEST_TRACE") != nullptr;
    if (trace) std::cerr << checks << ": " << why << std::endl;
    if (!ok) {
        std::cerr << "FAILED: " << why << '\n';
        std::exit(1);
    }
}

bool Near(double a, double b, double tolerance = 1e-9) {
    return std::fabs(a - b) <= tolerance;
}

MirrorQuat AxisAngle(double x, double y, double z, double radians) {
    const double s = std::sin(radians / 2.0);
    return MirrorQuat{x * s, y * s, z * s, std::cos(radians / 2.0)};
}

XrPosef Pose(const MirrorQuat& q) {
    XrPosef pose{};
    pose.orientation = {static_cast<float>(q.x), static_cast<float>(q.y), static_cast<float>(q.z),
                        static_cast<float>(q.w)};
    pose.position = {0.0f, 1.6f, 0.0f};
    return pose;
}

XrFovf Fov(double left, double right, double up, double down) {
    return XrFovf{static_cast<float>(left * kDeg), static_cast<float>(right * kDeg), static_cast<float>(up * kDeg),
                  static_cast<float>(down * kDeg)};
}

// A crop of the whole field of view.
MirrorCrop WholeCrop(const XrFovf& fov) {
    MirrorCrop crop;
    crop.center_x = 0.5 * (std::tan(fov.angleLeft) + std::tan(fov.angleRight));
    crop.center_y = 0.5 * (std::tan(fov.angleUp) + std::tan(fov.angleDown));
    crop.half_width = 0.5 * (std::tan(fov.angleRight) - std::tan(fov.angleLeft));
    crop.half_height = 0.5 * (std::tan(fov.angleUp) - std::tan(fov.angleDown));
    return crop;
}

bool SameHomography(const MirrorHomography& a, const MirrorHomography& b, float tolerance = 0.0f) {
    for (int i = 0; i < 9; ++i) {
        if (std::fabs(a[i] - b[i]) > tolerance) return false;
    }
    return true;
}

// ---- CPU ----

void TestQuaternions() {
    const MirrorQuat yaw = AxisAngle(0, 1, 0, 30 * kDeg);
    const MirrorQuat roll = AxisAngle(0, 0, 1, 20 * kDeg);
    Expect(Near(MirrorAngle(MirrorQuat{}, yaw), 30 * kDeg), "The angle of a 30 degree turn is 30 degrees");
    Expect(Near(MirrorAngle(yaw, yaw), 0.0, 1e-7), "An orientation is no angle from itself");
    const MirrorQuat negated{-yaw.x, -yaw.y, -yaw.z, -yaw.w};
    Expect(Near(MirrorAngle(yaw, negated), 0.0, 1e-7), "A quaternion and its negative are the same orientation");
    Expect(Near(MirrorAngle(MirrorQuat{}, MirrorSlerp(MirrorQuat{}, yaw, 0.25)), 7.5 * kDeg, 1e-9),
           "A quarter of the way along a 30 degree turn is 7.5 degrees");
    Expect(Near(MirrorAngle(MirrorSlerp(MirrorQuat{}, negated, 0.5), AxisAngle(0, 1, 0, 15 * kDeg)), 0.0, 1e-7),
           "Slerp takes the shorter arc");
    const MirrorQuat both = MirrorMultiply(yaw, roll);
    Expect(Near(MirrorAngle(MirrorMultiply(MirrorConjugate(both), both), MirrorQuat{}), 0.0, 1e-7),
           "An orientation times its conjugate is none");
    Expect(Near(MirrorNormalize(MirrorQuat{0, 0, 0, 0}).w, 1.0), "A zero quaternion normalizes to none");

    // Level: the roll goes, heading and pitch stay.
    Expect(Near(MirrorAngle(MirrorLevel(both), yaw), 0.0, 1e-7), "Levelling a yawed and rolled view leaves the yaw");
    const MirrorQuat pitched = MirrorMultiply(yaw, AxisAngle(1, 0, 0, -25 * kDeg));
    Expect(Near(MirrorAngle(MirrorLevel(MirrorMultiply(pitched, roll)), pitched), 0.0, 1e-7),
           "Levelling keeps heading and pitch");
    Expect(Near(MirrorAngle(MirrorLevel(pitched), pitched), 0.0, 1e-7), "A level view stays as it is");
    const MirrorQuat up = MirrorMultiply(AxisAngle(1, 0, 0, 89.5 * kDeg), roll);
    Expect(Near(MirrorAngle(MirrorLevel(up), up), 0.0, 1e-12), "Looking straight up there is no level: unchanged");
}

// The peak angle of the smoothed view from the centre of a yaw shake.
double ShakePeak(double strength, double hz, double amplitude) {
    MirrorOrientationFilter filter;
    double peak = 0.0;
    const double dt = 1.0 / 90.0;
    for (int frame = 0; frame < 900; ++frame) {
        const double t = frame * dt;
        const MirrorQuat smoothed =
            filter.Update(AxisAngle(0, 1, 0, amplitude * std::sin(2 * kPi * hz * t)), frame ? dt : 0.0, strength);
        if (frame > 450) peak = std::max(peak, MirrorAngle(MirrorQuat{}, smoothed));
    }
    return peak;
}

void TestFilter() {
    Expect(MirrorOrientationFilter::RestCutoff(0.0) == 0.0, "Strength 0 is no filter");
    Expect(Near(MirrorOrientationFilter::RestCutoff(100.0), 0.25, 1e-9) &&
               MirrorOrientationFilter::RestCutoff(1.0) < 6.0 && MirrorOrientationFilter::RestCutoff(1.0) > 5.0,
           "The cutoff runs from about 6 Hz down to 0.25 Hz");
    double last = 100.0;
    for (int strength = 1; strength <= 100; ++strength) {
        const double cutoff = MirrorOrientationFilter::RestCutoff(strength);
        Expect(cutoff < last, "More smoothing is a lower cutoff");
        last = cutoff;
    }

    // Strength 0 follows exactly.
    {
        MirrorOrientationFilter filter;
        for (int frame = 0; frame < 50; ++frame) {
            const MirrorQuat actual = AxisAngle(0.3, 0.9, 0.1, 0.02 * frame);
            const MirrorQuat out = filter.Update(actual, 1.0 / 90.0, 0.0);
            Expect(out.x == actual.x && out.y == actual.y && out.z == actual.z && out.w == actual.w,
                   "Strength 0 gives the head's orientation itself");
        }
    }
    // A shake is smoothed, more so with more strength.
    const double amplitude = 0.5 * kDeg;
    const double none = ShakePeak(0.0, 6.0, amplitude);
    const double medium = ShakePeak(60.0, 6.0, amplitude);
    const double most = ShakePeak(100.0, 6.0, amplitude);
    Expect(Near(none, amplitude, amplitude * 0.02), "Without smoothing the shake is all there");
    Expect(medium < 0.3 * amplitude, "At strength 60 a 6 Hz shake is cut to under a third");
    Expect(most < 0.5 * medium, "At strength 100 it is cut further");
    Expect(ShakePeak(20.0, 6.0, amplitude) > medium, "Less strength smooths less");

    // A step is approached without overshoot and reached.
    {
        MirrorOrientationFilter filter;
        const MirrorQuat target = AxisAngle(0, 1, 0, 20 * kDeg);
        filter.Update(MirrorQuat{}, 0.0, 60.0);
        double previous = 20 * kDeg;
        for (int frame = 0; frame < 900; ++frame) {
            const double left = MirrorAngle(filter.Update(target, 1.0 / 90.0, 60.0), target);
            Expect(left <= previous + 1e-12, "A step is approached steadily, never overshot");
            previous = left;
        }
        Expect(previous < 0.01 * kDeg, "A step is reached");
    }
    // A steady turn is followed with a lag that stays bounded, less with less strength.
    const auto lag = [](double strength) {
        MirrorOrientationFilter filter;
        double behind = 0.0;
        for (int frame = 0; frame < 900; ++frame) {
            const MirrorQuat actual = AxisAngle(0, 1, 0, 1.0 * frame / 90.0); // 1 rad/s
            behind = MirrorAngle(filter.Update(actual, frame ? 1.0 / 90.0 : 0.0, strength), actual);
        }
        return behind;
    };
    Expect(lag(60.0) < 10 * kDeg && lag(60.0) > 1 * kDeg, "A 57 degrees-a-second turn is followed within 10 degrees");
    Expect(lag(20.0) < lag(60.0) && lag(60.0) < lag(100.0), "More smoothing lags further behind a turn");
    // A gap, a step back in time and a first sample start again at the head.
    {
        MirrorOrientationFilter filter;
        const MirrorQuat away = AxisAngle(0, 1, 0, 60 * kDeg);
        filter.Update(MirrorQuat{}, 0.0, 80.0);
        Expect(MirrorAngle(filter.Update(away, 0.011, 80.0), away) > 30 * kDeg, "A frame later the view is still well behind");
        Expect(MirrorAngle(filter.Update(away, 0.75, 80.0), away) == 0.0, "After a gap the filter starts at the head");
        filter.Update(MirrorQuat{}, 0.011, 80.0);
        Expect(MirrorAngle(filter.Update(away, -0.011, 80.0), away) == 0.0, "Time going back starts it again");
        filter.Update(MirrorQuat{}, 0.011, 80.0);
        const MirrorQuat before = filter.Update(MirrorQuat{}, 0.011, 80.0);
        const MirrorQuat repeated = filter.Update(away, 0.0, 80.0);
        Expect(MirrorAngle(repeated, before) == 0.0, "A repeated display time holds the view, no jump to the head");
        filter.Reset();
        Expect(!filter.Primed() && MirrorAngle(filter.Update(away, 0.011, 80.0), away) == 0.0 && filter.Primed(),
               "A reset filter starts at the head");
    }
}

void TestCropAndHomography() {
    // A square, symmetric field of view: 16:9 takes the full width.
    const XrFovf square = Fov(-45, 45, 45, -45);
    MirrorCrop crop = MirrorCropFor(square, 16.0 / 9.0, 0.0);
    Expect(Near(crop.half_width, 1.0, 1e-6) && Near(crop.half_height, 9.0 / 16.0, 1e-6) &&
               Near(crop.center_x, 0.0, 1e-6) && Near(crop.center_y, 0.0, 1e-6),
           "16:9 in a square view is its full width, centred");
    crop = MirrorCropFor(square, 16.0 / 9.0, 15.0);
    Expect(Near(crop.half_width, 0.85, 1e-6) && Near(crop.half_height, 0.85 * 9.0 / 16.0, 1e-6),
           "A 15% margin keeps 85% of the picture's width and height");
    Expect(Near(MirrorCropFor(square, 16.0 / 9.0, 90.0).half_width, 0.6, 1e-6) &&
               Near(MirrorCropFor(square, 16.0 / 9.0, -5.0).half_width, 1.0, 1e-6),
           "The margin is clamped to 0..40");
    // A wide view: 1:1 takes the full height. An asymmetric one: centred in it.
    const XrFovf wide = Fov(-60, 50, 30, -40);
    crop = MirrorCropFor(wide, 1.0, 0.0);
    const double tall = 0.5 * (std::tan(30 * kDeg) + std::tan(40 * kDeg));
    Expect(Near(crop.half_height, tall, 1e-6) && Near(crop.half_width, tall, 1e-6), "1:1 in a wide view is its full height");
    Expect(Near(crop.center_x, 0.5 * (std::tan(50 * kDeg) - std::tan(60 * kDeg)), 1e-6) &&
               Near(crop.center_y, 0.5 * (std::tan(30 * kDeg) - std::tan(40 * kDeg)), 1e-6),
           "The crop is centred in an asymmetric field of view");

    // No rotation: the window's corners are the crop's corners in the image.
    crop = MirrorCropFor(wide, 16.0 / 9.0, 10.0);
    MirrorHomography h = MirrorHomographyFor(wide, crop, MirrorQuat{});
    const double left = std::tan(-60 * kDeg), right = std::tan(50 * kDeg), top = std::tan(30 * kDeg),
                 bottom = std::tan(-40 * kDeg);
    double u = 0, v = 0;
    Expect(MirrorMap(h, -1, 1, u, v) && Near(u, (crop.center_x - crop.half_width - left) / (right - left), 1e-6) &&
               Near(v, (top - (crop.center_y + crop.half_height)) / (top - bottom), 1e-6),
           "The window's top-left corner is the crop's");
    Expect(MirrorMap(h, 1, -1, u, v) && Near(u, (crop.center_x + crop.half_width - left) / (right - left), 1e-6) &&
               Near(v, (top - (crop.center_y - crop.half_height)) / (top - bottom), 1e-6),
           "The window's bottom-right corner is the crop's");
    Expect(MirrorFilled(h), "A crop inside the view fills the window");
    // The whole view, unrotated, is the unit square exactly at its corners.
    h = MirrorHomographyFor(square, WholeCrop(square), MirrorQuat{});
    Expect(MirrorMap(h, -1, 1, u, v) && Near(u, 0.0, 1e-6) && Near(v, 0.0, 1e-6) && MirrorMap(h, 1, -1, u, v) &&
               Near(u, 1.0, 1e-6) && Near(v, 1.0, 1e-6) && MirrorMap(h, 0, 0, u, v) && Near(u, 0.5, 1e-6) &&
               Near(v, 0.5, 1e-6),
           "The whole view maps to the whole rectangle");

    // The head yawed left of the smoothed view by 5 degrees: the window's
    // centre is 5 degrees to the right in the image.
    const MirrorQuat head = AxisAngle(0, 1, 0, 5 * kDeg);
    crop = MirrorCropFor(square, 16.0 / 9.0, 20.0);
    h = MirrorHomographyFor(square, crop, MirrorMultiply(MirrorConjugate(head), MirrorQuat{}));
    Expect(MirrorMap(h, 0, 0, u, v) && Near(u, (std::tan(5 * kDeg) + 1.0) / 2.0, 1e-6) && Near(v, 0.5, 1e-6),
           "A yaw moves the window sideways in the image by the angle's tangent");
    // Pitched up by 4 degrees: the window's centre is 4 degrees down in the image.
    const MirrorQuat nod = AxisAngle(1, 0, 0, 4 * kDeg);
    h = MirrorHomographyFor(square, crop, MirrorConjugate(nod));
    Expect(MirrorMap(h, 0, 0, u, v) && Near(u, 0.5, 1e-6) && Near(v, (1.0 + std::tan(4 * kDeg)) / 2.0, 1e-6),
           "A pitch moves the window up or down in the image");
    // A window point checked against rotating its ray by hand.
    const MirrorQuat mixed = MirrorMultiply(AxisAngle(0, 1, 0, -3 * kDeg), AxisAngle(0, 0, 1, 6 * kDeg));
    h = MirrorHomographyFor(wide, MirrorCropFor(wide, 4.0 / 3.0, 25.0), mixed);
    {
        const MirrorCrop c = MirrorCropFor(wide, 4.0 / 3.0, 25.0);
        const double x = 0.6, y = -0.4;
        const double ray[3] = {c.center_x + x * c.half_width, c.center_y + y * c.half_height, -1.0};
        // v' = q v q^-1
        const MirrorQuat r = MirrorMultiply(MirrorMultiply(mixed, MirrorQuat{ray[0], ray[1], ray[2], 0.0}),
                                            MirrorConjugate(mixed));
        const double eu = (r.x / -r.z - left) / (right - left);
        const double ev = (top - r.y / -r.z) / (top - bottom);
        Expect(MirrorMap(h, x, y, u, v) && Near(u, eu, 1e-6) && Near(v, ev, 1e-6),
               "The homography is the rotation of the window's rays");
    }
    // Behind the view and outside it.
    h = MirrorHomographyFor(square, crop, AxisAngle(0, 1, 0, 120 * kDeg));
    Expect(!MirrorMap(h, 0, 0, u, v) && !MirrorFilled(h), "A window turned behind the view maps nowhere");
    h = MirrorHomographyFor(square, MirrorCropFor(square, 16.0 / 9.0, 0.0), AxisAngle(0, 1, 0, 1 * kDeg));
    Expect(!MirrorFilled(h), "Without a margin any turn leaves the image");
    Expect(MirrorFilled(MirrorHomographyFor(square, crop, AxisAngle(0, 1, 0, 3 * kDeg))),
           "A small turn inside the margin stays filled");
    // No field of view at all: the whole rectangle, not a division by zero.
    h = MirrorHomographyFor(XrFovf{}, MirrorCrop{}, MirrorQuat{});
    Expect(MirrorMap(h, -1, 1, u, v) && Near(u, 0.0) && Near(v, 0.0) && MirrorMap(h, 1, -1, u, v) && Near(u, 1.0) &&
               Near(v, 1.0),
           "A view without a field of view shows its whole rectangle");
}

void TestStabilizer() {
    const XrFovf fov = Fov(-50, 45, 44, -48);
    MirrorSettings settings;
    settings.smoothing = 0.0;
    settings.margin = 15.0;
    const MirrorHomography plain =
        MirrorHomographyFor(fov, MirrorCropFor(fov, 16.0 / 9.0, settings.margin), MirrorQuat{});
    // Smoothing 0: the plain crop, bit for bit, whatever the head does.
    {
        MirrorStabilizer stabilizer;
        for (int frame = 0; frame < 200; ++frame) {
            const MirrorQuat head = AxisAngle(0.2, 0.9, 0.3, 0.05 * std::sin(frame * 0.7) + 0.01 * frame);
            const MirrorAim aim = stabilizer.Update(Pose(head), fov, 1'000'000'000ll + frame * 11'111'111ll, settings);
            Expect(SameHomography(aim.homography, plain) && aim.applied == 1.0,
                   "With smoothing 0 the window is the plain crop of the eye, exactly");
        }
    }
    // A head at rest: the plain crop at any strength.
    settings.smoothing = 80.0;
    {
        MirrorStabilizer stabilizer;
        const MirrorQuat head = AxisAngle(0, 1, 0, 40 * kDeg);
        for (int frame = 0; frame < 30; ++frame) {
            const MirrorAim aim = stabilizer.Update(Pose(head), fov, 5'000'000'000ll + frame * 11'111'111ll, settings);
            Expect(SameHomography(aim.homography, plain, 1e-6f), "A still head is shown as the plain crop");
        }
    }
    // A shake: the window's centre moves in the image against the shake, so
    // the picture stands still; the frame's own pose is only read.
    {
        MirrorStabilizer stabilizer;
        double swing = 0.0;
        for (int frame = 0; frame < 600; ++frame) {
            const double angle = 0.6 * kDeg * std::sin(2 * kPi * 7.0 * frame / 90.0);
            const XrPosef pose = Pose(AxisAngle(0, 1, 0, angle));
            const XrPosef before = pose;
            const MirrorAim aim = stabilizer.Update(pose, fov, 1'000'000'000ll + frame * 11'111'111ll, settings);
            Expect(std::memcmp(&pose, &before, sizeof(pose)) == 0, "The frame's pose is not written");
            double u = 0, v = 0;
            Expect(MirrorMap(aim.homography, 0, 0, u, v) && MirrorFilled(aim.homography) && aim.applied == 1.0,
                   "A shake inside the margin is taken out whole");
            if (frame > 300) {
                // Where the smoothed centre is in the image, as an angle from the head's.
                const double l = std::tan(fov.angleLeft), r = std::tan(fov.angleRight);
                const double centre = 0.5 * (l + r);
                const double shown = std::atan(l + u * (r - l)) - std::atan(centre);
                // The picture's direction in the world: the head's plus the offset.
                swing = std::max(swing, std::fabs(angle - shown));
            }
        }
        Expect(swing < 0.2 * 0.6 * kDeg, "The picture's direction swings by under a fifth of a 7 Hz shake");
    }
    // Anything the head does: the window never leaves the image.
    for (const double margin : {0.0, 5.0, 15.0, 40.0}) {
        for (const bool level : {false, true}) {
            MirrorStabilizer stabilizer;
            settings.margin = margin;
            settings.level_horizon = level;
            settings.smoothing = 100.0;
            double least = 1.0;
            for (int frame = 0; frame < 700; ++frame) {
                const double t = frame / 90.0;
                const MirrorQuat head = MirrorMultiply(
                    AxisAngle(0, 1, 0, 1.4 * std::sin(1.3 * t) + 0.02 * std::sin(40 * t)),
                    MirrorMultiply(AxisAngle(1, 0, 0, 0.5 * std::sin(2.1 * t)), AxisAngle(0, 0, 1, 0.6 * std::sin(1.7 * t))));
                const MirrorAim aim = stabilizer.Update(Pose(head), fov, 1'000'000'000ll + frame * 11'111'111ll, settings);
                Expect(MirrorFilled(aim.homography),
                       "The window stays inside the image (margin " + std::to_string(margin) + ")");
                least = std::min(least, aim.applied);
            }
            Expect(least < 1.0, "A fast, wide turn is more than the margin covers: part of it is given up");
        }
    }
    // No margin: nothing can be corrected, the plain picture.
    {
        MirrorStabilizer stabilizer;
        settings.margin = 0.0;
        settings.level_horizon = false;
        settings.smoothing = 100.0;
        const MirrorHomography whole = MirrorHomographyFor(fov, MirrorCropFor(fov, 16.0 / 9.0, 0.0), MirrorQuat{});
        stabilizer.Update(Pose(MirrorQuat{}), fov, 1'000'000'000ll, settings);
        const MirrorAim aim = stabilizer.Update(Pose(AxisAngle(0, 1, 0, 5 * kDeg)), fov, 1'011'111'111ll, settings);
        Expect(SameHomography(aim.homography, whole, 2e-3f) && aim.applied < 0.01,
               "Without a margin the picture follows the head");
    }
    // The level horizon: a head rolled by 10 degrees is shown level.
    {
        MirrorStabilizer stabilizer;
        const XrFovf square = Fov(-45, 45, 45, -45);
        settings.margin = 40.0;
        settings.smoothing = 0.0;
        settings.level_horizon = true;
        const MirrorQuat head = MirrorMultiply(AxisAngle(0, 1, 0, 70 * kDeg), AxisAngle(0, 0, 1, 10 * kDeg));
        const MirrorAim aim = stabilizer.Update(Pose(head), square, 1'000'000'000ll, settings);
        double u0 = 0, v0 = 0, u1 = 0, v1 = 0;
        Expect(MirrorMap(aim.homography, -1, 0, u0, v0) && MirrorMap(aim.homography, 1, 0, u1, v1) && aim.applied == 1.0,
               "A levelled window maps into the image");
        // In the image's tangent space (y up) the window's horizontal runs at
        // minus the head's roll.
        const double slope = std::atan2(-(v1 - v0) * 2.0, (u1 - u0) * 2.0);
        Expect(Near(slope, -10 * kDeg, 1e-4), "The window's horizontal is the world's, 10 degrees off the rolled eye's");
        double u = 0, v = 0;
        Expect(MirrorMap(aim.homography, 0, 0, u, v) && Near(u, 0.5, 1e-5) && Near(v, 0.5, 1e-5),
               "Levelling turns the picture about the view's axis");
        // Off, the same head gives the plain crop.
        settings.level_horizon = false;
        MirrorStabilizer plain_stabilizer;
        Expect(SameHomography(plain_stabilizer.Update(Pose(head), square, 1'000'000'000ll, settings).homography,
                              MirrorHomographyFor(square, MirrorCropFor(square, 16.0 / 9.0, 40.0), MirrorQuat{})),
               "Without the level horizon a rolled head is shown rolled");
        // More roll than the margin has room for: as much as fits.
        settings.level_horizon = true;
        settings.margin = 5.0;
        MirrorStabilizer tight;
        const MirrorAim partly =
            tight.Update(Pose(AxisAngle(0, 0, 1, 35 * kDeg)), square, 1'000'000'000ll, settings);
        Expect(MirrorFilled(partly.homography) && partly.applied < 1.0 && partly.applied > 0.0,
               "A roll the margin cannot cover is levelled as far as the image reaches");
    }
    // A picture of another shape than the settings' (a window that fills a monitor).
    {
        MirrorStabilizer stabilizer;
        settings = {};
        settings.smoothing = 0.0;
        const MirrorAim aim = stabilizer.Update(Pose(MirrorQuat{}), fov, 1'000'000'000ll, settings, 2560.0 / 1080.0);
        Expect(SameHomography(aim.homography, MirrorHomographyFor(fov, MirrorCropFor(fov, 2560.0 / 1080.0, settings.margin),
                                                                 MirrorQuat{})),
               "A picture of a monitor's shape is cropped to that shape");
        MirrorStabilizer other;
        Expect(SameHomography(other.Update(Pose(MirrorQuat{}), fov, 1'000'000'000ll, settings, 0.0).homography,
                              MirrorHomographyFor(fov, MirrorCropFor(fov, 16.0 / 9.0, settings.margin), MirrorQuat{})),
               "No shape given, the settings' aspect");
    }
    // A game that leaves the pose zeroed is shown plain, not as NaN.
    {
        MirrorStabilizer stabilizer;
        settings = {};
        XrPosef zero{};
        const MirrorAim aim = stabilizer.Update(zero, fov, 1'000'000'000ll, settings);
        bool finite = true;
        for (const float value : aim.homography) finite = finite && std::isfinite(value);
        Expect(finite && MirrorFilled(aim.homography), "A zeroed pose gives a finite, filled picture");
    }
}

void TestPacingAndSizes() {
    const auto shown = [](double game_hz, int frame_rate, int frames) {
        MirrorPacer pacer;
        int count = 0;
        for (int frame = 0; frame < frames; ++frame) {
            const XrTime time = 10'000'000'000ll + static_cast<XrTime>(std::llround(frame * 1e9 / game_hz));
            if (pacer.Due(time, frame_rate)) ++count;
        }
        return count;
    };
    Expect(shown(90.0, 60, 900) >= 598 && shown(90.0, 60, 900) <= 602, "A 90 Hz game is shown at 60 a second");
    Expect(shown(60.0, 60, 600) == 600, "A 60 Hz game is shown whole at 60");
    Expect(shown(72.0, 120, 720) == 720, "A cap above the game's rate shows every frame");
    Expect(shown(120.0, 30, 1200) >= 299 && shown(120.0, 30, 1200) <= 301, "A 120 Hz game is shown at 30 a second");
    Expect(shown(45.0, 60, 450) == 450, "A game below the cap is shown whole");
    Expect(shown(90.0, 15, 900) >= 149 && shown(90.0, 15, 900) <= 151, "15 a second is 15 a second");
    {
        MirrorPacer pacer;
        Expect(pacer.Due(1'000'000'000ll, 60), "The first frame is shown");
        Expect(!pacer.Due(1'005'000'000ll, 60), "A frame 5 ms later is not");
        Expect(pacer.Due(9'000'000'000ll, 60), "After a pause the next frame is shown");
        Expect(pacer.Due(500'000'000ll, 60), "A clock that went back starts again");
        pacer.Reset();
        Expect(pacer.Due(500'000'001ll, 60), "A reset pacer shows the next frame");
    }

    int width = 0, height = 0;
    MirrorPictureSize(1280, MirrorAspect::k16x9, width, height);
    Expect(width == 1280 && height == 720, "1280 wide at 16:9 is 720 high");
    MirrorPictureSize(1920, MirrorAspect::k21x9, width, height);
    Expect(width == 1920 && height == 823, "1920 wide at 21:9 is 823 high");
    MirrorPictureSize(800, MirrorAspect::k4x3, width, height);
    Expect(width == 800 && height == 600, "800 wide at 4:3 is 600 high");
    MirrorPictureSize(640, MirrorAspect::k1x1, width, height);
    Expect(width == 640 && height == 640, "1:1 is square");
    MirrorPictureSize(1600, MirrorAspect::k16x10, width, height);
    Expect(width == 1600 && height == 1000, "1600 wide at 16:10 is 1000 high");
    MirrorPictureSize(10, MirrorAspect::k16x9, width, height);
    Expect(width == 320 && height == 180, "The width is at least 320");
    MirrorPictureSize(99999, MirrorAspect::k16x9, width, height);
    Expect(width == 3840 && height == 2160, "The width is at most 3840");

    Expect(MirrorViewIndex(MirrorEye::Left, 2) == 0 && MirrorViewIndex(MirrorEye::Right, 2) == 1 &&
               MirrorViewIndex(MirrorEye::Right, 4) == 1 && MirrorViewIndex(MirrorEye::Right, 1) == 0 &&
               MirrorViewIndex(MirrorEye::Left, 0) == -1,
           "The eye's view is the stereo pair's, the only one of a mono frame, none of no views");
    Expect(MirrorStateName(MirrorState::kOff) == "off" && MirrorStateName(MirrorState::kPending) == "pending" &&
               MirrorStateName(MirrorState::kActive) == "active" && MirrorStateName(MirrorState::kHidden) == "hidden" &&
               MirrorStateName(MirrorState::kUnsupported) == "unsupported" &&
               MirrorStateName(MirrorState::kFailed) == "failed",
           "The states have the names the app reads");
}

// A Vulkan game's picture between the game's queue and the mirror's.
void TestRelay() {
    using Slot = MirrorRelay::Slot;
    MirrorRelay relay;
    Expect(relay.Idle() && relay.ToCopy() == 0 && relay.ToShow() == -1 && relay.Copying() == -1,
           "An empty relay copies into its first image and shows nothing");
    relay.CopyStarted(0);
    Expect(relay.Copying() == 0 && relay.ToCopy() == -1 && relay.ToShow() == -1 && !relay.Idle(),
           "While a copy is under way no other is started and nothing is shown");
    relay.CopyDone(0);
    Expect(relay.ToShow() == 0 && relay.ToCopy() == 1 && relay.Idle(), "A finished copy is the picture to show");
    relay.ShowStarted(0);
    relay.CopyStarted(1);
    Expect(relay.State(0) == Slot::kShowing && relay.State(1) == Slot::kCopying && relay.ToCopy() == -1 &&
               relay.ToShow() == -1,
           "One image is read by the mirror's queue while the game's writes the other");
    relay.CopyDone(1);
    Expect(relay.ToShow() == 1 && relay.ToCopy() == -1,
           "With one image still being shown and the other ready, nothing is copied");
    relay.ShowsDone();
    Expect(relay.State(0) == Slot::kFree && relay.ToCopy() == 0, "An image the mirror's queue is done with is free");
    // A picture never shown gives way to a newer one.
    relay.CopyStarted(0);
    relay.CopyDone(0);
    Expect(relay.State(1) == Slot::kFree && relay.ToShow() == 0, "A newer picture replaces one never shown");
    // A refused submission frees its image.
    relay.CopyStarted(1);
    relay.CopyFailed(1);
    Expect(relay.State(1) == Slot::kFree && relay.ToShow() == 0, "A copy that never ran frees its image");
    relay.DropReady();
    Expect(relay.ToShow() == -1 && relay.Idle(), "Pictures not shown can be dropped");
    relay.Reset();
    // Any order of events: an image is never written while it is read, never
    // shown before its copy is done, and at most one copy is under way.
    std::uint32_t state = 12345;
    const auto random = [&] {
        state = state * 1664525u + 1013904223u;
        return state >> 16;
    };
    bool sound = true;
    int shows = 0, copies = 0;
    for (int step = 0; step < 20000 && sound; ++step) {
        switch (random() % 6) {
        case 0:
            if (const int slot = relay.ToCopy(); slot >= 0) {
                sound = sound && relay.State(slot) == Slot::kFree && relay.Copying() == -1;
                relay.CopyStarted(slot);
                ++copies;
            }
            break;
        case 1:
            if (const int slot = relay.Copying(); slot >= 0) relay.CopyDone(slot);
            break;
        case 2:
            if (const int slot = relay.ToShow(); slot >= 0) {
                sound = sound && relay.State(slot) == Slot::kReady;
                relay.ShowStarted(slot);
                ++shows;
            }
            break;
        case 3:
            relay.ShowsDone();
            break;
        case 4:
            if (const int slot = relay.Copying(); slot >= 0 && random() % 8 == 0) relay.CopyFailed(slot);
            break;
        default:
            // Calls that do not apply change nothing.
            relay.CopyStarted(static_cast<int>(random() % 2));
            relay.ShowStarted(static_cast<int>(random() % 2));
            relay.CopyDone(relay.Copying() == 0 ? 1 : 0);
            break;
        }
        int copying = 0, ready = 0;
        for (int slot = 0; slot < MirrorRelay::kSlots; ++slot) {
            copying += relay.State(slot) == Slot::kCopying;
            ready += relay.State(slot) == Slot::kReady;
        }
        sound = sound && copying <= 1 && ready <= 1;
    }
    Expect(sound && shows > 1000 && copies > 1000,
           "In any order of events an image is never copied into while it is shown or shown before it is copied");
}

void TestShaderConstants() {
    Expect(sizeof(MirrorShaderConstants) == 80 && offsetof(MirrorShaderConstants, source_offset) == 48 &&
               offsetof(MirrorShaderConstants, target_size) == 64 && offsetof(MirrorShaderConstants, flags) == 72 &&
               offsetof(MirrorShaderConstants, spread) == 76,
           "The constants are laid out as the shader's buffer");
    const XrFovf fov = Fov(-45, 45, 45, -45);
    const MirrorHomography whole = MirrorHomographyFor(fov, WholeCrop(fov), MirrorQuat{});
    MirrorShaderConstants packed = PackMirrorConstants(whole, 7, 9, 400, 300, 400, 300, false);
    Expect(packed.source_offset[0] == 7 && packed.source_offset[1] == 9 && packed.source_size[0] == 400 &&
               packed.source_size[1] == 300 && packed.target_size[0] == 400.0f && packed.target_size[1] == 300.0f &&
               packed.flags == 0 && packed.rows[0][0] == whole[0] && packed.rows[1][2] == whole[5] &&
               packed.rows[2][2] == whole[8] && packed.rows[0][3] == 0.0f,
           "The constants carry the rectangle, the picture's size and the homography's rows");
    Expect(packed.spread == 0.0f, "A picture of the image's own size is sampled texel for texel");
    Expect(PackMirrorConstants(whole, 0, 0, 400, 300, 800, 600, true).spread == 0.0f &&
               PackMirrorConstants(whole, 0, 0, 400, 300, 800, 600, true).flags == kMirrorFlagEncodeSrgb,
           "An enlarged picture is not spread; linear texels are flagged for encoding");
    Expect(PackMirrorConstants(whole, 0, 0, 800, 600, 400, 300, false).spread > 0.999f &&
               PackMirrorConstants(whole, 0, 0, 3000, 3000, 640, 360, false).spread == 1.0f,
           "A picture shrunk to half or less is sampled at the full spread");
    const float between = PackMirrorConstants(whole, 0, 0, 600, 450, 400, 300, false).spread;
    Expect(std::fabs(between - 0.5f) < 1e-3f, "A picture shrunk by a third is spread half way");
    Expect(PackMirrorConstants(whole, 0, 0, 400, 300, 0, 0, false).spread == 0.0f, "No picture, no spread");

    // The whole view at its own shape: the window's corners are the view's.
    const MirrorHomography all = MirrorWholeView();
    double u = 0.0, v = 0.0;
    Expect(MirrorMap(all, -1.0, 1.0, u, v) && Near(u, 0.0) && Near(v, 0.0) && MirrorMap(all, 1.0, -1.0, u, v) &&
               Near(u, 1.0) && Near(v, 1.0) && MirrorMap(all, 0.0, 0.0, u, v) && Near(u, 0.5) && Near(v, 0.5) &&
               MirrorFilled(all),
           "The whole view maps the window's corners to the view's");

    // A quad views eye's focus view drawn over it: its constants follow the
    // eye's in the Direct3D 11 buffer.
    Expect(sizeof(MirrorFocusShaderConstants) == 48 && sizeof(MirrorFocusedShaderConstants) == 128 &&
               offsetof(MirrorFocusedShaderConstants, focus) == 80 && offsetof(MirrorFocusShaderConstants, offset) == 16 &&
               offsetof(MirrorFocusShaderConstants, size) == 24 && offsetof(MirrorFocusShaderConstants, feather) == 32 &&
               offsetof(MirrorFocusShaderConstants, spread) == 40,
           "The focus view's constants are laid out as the shader's buffer");
    MirrorFocusPlace place;
    place.rect = {0.25f, 0.2f, 0.75f, 0.8f};
    place.feather = {0.05f, 0.06f};
    // A 1000x600 picture of the whole eye: the focus view covers 500x360 of it.
    MirrorFocusShaderConstants focus = PackMirrorFocusConstants(all, place, 11, 13, 500, 360, 1000, 600);
    Expect(focus.rect[0] == 0.25f && focus.rect[1] == 0.2f && focus.rect[2] == 0.75f && focus.rect[3] == 0.8f &&
               focus.offset[0] == 11 && focus.offset[1] == 13 && focus.size[0] == 500 && focus.size[1] == 360 &&
               focus.feather[0] == 0.05f && focus.feather[1] == 0.06f,
           "The focus constants carry its place in the eye, its rectangle and the blend band");
    Expect(focus.spread == 0.0f, "A focus view at its own scale is sampled texel for texel");
    Expect(PackMirrorFocusConstants(all, place, 0, 0, 250, 180, 1000, 600).spread == 0.0f,
           "An enlarged focus view is not spread");
    Expect(PackMirrorFocusConstants(all, place, 0, 0, 1000, 720, 1000, 600).spread > 0.999f &&
               PackMirrorFocusConstants(all, place, 0, 0, 3000, 2000, 1000, 600).spread == 1.0f,
           "A focus view shrunk to half or less is sampled at the full spread");
    Expect(std::fabs(PackMirrorFocusConstants(all, place, 0, 0, 750, 540, 1000, 600).spread - 0.5f) < 1e-3f,
           "A focus view shrunk by a third is spread half way");
    // The eye itself may be enlarged where its focus view is shrunk.
    Expect(PackMirrorConstants(all, 0, 0, 350, 210, 1000, 600, false).spread == 0.0f &&
               PackMirrorFocusConstants(all, place, 0, 0, 1000, 720, 1000, 600).spread > 0.999f,
           "The focus view's spread is its own, whatever the eye's");
    MirrorFocusPlace none;
    Expect(PackMirrorFocusConstants(all, none, 0, 0, 500, 360, 1000, 600).spread == 0.0f &&
               PackMirrorFocusConstants(all, place, 0, 0, 500, 360, 0, 0).spread == 0.0f,
           "No focus rectangle or no picture, no spread");
}

void TestPlacement() {
    const auto parsed = ParseMirrorPlacement(L"120,-40,1280,720,1280");
    Expect(parsed && parsed->x == 120 && parsed->y == -40 && parsed->width == 1280 && parsed->height == 720 &&
               parsed->setting_width == 1280,
           "A placement parses");
    Expect(FormatMirrorPlacement(*parsed) == L"120,-40,1280,720,1280", "A placement is written as it is read");
    for (const wchar_t* bad : {L"", L"1,2,3", L"a,b,c,d,e", L"0,0,100,720,1280", L"0,0,1280,0,1280",
                               L"0,0,99999,720,1280", L"900000,0,1280,720,1280", L"0,0,1280,720,1280,5",
                               L"0,0,1280,720,1280x"}) {
        Expect(!ParseMirrorPlacement(bad), "A placement that is not one is refused");
    }
    // On a 2560x1440 desktop at the origin.
    const auto usable = [&](const MirrorPlacement& saved, int setting, int width, int height) {
        return UsableMirrorPlacement(saved, setting, width, height, 0, 0, 2560, 1440);
    };
    auto kept = usable({100, 100, 960, 540, 1280}, 1280, 1280, 720);
    Expect(kept && kept->x == 100 && kept->y == 100 && kept->width == 960 && kept->height == 540,
           "A remembered size is kept while the width setting is the one it was made under");
    kept = usable({100, 100, 960, 540, 1280}, 1920, 1920, 1080);
    Expect(kept && kept->x == 100 && kept->width == 1920 && kept->height == 1080 && kept->setting_width == 1920,
           "A width set since wins over the remembered size; the position stays");
    kept = usable({100, 100, 960, 540, 1280}, 1280, 1280, 960);
    Expect(kept && kept->width == 960 && kept->height == 720, "Another aspect keeps the width and takes its own height");
    Expect(!usable({-2000, 100, 960, 540, 1280}, 1280, 1280, 720) && !usable({2540, 100, 960, 540, 1280}, 1280, 1280, 720) &&
               !usable({100, -50, 960, 540, 1280}, 1280, 1280, 720) && !usable({100, 1400, 960, 540, 1280}, 1280, 1280, 720),
           "A window that would open off the desktop is placed afresh");
    Expect(usable({-800, 100, 960, 540, 1280}, 1280, 1280, 720).has_value(),
           "A window partly off the left edge with its title bar in reach is kept");
    Expect(!UsableMirrorPlacement(std::nullopt, 1280, 1280, 720, 0, 0, 2560, 1440), "Nothing remembered, nothing used");

    // Remembered per game in the registry (a key of the test's own).
    const std::wstring key = L"MirrorWindowsTest" + std::to_wstring(GetCurrentProcessId());
    const std::wstring path = L"Software\\VectorXR\\" + key;
    Expect(!LoadMirrorPlacement(key, "DCS.exe"), "No placement is remembered at first");
    SaveMirrorPlacement(key, "DCS.exe", {10, 20, 800, 450, 1280});
    SaveMirrorPlacement(key, "Flug\xC3\xA4hnlich.exe", {-5, 7, 640, 360, 640});
    const auto dcs = LoadMirrorPlacement(key, "DCS.exe");
    const auto other = LoadMirrorPlacement(key, "Flug\xC3\xA4hnlich.exe");
    Expect(dcs && dcs->x == 10 && dcs->y == 20 && dcs->width == 800 && dcs->height == 450 && dcs->setting_width == 1280,
           "A game's placement is remembered");
    Expect(other && other->x == -5 && other->width == 640 && !LoadMirrorPlacement(key, "MSFS.exe"),
           "Each game has its own, a name outside ASCII too");
    Expect(RegDeleteKeyW(HKEY_CURRENT_USER, path.c_str()) == ERROR_SUCCESS, "The test's key is removed");

    Expect(MirrorWindowTitle("DCS.exe") == L"VectorXR Mirror \u2013 DCS" &&
               MirrorWindowTitle("FlightSimulator2024.EXE") == L"VectorXR Mirror \u2013 FlightSimulator2024" &&
               MirrorWindowTitle("") == L"VectorXR Mirror",
           "The title names the game for OBS's window list");

    int width = 1000, height = 10;
    MirrorKeepAspect(16.0 / 9.0, true, width, height);
    Expect(width == 1000 && height == 563, "A drag of a side edge sets the height");
    width = 10;
    height = 450;
    MirrorKeepAspect(16.0 / 9.0, false, width, height);
    Expect(width == 800 && height == 450, "A drag of the top or bottom edge sets the width");
    width = 20;
    height = 20;
    MirrorKeepAspect(16.0 / 9.0, true, width, height);
    Expect(width == 160 && height == 90, "The picture is at least 160 wide");
}

bool WaitFor(const std::function<bool()>& condition, int milliseconds = 5000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

MirrorWindow::Options WindowOptions(int width, int height) {
    MirrorWindow::Options options;
    options.title = MirrorWindowTitle("mirror_tests.exe");
    options.picture_width = width;
    options.picture_height = height;
    options.aspect = static_cast<double>(width) / height;
    options.setting_width = width;
    options.on_desktop = false; // never shown on the desktop
    return options;
}

void TestWindow() {
    // The window's thread sees real pixels on a scaled display; so must the
    // test, to speak of the same rectangles.
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HWND foreground = GetForegroundWindow();
    MirrorWindow window;
    Expect(!window.Started() && !window.Handle() && !window.Drawable(), "A window not started is not there");
    std::vector<MirrorPlacement> saved;
    std::mutex saved_mutex;
    MirrorWindow::Options options = WindowOptions(640, 360);
    options.save = [&](const MirrorPlacement& placement) {
        std::scoped_lock lock(saved_mutex);
        saved.push_back(placement);
    };
    std::string error;
    const auto begun = std::chrono::steady_clock::now();
    Expect(window.Start(std::move(options), error), "The window's thread starts: " + error);
    Expect(std::chrono::steady_clock::now() - begun < std::chrono::milliseconds(500),
           "Starting the window does not wait for it");
    Expect(WaitFor([&] { return window.Handle() != nullptr; }), "The window is made on its own thread");
    const HWND handle = static_cast<HWND>(window.Handle());
    Expect(GetWindowThreadProcessId(handle, nullptr) != GetCurrentThreadId(),
           "The window belongs to a thread that is not the caller's");
    std::uint32_t width = 0, height = 0;
    window.PictureSize(width, height);
    Expect(width == 640 && height == 360 && window.Shown() && window.Drawable() && !window.Failed(),
           "The window's picture is of the size asked for");
    wchar_t title[128]{};
    GetWindowTextW(handle, title, 127);
    Expect(std::wstring(title) == L"VectorXR Mirror \u2013 mirror_tests", "The window carries the title");
    const LONG_PTR ex_style = GetWindowLongPtrW(handle, GWL_EXSTYLE);
    const LONG_PTR style = GetWindowLongPtrW(handle, GWL_STYLE);
    Expect((ex_style & WS_EX_NOACTIVATE) && (ex_style & WS_EX_APPWINDOW) && (style & WS_THICKFRAME) &&
               !(style & WS_MAXIMIZEBOX),
           "The window is never activated, is listed like an application's and can be resized");
    Expect(SendMessageW(handle, WM_MOUSEACTIVATE, 0, 0) == MA_NOACTIVATE, "A click does not activate it");
    Expect(!(ex_style & (WS_EX_LAYERED | WS_EX_TRANSPARENT)),
           "A plain window takes the mouse: it is moved, resized and closed with it");
    Expect(GetForegroundWindow() == foreground, "Making the window leaves the foreground window where it was");

    // Closing it hides it; showing brings it back; the window is the same.
    PostMessageW(handle, WM_CLOSE, 0, 0);
    Expect(WaitFor([&] { return !window.Shown(); }) && window.Handle() == handle && !window.Drawable() &&
               IsWindow(handle),
           "Closing the window hides it and keeps it");
    window.Show(true);
    Expect(WaitFor([&] { return window.Shown(); }) && window.Drawable(), "Showing brings it back");
    window.Show(false);
    Expect(WaitFor([&] { return !window.Shown(); }), "Hiding hides it");
    window.Show(true);
    Expect(WaitFor([&] { return window.Shown(); }), "And back again");

    // New settings resize it; a drag keeps the aspect.
    window.Reshape(800, 600, 4.0 / 3.0, 800, L"");
    Expect(WaitFor([&] {
               window.PictureSize(width, height);
               return width == 800 && height == 600;
           }),
           "A new size and aspect reshape the window");
    RECT frame{};
    GetWindowRect(handle, &frame);
    RECT dragged = frame;
    dragged.right += 200; // the right edge, 200 pixels out
    SendMessageW(handle, WM_SIZING, WMSZ_RIGHT, reinterpret_cast<LPARAM>(&dragged));
    Expect(dragged.right - dragged.left == frame.right - frame.left + 200 &&
               dragged.bottom - dragged.top == frame.bottom - frame.top + 150 && dragged.top == frame.top &&
               dragged.left == frame.left,
           "Dragging the right edge out by 200 grows a 4:3 picture's height by 150");
    dragged = frame;
    dragged.top -= 90;
    SendMessageW(handle, WM_SIZING, WMSZ_TOP, reinterpret_cast<LPARAM>(&dragged));
    Expect(dragged.bottom == frame.bottom && dragged.right - dragged.left == frame.right - frame.left + 120,
           "Dragging the top edge up by 90 grows its width by 120");
    SendMessageW(handle, WM_EXITSIZEMOVE, 0, 0);
    {
        std::scoped_lock lock(saved_mutex);
        Expect(!saved.empty() && saved.back().width == 800 && saved.back().height == 600 &&
                   saved.back().setting_width == 800 && saved.back().x == frame.left && saved.back().y == frame.top,
               "Where the window was left is handed on to be remembered");
    }

    // The picture's size is the settings', whatever size the window is
    // dragged to: only new settings change it.
    {
        std::uint32_t picture_width = 0, picture_height = 0;
        window.OutputSize(picture_width, picture_height);
        Expect(picture_width == 800 && picture_height == 600, "The picture has the settings' size");
        SetWindowPos(handle, nullptr, 0, 0, 500, 400, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        window.PictureSize(width, height);
        window.OutputSize(picture_width, picture_height);
        Expect(width < 800 && height < 600 && picture_width == 800 && picture_height == 600,
               "A smaller window keeps the picture's size");
        SetWindowPos(handle, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        window.PictureSize(width, height);
        Expect(width == 800 && height == 600, "The window is put back");
    }

    // Filling a monitor: borderless, of the monitor's size, still never
    // activated; a monitor that is not there leaves it a window.
    {
        const std::vector<MirrorMonitor> fake = {{L"\\\\.\\DISPLAY1", 0, 0, 2560, 1440, true},
                                                 {L"\\\\.\\DISPLAY2", 2560, 0, 3840, 2160, false},
                                                 {L"\\\\.\\DISPLAY3", 0, 0, 0, 0, false}};
        const MirrorMonitor* second = FindMirrorMonitor(fake, L"\\\\.\\display2");
        Expect(second && second->x == 2560 && second->width == 3840 && !second->primary,
               "A monitor is found by its device name, whatever the letter case");
        Expect(!FindMirrorMonitor(fake, L"") && !FindMirrorMonitor(fake, L"\\\\.\\DISPLAY9") &&
                   !FindMirrorMonitor(fake, L"\\\\.\\DISPLAY3") && !FindMirrorMonitor({}, L"\\\\.\\DISPLAY1"),
               "No name, an unknown name and a monitor without a size are no monitor: none is chosen for the user");
        Expect(MirrorMonitorName("\\\\.\\DISPLAY2") == L"\\\\.\\DISPLAY2", "The settings' name is Windows' name");

        const std::vector<MirrorMonitor> monitors = ListMirrorMonitors();
        const MirrorMonitor* primary = nullptr;
        for (const MirrorMonitor& monitor : monitors) {
            if (monitor.primary) primary = &monitor;
        }
        Expect(primary && primary->width > 0 && !primary->name.empty(), "The desktop's monitors are listed");
        std::size_t saves = 0;
        {
            std::scoped_lock lock(saved_mutex);
            saves = saved.size();
        }
        window.Reshape(800, 600, 4.0 / 3.0, 800, primary->name);
        Expect(WaitFor([&] { return window.Filling(); }), "The window goes to fill its monitor");
        Expect(WaitFor([&] {
                   window.PictureSize(width, height);
                   return width == static_cast<std::uint32_t>(primary->width) &&
                          height == static_cast<std::uint32_t>(primary->height);
               }),
               "A window that fills a monitor has the monitor's size");
        RECT filled{};
        GetWindowRect(handle, &filled);
        const LONG_PTR fill_style = GetWindowLongPtrW(handle, GWL_STYLE);
        Expect(filled.left == primary->x && filled.top == primary->y && !(fill_style & WS_CAPTION) &&
                   !(fill_style & WS_THICKFRAME) && (GetWindowLongPtrW(handle, GWL_EXSTYLE) & WS_EX_NOACTIVATE) &&
                   !window.FillMissing() && window.Shown() && window.Drawable(),
               "It covers the monitor without a border and is still never activated");
        Expect(SendMessageW(handle, WM_MOUSEACTIVATE, 0, 0) == MA_NOACTIVATE && GetForegroundWindow() == foreground,
               "Filling a monitor takes neither a click's activation nor the foreground");
        const LONG_PTR through = WS_EX_LAYERED | WS_EX_TRANSPARENT;
        BYTE alpha = 0;
        DWORD blend = 0;
        Expect((GetWindowLongPtrW(handle, GWL_EXSTYLE) & through) == through &&
                   GetLayeredWindowAttributes(handle, nullptr, &alpha, &blend) && alpha == 255 && blend == LWA_ALPHA,
               "A window that fills a monitor lets the mouse through to what is under it, and is opaque");
        Expect(GetWindowLongPtrW(handle, GWL_EXSTYLE) & WS_EX_TOPMOST, "It stays above the other windows");
        SendMessageW(handle, WM_EXITSIZEMOVE, 0, 0);
        {
            std::scoped_lock lock(saved_mutex);
            Expect(saved.size() == saves, "Where it fills a monitor is not remembered as the window's place");
        }
        // The monitor unplugged (a name that is not there): a window again, where it was.
        window.Reshape(800, 600, 4.0 / 3.0, 800, L"\\\\.\\DISPLAY77");
        Expect(WaitFor([&] { return !window.Filling() && window.FillMissing(); }),
               "A monitor that is not connected leaves the mirror a window");
        Expect(WaitFor([&] {
                   window.PictureSize(width, height);
                   return width == 800 && height == 600;
               }),
               "Back in a window the picture has the settings' size");
        RECT back{};
        GetWindowRect(handle, &back);
        Expect(back.left == frame.left && back.top == frame.top &&
                   (GetWindowLongPtrW(handle, GWL_STYLE) & WS_THICKFRAME),
               "The window is back where it stood, with its border");
        Expect(!(GetWindowLongPtrW(handle, GWL_EXSTYLE) & through), "Back in a window it takes the mouse again");
        window.Reshape(800, 600, 4.0 / 3.0, 800, L"");
        Expect(WaitFor([&] { return !window.FillMissing(); }) && !window.Filling(), "No monitor asked for, none missing");
    }

    // Tasks handed to the window's thread run there, in order; none is
    // taken without a window.
    {
        std::vector<int> order;
        std::atomic<DWORD> ran_on{0};
        std::atomic<int> ran{0};
        const auto begun_posting = std::chrono::steady_clock::now();
        for (int i = 0; i < 5; ++i) {
            Expect(window.Post([&, i] {
                       order.push_back(i);
                       ran_on = GetCurrentThreadId();
                       if (i == 0) Sleep(100);
                       ++ran;
                   }),
                   "A task is handed to the window's thread");
        }
        Expect(std::chrono::steady_clock::now() - begun_posting < std::chrono::milliseconds(80),
               "Handing tasks over does not wait for them");
        Expect(WaitFor([&] { return ran.load() == 5; }) && order == std::vector<int>({0, 1, 2, 3, 4}) &&
                   ran_on.load() == GetWindowThreadProcessId(handle, nullptr) && ran_on.load() != GetCurrentThreadId(),
               "The tasks run on the window's thread in the order handed over");
    }

    window.Stop();
    Expect(!window.Started() && !window.Handle() && !IsWindow(handle) && !window.Shown() && !window.Drawable(),
           "Stopping destroys the window and ends its thread");
    Expect(!window.Post([] {}), "Without a window no task is taken");
    // Opened to fill a monitor from the start.
    {
        MirrorWindow::Options filling = WindowOptions(640, 360);
        const std::vector<MirrorMonitor> monitors = ListMirrorMonitors();
        filling.fill_monitor = monitors.front().name;
        MirrorWindow filled;
        Expect(filled.Start(std::move(filling), error) && WaitFor([&] { return filled.Handle() != nullptr; }),
               "A window that fills a monitor is made");
        filled.PictureSize(width, height);
        Expect(filled.Filling() && width == static_cast<std::uint32_t>(monitors.front().width) &&
                   height == static_cast<std::uint32_t>(monitors.front().height),
               "It fills the monitor from the start");
        const LONG_PTR through = WS_EX_LAYERED | WS_EX_TRANSPARENT;
        Expect((GetWindowLongPtrW(static_cast<HWND>(filled.Handle()), GWL_EXSTYLE) & through) == through,
               "...and lets the mouse through from the start");
        filled.OutputSize(width, height);
        Expect(width == static_cast<std::uint32_t>(monitors.front().width) &&
                   height == static_cast<std::uint32_t>(monitors.front().height),
               "Its picture has the monitor's size");
        MirrorWindow::Options missing = WindowOptions(640, 360);
        missing.fill_monitor = L"\\\\.\\DISPLAY77";
        MirrorWindow fallback;
        Expect(fallback.Start(std::move(missing), error) && WaitFor([&] { return fallback.Handle() != nullptr; }),
               "A window whose monitor is not connected is made");
        fallback.PictureSize(width, height);
        Expect(!fallback.Filling() && fallback.FillMissing() && width == 640 && height == 360,
               "It opens as a window of the settings' size");
        fallback.OutputSize(width, height);
        Expect(width == 640 && height == 360, "Its picture has the settings' size");
        // Opened at a remembered size: the window's, not the picture's.
        MirrorWindow::Options remembered = WindowOptions(640, 360);
        // On the monitor the first window opened on (its frame's corner).
        remembered.placement = MirrorPlacement{frame.left, frame.top, 480, 270, remembered.setting_width};
        MirrorWindow placed;
        Expect(placed.Start(std::move(remembered), error) && WaitFor([&] { return placed.Handle() != nullptr; }),
               "A window with a remembered place is made");
        std::uint32_t picture_width = 0, picture_height = 0;
        placed.PictureSize(width, height);
        placed.OutputSize(picture_width, picture_height);
        Expect(width == 480 && height == 270 && picture_width == 640 && picture_height == 360,
               "It opens at the remembered size with the picture at the settings' size (window " +
                   std::to_string(width) + "x" + std::to_string(height) + ", picture " +
                   std::to_string(picture_width) + "x" + std::to_string(picture_height) + ")");
    }
    window.Stop();
    Expect(window.Start(WindowOptions(320, 180), error) && WaitFor([&] { return window.Handle() != nullptr; }),
           "The window can be started again");
    window.PictureSize(width, height);
    Expect(width == 320 && height == 180, "The second window has its own size");
    // Destroyed with the object: its thread ends, the class is free again.
    {
        MirrorWindow scoped;
        Expect(scoped.Start(WindowOptions(320, 180), error) && WaitFor([&] { return scoped.Handle() != nullptr; }),
               "Two windows live side by side");
    }
    window.Stop();
    Expect(GetForegroundWindow() == foreground, "The foreground window never changed");
}

// ---- WARP ----

std::uint8_t Pattern(std::uint32_t x, std::uint32_t y, std::uint32_t slice, std::uint32_t channel) {
    const double wave = std::sin(x * (0.11 + 0.03 * channel) + slice) + std::cos(y * (0.07 + 0.02 * channel));
    return static_cast<std::uint8_t>(std::clamp(128.0 + wave * 45.0 + ((x * 7 + y * 13 + channel * 5) % 9), 0.0, 255.0));
}

std::vector<std::uint8_t> PatternImage(std::uint32_t width, std::uint32_t height, std::uint32_t slice) {
    std::vector<std::uint8_t> data(static_cast<std::size_t>(width) * height * 4);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            for (std::uint32_t c = 0; c < 4; ++c) {
                data[(static_cast<std::size_t>(y) * width + x) * 4 + c] =
                    c == 3 ? static_cast<std::uint8_t>(x + y) : Pattern(x, y, slice, c);
            }
        }
    }
    return data;
}

std::int64_t Typeless(std::int64_t format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    default: return format;
    }
}

double SrgbDecode(double value) {
    return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}
double SrgbEncode(double value) {
    value = std::clamp(value, 0.0, 1.0);
    return value <= 0.0031308 ? 12.92 * value : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
}

std::uint16_t Half(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, 4);
    const std::uint32_t sign = (bits >> 16) & 0x8000u;
    const int exponent = static_cast<int>((bits >> 23) & 0xff) - 127 + 15;
    const std::uint32_t mantissa = bits & 0x7fffffu;
    if (exponent <= 0) return static_cast<std::uint16_t>(sign);
    if (exponent >= 31) return static_cast<std::uint16_t>(sign | 0x7c00u);
    return static_cast<std::uint16_t>(sign | (static_cast<std::uint32_t>(exponent) << 10) | (mantissa >> 13));
}

// What the two APIs' tests need of a device.
struct Rig {
    virtual ~Rig() = default;
    virtual SubmittedImageApi Api() const = 0;
    // `typed`: in the format itself; otherwise in its typeless family, as
    // runtimes make swapchain images.
    virtual void* Image(std::int64_t format, std::uint32_t width, std::uint32_t height, std::uint32_t slices,
                        std::uint32_t samples, bool readable, bool typed) = 0;
    virtual void Fill(void* image, std::int64_t typed_format, std::uint32_t slice, const void* data,
                      std::uint32_t bytes_per_texel) = 0;
    virtual void ReleaseImages() = 0;
    virtual void Idle() = 0;
};

struct Harness {
    Rig& rig;
    SubmittedImageGpu& gpu;
    std::uintptr_t next{0x10};

    XrSwapchain Track(void* texture) {
        const XrSwapchain swapchain = reinterpret_cast<XrSwapchain>(next++);
        std::string error;
        bool tracked = false;
        if (rig.Api() == SubmittedImageApi::kD3D11) {
            XrSwapchainImageD3D11KHR image{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
            image.texture = static_cast<ID3D11Texture2D*>(texture);
            tracked = gpu.Track(swapchain, reinterpret_cast<XrSwapchainImageBaseHeader*>(&image), 1, error);
        } else {
            XrSwapchainImageD3D12KHR image{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
            image.texture = static_cast<ID3D12Resource*>(texture);
            tracked = gpu.Track(swapchain, reinterpret_cast<XrSwapchainImageBaseHeader*>(&image), 1, error);
        }
        Expect(tracked, "A swapchain's image is tracked: " + error);
        return swapchain;
    }
    XrSwapchain Source(std::int64_t format, std::uint32_t width, std::uint32_t height, std::uint32_t slices,
                       std::uint32_t samples = 1, bool readable = true, void** image = nullptr) {
        void* texture = rig.Image(format, width, height, slices, samples, readable, false);
        if (image) *image = texture;
        return Track(texture);
    }
    // An 8-bit RGBA image to draw into in place of the window.
    XrSwapchain Target(std::uint32_t width, std::uint32_t height, std::int64_t format = DXGI_FORMAT_R8G8B8A8_UNORM) {
        return Track(rig.Image(format, width, height, 1, 1, true, true));
    }
    static SubmittedView View(XrSwapchain swapchain, std::int64_t format, std::uint32_t width, std::uint32_t height,
                              std::uint32_t slices, std::uint32_t slice, XrRect2Di rect, std::uint32_t samples = 1) {
        SubmittedView view;
        view.resolved = true;
        view.swapchain = swapchain;
        view.array_slice = slice;
        view.rect = rect;
        view.format = format;
        view.width = width;
        view.height = height;
        view.array_size = slices;
        view.sample_count = samples;
        return view;
    }
    std::vector<std::uint8_t> Read(XrSwapchain target, std::uint32_t width, std::uint32_t height) {
        const SubmittedView view = View(target, DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, 0,
                                        {{0, 0}, {static_cast<int>(width), static_cast<int>(height)}});
        std::string error;
        const std::uint64_t ticket = gpu.Readback(view, error);
        Expect(ticket != 0, "The picture is read back: " + error);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        SubmittedPixels pixels;
        for (;;) {
            const ReadbackState state = gpu.Poll(ticket, pixels, error);
            if (state == ReadbackState::kReady) break;
            Expect(state == ReadbackState::kPending, "The readback fails: " + error);
            Expect(std::chrono::steady_clock::now() < deadline, "The readback never finishes");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::vector<std::uint8_t> out(static_cast<std::size_t>(width) * height * 4);
        for (std::uint32_t y = 0; y < height; ++y) {
            std::memcpy(out.data() + static_cast<std::size_t>(y) * width * 4,
                        pixels.data + static_cast<std::size_t>(y) * pixels.row_pitch, static_cast<std::size_t>(width) * 4);
        }
        gpu.Finish(ticket);
        return out;
    }
    // Drawn into the target, waiting out a GPU that is still on the last draw.
    std::vector<std::uint8_t> Draw(const SubmittedView& view, XrSwapchain target, std::uint32_t width,
                                   std::uint32_t height, const MirrorHomography& h) {
        MirrorDraw draw;
        draw.target = target;
        draw.width = width;
        draw.height = height;
        draw.homography = h;
        std::string error;
        for (int attempt = 0;; ++attempt) {
            const MirrorDrawResult result = gpu.DrawMirror(view, draw, error);
            Expect(result != MirrorDrawResult::kFailed, "The picture is drawn: " + error);
            if (result == MirrorDrawResult::kShown) break;
            Expect(attempt < 200, "The GPU never takes the draw");
            rig.Idle();
        }
        return Read(target, width, height);
    }
};

// The shader's math on the CPU: the texels as the view gives them (rgb, 0..1
// or beyond), a rectangle's width and height.
struct Reference {
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::function<std::array<double, 3>(int x, int y)> texel;

    std::array<double, 3> Sample(const MirrorShaderConstants& c, double px, double py) const {
        const double x = px / c.target_size[0] * 2.0 - 1.0;
        const double y = 1.0 - py / c.target_size[1] * 2.0;
        const double w = c.rows[2][0] * x + c.rows[2][1] * y + c.rows[2][2];
        if (w <= 1e-6) return {0, 0, 0};
        const double u = (c.rows[0][0] * x + c.rows[0][1] * y + c.rows[0][2]) / w;
        const double v = (c.rows[1][0] * x + c.rows[1][1] * y + c.rows[1][2]) / w;
        if (u < 0 || v < 0 || u > 1 || v > 1) return {0, 0, 0};
        const double tx = u * width - 0.5, ty = v * height - 0.5;
        const double bx = std::floor(tx), by = std::floor(ty);
        const double fx = tx - bx, fy = ty - by;
        const auto at = [&](int ox, int oy) {
            return texel(std::clamp(static_cast<int>(bx) + ox, 0, static_cast<int>(width) - 1),
                         std::clamp(static_cast<int>(by) + oy, 0, static_cast<int>(height) - 1));
        };
        const auto a = at(0, 0), b = at(1, 0), d = at(0, 1), e = at(1, 1);
        std::array<double, 3> out{};
        for (int i = 0; i < 3; ++i) {
            out[i] = (a[i] * (1 - fx) + b[i] * fx) * (1 - fy) + (d[i] * (1 - fx) + e[i] * fx) * fy;
        }
        return out;
    }
    // The picture as 8-bit RGBA.
    std::vector<double> Picture(const MirrorHomography& h, std::uint32_t target_width, std::uint32_t target_height,
                                bool encode) const {
        const MirrorShaderConstants c = PackMirrorConstants(h, 0, 0, static_cast<std::int32_t>(width),
                                                            static_cast<std::int32_t>(height), target_width,
                                                            target_height, encode);
        const double offset = 0.25 * c.spread;
        std::vector<double> out(static_cast<std::size_t>(target_width) * target_height * 3);
        for (std::uint32_t y = 0; y < target_height; ++y) {
            for (std::uint32_t x = 0; x < target_width; ++x) {
                std::array<double, 3> sum{};
                for (const auto& [ox, oy] : {std::pair{-offset, -offset}, std::pair{offset, -offset},
                                              std::pair{-offset, offset}, std::pair{offset, offset}}) {
                    const auto sample = Sample(c, x + 0.5 + ox, y + 0.5 + oy);
                    for (int i = 0; i < 3; ++i) sum[i] += sample[i];
                }
                for (int i = 0; i < 3; ++i) {
                    double value = sum[i] * 0.25;
                    value = encode ? SrgbEncode(value) : std::clamp(value, 0.0, 1.0);
                    out[(static_cast<std::size_t>(y) * target_width + x) * 3 + i] = value * 255.0;
                }
            }
        }
        return out;
    }
};

struct Difference {
    double worst{0.0};
    double mean{0.0};
    bool alpha{true};
};
Difference Compare(const std::vector<std::uint8_t>& drawn, const std::vector<double>& expected) {
    Difference out;
    const std::size_t pixels = expected.size() / 3;
    for (std::size_t i = 0; i < pixels; ++i) {
        for (int c = 0; c < 3; ++c) {
            const double off = std::fabs(drawn[i * 4 + c] - expected[i * 3 + c]);
            out.worst = std::max(out.worst, off);
            out.mean += off;
        }
        out.alpha = out.alpha && drawn[i * 4 + 3] == 255;
    }
    out.mean /= static_cast<double>(pixels * 3);
    return out;
}

void DrawTests(Rig& rig, SubmittedImageGpu& gpu, const std::string& api) {
    Harness harness{rig, gpu};
    std::string error;
    Expect(gpu.SupportsMirror(), api + " has a mirror");

    // A double-wide, two-slice typeless image, as a game's; the right eye's
    // rectangle of slice 1.
    const std::uint32_t image_width = 224, image_height = 100;
    const XrRect2Di rect{{116, 8}, {96, 80}};
    void* image = nullptr;
    const XrSwapchain source = harness.Source(DXGI_FORMAT_R8G8B8A8_UNORM, image_width, image_height, 2, 1, true, &image);
    const std::vector<std::uint8_t> other = PatternImage(image_width, image_height, 3);
    const std::vector<std::uint8_t> pattern = PatternImage(image_width, image_height, 5);
    rig.Fill(image, DXGI_FORMAT_R8G8B8A8_UNORM, 0, other.data(), 4);
    rig.Fill(image, DXGI_FORMAT_R8G8B8A8_UNORM, 1, pattern.data(), 4);
    const SubmittedView view =
        Harness::View(source, DXGI_FORMAT_R8G8B8A8_UNORM, image_width, image_height, 2, 1, rect);
    Reference reference;
    reference.width = 96;
    reference.height = 80;
    reference.texel = [&](int x, int y) {
        const std::uint8_t* texel =
            &pattern[(static_cast<std::size_t>(y + rect.offset.y) * image_width + x + rect.offset.x) * 4];
        return std::array<double, 3>{texel[0] / 255.0, texel[1] / 255.0, texel[2] / 255.0};
    };
    // The eye's field of view, a little asymmetric, 6:5 like the rectangle.
    const XrFovf fov = Fov(-48, 40, 0, 0);
    XrFovf shaped = fov;
    const double span = std::tan(40 * kDeg) + std::tan(48 * kDeg);
    shaped.angleUp = static_cast<float>(std::atan(span * 80.0 / 96.0 * 0.45));
    shaped.angleDown = static_cast<float>(-std::atan(span * 80.0 / 96.0 * 0.55));

    // Identity: the whole rectangle at its own size is the rectangle's texels.
    {
        const XrSwapchain target = harness.Target(96, 80);
        const MirrorHomography whole = MirrorHomographyFor(shaped, WholeCrop(shaped), MirrorQuat{});
        const std::vector<std::uint8_t> drawn = harness.Draw(view, target, 96, 80, whole);
        bool same = true, alpha = true;
        for (std::uint32_t y = 0; y < 80; ++y) {
            for (std::uint32_t x = 0; x < 96; ++x) {
                const std::uint8_t* got = &drawn[(static_cast<std::size_t>(y) * 96 + x) * 4];
                const std::uint8_t* want =
                    &pattern[(static_cast<std::size_t>(y + rect.offset.y) * image_width + x + rect.offset.x) * 4];
                same = same && got[0] == want[0] && got[1] == want[1] && got[2] == want[2];
                alpha = alpha && got[3] == 255;
            }
        }
        Expect(same, api + ": with no rotation and no zoom the picture is the eye's texels, exactly");
        Expect(alpha, api + ": the picture is opaque");
        // And through the stabilizer with smoothing 0, at the crop's own scale.
        MirrorSettings settings;
        settings.smoothing = 0.0;
        settings.margin = 0.0;
        settings.aspect = MirrorAspect::k1x1;
        MirrorStabilizer stabilizer;
        const MirrorAim aim =
            stabilizer.Update(Pose(AxisAngle(0.1, 0.9, 0.2, 0.8)), shaped, 1'000'000'000ll, settings);
        const XrSwapchain square = harness.Target(80, 80);
        const Difference plain = Compare(harness.Draw(view, square, 80, 80, aim.homography),
                                         reference.Picture(aim.homography, 80, 80, false));
        Expect(plain.worst <= 1.0 && plain.alpha, api + ": smoothing 0 draws the plain crop (worst " +
                                                      std::to_string(plain.worst) + ")");
    }
    // A known rotation (yaw, pitch and roll) with a margin, 16:9.
    {
        const XrSwapchain target = harness.Target(160, 90);
        const MirrorQuat correction = MirrorMultiply(
            AxisAngle(0, 1, 0, -3 * kDeg), MirrorMultiply(AxisAngle(1, 0, 0, 2 * kDeg), AxisAngle(0, 0, 1, 5 * kDeg)));
        const MirrorHomography h = MirrorHomographyFor(shaped, MirrorCropFor(shaped, 16.0 / 9.0, 20.0), correction);
        Expect(MirrorFilled(h), "The test's rotation stays inside the image");
        const std::vector<std::uint8_t> drawn = harness.Draw(view, target, 160, 90, h);
        const Difference rotated = Compare(drawn, reference.Picture(h, 160, 90, false));
        Expect(rotated.worst <= 2.0 && rotated.mean < 0.6 && rotated.alpha,
               api + ": a rotated picture matches the reference (worst " + std::to_string(rotated.worst) + ", mean " +
                   std::to_string(rotated.mean) + ")");
        // It is not the unrotated picture.
        const Difference unrotated = Compare(
            drawn, reference.Picture(MirrorHomographyFor(shaped, MirrorCropFor(shaped, 16.0 / 9.0, 20.0), MirrorQuat{}),
                                     160, 90, false));
        Expect(unrotated.mean > 3.0, api + ": the rotation moves the picture");
        // The same target again with another aim: nothing of the last draw is left.
        const MirrorHomography back = MirrorHomographyFor(shaped, MirrorCropFor(shaped, 16.0 / 9.0, 20.0), MirrorQuat{});
        const Difference again = Compare(harness.Draw(view, target, 160, 90, back), reference.Picture(back, 160, 90, false));
        Expect(again.worst <= 2.0, api + ": each draw is the whole picture");
    }
    // Shrunk to a third: four spread samples a pixel.
    {
        const XrSwapchain target = harness.Target(32, 18);
        const MirrorHomography h =
            MirrorHomographyFor(shaped, MirrorCropFor(shaped, 16.0 / 9.0, 10.0), AxisAngle(0, 0, 1, -2 * kDeg));
        const Difference shrunk = Compare(harness.Draw(view, target, 32, 18, h), reference.Picture(h, 32, 18, false));
        Expect(shrunk.worst <= 2.0 && shrunk.mean < 0.6,
               api + ": a shrunk picture matches the reference (worst " + std::to_string(shrunk.worst) + ")");
    }
    // Outside the eye's rectangle: black, and nothing of the other eye or slice.
    {
        const XrSwapchain target = harness.Target(96, 80);
        const MirrorHomography h = MirrorHomographyFor(shaped, WholeCrop(shaped), AxisAngle(0, 1, 0, 25 * kDeg));
        const std::vector<std::uint8_t> drawn = harness.Draw(view, target, 96, 80, h);
        const std::vector<double> expected = reference.Picture(h, 96, 80, false);
        int black = 0, lit = 0;
        bool agrees = true;
        for (std::size_t i = 0; i < drawn.size() / 4; ++i) {
            const bool is_black = drawn[i * 4] == 0 && drawn[i * 4 + 1] == 0 && drawn[i * 4 + 2] == 0;
            const bool want_black = expected[i * 3] == 0.0 && expected[i * 3 + 1] == 0.0 && expected[i * 3 + 2] == 0.0;
            black += is_black;
            lit += !is_black;
            // A pixel half in, half out differs by rounding at the edge only.
            if (want_black != is_black && std::fabs(drawn[i * 4] - expected[i * 3]) > 40) agrees = false;
        }
        Expect(black > 500 && lit > 500 && agrees, api + ": what lies outside the eye's rectangle is black");
        const Difference edge = Compare(drawn, reference.Picture(h, 96, 80, false));
        Expect(edge.mean < 0.6, api + ": a picture partly outside matches the reference");
    }
    // An sRGB image: its linear texels are encoded for the window.
    {
        void* srgb_image = nullptr;
        const XrSwapchain srgb =
            harness.Source(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 96, 80, 1, 1, true, &srgb_image);
        const std::vector<std::uint8_t> data = PatternImage(96, 80, 1);
        rig.Fill(srgb_image, DXGI_FORMAT_R8G8B8A8_UNORM, 0, data.data(), 4);
        const SubmittedView srgb_view =
            Harness::View(srgb, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 96, 80, 1, 0, {{0, 0}, {96, 80}});
        const XrSwapchain target = harness.Target(96, 80);
        const MirrorHomography whole = MirrorHomographyFor(shaped, WholeCrop(shaped), MirrorQuat{});
        const std::vector<std::uint8_t> drawn = harness.Draw(srgb_view, target, 96, 80, whole);
        int worst = 0;
        for (std::size_t i = 0; i < drawn.size() / 4; ++i) {
            for (int c = 0; c < 3; ++c) worst = std::max(worst, std::abs(drawn[i * 4 + c] - data[i * 4 + c]));
        }
        Expect(worst <= 1, api + ": an sRGB image is shown as it is stored (worst " + std::to_string(worst) + ")");
        Reference linear;
        linear.width = 96;
        linear.height = 80;
        linear.texel = [&](int x, int y) {
            const std::uint8_t* texel = &data[(static_cast<std::size_t>(y) * 96 + x) * 4];
            return std::array<double, 3>{SrgbDecode(texel[0] / 255.0), SrgbDecode(texel[1] / 255.0),
                                         SrgbDecode(texel[2] / 255.0)};
        };
        const XrSwapchain small_target = harness.Target(48, 27);
        const MirrorHomography h =
            MirrorHomographyFor(shaped, MirrorCropFor(shaped, 16.0 / 9.0, 10.0), AxisAngle(0, 1, 0, 2 * kDeg));
        const Difference mixed = Compare(harness.Draw(srgb_view, small_target, 48, 27, h), linear.Picture(h, 48, 27, true));
        Expect(mixed.worst <= 2.0, api + ": sRGB texels are mixed linear and encoded (worst " +
                                       std::to_string(mixed.worst) + ")");
    }
    // Blue-first images: the channels land where they belong.
    {
        void* bgra_image = nullptr;
        const XrSwapchain bgra = harness.Source(DXGI_FORMAT_B8G8R8A8_UNORM, 40, 30, 1, 1, true, &bgra_image);
        std::vector<std::uint8_t> data(40 * 30 * 4);
        for (std::size_t i = 0; i < data.size() / 4; ++i) {
            data[i * 4] = 200; // blue
            data[i * 4 + 1] = 100;
            data[i * 4 + 2] = 30; // red
            data[i * 4 + 3] = 7;
        }
        rig.Fill(bgra_image, DXGI_FORMAT_B8G8R8A8_UNORM, 0, data.data(), 4);
        const XrSwapchain target = harness.Target(40, 30);
        const XrFovf plain = Fov(-40, 40, 30, -30);
        const std::vector<std::uint8_t> drawn =
            harness.Draw(Harness::View(bgra, DXGI_FORMAT_B8G8R8A8_UNORM, 40, 30, 1, 0, {{0, 0}, {40, 30}}), target, 40,
                         30, MirrorHomographyFor(plain, WholeCrop(plain), MirrorQuat{}));
        Expect(drawn[0] == 30 && drawn[1] == 100 && drawn[2] == 200 && drawn[3] == 255,
               api + ": a blue-first image's red is red in the picture");
    }
    // Half floats: linear, encoded, clamped to what a window can show.
    {
        void* float_image = nullptr;
        const XrSwapchain floats = harness.Source(DXGI_FORMAT_R16G16B16A16_FLOAT, 16, 16, 1, 1, true, &float_image);
        std::vector<std::uint16_t> data(16 * 16 * 4);
        for (std::size_t i = 0; i < data.size() / 4; ++i) {
            data[i * 4] = Half(0.25f);
            data[i * 4 + 1] = Half(4.0f);
            data[i * 4 + 2] = Half(-1.0f);
            data[i * 4 + 3] = Half(0.5f);
        }
        rig.Fill(float_image, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, data.data(), 8);
        const XrSwapchain target = harness.Target(16, 16);
        const XrFovf plain = Fov(-40, 40, 40, -40);
        const std::vector<std::uint8_t> drawn =
            harness.Draw(Harness::View(floats, DXGI_FORMAT_R16G16B16A16_FLOAT, 16, 16, 1, 0, {{0, 0}, {16, 16}}), target,
                         16, 16, MirrorHomographyFor(plain, WholeCrop(plain), MirrorQuat{}));
        Expect(std::abs(drawn[0] - 137) <= 1 && drawn[1] == 255 && drawn[2] == 0 && drawn[3] == 255,
               api + ": half floats are encoded and clamped to 0..1");
    }
    // Refusals leave the mirror usable.
    {
        const XrSwapchain target = harness.Target(96, 80);
        const MirrorHomography whole = MirrorHomographyFor(shaped, WholeCrop(shaped), MirrorQuat{});
        MirrorDraw draw;
        draw.target = target;
        draw.width = 96;
        draw.height = 80;
        draw.homography = whole;
        const auto refused = [&](SubmittedView bad, MirrorDraw bad_draw, const std::string& what) {
            rig.Idle();
            std::string why;
            Expect(gpu.DrawMirror(bad, bad_draw, why) == MirrorDrawResult::kFailed && !why.empty(),
                   api + ": " + what + " is refused");
        };
        SubmittedView bad = view;
        bad.swapchain = reinterpret_cast<XrSwapchain>(std::uintptr_t{0x999});
        refused(bad, draw, "an untracked source");
        bad = view;
        bad.image_index = 3;
        refused(bad, draw, "an image index outside the swapchain");
        bad = view;
        bad.array_slice = 2;
        refused(bad, draw, "a slice outside the array");
        bad = view;
        bad.rect = {{200, 8}, {96, 80}};
        refused(bad, draw, "a rectangle outside the image");
        bad = view;
        bad.rect.extent.width = 0;
        refused(bad, draw, "an empty rectangle");
        bad = view;
        bad.format = DXGI_FORMAT_R11G11B10_FLOAT;
        refused(bad, draw, "a format that is not read");
        bad = view;
        bad.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        refused(bad, draw, "a format of another family than the image's");
        const XrSwapchain multisampled = harness.Source(DXGI_FORMAT_R8G8B8A8_UNORM, 32, 32, 1, 4);
        refused(Harness::View(multisampled, DXGI_FORMAT_R8G8B8A8_UNORM, 32, 32, 1, 0, {{0, 0}, {32, 32}}, 4), draw,
                "a multisampled source");
        MirrorDraw bad_draw = draw;
        bad_draw.target = XR_NULL_HANDLE;
        refused(view, bad_draw, "a draw with neither a window nor a target");
        bad_draw = draw;
        bad_draw.width = 0;
        refused(view, bad_draw, "a picture of no size");
        bad_draw = draw;
        bad_draw.width = 200;
        refused(view, bad_draw, "a target smaller than the picture");
        bad_draw = draw;
        bad_draw.target = harness.Target(96, 80, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
        refused(view, bad_draw, "an sRGB target");
        bad_draw = draw;
        bad_draw.target_image = 1;
        refused(view, bad_draw, "a target image outside its swapchain");
        const Difference after =
            Compare(harness.Draw(view, target, 96, 80, whole), reference.Picture(whole, 96, 80, false));
        Expect(after.worst <= 1.0, api + ": after refusals the mirror draws as before");
    }

    // A window (never put on the desktop): frames are presented without ever
    // waiting, through a resize, a hide and the window's end.
    {
        MirrorWindow window;
        Expect(window.Start(WindowOptions(320, 180), error) && WaitFor([&] { return window.Handle() != nullptr; }),
               "The test's window is made: " + error);
        MirrorStabilizer stabilizer;
        MirrorSettings settings;
        settings.smoothing = 70.0;
        int shown = 0, skipped = 0;
        double worst_ms = 0.0, total_ms = 0.0;
        const auto frame = [&](int number) {
            MirrorDraw draw;
            draw.window = window.Handle();
            window.OutputSize(draw.width, draw.height);
            draw.on_window_thread = [&window](std::function<void()> task) { return window.Post(std::move(task)); };
            const MirrorQuat head = AxisAngle(0, 1, 0, 0.5 * kDeg * std::sin(number * 0.9));
            draw.homography =
                stabilizer.Update(Pose(head), shaped, 1'000'000'000ll + number * 11'111'111ll, settings).homography;
            std::string why;
            const auto begun = std::chrono::steady_clock::now();
            const MirrorDrawResult result = gpu.DrawMirror(view, draw, why);
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begun).count();
            worst_ms = std::max(worst_ms, ms);
            total_ms += ms;
            Expect(result != MirrorDrawResult::kFailed, api + ": a frame is drawn into the window: " + why);
            (result == MirrorDrawResult::kShown ? shown : skipped)++;
        };
        for (int number = 0; number < 40; ++number) {
            frame(number);
            // The swap chain is made on the window's thread: a moment for it.
            if (shown == 0) Sleep(5);
        }
        Expect(shown > 0, api + ": frames reach the window");
        const int before_resize = shown;
        window.Reshape(480, 360, 4.0 / 3.0, 480, L"");
        std::uint32_t width = 0, height = 0;
        Expect(WaitFor([&] {
                   window.OutputSize(width, height);
                   return width == 480 && height == 360;
               }),
               "The window is resized");
        for (int number = 40; number < 80; ++number) {
            frame(number);
            // Give the software GPU a moment now and then, as a game's frame does.
            if (number % 4 == 0) rig.Idle();
            // The new size's swap chain is made on the window's thread.
            if (shown == before_resize) Sleep(5);
        }
        Expect(shown > before_resize, api + ": frames reach the window at its new picture size");
        // Dragged and resized on the desktop, sixty sizes: frames go on
        // being shown, and not one swap chain is made for it.
        {
            rig.Idle();
            const std::uint32_t made_before = MirrorSwapChainsMadeForTests();
            const int shown_before = shown;
            const HWND handle = static_cast<HWND>(window.Handle());
            RECT place{};
            GetWindowRect(handle, &place);
            int other_sizes = 0;
            for (int step = 0; step < 60; ++step) {
                const int wide = 240 + step * 13;
                SetWindowPos(handle, nullptr, place.left + step, place.top + step, wide, wide * 3 / 4,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                std::uint32_t client_width = 0, client_height = 0, picture_width = 0, picture_height = 0;
                window.PictureSize(client_width, client_height);
                window.OutputSize(picture_width, picture_height);
                Expect(picture_width == 480 && picture_height == 360,
                       api + ": the picture's size stays the settings' while the window is resized");
                other_sizes += client_width != 480;
                frame(1000 + step);
                if (step % 4 == 0) rig.Idle();
            }
            Expect(other_sizes >= 55, api + ": the window did have other sizes");
            Expect(MirrorSwapChainsMadeForTests() == made_before,
                   api + ": resizing the window sixty times makes no swap chain (" +
                       std::to_string(MirrorSwapChainsMadeForTests() - made_before) + " made)");
            Expect(shown > shown_before, api + ": frames are shown while the window is resized");
            std::cout << api << " resize: " << shown - shown_before
                      << " of 60 frames shown across 60 window sizes, no swap chain made" << std::endl;
        }
        // Twenty back to back with no pause at all: none may wait.
        for (int number = 80; number < 100; ++number) frame(number);
        Expect(worst_ms < 1500.0, api + ": no frame stalls (worst " + std::to_string(worst_ms) + " ms)");
        std::cout << api << " window: " << shown << " frames shown, " << skipped << " skipped, worst call "
                  << worst_ms << " ms, mean " << total_ms / 160.0 << " ms\n";
        // The swap chain goes, then the window; a target draw still works.
        rig.Idle();
        gpu.CloseMirror();
        gpu.CloseMirror();
        window.Stop();
        const XrSwapchain target = harness.Target(96, 80);
        const MirrorHomography whole = MirrorHomographyFor(shaped, WholeCrop(shaped), MirrorQuat{});
        Expect(Compare(harness.Draw(view, target, 96, 80, whole), reference.Picture(whole, 96, 80, false)).worst <= 1.0,
               api + ": the mirror draws on after its window closed");
        // A second window, with no thread of its own offered to make the
        // swap chain on: it is made by the caller, anew for the window.
        MirrorWindow second;
        Expect(second.Start(WindowOptions(200, 200), error) && WaitFor([&] { return second.Handle() != nullptr; }),
               "A second window is made");
        MirrorDraw draw;
        draw.window = second.Handle();
        second.OutputSize(draw.width, draw.height);
        draw.homography = whole;
        int reached = 0;
        for (int attempt = 0; attempt < 20; ++attempt) {
            const MirrorDrawResult result = gpu.DrawMirror(view, draw, error);
            Expect(result != MirrorDrawResult::kFailed, api + ": a frame is drawn into the second window: " + error);
            reached += result == MirrorDrawResult::kShown;
            rig.Idle();
        }
        Expect(reached > 0, api + ": frames reach the second window");
        // The window destroyed under the swap chain: a failure or a skip, never a hang or a crash.
        second.Stop();
        const MirrorDrawResult gone = gpu.DrawMirror(view, draw, error);
        Expect(gone == MirrorDrawResult::kFailed || gone == MirrorDrawResult::kSkipped || gone == MirrorDrawResult::kShown,
               api + ": a frame for a window that is gone returns");
        rig.Idle();
        gpu.CloseMirror();
    }
    // The window's swap chain is made on the window's own thread: however
    // long that takes (made slow here), no draw call waits for it; the
    // frames meanwhile are skipped.
    {
        SetMirrorSwapChainDelayForTests(400);
        MirrorWindow window;
        Expect(window.Start(WindowOptions(320, 180), error) && WaitFor([&] { return window.Handle() != nullptr; }),
               "The slow test's window is made: " + error);
        const MirrorHomography whole = MirrorHomographyFor(shaped, WholeCrop(shaped), MirrorQuat{});
        // Draws every few milliseconds until a frame is shown; the worst
        // call, and how long the frames were skipped.
        const auto until_shown = [&](double& worst_ms, double& waited_ms, int& skipped) {
            worst_ms = 0.0;
            skipped = 0;
            const auto started = std::chrono::steady_clock::now();
            for (;;) {
                MirrorDraw draw;
                draw.window = window.Handle();
                window.OutputSize(draw.width, draw.height);
                draw.homography = whole;
                draw.on_window_thread = [&window](std::function<void()> task) { return window.Post(std::move(task)); };
                std::string why;
                const auto begun = std::chrono::steady_clock::now();
                const MirrorDrawResult result = gpu.DrawMirror(view, draw, why);
                const auto ended = std::chrono::steady_clock::now();
                worst_ms = std::max(worst_ms, std::chrono::duration<double, std::milli>(ended - begun).count());
                waited_ms = std::chrono::duration<double, std::milli>(ended - started).count();
                Expect(result != MirrorDrawResult::kFailed, api + ": a frame while the swap chain is made: " + why);
                if (result == MirrorDrawResult::kShown) return;
                ++skipped;
                Expect(waited_ms < 20000.0, api + ": the swap chain is never there");
                rig.Idle();
                Sleep(2);
            }
        };
        double worst_ms = 0.0, waited_ms = 0.0;
        int skipped = 0;
        until_shown(worst_ms, waited_ms, skipped);
        Expect(waited_ms >= 380.0 && skipped >= 5,
               api + ": while the swap chain is being made the frames are skipped (" + std::to_string(waited_ms) + " ms)");
        Expect(worst_ms < 150.0, api + ": opening the window stalls no draw call (worst " + std::to_string(worst_ms) +
                                     " ms with a 400 ms creation)");
        std::cout << api << " slow creation: opening skipped " << skipped << " calls over " << waited_ms
                  << " ms, worst call " << worst_ms << " ms";
        // A new picture size (the settings') makes a new one, the same way.
        window.Reshape(400, 300, 4.0 / 3.0, 400, L"");
        std::uint32_t width = 0, height = 0;
        Expect(WaitFor([&] {
                   window.OutputSize(width, height);
                   return width == 400 && height == 300;
               }),
               "The slow test's window is resized");
        until_shown(worst_ms, waited_ms, skipped);
        Expect(waited_ms >= 380.0 && skipped >= 5 && worst_ms < 150.0,
               api + ": a new picture size stalls no draw call either (worst " + std::to_string(worst_ms) + " ms)");
        std::cout << "; a new picture size skipped " << skipped << " calls over " << waited_ms << " ms, worst call " << worst_ms
                  << " ms" << std::endl;
        // Closed while a new one is being made: nothing waits, nothing is left.
        window.Reshape(320, 180, 16.0 / 9.0, 320, L"");
        Expect(WaitFor([&] {
                   window.OutputSize(width, height);
                   return width == 320;
               }),
               "The slow test's window is resized back");
        rig.Idle();
        MirrorDraw draw;
        draw.window = window.Handle();
        draw.width = 320;
        draw.height = 180;
        draw.homography = whole;
        draw.on_window_thread = [&window](std::function<void()> task) { return window.Post(std::move(task)); };
        Expect(gpu.DrawMirror(view, draw, error) == MirrorDrawResult::kSkipped,
               api + ": the first frame at a new size is skipped while its swap chain is made");
        const auto begun = std::chrono::steady_clock::now();
        gpu.CloseMirror();
        Expect(std::chrono::steady_clock::now() - begun < std::chrono::milliseconds(150),
               api + ": closing the mirror does not wait for a swap chain being made");
        window.Stop();
        SetMirrorSwapChainDelayForTests(0);
        const XrSwapchain target = harness.Target(96, 80);
        Expect(Compare(harness.Draw(view, target, 96, 80, whole), reference.Picture(whole, 96, 80, false)).worst <= 1.0,
               api + ": the mirror draws on after a swap chain was abandoned in the making");
    }
    // The swapchain destroyed right behind a draw.
    {
        void* brief_image = nullptr;
        const XrSwapchain brief = harness.Source(DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 1, true, &brief_image);
        const XrSwapchain target = harness.Target(64, 64);
        const XrFovf plain = Fov(-40, 40, 40, -40);
        MirrorDraw draw;
        draw.target = target;
        draw.width = 64;
        draw.height = 64;
        draw.homography = MirrorHomographyFor(plain, WholeCrop(plain), MirrorQuat{});
        rig.Idle();
        Expect(gpu.DrawMirror(Harness::View(brief, DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 0, {{0, 0}, {64, 64}}), draw,
                              error) == MirrorDrawResult::kShown,
               api + ": a last draw from a swapchain: " + error);
        gpu.Forget(brief);
        Expect(!gpu.Tracked(brief), api + ": the swapchain is forgotten behind its draw");
        rig.Idle();
    }
}

// ---- Direct3D 11 ----

struct D3D11Rig final : Rig {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    std::vector<ComPtr<ID3D11Texture2D>> images;

    SubmittedImageApi Api() const override { return SubmittedImageApi::kD3D11; }
    void* Image(std::int64_t format, std::uint32_t width, std::uint32_t height, std::uint32_t slices,
                std::uint32_t samples, bool readable, bool typed) override {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = slices;
        desc.Format = static_cast<DXGI_FORMAT>(typed ? format : Typeless(format));
        desc.SampleDesc.Count = samples;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | (readable ? D3D11_BIND_SHADER_RESOURCE : 0);
        ComPtr<ID3D11Texture2D> texture;
        Expect(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)), "A Direct3D 11 texture is created");
        images.push_back(texture);
        return texture.Get();
    }
    void Fill(void* image, std::int64_t, std::uint32_t slice, const void* data, std::uint32_t bytes_per_texel) override {
        auto* texture = static_cast<ID3D11Texture2D*>(image);
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        context->UpdateSubresource(texture, D3D11CalcSubresource(0, slice, 1), nullptr, data,
                                   desc.Width * bytes_per_texel, 0);
    }
    void ReleaseImages() override { images.clear(); }
    void Idle() override { context->Flush(); }
};

int D3D11Tests() {
    D3D11Rig rig;
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    Expect(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                                       &rig.device, nullptr, &rig.context)),
           "A Direct3D 11 WARP device is created");
    std::string error;
    for (const bool context_states : {true, false}) {
        const std::string api = context_states ? "Direct3D 11" : "Direct3D 11 (state saved and restored)";
        // The second pass on a device that guards its immediate context
        // against other threads: there the window's thread also makes a new
        // swap chain's first present.
        ComPtr<ID3D10Multithread> guarded;
        Expect(SUCCEEDED(rig.context.As(&guarded)), "The context's multithread interface");
        guarded->SetMultithreadProtected(context_states ? FALSE : TRUE);
        std::unique_ptr<SubmittedImageGpu> gpu = CreateSubmittedImageD3D11(rig.device.Get(), context_states);
        Expect(gpu != nullptr, "The Direct3D 11 backend is made");
        DrawTests(rig, *gpu, api);

        Harness harness{rig, *gpu};
        // A source the runtime made without shader-resource binding is copied first.
        {
            void* image = nullptr;
            const XrSwapchain source = harness.Source(DXGI_FORMAT_R8G8B8A8_UNORM, 80, 60, 2, 1, false, &image);
            const std::vector<std::uint8_t> data = PatternImage(80, 60, 9);
            rig.Fill(image, DXGI_FORMAT_R8G8B8A8_UNORM, 1, data.data(), 4);
            const XrRect2Di rect{{40, 10}, {40, 30}};
            const XrSwapchain target = harness.Target(40, 30);
            const XrFovf plain = Fov(-40, 40, 30, -30);
            const std::vector<std::uint8_t> drawn =
                harness.Draw(Harness::View(source, DXGI_FORMAT_R8G8B8A8_UNORM, 80, 60, 2, 1, rect), target, 40, 30,
                             MirrorHomographyFor(plain, WholeCrop(plain), MirrorQuat{}));
            bool same = true;
            for (std::uint32_t y = 0; y < 30; ++y) {
                for (std::uint32_t x = 0; x < 40; ++x) {
                    for (int c = 0; c < 3; ++c) {
                        same = same && drawn[(y * 40 + x) * 4 + c] == data[((y + 10) * 80 + x + 40) * 4 + c];
                    }
                }
            }
            Expect(same, api + ": a source that cannot be bound for reading is copied and shown exactly");
        }
        // The game's context state is as it was after a frame.
        {
            ComPtr<ID3D11Texture2D> scratch = static_cast<ID3D11Texture2D*>(
                rig.Image(DXGI_FORMAT_R8G8B8A8_UNORM, 8, 8, 1, 1, true, true));
            ComPtr<ID3D11RenderTargetView> game_target;
            ComPtr<ID3D11ShaderResourceView> game_resource;
            ComPtr<ID3D11Texture2D> sampled = static_cast<ID3D11Texture2D*>(
                rig.Image(DXGI_FORMAT_R8G8B8A8_UNORM, 8, 8, 1, 1, true, true));
            Expect(SUCCEEDED(rig.device->CreateRenderTargetView(scratch.Get(), nullptr, &game_target)) &&
                       SUCCEEDED(rig.device->CreateShaderResourceView(sampled.Get(), nullptr, &game_resource)),
                   "The stand-in game's views are made");
            D3D11_BLEND_DESC blend_desc{};
            blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED;
            ComPtr<ID3D11BlendState> blend;
            D3D11_RASTERIZER_DESC raster_desc{};
            raster_desc.FillMode = D3D11_FILL_SOLID;
            raster_desc.CullMode = D3D11_CULL_FRONT;
            raster_desc.ScissorEnable = TRUE;
            ComPtr<ID3D11RasterizerState> raster;
            D3D11_DEPTH_STENCIL_DESC depth_desc{};
            depth_desc.DepthEnable = TRUE;
            depth_desc.DepthFunc = D3D11_COMPARISON_NEVER;
            ComPtr<ID3D11DepthStencilState> depth;
            Expect(SUCCEEDED(rig.device->CreateBlendState(&blend_desc, &blend)) &&
                       SUCCEEDED(rig.device->CreateRasterizerState(&raster_desc, &raster)) &&
                       SUCCEEDED(rig.device->CreateDepthStencilState(&depth_desc, &depth)),
                   "The stand-in game's states are made");
            ID3D11RenderTargetView* targets[] = {game_target.Get()};
            rig.context->OMSetRenderTargets(1, targets, nullptr);
            const float factor[4] = {0.1f, 0.2f, 0.3f, 0.4f};
            rig.context->OMSetBlendState(blend.Get(), factor, 0x0f0f0f0fu);
            rig.context->OMSetDepthStencilState(depth.Get(), 3);
            rig.context->RSSetState(raster.Get());
            const D3D11_VIEWPORT viewport{1.0f, 2.0f, 7.0f, 5.0f, 0.0f, 1.0f};
            rig.context->RSSetViewports(1, &viewport);
            const D3D11_RECT scissor{0, 0, 1, 1};
            rig.context->RSSetScissorRects(1, &scissor);
            rig.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
            ID3D11ShaderResourceView* resources[] = {game_resource.Get()};
            rig.context->PSSetShaderResources(0, 1, resources);

            void* image = nullptr;
            const XrSwapchain source = harness.Source(DXGI_FORMAT_R8G8B8A8_UNORM, 32, 32, 1, 1, true, &image);
            const std::vector<std::uint8_t> data = PatternImage(32, 32, 2);
            rig.Fill(image, DXGI_FORMAT_R8G8B8A8_UNORM, 0, data.data(), 4);
            const XrSwapchain target = harness.Target(32, 32);
            const XrFovf plain = Fov(-40, 40, 40, -40);
            const std::vector<std::uint8_t> drawn =
                harness.Draw(Harness::View(source, DXGI_FORMAT_R8G8B8A8_UNORM, 32, 32, 1, 0, {{0, 0}, {32, 32}}), target,
                             32, 32, MirrorHomographyFor(plain, WholeCrop(plain), MirrorQuat{}));
            bool same = true;
            for (std::size_t i = 0; i < drawn.size() / 4; ++i) {
                for (int c = 0; c < 3; ++c) same = same && drawn[i * 4 + c] == data[i * 4 + c];
            }
            Expect(same, api + ": the picture is drawn whole whatever state the game left (blend, depth, cull, scissor)");

            ComPtr<ID3D11RenderTargetView> now_target;
            rig.context->OMGetRenderTargets(1, &now_target, nullptr);
            ComPtr<ID3D11BlendState> now_blend;
            float now_factor[4]{};
            UINT now_mask = 0;
            rig.context->OMGetBlendState(&now_blend, now_factor, &now_mask);
            ComPtr<ID3D11DepthStencilState> now_depth;
            UINT now_ref = 0;
            rig.context->OMGetDepthStencilState(&now_depth, &now_ref);
            ComPtr<ID3D11RasterizerState> now_raster;
            rig.context->RSGetState(&now_raster);
            D3D11_VIEWPORT now_viewport{};
            UINT viewports = 1;
            rig.context->RSGetViewports(&viewports, &now_viewport);
            D3D11_PRIMITIVE_TOPOLOGY now_topology{};
            rig.context->IAGetPrimitiveTopology(&now_topology);
            ComPtr<ID3D11ShaderResourceView> now_resource;
            rig.context->PSGetShaderResources(0, 1, &now_resource);
            ComPtr<ID3D11VertexShader> now_vertex;
            rig.context->VSGetShader(&now_vertex, nullptr, nullptr);
            Expect(now_target == game_target && now_blend == blend && now_factor[2] == 0.3f && now_mask == 0x0f0f0f0fu &&
                       now_depth == depth && now_ref == 3 && now_raster == raster && viewports == 1 &&
                       now_viewport.Width == 7.0f && now_viewport.TopLeftY == 2.0f &&
                       now_topology == D3D11_PRIMITIVE_TOPOLOGY_LINELIST && now_resource == game_resource &&
                       !now_vertex,
                   api + ": the game's context state is as it left it");
            rig.context->ClearState();
        }
        // A quad views eye that was not composed into one image: its focus
        // view drawn over the part of the eye it covers.
        for (const bool readable : {true, false}) {
            const std::string how = api + (readable ? "" : ", images copied first");
            Expect(gpu->SupportsFocus(), how + ": a focus view can be drawn over its eye");
            // The eye's own (peripheral) view: 64x48 of a larger image, shown
            // at twice its size. The focus view: 64x48 texels of slice 1 of
            // another image, over the middle half of the eye, which is 64x48
            // pixels of the picture: its texels one to one.
            void* eye_image = nullptr;
            void* focus_image = nullptr;
            const XrSwapchain eye_source =
                harness.Source(DXGI_FORMAT_R8G8B8A8_UNORM, 96, 64, 1, 1, readable, &eye_image);
            const XrSwapchain focus_source =
                harness.Source(DXGI_FORMAT_R8G8B8A8_UNORM, 100, 60, 2, 1, readable, &focus_image);
            const std::vector<std::uint8_t> eye_data = PatternImage(96, 64, 3);
            const std::vector<std::uint8_t> focus_data = PatternImage(100, 60, 7);
            rig.Fill(eye_image, DXGI_FORMAT_R8G8B8A8_UNORM, 0, eye_data.data(), 4);
            rig.Fill(focus_image, DXGI_FORMAT_R8G8B8A8_UNORM, 1, focus_data.data(), 4);
            const XrRect2Di eye_rect{{20, 10}, {64, 48}};
            const XrRect2Di focus_rect{{30, 8}, {64, 48}};
            const SubmittedView eye_view = Harness::View(eye_source, DXGI_FORMAT_R8G8B8A8_UNORM, 96, 64, 1, 0, eye_rect);
            SubmittedFocus focus;
            focus.present = true;
            focus.view = Harness::View(focus_source, DXGI_FORMAT_R8G8B8A8_UNORM, 100, 60, 2, 1, focus_rect);
            focus.place.rect = {0.25f, 0.25f, 0.75f, 0.75f};
            focus.place.feather = {4.0f / 128.0f, 4.0f / 96.0f}; // four pixels of the picture
            const XrSwapchain target = harness.Target(128, 96);
            const auto draw_with = [&](const SubmittedFocus& over, std::string& why) {
                MirrorDraw draw;
                draw.target = target;
                draw.width = 128;
                draw.height = 96;
                draw.homography = MirrorWholeView();
                draw.focus = over;
                return gpu->DrawMirror(eye_view, draw, why);
            };
            std::string why;
            Expect(draw_with(SubmittedFocus{}, why) == MirrorDrawResult::kShown, how + ": the eye alone is drawn: " + why);
            const std::vector<std::uint8_t> alone = harness.Read(target, 128, 96);
            // A stand-in game's resource in the second slot, which only the
            // focus view's draw uses.
            ComPtr<ID3D11Texture2D> sampled = static_cast<ID3D11Texture2D*>(
                rig.Image(DXGI_FORMAT_R8G8B8A8_UNORM, 8, 8, 1, 1, true, true));
            ComPtr<ID3D11ShaderResourceView> game_resource;
            Expect(SUCCEEDED(rig.device->CreateShaderResourceView(sampled.Get(), nullptr, &game_resource)),
                   "The stand-in game's resource view is made");
            ID3D11ShaderResourceView* const resources[] = {game_resource.Get()};
            rig.context->PSSetShaderResources(1, 1, resources);
            Expect(draw_with(focus, why) == MirrorDrawResult::kShown, how + ": the eye with its focus view is drawn: " + why);
            ComPtr<ID3D11ShaderResourceView> now_resource;
            rig.context->PSGetShaderResources(1, 1, &now_resource);
            Expect(now_resource == game_resource, how + ": the game's second resource slot is as it left it");
            rig.context->ClearState();
            const std::vector<std::uint8_t> composed = harness.Read(target, 128, 96);
            bool outside = true, inside = true, band = true, differs = false;
            int band_pixels = 0;
            for (std::uint32_t y = 0; y < 96; ++y) {
                for (std::uint32_t x = 0; x < 128; ++x) {
                    const std::uint8_t* got = &composed[(static_cast<std::size_t>(y) * 128 + x) * 4];
                    const std::uint8_t* eye = &alone[(static_cast<std::size_t>(y) * 128 + x) * 4];
                    // Pixels from the focus rectangle's edge (32..96 across, 24..72 down).
                    const double edge = std::min(std::min(x + 0.5 - 32.0, 96.0 - (x + 0.5)),
                                                 std::min(y + 0.5 - 24.0, 72.0 - (y + 0.5)));
                    if (edge < 0.0) {
                        for (int c = 0; c < 3; ++c) outside = outside && got[c] == eye[c];
                        continue;
                    }
                    const std::uint8_t* texel =
                        &focus_data[(static_cast<std::size_t>(y - 24 + 8) * 100 + (x - 32 + 30)) * 4];
                    if (edge >= 4.0) {
                        for (int c = 0; c < 3; ++c) {
                            inside = inside && std::abs(static_cast<int>(got[c]) - texel[c]) <= 1;
                            differs = differs || got[c] != eye[c];
                        }
                        continue;
                    }
                    // In the band: the compositor's smooth step from the eye to the focus view.
                    const double t = edge / 4.0;
                    const double share = t * t * (3.0 - 2.0 * t);
                    ++band_pixels;
                    for (int c = 0; c < 3; ++c) {
                        const double want = eye[c] + (static_cast<double>(texel[c]) - eye[c]) * share;
                        band = band && std::fabs(got[c] - want) <= 2.0;
                    }
                }
            }
            Expect(outside, how + ": outside the focus view the picture is the eye's own view, bit for bit");
            Expect(inside && differs, how + ": inside it the picture is the focus view's texels, of its slice and rectangle");
            Expect(band && band_pixels > 0, how + ": across the band the two are blended as the quad views compositor blends");
            Expect(composed[3] == 255 && composed[(48 * 128 + 64) * 4 + 3] == 255, how + ": the picture is opaque");

            // A screenshot of the eye with its focus view: the same picture, read back.
            const std::uint64_t ticket = gpu->ReadbackComposed(eye_view, focus, 128, 96, why);
            Expect(ticket != 0, how + ": the eye with its focus view is read back: " + why);
            SubmittedPixels pixels;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            ReadbackState state = ReadbackState::kPending;
            while ((state = gpu->Poll(ticket, pixels, why)) == ReadbackState::kPending &&
                   std::chrono::steady_clock::now() < deadline) {
                rig.Idle();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            Expect(state == ReadbackState::kReady && pixels.width == 128 && pixels.height == 96 &&
                       pixels.format.kind == SubmittedPixelKind::kUnorm8 && !pixels.format.srgb && !pixels.format.bgra,
                   how + ": the readback is the picture's size, 8-bit and display-encoded: " + why);
            bool same = state == ReadbackState::kReady;
            for (std::uint32_t y = 0; same && y < 96; ++y) {
                for (std::uint32_t x = 0; x < 128; ++x) {
                    for (int c = 0; c < 3; ++c) {
                        same = same && pixels.data[static_cast<std::size_t>(y) * pixels.row_pitch + x * 4 + c] ==
                                           composed[(static_cast<std::size_t>(y) * 128 + x) * 4 + c];
                    }
                }
            }
            Expect(same, how + ": the readback holds the picture the window would show");
            gpu->Finish(ticket);
            // A larger picture than the first, then the first's size again.
            const std::uint64_t larger = gpu->ReadbackComposed(eye_view, focus, 256, 192, why);
            Expect(larger != 0, how + ": a larger picture is read back: " + why);
            gpu->Finish(larger);

            // A focus view that cannot be drawn fails the draw and leaves the mirror usable.
            SubmittedFocus bad = focus;
            bad.view.rect = {{60, 8}, {64, 48}};
            Expect(draw_with(bad, why) == MirrorDrawResult::kFailed && why.find("focus view") != std::string::npos,
                   how + ": a focus rectangle outside its image is refused: " + why);
            bad = focus;
            bad.view.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            Expect(draw_with(bad, why) == MirrorDrawResult::kFailed && why.find("encoded") != std::string::npos,
                   how + ": a focus view encoded otherwise than its eye is refused: " + why);
            Expect(gpu->ReadbackComposed(eye_view, bad, 128, 96, why) == 0 && gpu->ReadbackComposed(eye_view, focus, 0, 96, why) == 0 &&
                       gpu->ReadbackComposed(eye_view, SubmittedFocus{}, 128, 96, why) == 0,
                   how + ": a readback that cannot be drawn is refused");
            // A focus view whose swapchain is not known is not drawn: the eye alone.
            bad = focus;
            bad.view.swapchain = reinterpret_cast<XrSwapchain>(std::uintptr_t{0x7777});
            Expect(draw_with(bad, why) == MirrorDrawResult::kShown, how + ": an unknown focus image leaves the eye alone: " + why);
            const std::vector<std::uint8_t> again = harness.Read(target, 128, 96);
            Expect(again == alone, how + ": ...the picture then is the eye's own view");
            Expect(draw_with(focus, why) == MirrorDrawResult::kShown, how + ": the mirror draws on after refusals: " + why);

            // A focus view shrunk into its rectangle (160x120 texels over
            // 64x48 pixels): a flat view stays flat, with no pixel of the
            // eye inside the band's inner edge.
            void* flat_image = nullptr;
            const XrSwapchain flat_source =
                harness.Source(DXGI_FORMAT_R8G8B8A8_UNORM, 160, 120, 1, 1, readable, &flat_image);
            std::vector<std::uint8_t> flat(160 * 120 * 4);
            for (std::size_t i = 0; i < flat.size(); i += 4) {
                flat[i] = 200;
                flat[i + 1] = 100;
                flat[i + 2] = 50;
                flat[i + 3] = 255;
            }
            rig.Fill(flat_image, DXGI_FORMAT_R8G8B8A8_UNORM, 0, flat.data(), 4);
            SubmittedFocus shrunk = focus;
            shrunk.view = Harness::View(flat_source, DXGI_FORMAT_R8G8B8A8_UNORM, 160, 120, 1, 0, {{0, 0}, {160, 120}});
            Expect(draw_with(shrunk, why) == MirrorDrawResult::kShown, how + ": a shrunk focus view is drawn: " + why);
            const std::vector<std::uint8_t> shrunk_picture = harness.Read(target, 128, 96);
            bool flat_inside = true;
            for (std::uint32_t y = 30; y < 66; ++y) {
                for (std::uint32_t x = 38; x < 90; ++x) {
                    const std::uint8_t* got = &shrunk_picture[(static_cast<std::size_t>(y) * 128 + x) * 4];
                    flat_inside = flat_inside && std::abs(got[0] - 200) <= 1 && std::abs(got[1] - 100) <= 1 &&
                                  std::abs(got[2] - 50) <= 1;
                }
            }
            Expect(flat_inside, how + ": a shrunk focus view fills its rectangle");
            gpu->Forget(flat_source);
            gpu->Forget(focus_source);
            gpu->Forget(eye_source);
        }
        // Linear texels (an sRGB format's view): the two views are blended as
        // stored and then encoded for the picture.
        {
            void* eye_image = nullptr;
            void* focus_image = nullptr;
            const XrSwapchain eye_source = harness.Source(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 32, 32, 1, 1, true, &eye_image);
            const XrSwapchain focus_source =
                harness.Source(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 32, 32, 1, 1, true, &focus_image);
            const std::vector<std::uint8_t> eye_data = PatternImage(32, 32, 4);
            const std::vector<std::uint8_t> focus_data = PatternImage(32, 32, 5);
            rig.Fill(eye_image, DXGI_FORMAT_R8G8B8A8_UNORM, 0, eye_data.data(), 4);
            rig.Fill(focus_image, DXGI_FORMAT_R8G8B8A8_UNORM, 0, focus_data.data(), 4);
            SubmittedFocus focus;
            focus.present = true;
            focus.view = Harness::View(focus_source, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 32, 32, 1, 0, {{0, 0}, {32, 32}});
            focus.place.rect = {0.0f, 0.0f, 1.0f, 1.0f};
            focus.place.feather = {1.0f / 32.0f, 1.0f / 32.0f};
            const XrSwapchain target = harness.Target(32, 32);
            MirrorDraw draw;
            draw.target = target;
            draw.width = 32;
            draw.height = 32;
            draw.homography = MirrorWholeView();
            draw.focus = focus;
            std::string why;
            Expect(gpu->DrawMirror(Harness::View(eye_source, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 32, 32, 1, 0,
                                                 {{0, 0}, {32, 32}}),
                                   draw, why) == MirrorDrawResult::kShown,
                   api + ": sRGB views are drawn with a focus view: " + why);
            const std::vector<std::uint8_t> drawn = harness.Read(target, 32, 32);
            bool stored = true;
            for (std::uint32_t y = 2; y < 30; ++y) {
                for (std::uint32_t x = 2; x < 30; ++x) {
                    for (int c = 0; c < 3; ++c) {
                        stored = stored && std::abs(static_cast<int>(drawn[(y * 32 + x) * 4 + c]) -
                                                    focus_data[(y * 32 + x) * 4 + c]) <= 1;
                    }
                }
            }
            Expect(stored, api + ": an sRGB focus view is shown as its stored values");
            gpu->Forget(focus_source);
            gpu->Forget(eye_source);
        }
        // Nothing of the session's images is held after the teardown.
        ComPtr<ID3D11Texture2D> held = rig.images.front();
        gpu.reset();
        rig.context->ClearState();
        rig.context->Flush();
        rig.ReleaseImages();
        Expect(held.Reset() == 0, api + ": no image reference is left after the teardown");
    }
    // A game's device made D3D11_CREATE_DEVICE_SINGLETHREADED serializes
    // nothing: its window's swap chain is never made on another thread.
    {
        D3D11Rig single;
        Expect(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_SINGLETHREADED,
                                           &level, 1, D3D11_SDK_VERSION, &single.device, nullptr, &single.context)),
               "A single-threaded Direct3D 11 WARP device is created");
        std::unique_ptr<SubmittedImageGpu> gpu = CreateSubmittedImageD3D11(single.device.Get(), true);
        Expect(gpu != nullptr, "The single-threaded device's backend is made");
        Harness harness{single, *gpu};
        void* image = nullptr;
        const XrSwapchain source = harness.Source(DXGI_FORMAT_R8G8B8A8_UNORM, 64, 48, 1, 1, true, &image);
        const SubmittedView view =
            Harness::View(source, DXGI_FORMAT_R8G8B8A8_UNORM, 64, 48, 1, 0, XrRect2Di{{0, 0}, {64, 48}});
        MirrorWindow window;
        Expect(window.Start(WindowOptions(320, 180), error) && WaitFor([&] { return window.Handle() != nullptr; }),
               "The single-threaded test's window is made: " + error);
        int posted = 0, shown = 0;
        const DWORD drawing_thread = GetCurrentThreadId();
        for (int number = 0; number < 10; ++number) {
            MirrorDraw draw;
            draw.window = window.Handle();
            window.OutputSize(draw.width, draw.height);
            draw.on_window_thread = [&](std::function<void()> task) {
                ++posted;
                return window.Post(std::move(task));
            };
            draw.homography = MirrorHomographyFor(Fov(-40, 40, 30, -30), WholeCrop(Fov(-40, 40, 30, -30)), MirrorQuat{});
            std::string why;
            const MirrorDrawResult result = gpu->DrawMirror(view, draw, why);
            Expect(result != MirrorDrawResult::kFailed, "Single-threaded device: a frame is drawn: " + why);
            Expect(GetCurrentThreadId() == drawing_thread, "Single-threaded device: drawn on the drawing thread");
            if (result == MirrorDrawResult::kShown) ++shown;
        }
        Expect(posted == 0 && shown > 0,
               "Single-threaded device: the swap chain is made on the drawing thread and frames are shown (" +
                   std::to_string(posted) + " posted, " + std::to_string(shown) + " shown)");
        gpu->CloseMirror();
        window.RequestStop();
        Expect(window.Join(3000), "The single-threaded test's window ends");
        gpu.reset();
        single.context->ClearState();
        single.context->Flush();
    }
    std::cout << "Direct3D 11 (WARP) mirror window: " << checks << " checks passed\n";
    return 0;
}

// ---- Direct3D 12 ----

struct D3D12Rig final : Rig {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    std::uint64_t serial{0};
    std::vector<ComPtr<ID3D12Resource>> images;

    SubmittedImageApi Api() const override { return SubmittedImageApi::kD3D12; }

    void Execute(const std::function<void(ID3D12GraphicsCommandList*)>& record) {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands;
        Expect(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
                   SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                       IID_PPV_ARGS(&commands))),
               "The test's command list is created");
        record(commands.Get());
        Expect(SUCCEEDED(commands->Close()), "The test's command list closes");
        ID3D12CommandList* lists[] = {commands.Get()};
        queue->ExecuteCommandLists(1, lists);
        Wait();
    }
    void Wait() {
        Expect(SUCCEEDED(queue->Signal(fence.Get(), ++serial)), "The test's fence is signalled");
        const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Expect(event && SUCCEEDED(fence->SetEventOnCompletion(serial, event)) &&
                   WaitForSingleObject(event, 30000) == WAIT_OBJECT_0,
               "The test's GPU work finishes");
        CloseHandle(event);
    }
    // In RENDER_TARGET, as a released swapchain image rests.
    void* Image(std::int64_t format, std::uint32_t width, std::uint32_t height, std::uint32_t slices,
                std::uint32_t samples, bool, bool typed) override {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = static_cast<UINT16>(slices);
        desc.MipLevels = 1;
        desc.Format = static_cast<DXGI_FORMAT>(typed ? format : Typeless(format));
        desc.SampleDesc.Count = samples;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ComPtr<ID3D12Resource> texture;
        Expect(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                         D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                         IID_PPV_ARGS(&texture))),
               "A Direct3D 12 texture is created");
        images.push_back(texture);
        return texture.Get();
    }
    void Fill(void* image, std::int64_t typed_format, std::uint32_t slice, const void* data,
              std::uint32_t bytes_per_texel) override {
        auto* texture = static_cast<ID3D12Resource*>(image);
        const D3D12_RESOURCE_DESC desc = texture->GetDesc();
        D3D12_RESOURCE_DESC one = desc;
        one.DepthOrArraySize = 1;
        one.Format = static_cast<DXGI_FORMAT>(typed_format);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64 total = 0;
        device->GetCopyableFootprints(&one, 0, 1, 0, &footprint, nullptr, nullptr, &total);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = total;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> upload;
        Expect(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&upload))),
               "The test's upload buffer is created");
        std::uint8_t* mapped = nullptr;
        Expect(SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))), "The upload buffer maps");
        const std::size_t row = static_cast<std::size_t>(desc.Width) * bytes_per_texel;
        for (std::uint32_t y = 0; y < desc.Height; ++y) {
            std::memcpy(mapped + static_cast<std::size_t>(y) * footprint.Footprint.RowPitch,
                        static_cast<const std::uint8_t*>(data) + y * row, row);
        }
        upload->Unmap(0, nullptr);
        footprint.Footprint.Format = desc.Format;
        Execute([&](ID3D12GraphicsCommandList* commands) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition = {texture, slice, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST};
            commands->ResourceBarrier(1, &barrier);
            D3D12_TEXTURE_COPY_LOCATION to{};
            to.pResource = texture;
            to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.SubresourceIndex = slice;
            D3D12_TEXTURE_COPY_LOCATION from{};
            from.pResource = upload.Get();
            from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            from.PlacedFootprint = footprint;
            commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            commands->ResourceBarrier(1, &barrier);
        });
    }
    void ReleaseImages() override {
        Wait();
        images.clear();
    }
    void Idle() override { Wait(); }
};

int D3D12Tests() {
    D3D12Rig rig;
    bool debug_layer = false;
    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            debug_layer = true;
        }
    }
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    Expect(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))),
           "The WARP adapter is found");
    Expect(SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&rig.device))),
           "A Direct3D 12 WARP device is created");
    ComPtr<ID3D12InfoQueue> messages;
    if (debug_layer && FAILED(rig.device.As(&messages))) debug_layer = false;
    // Errors are collected and reported at the end, not raised as exceptions.
    ComPtr<IDXGIInfoQueue> dxgi_messages;
    if (debug_layer) {
        for (const D3D12_MESSAGE_SEVERITY severity :
             {D3D12_MESSAGE_SEVERITY_CORRUPTION, D3D12_MESSAGE_SEVERITY_ERROR, D3D12_MESSAGE_SEVERITY_WARNING}) {
            messages->SetBreakOnSeverity(severity, FALSE);
        }
        if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgi_messages)))) {
            for (const DXGI_INFO_QUEUE_MESSAGE_SEVERITY severity :
                 {DXGI_INFO_QUEUE_MESSAGE_SEVERITY_CORRUPTION, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR,
                  DXGI_INFO_QUEUE_MESSAGE_SEVERITY_WARNING}) {
                dxgi_messages->SetBreakOnSeverity(DXGI_DEBUG_ALL, severity, FALSE);
            }
        }
    }
    D3D12_COMMAND_QUEUE_DESC queue{};
    Expect(SUCCEEDED(rig.device->CreateCommandQueue(&queue, IID_PPV_ARGS(&rig.queue))) &&
               SUCCEEDED(rig.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&rig.fence))),
           "The session's queue is created");
    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    binding.device = rig.device.Get();
    binding.queue = rig.queue.Get();
    std::string error;
    std::unique_ptr<SubmittedImageGpu> gpu = CreateSubmittedImageGpu(&binding, error);
    Expect(gpu && gpu->Api() == SubmittedImageApi::kD3D12, "The Direct3D 12 backend is made: " + error);
    DrawTests(rig, *gpu, "Direct3D 12");

    // A draw while the GPU has the last one is skipped, not queued behind it.
    {
        Harness harness{rig, *gpu};
        void* image = nullptr;
        const XrSwapchain source = harness.Source(DXGI_FORMAT_R8G8B8A8_UNORM, 512, 512, 1, 1, true, &image);
        const XrSwapchain target = harness.Target(512, 512);
        const XrFovf plain = Fov(-40, 40, 40, -40);
        MirrorDraw draw;
        draw.target = target;
        draw.width = 512;
        draw.height = 512;
        draw.homography = MirrorHomographyFor(plain, WholeCrop(plain), MirrorQuat{});
        const SubmittedView view = Harness::View(source, DXGI_FORMAT_R8G8B8A8_UNORM, 512, 512, 1, 0, {{0, 0}, {512, 512}});
        rig.Idle();
        int shown = 0, skipped = 0;
        for (int i = 0; i < 24; ++i) {
            const MirrorDrawResult result = gpu->DrawMirror(view, draw, error);
            Expect(result != MirrorDrawResult::kFailed, "Back-to-back draws do not fail: " + error);
            (result == MirrorDrawResult::kShown ? shown : skipped)++;
        }
        Expect(shown >= 1 && shown + skipped == 24, "Back-to-back draws are drawn or skipped, never queued up");
        std::cout << "Direct3D 12 back to back: " << shown << " drawn, " << skipped << " skipped\n";
        rig.Idle();
        // Reset right behind a draw, then usable again.
        Expect(gpu->DrawMirror(view, draw, error) == MirrorDrawResult::kShown, "A last draw: " + error);
        ComPtr<ID3D12Resource> held = static_cast<ID3D12Resource*>(image);
        gpu->Reset();
        Expect(!gpu->Tracked(source) && !gpu->Tracked(target), "A reset forgets everything");
        const XrSwapchain again = harness.Track(image);
        const XrSwapchain target_again = harness.Target(512, 512);
        draw.target = target_again;
        SubmittedView view_again = view;
        view_again.swapchain = again;
        Expect(gpu->DrawMirror(view_again, draw, error) == MirrorDrawResult::kShown,
               "The mirror is set up again after a reset: " + error);
        gpu.reset();
        rig.ReleaseImages();
        Expect(held.Reset() == 0, "Nothing of a swapchain's images is held after the teardown");
    }
    if (debug_layer) {
        std::string reported;
        const UINT64 count = messages->GetNumStoredMessages();
        int problems = 0;
        for (UINT64 i = 0; i < count; ++i) {
            SIZE_T size = 0;
            messages->GetMessage(i, nullptr, &size);
            std::vector<char> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (FAILED(messages->GetMessage(i, message, &size))) continue;
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                ++problems;
                if (reported.size() < 2000) reported += std::string("\n  ") + message->pDescription;
            }
        }
        Expect(problems == 0, "The Direct3D 12 debug layer reports no error (states, descriptors, lifetimes):" + reported);
        if (dxgi_messages) {
            reported.clear();
            const UINT64 dxgi_count = dxgi_messages->GetNumStoredMessages(DXGI_DEBUG_DXGI);
            for (UINT64 i = 0; i < dxgi_count; ++i) {
                SIZE_T size = 0;
                dxgi_messages->GetMessage(DXGI_DEBUG_DXGI, i, nullptr, &size);
                std::vector<char> storage(size);
                auto* message = reinterpret_cast<DXGI_INFO_QUEUE_MESSAGE*>(storage.data());
                if (FAILED(dxgi_messages->GetMessage(DXGI_DEBUG_DXGI, i, message, &size))) continue;
                if (message->Severity <= DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR) {
                    ++problems;
                    if (reported.size() < 2000) reported += std::string("\n  ") + message->pDescription;
                }
            }
            Expect(problems == 0, "DXGI's debug layer reports no error for the window's swap chain:" + reported);
        }
    }
    std::cout << "Direct3D 12 (WARP) mirror window: " << checks << " checks passed"
              << (debug_layer ? " (debug layer on)" : " (debug layer not installed)") << '\n';
    return 0;
}

// ---- cost on the real adapter (`measure`; run by hand, never by ctest) ----

struct Cost {
    std::vector<double> gpu_ms; // per shown frame
    std::vector<double> cpu_shown_ms;
    std::vector<double> cpu_skipped_ms;
    int shown{0};
    int skipped{0};
};

double Mean(const std::vector<double>& values) {
    double sum = 0.0;
    for (const double value : values) sum += value;
    return values.empty() ? 0.0 : sum / static_cast<double>(values.size());
}
double Worst(const std::vector<double>& values) {
    return values.empty() ? 0.0 : *std::max_element(values.begin(), values.end());
}
double Median(std::vector<double> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

std::uint64_t LocalVideoMemory(IUnknown* device_or_adapter_owner, IDXGIAdapter* known = nullptr) {
    ComPtr<IDXGIAdapter> adapter = known;
    if (!adapter) {
        ComPtr<IDXGIDevice> dxgi;
        if (FAILED(device_or_adapter_owner->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter))) {
            return 0;
        }
    }
    ComPtr<IDXGIAdapter3> adapter3;
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (FAILED(adapter.As(&adapter3)) ||
        FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
        return 0;
    }
    return info.CurrentUsage;
}

// The measured window given another size on the desktop, as a drag of its
// corner does step by step.
void DragMeasuredWindow(MirrorWindow& window, int width, int height, int step) {
    const int wide = width - 16 * step;
    SetWindowPos(static_cast<HWND>(window.Handle()), nullptr, 0, 0, wide, wide * height / width,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void Report(const std::string& what, const Cost& cost, std::int64_t vram_bytes, std::uint32_t width,
            std::uint32_t height) {
    std::cout << what << ": " << cost.shown << " shown, " << cost.skipped << " skipped; GPU per shown frame median "
              << Median(cost.gpu_ms) << " ms, mean " << Mean(cost.gpu_ms) << " ms, worst " << Worst(cost.gpu_ms)
              << " ms; CPU in the call, shown: median " << Median(cost.cpu_shown_ms) << " ms, mean "
              << Mean(cost.cpu_shown_ms) << " ms, worst "
              << Worst(cost.cpu_shown_ms) << " ms; skipped: mean " << Mean(cost.cpu_skipped_ms) << " ms, worst "
              << Worst(cost.cpu_skipped_ms) << " ms; video memory while open +" << vram_bytes / (1024.0 * 1024.0)
              << " MB (two " << width << "x" << height << " 8-bit buffers are "
              << 2.0 * width * height * 4 / (1024.0 * 1024.0) << " MB)" << std::endl;
}

// The eye image a game would submit: 3600x3600, typeless 8-bit sRGB.
constexpr std::uint32_t kEye = 3600;

int MeasureD3D11() {
    D3D11Rig rig;
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                                 &rig.device, nullptr, &rig.context))) {
        std::cout << "Direct3D 11: no hardware device\n";
        return 0;
    }
    std::unique_ptr<SubmittedImageGpu> gpu = CreateSubmittedImageD3D11(rig.device.Get(), true);
    Harness harness{rig, *gpu};
    const std::vector<std::uint8_t> data = PatternImage(kEye, kEye, 1);
    const XrFovf fov = Fov(-50, 45, 44, -48);
    for (const bool readable : {true, false}) {
        void* image = nullptr;
        const XrSwapchain source = harness.Source(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, kEye, kEye, 1, 1, readable, &image);
        rig.Fill(image, DXGI_FORMAT_R8G8B8A8_UNORM, 0, data.data(), 4);
        const SubmittedView view = Harness::View(source, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, kEye, kEye, 1, 0,
                                                 {{0, 0}, {static_cast<int>(kEye), static_cast<int>(kEye)}});
        for (const auto& [width, height] : {std::pair{1920, 1080}, std::pair{2560, 1440}}) {
            rig.context->Flush();
            const std::uint64_t before = LocalVideoMemory(rig.device.Get());
            MirrorWindow window;
            std::string error;
            Expect(window.Start(WindowOptions(width, height), error) && WaitFor([&] { return window.Handle() != nullptr; }),
                   "The measured window is made: " + error);
            MirrorStabilizer stabilizer;
            MirrorSettings settings;
            Cost cost;
            const std::uint32_t made_before = MirrorSwapChainsMadeForTests();
            std::uint64_t during = before;
            D3D11_QUERY_DESC disjoint_desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            D3D11_QUERY_DESC stamp_desc{D3D11_QUERY_TIMESTAMP, 0};
            for (int number = 0; number < 700 && cost.shown < 60; ++number) {
                MirrorDraw draw;
                draw.window = window.Handle();
                window.OutputSize(draw.width, draw.height);
                draw.on_window_thread = [&window](std::function<void()> task) { return window.Post(std::move(task)); };
                if (cost.shown >= 20 && cost.shown < 40) {
                    // Resized on the desktop in the middle, another size at
                    // every frame: its cost is in the figures too.
                    DragMeasuredWindow(window, width, height, cost.shown - 19);
                }
                draw.homography = stabilizer
                                      .Update(Pose(AxisAngle(0, 1, 0, 0.5 * kDeg * std::sin(number * 0.9))), fov,
                                              1'000'000'000ll + number * 11'111'111ll, settings)
                                      .homography;
                ComPtr<ID3D11Query> disjoint, first, last;
                rig.device->CreateQuery(&disjoint_desc, &disjoint);
                rig.device->CreateQuery(&stamp_desc, &first);
                rig.device->CreateQuery(&stamp_desc, &last);
                rig.context->Begin(disjoint.Get());
                rig.context->End(first.Get());
                const auto begun = std::chrono::steady_clock::now();
                const MirrorDrawResult result = gpu->DrawMirror(view, draw, error);
                const double cpu =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begun).count();
                rig.context->End(last.Get());
                rig.context->End(disjoint.Get());
                Expect(result != MirrorDrawResult::kFailed, "A measured frame is drawn: " + error);
                D3D11_QUERY_DATA_TIMESTAMP_DISJOINT clock{};
                UINT64 t0 = 0, t1 = 0;
                while (rig.context->GetData(disjoint.Get(), &clock, sizeof(clock), 0) == S_FALSE) Sleep(0);
                while (rig.context->GetData(first.Get(), &t0, sizeof(t0), 0) == S_FALSE) Sleep(0);
                while (rig.context->GetData(last.Get(), &t1, sizeof(t1), 0) == S_FALSE) Sleep(0);
                if (result == MirrorDrawResult::kShown) {
                    ++cost.shown;
                    cost.cpu_shown_ms.push_back(cpu);
                    if (!clock.Disjoint && clock.Frequency) {
                        cost.gpu_ms.push_back(static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(clock.Frequency));
                    }
                    during = std::max(during, LocalVideoMemory(rig.device.Get()));
                } else {
                    ++cost.skipped;
                    cost.cpu_skipped_ms.push_back(cpu);
                }
                Sleep(4); // a game's frame goes by
            }
            Report(std::string("Direct3D 11, ") + (readable ? "image sampled directly" : "image copied first (no shader-resource binding)") +
                       ", " + std::to_string(width) + "x" + std::to_string(height),
                   cost, static_cast<std::int64_t>(during) - static_cast<std::int64_t>(before), width, height);
            std::cout << "  swap chains made for this window, through 20 window sizes: "
                      << MirrorSwapChainsMadeForTests() - made_before << std::endl;
            gpu->CloseMirror();
            window.Stop();
        }
        gpu->Forget(source);
    }
    gpu.reset();
    return 0;
}

int MeasureD3D12() {
    D3D12Rig rig;
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumAdapters1(0, &adapter)) ||
        FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&rig.device)))) {
        std::cout << "Direct3D 12: no hardware device\n";
        return 0;
    }
    DXGI_ADAPTER_DESC1 adapter_desc{};
    adapter->GetDesc1(&adapter_desc);
    std::wcout << L"Adapter: " << adapter_desc.Description << std::endl;
    D3D12_COMMAND_QUEUE_DESC queue{};
    Expect(SUCCEEDED(rig.device->CreateCommandQueue(&queue, IID_PPV_ARGS(&rig.queue))) &&
               SUCCEEDED(rig.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&rig.fence))),
           "The measured queue is created");
    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    binding.device = rig.device.Get();
    binding.queue = rig.queue.Get();
    std::string error;
    std::unique_ptr<SubmittedImageGpu> gpu = CreateSubmittedImageGpu(&binding, error);
    Expect(gpu != nullptr, "The Direct3D 12 backend is made: " + error);
    Harness harness{rig, *gpu};
    const std::vector<std::uint8_t> data = PatternImage(kEye, kEye, 1);
    void* image = nullptr;
    const XrSwapchain source = harness.Source(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, kEye, kEye, 1, 1, true, &image);
    rig.Fill(image, DXGI_FORMAT_R8G8B8A8_UNORM, 0, data.data(), 4);
    const SubmittedView view = Harness::View(source, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, kEye, kEye, 1, 0,
                                             {{0, 0}, {static_cast<int>(kEye), static_cast<int>(kEye)}});
    const XrFovf fov = Fov(-50, 45, 44, -48);

    // Timestamps before and after the mirror's own command list. The list
    // before first clears a large image, so the GPU is still busy when the
    // mirror's list arrives and the two timestamps bracket only its work.
    D3D12_QUERY_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    heap_desc.Count = 2;
    ComPtr<ID3D12QueryHeap> query_heap;
    D3D12_HEAP_PROPERTIES readback_heap{};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer_desc{};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = 16;
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.SampleDesc.Count = 1;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> stamps;
    Expect(SUCCEEDED(rig.device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&query_heap))) &&
               SUCCEEDED(rig.device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                             IID_PPV_ARGS(&stamps))),
           "The timestamp queries are made");
    ComPtr<ID3D12Resource> busy = static_cast<ID3D12Resource*>(
        rig.Image(DXGI_FORMAT_R8G8B8A8_UNORM, 8192, 8192, 1, 1, true, true));
    ComPtr<ID3D12DescriptorHeap> busy_heap;
    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
    rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_desc.NumDescriptors = 1;
    Expect(SUCCEEDED(rig.device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&busy_heap))), "The busy work's view heap");
    rig.device->CreateRenderTargetView(busy.Get(), nullptr, busy_heap->GetCPUDescriptorHandleForHeapStart());
    UINT64 frequency = 0;
    rig.queue->GetTimestampFrequency(&frequency);
    ComPtr<ID3D12CommandAllocator> allocators[2];
    ComPtr<ID3D12GraphicsCommandList> lists[2];
    for (int i = 0; i < 2; ++i) {
        Expect(SUCCEEDED(rig.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[i]))) &&
                   SUCCEEDED(rig.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[i].Get(), nullptr,
                                                           IID_PPV_ARGS(&lists[i]))) &&
                   SUCCEEDED(lists[i]->Close()),
               "The timestamp lists are made");
    }

    // The busy work's image is touched once before anything is timed (its
    // first clear commits its memory).
    rig.Execute([&](ID3D12GraphicsCommandList* commands) {
        const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        commands->ClearRenderTargetView(busy_heap->GetCPUDescriptorHandleForHeapStart(), color, 0, nullptr);
    });
    for (const auto& [width, height] : {std::pair{1920, 1080}, std::pair{2560, 1440}}) {
        rig.Idle();
        const std::uint64_t before = LocalVideoMemory(nullptr, adapter.Get());
        MirrorWindow window;
        Expect(window.Start(WindowOptions(width, height), error) && WaitFor([&] { return window.Handle() != nullptr; }),
               "The measured window is made: " + error);
        MirrorStabilizer stabilizer;
        MirrorSettings settings;
        Cost cost;
        const std::uint32_t made_before = MirrorSwapChainsMadeForTests();
        std::uint64_t during = before;
        for (int number = 0; number < 700 && cost.shown < 60; ++number) {
            MirrorDraw draw;
            draw.window = window.Handle();
            window.OutputSize(draw.width, draw.height);
            draw.on_window_thread = [&window](std::function<void()> task) { return window.Post(std::move(task)); };
            if (cost.shown >= 20 && cost.shown < 40) DragMeasuredWindow(window, width, height, cost.shown - 19);
            draw.homography = stabilizer
                                  .Update(Pose(AxisAngle(0, 1, 0, 0.5 * kDeg * std::sin(number * 0.9))), fov,
                                          1'000'000'000ll + number * 11'111'111ll, settings)
                                  .homography;
            rig.Idle();
            allocators[0]->Reset();
            lists[0]->Reset(allocators[0].Get(), nullptr);
            const float color[4] = {0.1f, 0.2f, 0.3f, 1.0f};
            lists[0]->ClearRenderTargetView(busy_heap->GetCPUDescriptorHandleForHeapStart(), color, 0, nullptr);
            lists[0]->EndQuery(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
            lists[0]->Close();
            allocators[1]->Reset();
            lists[1]->Reset(allocators[1].Get(), nullptr);
            lists[1]->EndQuery(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            lists[1]->ResolveQueryData(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, stamps.Get(), 0);
            lists[1]->Close();
            ID3D12CommandList* first[] = {lists[0].Get()};
            ID3D12CommandList* last[] = {lists[1].Get()};
            rig.queue->ExecuteCommandLists(1, first);
            const auto begun = std::chrono::steady_clock::now();
            const MirrorDrawResult result = gpu->DrawMirror(view, draw, error);
            const double cpu =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begun).count();
            rig.queue->ExecuteCommandLists(1, last);
            Expect(result != MirrorDrawResult::kFailed, "A measured frame is drawn: " + error);
            rig.Idle();
            if (result == MirrorDrawResult::kShown) {
                ++cost.shown;
                cost.cpu_shown_ms.push_back(cpu);
                UINT64* mapped = nullptr;
                const D3D12_RANGE range{0, 16};
                if (frequency && SUCCEEDED(stamps->Map(0, &range, reinterpret_cast<void**>(&mapped)))) {
                    cost.gpu_ms.push_back(static_cast<double>(mapped[1] - mapped[0]) * 1000.0 /
                                          static_cast<double>(frequency));
                    const D3D12_RANGE none{0, 0};
                    stamps->Unmap(0, &none);
                }
                during = std::max(during, LocalVideoMemory(nullptr, adapter.Get()));
            } else {
                ++cost.skipped;
                cost.cpu_skipped_ms.push_back(cpu);
            }
            Sleep(4);
        }
        Report("Direct3D 12, image sampled directly, " + std::to_string(width) + "x" + std::to_string(height), cost,
               static_cast<std::int64_t>(during) - static_cast<std::int64_t>(before), width, height);
        std::cout << "  swap chains made for this window, through 20 window sizes: "
                  << MirrorSwapChainsMadeForTests() - made_before << std::endl;
        rig.Idle();
        gpu->CloseMirror();
        window.Stop();
    }
    rig.Idle();
    gpu.reset();
    return 0;
}

// ---- By hand: the window on the real desktop ----
//
// What no test above can see, because their windows are never put on the
// desktop: that a window which fills a monitor, layered so the mouse passes
// through it, still shows the picture its flip-model swap chain presents, and
// that Windows hands the mouse to what is under it. One short run by hand on
// the real adapter, through the GPU lock: a small window, then a monitor
// filled for a second or two, twice.

bool DesktopColour(int x, int y, COLORREF& colour) {
    const HDC screen = GetDC(nullptr);
    const HDC memory = CreateCompatibleDC(screen);
    const HBITMAP bitmap = CreateCompatibleBitmap(screen, 1, 1);
    const HGDIOBJ before = SelectObject(memory, bitmap);
    // CAPTUREBLT: layered windows are part of the copy.
    const bool copied = BitBlt(memory, 0, 0, 1, 1, screen, x, y, SRCCOPY | CAPTUREBLT) != 0;
    colour = GetPixel(memory, 0, 0);
    SelectObject(memory, before);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return copied;
}

int ProbeDesktop(Rig& rig, SubmittedImageGpu& gpu, const std::string& api) {
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const std::vector<MirrorMonitor> monitors = ListMirrorMonitors();
    if (monitors.empty()) {
        std::cout << api << ": no monitor\n";
        return 1;
    }
    // A monitor that is not the primary one where there is one.
    const MirrorMonitor* monitor = &monitors.front();
    for (const MirrorMonitor& candidate : monitors) {
        if (!candidate.primary) monitor = &candidate;
    }
    Harness harness{rig, gpu};
    constexpr std::uint32_t kSide = 64;
    std::vector<std::uint8_t> magenta(kSide * kSide * 4);
    for (std::size_t i = 0; i < magenta.size(); i += 4) {
        magenta[i] = 255;
        magenta[i + 1] = 0;
        magenta[i + 2] = 255;
        magenta[i + 3] = 255;
    }
    void* image = nullptr;
    const XrSwapchain source = harness.Source(DXGI_FORMAT_R8G8B8A8_UNORM, kSide, kSide, 1, 1, true, &image);
    rig.Fill(image, DXGI_FORMAT_R8G8B8A8_UNORM, 0, magenta.data(), 4);
    const SubmittedView view =
        Harness::View(source, DXGI_FORMAT_R8G8B8A8_UNORM, kSide, kSide, 1, 0, {{0, 0}, {kSide, kSide}});
    const XrFovf fov = Fov(-45, 45, 45, -45);

    MirrorWindow window;
    MirrorWindow::Options options = WindowOptions(480, 270);
    options.on_desktop = true;
    options.placement = MirrorPlacement{monitor->x + 120, monitor->y + 120, 480, 270, 480};
    std::string error;
    if (!window.Start(std::move(options), error) || !WaitFor([&] { return window.Handle() != nullptr; })) {
        std::cout << api << ": the window could not be made: " << error << "\n";
        return 1;
    }
    const HWND handle = static_cast<HWND>(window.Handle());
    MirrorStabilizer stabilizer;
    MirrorSettings settings;
    int number = 0;
    const auto show = [&](int frames) {
        int shown = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (shown < frames && std::chrono::steady_clock::now() < deadline) {
            MirrorDraw draw;
            draw.window = window.Handle();
            window.OutputSize(draw.width, draw.height);
            draw.on_window_thread = [&window](std::function<void()> task) { return window.Post(std::move(task)); };
            draw.homography =
                stabilizer.Update(Pose(AxisAngle(0, 1, 0, 0)), fov, 1'000'000'000ll + number++ * 11'111'111ll, settings)
                    .homography;
            const MirrorDrawResult result = gpu.DrawMirror(view, draw, error);
            if (result == MirrorDrawResult::kFailed) {
                std::cout << api << ": a frame failed: " << error << "\n";
                return -1;
            }
            if (result == MirrorDrawResult::kShown) ++shown;
            rig.Idle();
            Sleep(8);
        }
        // The desktop's compositor takes the last frame.
        Sleep(120);
        return shown;
    };
    int failures = 0;
    const auto look = [&](const std::string& stage, bool through_expected) {
        RECT client{};
        GetClientRect(handle, &client);
        POINT centre{client.right / 2, client.bottom / 2};
        ClientToScreen(handle, &centre);
        COLORREF colour = 0;
        const bool copied = DesktopColour(centre.x, centre.y, colour);
        const bool pictured = copied && GetRValue(colour) > 200 && GetGValue(colour) < 60 && GetBValue(colour) > 200;
        const HWND under = WindowFromPoint(centre);
        const bool through = under != handle;
        const LONG_PTR ex_style = GetWindowLongPtrW(handle, GWL_EXSTYLE);
        std::cout << api << ", " << stage << ": desktop pixel at " << centre.x << "," << centre.y << " is "
                  << static_cast<int>(GetRValue(colour)) << "," << static_cast<int>(GetGValue(colour)) << ","
                  << static_cast<int>(GetBValue(colour)) << (pictured ? " (the picture)" : " (NOT the picture)")
                  << "; the mouse there goes to " << (through ? "another window" : "the mirror")
                  << (through == through_expected ? "" : " (NOT as meant)") << "; layered "
                  << ((ex_style & WS_EX_LAYERED) ? "yes" : "no") << ", transparent "
                  << ((ex_style & WS_EX_TRANSPARENT) ? "yes" : "no") << ", topmost "
                  << ((ex_style & WS_EX_TOPMOST) ? "yes" : "no") << std::endl;
        wchar_t under_class[96]{};
        wchar_t under_title[96]{};
        if (under) {
            GetClassNameW(under, under_class, 95);
            GetWindowTextW(under, under_title, 95);
        }
        std::wcout << L"    window under that point: class \"" << under_class << L"\", title \"" << under_title
                   << L"\"; the mirror is " << (IsWindowVisible(handle) ? L"visible" : L"not visible") << std::endl;
        if (!pictured || through != through_expected) ++failures;
    };
    const auto above = [&] {
        // A plain window is not above the others; for the look at the
        // desktop it must not be behind one.
        SetWindowPos(handle, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    };
    const HWND foreground = GetForegroundWindow();
    const double aspect = static_cast<double>(monitor->width) / monitor->height;
    // A locked or disconnected desktop shows nothing of any window: the
    // looks below would then say nothing about the mirror.
    wchar_t desktop_name[64]{};
    {
        if (const HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS)) {
            DWORD needed = 0;
            GetUserObjectInformationW(input, UOI_NAME, desktop_name, sizeof(desktop_name) - sizeof(wchar_t), &needed);
            CloseDesktop(input);
        }
        std::wcout << L"The desktop taking input: \"" << desktop_name << L"\" (\"Default\" is the user's own)"
                   << std::endl;
    }
    {
        // The lock screen covers every window with one of its own.
        above();
        Sleep(200);
        RECT frame{};
        GetWindowRect(handle, &frame);
        wchar_t cover[96]{};
        if (const HWND under = WindowFromPoint(POINT{(frame.left + frame.right) / 2, (frame.top + frame.bottom) / 2})) {
            GetClassNameW(under, cover, 95);
        }
        if (std::wstring(cover).find(L"LockScreen") != std::wstring::npos || std::wstring(desktop_name) != L"Default") {
            std::cout << api << ": the PC is locked, so nothing of the desktop can be looked at. Run this again "
                      << "unlocked." << std::endl;
            window.Stop();
            gpu.Forget(source);
            return 77;
        }
    }

    above();
    int shown = show(30);
    std::cout << api << ": " << shown << " frames shown in a plain window" << std::endl;
    look("a plain window", false);

    std::uint32_t made = MirrorSwapChainsMadeForTests();
    window.Reshape(480, 270, 16.0 / 9.0, 480, monitor->name);
    WaitFor([&] { return window.Filling(); });
    shown = show(40);
    std::cout << api << ": " << shown << " frames shown filling the monitor (" << monitor->width << "x"
              << monitor->height << "), swap chains made for it: " << MirrorSwapChainsMadeForTests() - made
              << std::endl;
    look("filling the monitor", true);

    window.Reshape(480, 270, 16.0 / 9.0, 480, L"");
    WaitFor([&] { return !window.Filling(); });
    above();
    shown = show(30);
    std::cout << api << ": " << shown << " frames shown back in a window" << std::endl;
    look("back in a window", false);

    // A picture of the monitor's own size: the swap chain is kept when the
    // window goes to fill the monitor, and becomes a layered window's.
    window.Reshape(monitor->width, monitor->height, aspect, monitor->width, L"");
    show(20);
    made = MirrorSwapChainsMadeForTests();
    window.Reshape(monitor->width, monitor->height, aspect, monitor->width, monitor->name);
    WaitFor([&] { return window.Filling(); });
    shown = show(40);
    std::cout << api << ": " << shown << " frames shown filling the monitor with the swap chain it had, made anew: "
              << MirrorSwapChainsMadeForTests() - made << std::endl;
    look("filling the monitor, swap chain kept", true);
    window.Reshape(monitor->width, monitor->height, aspect, monitor->width, L"");
    WaitFor([&] { return !window.Filling(); });
    above();
    shown = show(30);
    std::cout << api << ": " << shown << " frames shown back in a window with the swap chain it had, made anew: "
              << MirrorSwapChainsMadeForTests() - made << std::endl;
    look("back in a window, swap chain kept", false);

    if (GetForegroundWindow() != foreground) {
        std::cout << api << ": the foreground window changed (NOT as meant)" << std::endl;
        ++failures;
    }
    gpu.CloseMirror();
    window.Stop();
    gpu.Forget(source);
    return failures;
}

int ProbeDesktopD3D11() {
    D3D11Rig rig;
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                                 &rig.device, nullptr, &rig.context))) {
        std::cout << "Direct3D 11: no hardware device\n";
        return 1;
    }
    std::unique_ptr<SubmittedImageGpu> gpu = CreateSubmittedImageD3D11(rig.device.Get(), true);
    return ProbeDesktop(rig, *gpu, "Direct3D 11");
}

int ProbeDesktopD3D12() {
    D3D12Rig rig;
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumAdapters1(0, &adapter)) ||
        FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&rig.device)))) {
        std::cout << "Direct3D 12: no hardware device\n";
        return 1;
    }
    D3D12_COMMAND_QUEUE_DESC queue{};
    if (FAILED(rig.device->CreateCommandQueue(&queue, IID_PPV_ARGS(&rig.queue))) ||
        FAILED(rig.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&rig.fence)))) {
        std::cout << "Direct3D 12: no queue\n";
        return 1;
    }
    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    binding.device = rig.device.Get();
    binding.queue = rig.queue.Get();
    std::string error;
    std::unique_ptr<SubmittedImageGpu> gpu = CreateSubmittedImageGpu(&binding, error);
    if (!gpu) {
        std::cout << "Direct3D 12: " << error << "\n";
        return 1;
    }
    return ProbeDesktop(rig, *gpu, "Direct3D 12");
}

} // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "d3d11") return D3D11Tests();
    if (mode == "d3d12") return D3D12Tests();
    if (mode == "measure") {
        // The real adapter: one short run by hand, through the GPU lock.
        MeasureD3D12();
        return MeasureD3D11();
    }
    if (mode == "desktop") {
        // The window shown on the real desktop: by hand, through the GPU lock.
        const int d3d11 = ProbeDesktopD3D11();
        if (d3d11 == 77) return 77; // nothing to look at: not run
        const int d3d12 = ProbeDesktopD3D12();
        return d3d11 + d3d12 == 0 ? 0 : 1;
    }
    if (!mode.empty()) {
        std::cerr << "Unknown mode " << mode << '\n';
        return 2;
    }
    TestQuaternions();
    TestFilter();
    TestCropAndHomography();
    TestStabilizer();
    TestPacingAndSizes();
    TestRelay();
    TestShaderConstants();
    TestPlacement();
    TestWindow();
    std::cout << "Stabilized mirror window (CPU): " << checks << " checks passed\n";
    return 0;
}

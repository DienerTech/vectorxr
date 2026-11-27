#include "depthxr/mirror.h"

#include <algorithm>
#include <cmath>

namespace depthxr {
namespace {

constexpr double kPi = 3.14159265358979323846;
// The cutoff at rest: from 6 Hz at the lowest strength (a 27 ms time
// constant, barely there) down to 0.25 Hz at 100 (0.64 s).
constexpr double kCutoffLow = 6.0;
constexpr double kCutoffHigh = 0.25;
// Hz of cutoff added per rad/s of head speed: a turn of 2 rad/s adds 1 Hz.
constexpr double kSpeedCutoff = 0.5;
// The head speed itself is low-passed at this cutoff.
constexpr double kSpeedSmoothingHz = 1.0;
constexpr double kMaxStepSeconds = 0.5;

struct Mat3 {
    double m[9]{};
};

Mat3 Multiply(const Mat3& a, const Mat3& b) {
    Mat3 out;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            double sum = 0.0;
            for (int k = 0; k < 3; ++k) sum += a.m[row * 3 + k] * b.m[k * 3 + column];
            out.m[row * 3 + column] = sum;
        }
    }
    return out;
}

Mat3 RotationOf(const MirrorQuat& q) {
    const double x = q.x, y = q.y, z = q.z, w = q.w;
    Mat3 out;
    out.m[0] = 1.0 - 2.0 * (y * y + z * z);
    out.m[1] = 2.0 * (x * y - z * w);
    out.m[2] = 2.0 * (x * z + y * w);
    out.m[3] = 2.0 * (x * y + z * w);
    out.m[4] = 1.0 - 2.0 * (x * x + z * z);
    out.m[5] = 2.0 * (y * z - x * w);
    out.m[6] = 2.0 * (x * z - y * w);
    out.m[7] = 2.0 * (y * z + x * w);
    out.m[8] = 1.0 - 2.0 * (x * x + y * y);
    return out;
}

double Alpha(double cutoff_hz, double seconds) {
    return 1.0 - std::exp(-2.0 * kPi * cutoff_hz * seconds);
}

MirrorQuat FromXr(const XrQuaternionf& q) {
    MirrorQuat out{q.x, q.y, q.z, q.w};
    const double length = std::sqrt(out.x * out.x + out.y * out.y + out.z * out.z + out.w * out.w);
    // A pose a game left zeroed has no orientation: taken as none.
    if (!(length > 1e-6) || !std::isfinite(length)) return MirrorQuat{};
    return MirrorNormalize(out);
}

// The largest part of the correction, 0..1, that keeps the window inside the
// view's image.
double FittingShare(const XrFovf& fov, const MirrorCrop& crop, const MirrorQuat& correction) {
    if (MirrorFilled(MirrorHomographyFor(fov, crop, correction))) return 1.0;
    double low = 0.0;
    double high = 1.0;
    for (int i = 0; i < 14; ++i) {
        const double middle = 0.5 * (low + high);
        if (MirrorFilled(MirrorHomographyFor(fov, crop, MirrorSlerp(MirrorQuat{}, correction, middle)))) {
            low = middle;
        } else {
            high = middle;
        }
    }
    return low;
}

} // namespace

MirrorQuat MirrorNormalize(const MirrorQuat& q) {
    const double length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(length > 0.0) || !std::isfinite(length)) return MirrorQuat{};
    return MirrorQuat{q.x / length, q.y / length, q.z / length, q.w / length};
}

MirrorQuat MirrorMultiply(const MirrorQuat& a, const MirrorQuat& b) {
    return MirrorQuat{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                      a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

MirrorQuat MirrorConjugate(const MirrorQuat& q) {
    return MirrorQuat{-q.x, -q.y, -q.z, q.w};
}

MirrorQuat MirrorSlerp(const MirrorQuat& a, const MirrorQuat& b, double t) {
    if (t <= 0.0) return a;
    if (t >= 1.0) return b;
    double dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    MirrorQuat end = b;
    if (dot < 0.0) {
        dot = -dot;
        end = MirrorQuat{-b.x, -b.y, -b.z, -b.w};
    }
    double from = 1.0 - t;
    double to = t;
    if (dot < 0.9995) {
        const double angle = std::acos(std::min(dot, 1.0));
        const double sine = std::sin(angle);
        from = std::sin((1.0 - t) * angle) / sine;
        to = std::sin(t * angle) / sine;
    }
    return MirrorNormalize(MirrorQuat{from * a.x + to * end.x, from * a.y + to * end.y, from * a.z + to * end.z,
                                      from * a.w + to * end.w});
}

double MirrorAngle(const MirrorQuat& a, const MirrorQuat& b) {
    const MirrorQuat delta = MirrorMultiply(MirrorConjugate(a), b);
    const double vector = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
    return 2.0 * std::atan2(vector, std::abs(delta.w));
}

MirrorQuat MirrorLevel(const MirrorQuat& q) {
    const Mat3 r = RotationOf(q);
    // Forward is the view's -Z.
    const double fx = -r.m[2], fy = -r.m[5], fz = -r.m[8];
    // right = forward x up, up being +Y.
    double rx = -fz, ry = 0.0, rz = fx;
    const double length = std::sqrt(rx * rx + rz * rz);
    if (length < 0.05) return q;
    rx /= length;
    rz /= length;
    // up' = right x forward.
    const double ux = ry * fz - rz * fy, uy = rz * fx - rx * fz, uz = rx * fy - ry * fx;
    // Columns right, up', -forward.
    const double m00 = rx, m01 = ux, m02 = -fx;
    const double m10 = ry, m11 = uy, m12 = -fy;
    const double m20 = rz, m21 = uz, m22 = -fz;
    MirrorQuat out;
    const double trace = m00 + m11 + m22;
    if (trace > 0.0) {
        const double s = std::sqrt(trace + 1.0) * 2.0;
        out = MirrorQuat{(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25 * s};
    } else if (m00 > m11 && m00 > m22) {
        const double s = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
        out = MirrorQuat{0.25 * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
    } else if (m11 > m22) {
        const double s = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
        out = MirrorQuat{(m01 + m10) / s, 0.25 * s, (m12 + m21) / s, (m02 - m20) / s};
    } else {
        const double s = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
        out = MirrorQuat{(m02 + m20) / s, (m12 + m21) / s, 0.25 * s, (m10 - m01) / s};
    }
    return MirrorNormalize(out);
}

double MirrorOrientationFilter::RestCutoff(double strength) {
    if (!(strength > 0.0)) return 0.0;
    const double share = std::min(strength, 100.0) / 100.0;
    return kCutoffLow * std::pow(kCutoffHigh / kCutoffLow, share);
}

MirrorQuat MirrorOrientationFilter::Update(const MirrorQuat& actual, double seconds, double strength) {
    const double rest = RestCutoff(strength);
    if (primed_ && seconds == 0.0 && rest > 0.0) {
        // A frame for the same moment again (a repeated display time): the
        // smoothed view holds, no jump to the raw head.
        return smoothed_;
    }
    if (!primed_ || !(seconds > 0.0) || seconds > kMaxStepSeconds || !std::isfinite(seconds) || rest <= 0.0) {
        primed_ = true;
        smoothed_ = actual;
        last_actual_ = actual;
        speed_ = 0.0;
        return smoothed_;
    }
    const double speed = MirrorAngle(last_actual_, actual) / seconds;
    last_actual_ = actual;
    speed_ += (speed - speed_) * Alpha(kSpeedSmoothingHz, seconds);
    smoothed_ = MirrorSlerp(smoothed_, actual, Alpha(rest + kSpeedCutoff * speed_, seconds));
    return smoothed_;
}

MirrorCrop MirrorCropFor(const XrFovf& fov, double aspect, double margin_percent) {
    const double left = std::tan(static_cast<double>(fov.angleLeft));
    const double right = std::tan(static_cast<double>(fov.angleRight));
    const double up = std::tan(static_cast<double>(fov.angleUp));
    const double down = std::tan(static_cast<double>(fov.angleDown));
    MirrorCrop crop;
    crop.center_x = 0.5 * (left + right);
    crop.center_y = 0.5 * (up + down);
    const double width = std::abs(right - left);
    const double height = std::abs(up - down);
    if (!(aspect > 0.0) || !std::isfinite(aspect)) aspect = 1.0;
    if (width > aspect * height) {
        crop.half_height = 0.5 * height;
        crop.half_width = crop.half_height * aspect;
    } else {
        crop.half_width = 0.5 * width;
        crop.half_height = crop.half_width / aspect;
    }
    const double keep = 1.0 - std::clamp(std::isfinite(margin_percent) ? margin_percent : 0.0, 0.0, 40.0) / 100.0;
    crop.half_width *= keep;
    crop.half_height *= keep;
    return crop;
}

MirrorHomography MirrorHomographyFor(const XrFovf& fov, const MirrorCrop& crop, const MirrorQuat& correction) {
    const double left = std::tan(static_cast<double>(fov.angleLeft));
    const double right = std::tan(static_cast<double>(fov.angleRight));
    const double up = std::tan(static_cast<double>(fov.angleUp));
    const double down = std::tan(static_cast<double>(fov.angleDown));
    const double width = right - left;
    const double height = up - down;
    MirrorHomography out{};
    if (std::abs(width) < 1e-9 || std::abs(height) < 1e-9) {
        // No field of view to aim in: the whole rectangle.
        out = {0.5f, 0.0f, 0.5f, 0.0f, -0.5f, 0.5f, 0.0f, 0.0f, 1.0f};
        return out;
    }
    // Window point to a direction in the window's (smoothed) view.
    Mat3 to_ray;
    to_ray.m[0] = crop.half_width;
    to_ray.m[2] = crop.center_x;
    to_ray.m[4] = crop.half_height;
    to_ray.m[5] = crop.center_y;
    to_ray.m[8] = -1.0;
    // A direction d in the actual view to (u*w, v*w, w), w = -d.z:
    //   u = (d.x / w - left) / width, v = (up - d.y / w) / height.
    Mat3 to_image;
    to_image.m[0] = 1.0 / width;
    to_image.m[2] = left / width;
    to_image.m[4] = -1.0 / height;
    to_image.m[5] = -up / height;
    to_image.m[8] = -1.0;
    const Mat3 h = Multiply(to_image, Multiply(RotationOf(MirrorNormalize(correction)), to_ray));
    for (int i = 0; i < 9; ++i) out[i] = static_cast<float>(h.m[i]);
    return out;
}

bool MirrorMap(const MirrorHomography& h, double x, double y, double& u, double& v) {
    const double w = h[6] * x + h[7] * y + h[8];
    if (!(w > 1e-6)) return false;
    u = (h[0] * x + h[1] * y + h[2]) / w;
    v = (h[3] * x + h[4] * y + h[5]) / w;
    return true;
}

bool MirrorFilled(const MirrorHomography& h) {
    // A rectangle's image under a homography with w > 0 at its corners is
    // the convex quadrilateral of the corners' images.
    constexpr double kSlack = 1e-4;
    for (const auto& [x, y] : {std::pair{-1.0, -1.0}, std::pair{1.0, -1.0}, std::pair{-1.0, 1.0}, std::pair{1.0, 1.0}}) {
        double u = 0.0;
        double v = 0.0;
        if (!MirrorMap(h, x, y, u, v)) return false;
        if (u < -kSlack || u > 1.0 + kSlack || v < -kSlack || v > 1.0 + kSlack) return false;
    }
    return true;
}

MirrorShaderConstants PackMirrorConstants(const MirrorHomography& h, std::int32_t offset_x, std::int32_t offset_y,
                                          std::int32_t width, std::int32_t height, std::uint32_t target_width,
                                          std::uint32_t target_height, bool encode_srgb) {
    MirrorShaderConstants out{};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) out.rows[row][column] = h[row * 3 + column];
    }
    out.source_offset[0] = offset_x;
    out.source_offset[1] = offset_y;
    out.source_size[0] = width;
    out.source_size[1] = height;
    out.target_size[0] = static_cast<float>(target_width);
    out.target_size[1] = static_cast<float>(target_height);
    out.flags = encode_srgb ? kMirrorFlagEncodeSrgb : 0u;
    // Texels a window pixel covers at the window's centre, along each axis.
    double u0 = 0.0, v0 = 0.0, u1 = 0.0, v1 = 0.0, u2 = 0.0, v2 = 0.0;
    double texels = 1.0;
    if (target_width > 0 && target_height > 0 && MirrorMap(h, 0.0, 0.0, u0, v0) &&
        MirrorMap(h, 2.0 / target_width, 0.0, u1, v1) && MirrorMap(h, 0.0, 2.0 / target_height, u2, v2)) {
        const double across = std::hypot((u1 - u0) * width, (v1 - v0) * height);
        const double along = std::hypot((u2 - u0) * width, (v2 - v0) * height);
        texels = std::max(across, along);
    }
    // A picture at the image's own scale (within rounding) is not spread at
    // all: its texels as they are.
    const double spread = std::clamp(texels - 1.0, 0.0, 1.0);
    out.spread = spread < 1e-3 ? 0.0f : static_cast<float>(spread);
    return out;
}

MirrorHomography MirrorWholeView() {
    return {0.5f, 0.0f, 0.5f, 0.0f, -0.5f, 0.5f, 0.0f, 0.0f, 1.0f};
}

MirrorFocusShaderConstants PackMirrorFocusConstants(const MirrorHomography& h, const MirrorFocusPlace& place,
                                                    std::int32_t offset_x, std::int32_t offset_y, std::int32_t width,
                                                    std::int32_t height, std::uint32_t target_width,
                                                    std::uint32_t target_height) {
    MirrorFocusShaderConstants out{};
    for (int i = 0; i < 4; ++i) out.rect[i] = place.rect[i];
    out.offset[0] = offset_x;
    out.offset[1] = offset_y;
    out.size[0] = width;
    out.size[1] = height;
    out.feather[0] = place.feather[0];
    out.feather[1] = place.feather[1];
    // Focus texels a window pixel covers at the window's centre: the eye's
    // uv it covers, over the focus view's share of the eye, in its texels.
    double u0 = 0.0, v0 = 0.0, u1 = 0.0, v1 = 0.0, u2 = 0.0, v2 = 0.0;
    double texels = 1.0;
    const double across_uv = static_cast<double>(place.rect[2]) - place.rect[0];
    const double down_uv = static_cast<double>(place.rect[3]) - place.rect[1];
    if (target_width > 0 && target_height > 0 && across_uv > 0.0 && down_uv > 0.0 && MirrorMap(h, 0.0, 0.0, u0, v0) &&
        MirrorMap(h, 2.0 / target_width, 0.0, u1, v1) && MirrorMap(h, 0.0, 2.0 / target_height, u2, v2)) {
        const double across = std::hypot((u1 - u0) / across_uv * width, (v1 - v0) / down_uv * height);
        const double along = std::hypot((u2 - u0) / across_uv * width, (v2 - v0) / down_uv * height);
        texels = std::max(across, along);
    }
    const double spread = std::clamp(texels - 1.0, 0.0, 1.0);
    out.spread = spread < 1e-3 ? 0.0f : static_cast<float>(spread);
    return out;
}

void MirrorStabilizer::Reset() {
    filter_.Reset();
    last_time_ = 0;
}

MirrorAim MirrorStabilizer::Update(const XrPosef& pose, const XrFovf& fov, XrTime display_time,
                                   const MirrorSettings& settings, double aspect) {
    MirrorAim aim;
    const MirrorCrop crop =
        MirrorCropFor(fov, aspect > 0.0 ? aspect : MirrorAspectRatio(settings.aspect), settings.margin);
    const MirrorQuat actual = FromXr(pose.orientation);
    const double seconds = last_time_ != 0 ? static_cast<double>(display_time - last_time_) * 1e-9 : 0.0;
    last_time_ = display_time;
    MirrorQuat aimed = filter_.Update(actual, seconds, settings.smoothing);
    const bool smoothing = settings.smoothing > 0.0;
    if (!smoothing && !settings.level_horizon) {
        aim.homography = MirrorHomographyFor(fov, crop, MirrorQuat{});
        return aim;
    }
    if (smoothing) {
        // What the margin cannot cover is given up, and the filter goes on
        // from there: the picture is dragged along, it never shows an edge.
        const MirrorQuat correction = MirrorMultiply(MirrorConjugate(actual), aimed);
        const double share = FittingShare(fov, crop, correction);
        if (share < 1.0) {
            aimed = MirrorNormalize(MirrorMultiply(actual, MirrorSlerp(MirrorQuat{}, correction, share)));
            filter_.Set(aimed);
        }
        aim.applied = share;
    }
    MirrorQuat correction = MirrorMultiply(MirrorConjugate(actual), aimed);
    if (settings.level_horizon) {
        const MirrorQuat level = MirrorMultiply(MirrorConjugate(actual), MirrorLevel(aimed));
        // As much of the levelling as fits; the filter keeps its own state.
        const double share = FittingShare(fov, crop, level);
        correction = share >= 1.0 ? level : MirrorSlerp(MirrorQuat{}, level, share);
        aim.applied = std::min(aim.applied, share);
        if (!MirrorFilled(MirrorHomographyFor(fov, crop, correction))) {
            // The smoothed aim fitted and its levelled one does not at any
            // share (the path between is not straight): the smoothed aim.
            correction = MirrorMultiply(MirrorConjugate(actual), aimed);
        }
    }
    aim.homography = MirrorHomographyFor(fov, crop, correction);
    return aim;
}

bool MirrorPacer::Due(XrTime display_time, int frame_rate) {
    const XrDuration period = 1000000000ll / std::clamp(frame_rate, 1, 1000);
    // A frame a little early for its slot (the game's rate just above the
    // window's) is still that slot's frame.
    const XrDuration slack = period / 8;
    if (next_ == 0 || display_time < next_ - 4 * period || display_time > next_ + 4 * period) {
        next_ = display_time + period;
        return true;
    }
    if (display_time + slack < next_) return false;
    next_ += period;
    if (next_ <= display_time) next_ = display_time + period;
    return true;
}

int MirrorRelay::ToCopy() const {
    if (Copying() >= 0) return -1;
    for (int slot = 0; slot < kSlots; ++slot) {
        if (slots_[slot] == Slot::kFree) return slot;
    }
    return -1;
}

void MirrorRelay::CopyStarted(int slot) {
    if (slot >= 0 && slot < kSlots && slots_[slot] == Slot::kFree) slots_[slot] = Slot::kCopying;
}

int MirrorRelay::Copying() const {
    for (int slot = 0; slot < kSlots; ++slot) {
        if (slots_[slot] == Slot::kCopying) return slot;
    }
    return -1;
}

void MirrorRelay::CopyDone(int slot) {
    if (slot < 0 || slot >= kSlots || slots_[slot] != Slot::kCopying) return;
    for (Slot& other : slots_) {
        if (other == Slot::kReady) other = Slot::kFree;
    }
    slots_[slot] = Slot::kReady;
}

void MirrorRelay::CopyFailed(int slot) {
    if (slot >= 0 && slot < kSlots && slots_[slot] == Slot::kCopying) slots_[slot] = Slot::kFree;
}

int MirrorRelay::ToShow() const {
    for (int slot = 0; slot < kSlots; ++slot) {
        if (slots_[slot] == Slot::kReady) return slot;
    }
    return -1;
}

void MirrorRelay::ShowStarted(int slot) {
    if (slot >= 0 && slot < kSlots && slots_[slot] == Slot::kReady) slots_[slot] = Slot::kShowing;
}

void MirrorRelay::ShowsDone() {
    for (Slot& slot : slots_) {
        if (slot == Slot::kShowing) slot = Slot::kFree;
    }
}

bool MirrorRelay::Idle() const {
    for (const Slot slot : slots_) {
        if (slot == Slot::kCopying || slot == Slot::kShowing) return false;
    }
    return true;
}

void MirrorRelay::DropReady() {
    for (Slot& slot : slots_) {
        if (slot == Slot::kReady) slot = Slot::kFree;
    }
}

void MirrorRelay::Reset() {
    for (Slot& slot : slots_) slot = Slot::kFree;
}

double MirrorAspectRatio(MirrorAspect aspect) {
    switch (aspect) {
    case MirrorAspect::k16x10:
        return 16.0 / 10.0;
    case MirrorAspect::k4x3:
        return 4.0 / 3.0;
    case MirrorAspect::k1x1:
        return 1.0;
    case MirrorAspect::k21x9:
        return 21.0 / 9.0;
    case MirrorAspect::k16x9:
    default:
        return 16.0 / 9.0;
    }
}

void MirrorPictureSize(int width, MirrorAspect aspect, int& out_width, int& out_height) {
    out_width = std::clamp(width, 320, 3840);
    out_height = std::max(1, static_cast<int>(std::lround(out_width / MirrorAspectRatio(aspect))));
}

int MirrorViewIndex(MirrorEye eye, std::uint32_t view_count) {
    if (view_count == 0) return -1;
    if (eye == MirrorEye::Right && view_count >= 2) return 1;
    return 0;
}

std::string_view MirrorStateName(MirrorState state) {
    switch (state) {
    case MirrorState::kOff: return "off";
    case MirrorState::kPending: return "pending";
    case MirrorState::kActive: return "active";
    case MirrorState::kHidden: return "hidden";
    case MirrorState::kUnsupported: return "unsupported";
    case MirrorState::kFailed: return "failed";
    }
    return "off";
}

} // namespace depthxr

#include "camera.h"

#include <algorithm>

namespace gta2 {
namespace {
// Straight down is representable now that ViewMatrix picks a north up vector
// there, so only guard against passing the pole.
constexpr float kPitchLimit = 1.5705f;
}

Vec3 Camera::Forward() const {
    const float cosPitch = std::cos(pitch_);
    return {std::sin(yaw_) * cosPitch, std::sin(pitch_), std::cos(yaw_) * cosPitch};
}

void Camera::Move(float forward, float right, float up, float distance) {
    const Vec3 f = Forward();
    const Vec3 r = Normalize(Cross(Vec3{0.0f, 1.0f, 0.0f}, f));
    position_ = position_ + (f * forward + r * right + Vec3{0.0f, up, 0.0f}) * distance;
}

void Camera::Look(float yawDelta, float pitchDelta) {
    yaw_ += yawDelta;
    pitch_ = std::clamp(pitch_ + pitchDelta, -kPitchLimit, kPitchLimit);
}

void Camera::FrameTarget(const Vec3& target, float distance) {
    position_ = target - Forward() * distance;
}

Mat4 Camera::ViewMatrix() const {
    const Vec3 forward = Forward();
    // Looking straight down makes a +Y up vector degenerate, which is exactly
    // the case GTA2's near-top-down view sits in; fall back to north-up there.
    const Vec3 up = std::abs(forward.y) > 0.999f ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{0.0f, 1.0f, 0.0f};
    return LookAtLH(position_, position_ + forward, up);
}

Mat4 Camera::ProjectionMatrix(float aspect) const {
    return PerspectiveFovLH(fovY_, aspect, nearZ_, farZ_);
}

}  // namespace gta2

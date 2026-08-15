// Free-fly perspective camera.
//
// Perspective rather than the orthographic projection GTA2 itself used: Remix
// classifies orthographic draws as UI and skips path tracing them.
#pragma once

#include "math3d.h"

namespace gta2 {

class Camera {
public:
    void Move(float forward, float right, float up, float distance);
    void Look(float yawDelta, float pitchDelta);

    // Places the camera so `target` sits at the centre of view, `distance` away
    // along the current orientation. Used to follow the game's camera.
    void FrameTarget(const Vec3& target, float distance);

    Mat4 ViewMatrix() const;
    Mat4 ProjectionMatrix(float aspect) const;

    const Vec3& Position() const { return position_; }
    void SetPosition(const Vec3& p) { position_ = p; }
    void SetOrientation(float yaw, float pitch) { yaw_ = yaw; pitch_ = pitch; }
    float FovY() const { return fovY_; }
    void SetFovY(float fov) { fovY_ = fov; }

private:
    Vec3 Forward() const;

    Vec3 position_ = {128.0f, 24.0f, 104.0f};
    float yaw_ = 0.0f;                  // 0 looks north (+Z)
    // A narrow field of view seen from further back flattens the perspective
    // towards GTA2's near-top-down look while staying a true perspective
    // frustum, which is what keeps Remix path tracing the world.
    float pitch_ = -1.30f;
    float fovY_ = 0.6981f;              // 40 degrees
    float nearZ_ = 0.05f;
    float farZ_ = 2000.0f;
};

}  // namespace gta2

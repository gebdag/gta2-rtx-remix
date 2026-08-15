// Minimal left-handed matrix helpers.
//
// D3DX is not part of the modern Windows SDK, so the handful of matrix builders
// this renderer needs are written out directly.
#pragma once

#include <cmath>

namespace gta2 {

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

struct Mat4 {
    float m[4][4] = {};
};

inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }

inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

inline Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline Vec3 Normalize(const Vec3& v) {
    const float length = std::sqrt(Dot(v, v));
    return length > 0.0f ? v * (1.0f / length) : v;
}

inline Mat4 Identity() {
    Mat4 out;
    out.m[0][0] = out.m[1][1] = out.m[2][2] = out.m[3][3] = 1.0f;
    return out;
}

inline Mat4 LookAtLH(const Vec3& eye, const Vec3& target, const Vec3& up) {
    const Vec3 zaxis = Normalize(target - eye);
    const Vec3 xaxis = Normalize(Cross(up, zaxis));
    const Vec3 yaxis = Cross(zaxis, xaxis);

    Mat4 out;
    out.m[0][0] = xaxis.x; out.m[0][1] = yaxis.x; out.m[0][2] = zaxis.x; out.m[0][3] = 0.0f;
    out.m[1][0] = xaxis.y; out.m[1][1] = yaxis.y; out.m[1][2] = zaxis.y; out.m[1][3] = 0.0f;
    out.m[2][0] = xaxis.z; out.m[2][1] = yaxis.z; out.m[2][2] = zaxis.z; out.m[2][3] = 0.0f;
    out.m[3][0] = -Dot(xaxis, eye);
    out.m[3][1] = -Dot(yaxis, eye);
    out.m[3][2] = -Dot(zaxis, eye);
    out.m[3][3] = 1.0f;
    return out;
}

// m[3][3] stays 0 here, which is also how Remix tells a perspective projection
// from an orthographic one when deciding whether a draw is world geometry or UI.
inline Mat4 PerspectiveFovLH(float fovY, float aspect, float nearZ, float farZ) {
    const float h = 1.0f / std::tan(fovY * 0.5f);
    const float w = h / aspect;
    const float q = farZ / (farZ - nearZ);

    Mat4 out;
    out.m[0][0] = w;
    out.m[1][1] = h;
    out.m[2][2] = q;
    out.m[2][3] = 1.0f;
    out.m[3][2] = -q * nearZ;
    return out;
}

}  // namespace gta2

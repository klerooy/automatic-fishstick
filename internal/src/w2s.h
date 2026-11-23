#pragma once
#include <cmath>

struct Vec3 { float x, y, z; };
struct Vec4 { float x, y, z, w; };

struct Camera {
    Vec3 pos;
    Vec4 rot;
    Vec3 fwd, right, up;
    float fov = 60.0f;
    bool valid = false;
};

// Unity left-handed: +X right, +Y up, +Z forward. Vertical FOV degrees.
inline bool w2s(const Camera& cam, const Vec3& world, int w, int h, float& sx, float& sy) {
    float dx = world.x - cam.pos.x;
    float dy = world.y - cam.pos.y;
    float dz = world.z - cam.pos.z;
    float depth = dx * cam.fwd.x + dy * cam.fwd.y + dz * cam.fwd.z;
    if (depth < 0.05f) return false;
    float px = dx * cam.right.x + dy * cam.right.y + dz * cam.right.z;
    float py = dx * cam.up.x + dy * cam.up.y + dz * cam.up.z;
    float tan_half = tanf(cam.fov * 0.5f * 3.14159265f / 180.0f);
    if (tan_half < 0.001f) return false;
    float aspect = (float)w / (float)h;
    sx = w * 0.5f + (px / (depth * tan_half * aspect)) * w * 0.5f;
    sy = h * 0.5f - (py / (depth * tan_half)) * h * 0.5f;
    return std::isfinite(sx) && std::isfinite(sy);
}

inline float distance(const Vec3& a, const Vec3& b) {
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return sqrtf(dx*dx + dy*dy + dz*dz);
}
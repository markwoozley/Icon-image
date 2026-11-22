#pragma once
#include <cmath>
#include <cstdint>
#include <vector>

struct Vec3 {
    double x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(double a, double b, double c) : x(a), y(b), z(c) {}
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
};
inline double Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline double Len(const Vec3& a) { return std::sqrt(Dot(a, a)); }
inline Vec3 Norm(const Vec3& a) {
    double l = Len(a);
    return l > 1e-300 ? a * (1.0 / l) : Vec3(0, 0, 0);
}

// Triangles plus optional feature lines (e.g. CAD edges) to draw on top.
struct Scene {
    std::vector<float> tris;   // 9 floats per triangle
    std::vector<float> lines;  // 6 floats per segment
    void AddTri(const Vec3& a, const Vec3& b, const Vec3& c) {
        float v[9] = {(float)a.x, (float)a.y, (float)a.z, (float)b.x, (float)b.y,
                      (float)b.z, (float)c.x, (float)c.y, (float)c.z};
        tris.insert(tris.end(), v, v + 9);
    }
    void AddLine(const Vec3& a, const Vec3& b) {
        float v[6] = {(float)a.x, (float)a.y, (float)a.z, (float)b.x, (float)b.y, (float)b.z};
        lines.insert(lines.end(), v, v + 6);
    }
    bool Empty() const { return tris.empty() && lines.empty(); }
};

// 32-bit BGRA image with premultiplied alpha, top-down rows.
struct Image {
    int width = 0, height = 0;
    std::vector<uint8_t> bgra;
};

bool LoadStl(const uint8_t* data, size_t len, Scene& scene);
bool LooksLikeStl(const uint8_t* data, size_t len);
bool LoadStep(const uint8_t* data, size_t len, Scene& scene);
bool LooksLikeStep(const uint8_t* data, size_t len);

// Renders the scene in an isometric view into a size x size image.
bool RenderScene(const Scene& scene, int size, Image& out);

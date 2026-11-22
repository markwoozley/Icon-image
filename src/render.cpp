// Tiny software rasterizer: orthographic isometric view, z-buffer, flat
// shading, supersampled anti-aliasing and an optional edge overlay.
#include "mesh.h"
#include <algorithm>
#include <cfloat>

namespace {

struct Buffer {
    int w, h;
    std::vector<float> depth;
    std::vector<float> rgb;  // linear-ish 0..1
    std::vector<uint8_t> cov;
    Buffer(int W, int H) : w(W), h(H), depth((size_t)W * H, -FLT_MAX), rgb((size_t)W * H * 3, 0), cov((size_t)W * H, 0) {}
};

struct View {
    Vec3 right, up, fwd;  // fwd points towards the viewer
    double scale, cx, cy, ox, oy;
    Vec3 Project(const Vec3& p) const {
        return {(Dot(p, right) - cx) * scale + ox, oy - (Dot(p, up) - cy) * scale, Dot(p, fwd) * scale};
    }
};

void RasterTri(Buffer& b, const Vec3& a, const Vec3& c1, const Vec3& c2, const float col[3]) {
    double minx = std::min({a.x, c1.x, c2.x}), maxx = std::max({a.x, c1.x, c2.x});
    double miny = std::min({a.y, c1.y, c2.y}), maxy = std::max({a.y, c1.y, c2.y});
    int x0 = std::max(0, (int)std::floor(minx)), x1 = std::min(b.w - 1, (int)std::ceil(maxx));
    int y0 = std::max(0, (int)std::floor(miny)), y1 = std::min(b.h - 1, (int)std::ceil(maxy));
    if (x0 > x1 || y0 > y1) return;
    double area = (c1.x - a.x) * (c2.y - a.y) - (c1.y - a.y) * (c2.x - a.x);
    if (std::fabs(area) < 1e-12) return;
    double inv = 1.0 / area;
    for (int y = y0; y <= y1; y++) {
        double py = y + 0.5;
        for (int x = x0; x <= x1; x++) {
            double px = x + 0.5;
            double w0 = ((c1.x - px) * (c2.y - py) - (c1.y - py) * (c2.x - px)) * inv;
            double w1 = ((c2.x - px) * (a.y - py) - (c2.y - py) * (a.x - px)) * inv;
            double w2 = 1.0 - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            float z = (float)(w0 * a.z + w1 * c1.z + w2 * c2.z);
            size_t i = (size_t)y * b.w + x;
            if (z <= b.depth[i]) continue;
            b.depth[i] = z;
            b.rgb[i * 3] = col[0]; b.rgb[i * 3 + 1] = col[1]; b.rgb[i * 3 + 2] = col[2];
            b.cov[i] = 1;
        }
    }
}

void RasterLine(Buffer& b, const Vec3& a, const Vec3& c, float radius, float bias, const float col[3]) {
    double dx = c.x - a.x, dy = c.y - a.y;
    int steps = (int)std::ceil(std::max(std::fabs(dx), std::fabs(dy)) / std::max(0.5f, radius * 0.5f)) + 1;
    if (steps > 100000) return;
    int r = (int)std::ceil(radius);
    for (int s = 0; s <= steps; s++) {
        double t = (double)s / steps;
        double px = a.x + dx * t, py = a.y + dy * t;
        float z = (float)(a.z + (c.z - a.z) * t) + bias;
        int cx = (int)std::floor(px), cy = (int)std::floor(py);
        for (int y = cy - r; y <= cy + r; y++) {
            if (y < 0 || y >= b.h) continue;
            for (int x = cx - r; x <= cx + r; x++) {
                if (x < 0 || x >= b.w) continue;
                double ddx = x + 0.5 - px, ddy = y + 0.5 - py;
                if (ddx * ddx + ddy * ddy > radius * radius) continue;
                size_t i = (size_t)y * b.w + x;
                if (b.cov[i] && z < b.depth[i]) continue;
                if (!b.cov[i]) b.depth[i] = z - bias;
                b.rgb[i * 3] = col[0]; b.rgb[i * 3 + 1] = col[1]; b.rgb[i * 3 + 2] = col[2];
                b.cov[i] = 1;
            }
        }
    }
}

}  // namespace

bool RenderScene(const Scene& scene, int size, Image& out) {
    if (scene.Empty() || size <= 0) return false;
    // Isometric-style view, Z up, looking from front-right-top.
    View v;
    v.fwd = Norm(Vec3(1, -1, 0.85));
    v.right = Norm(Cross(Vec3(0, 0, 1), v.fwd));
    v.up = Cross(v.fwd, v.right);

    // Bounds of the projected geometry.
    double minx = DBL_MAX, maxx = -DBL_MAX, miny = DBL_MAX, maxy = -DBL_MAX, minz = DBL_MAX, maxz = -DBL_MAX;
    auto acc = [&](const float* f, size_t count) {
        for (size_t i = 0; i + 2 < count; i += 3) {
            Vec3 p(f[i], f[i + 1], f[i + 2]);
            double x = Dot(p, v.right), y = Dot(p, v.up), z = Dot(p, v.fwd);
            minx = std::min(minx, x); maxx = std::max(maxx, x);
            miny = std::min(miny, y); maxy = std::max(maxy, y);
            minz = std::min(minz, z); maxz = std::max(maxz, z);
        }
    };
    acc(scene.tris.data(), scene.tris.size());
    acc(scene.lines.data(), scene.lines.size());
    double ext = std::max(maxx - minx, maxy - miny);
    if (!(ext > 0) || !std::isfinite(ext)) return false;

    const int ss = size >= 512 ? 2 : 3;  // supersampling factor
    int W = std::min(size * ss, 4096);
    double s = (double)W / size;
    Buffer buf(W, W);
    double margin = 0.06 * W;
    v.scale = (W - 2 * margin) / ext;
    v.cx = (minx + maxx) * 0.5;
    v.cy = (miny + maxy) * 0.5;
    v.ox = W * 0.5;
    v.oy = W * 0.5;

    const Vec3 light = Norm(v.right * -0.45 + v.up * 0.65 + v.fwd * 0.75);
    const Vec3 light2 = Norm(v.right * 0.7 + v.up * -0.2 + v.fwd * 0.4);
    const float base[3] = {0.62f, 0.69f, 0.80f};
    const float* t = scene.tris.data();
    for (size_t i = 0; i + 8 < scene.tris.size(); i += 9) {
        Vec3 a(t[i], t[i + 1], t[i + 2]), b(t[i + 3], t[i + 4], t[i + 5]), c(t[i + 6], t[i + 7], t[i + 8]);
        Vec3 n = Norm(Cross(b - a, c - a));
        if (Dot(n, v.fwd) < 0) n = -n;  // two-sided lighting
        double d = std::max(0.0, Dot(n, light)), d2 = std::max(0.0, Dot(n, light2));
        Vec3 h = Norm(light + v.fwd);
        double spec = std::pow(std::max(0.0, Dot(n, h)), 40.0) * 0.25;
        double k = 0.30 + 0.62 * d + 0.18 * d2;
        float col[3];
        for (int ch = 0; ch < 3; ch++) col[ch] = (float)std::min(1.0, base[ch] * k + spec);
        RasterTri(buf, v.Project(a), v.Project(b), v.Project(c), col);
    }

    if (!scene.lines.empty()) {
        const float edge[3] = {0.13f, 0.16f, 0.22f};
        bool wireOnly = scene.tris.empty();
        float radius = (float)(s * (size <= 64 ? 0.45 : 0.6));
        float bias = (float)((maxz - minz) * v.scale * 0.004 + s * 1.5);
        const float* l = scene.lines.data();
        for (size_t i = 0; i + 5 < scene.lines.size(); i += 6) {
            Vec3 a(l[i], l[i + 1], l[i + 2]), b(l[i + 3], l[i + 4], l[i + 5]);
            RasterLine(buf, v.Project(a), v.Project(b), wireOnly ? radius * 1.3f : radius, bias, edge);
        }
    }

    // Downsample into premultiplied BGRA.
    out.width = size;
    out.height = size;
    out.bgra.assign((size_t)size * size * 4, 0);
    int iss = (int)std::round(s);
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            double r = 0, g = 0, bl = 0, a = 0;
            for (int sy = 0; sy < iss; sy++) {
                for (int sx = 0; sx < iss; sx++) {
                    int px = x * iss + sx, py = y * iss + sy;
                    if (px >= W || py >= W) continue;
                    size_t i = (size_t)py * W + px;
                    if (!buf.cov[i]) continue;
                    r += buf.rgb[i * 3]; g += buf.rgb[i * 3 + 1]; bl += buf.rgb[i * 3 + 2]; a += 1;
                }
            }
            double n = iss * iss;
            uint8_t* o = &out.bgra[((size_t)y * size + x) * 4];
            o[0] = (uint8_t)std::lround(bl / n * 255);
            o[1] = (uint8_t)std::lround(g / n * 255);
            o[2] = (uint8_t)std::lround(r / n * 255);
            o[3] = (uint8_t)std::lround(a / n * 255);
        }
    }
    return true;
}

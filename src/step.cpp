// STEP (ISO 10303-21) reader that turns B-rep geometry into a triangle mesh
// good enough for a thumbnail: edges are sampled from their curves (lines,
// circles, ellipses, B-splines, polylines), and faces are triangulated from
// their boundary loops in the surface's parameter space. Assembly placements
// (shape representation relationships and mapped items) are honoured.
#include "mesh.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace {

const double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------
struct Param {
    enum Kind { None, Num, Ref, Str, Enum, List, Typed } kind = None;
    double num = 0;
    int ref = 0;
    std::string str;          // string, enum, or typed-parameter name
    std::vector<Param> list;  // list items, or typed-parameter arguments
};

struct Part {
    std::string type;
    std::vector<Param> args;
};

class Parser {
public:
    Parser(const char* p, const char* e) : p_(p), e_(e) {}

    void Ws() {
        for (;;) {
            while (p_ < e_ && (unsigned char)*p_ <= ' ') p_++;
            if (p_ + 1 < e_ && p_[0] == '/' && p_[1] == '*') {
                const char* c = p_ + 2;
                while (c + 1 < e_ && !(c[0] == '*' && c[1] == '/')) c++;
                p_ = c + 2 <= e_ ? c + 2 : e_;
                continue;
            }
            break;
        }
    }
    bool Eat(char c) {
        Ws();
        if (p_ < e_ && *p_ == c) { p_++; return true; }
        return false;
    }
    std::string Ident() {
        Ws();
        const char* s = p_;
        while (p_ < e_ && (std::isalnum((unsigned char)*p_) || *p_ == '_')) p_++;
        std::string r(s, p_);
        for (char& c : r) c = (char)std::toupper((unsigned char)c);
        return r;
    }
    bool ParseParam(Param& out, int depth = 0) {
        Ws();
        if (p_ >= e_ || depth > 64) return false;
        char c = *p_;
        if (c == '#') {
            p_++;
            out.kind = Param::Ref;
            out.ref = (int)std::strtol(p_, (char**)&p_, 10);
            return true;
        }
        if (c == '\'') {
            out.kind = Param::Str;
            p_++;
            while (p_ < e_) {
                if (*p_ == '\'') {
                    if (p_ + 1 < e_ && p_[1] == '\'') { out.str.push_back('\''); p_ += 2; continue; }
                    p_++;
                    break;
                }
                out.str.push_back(*p_++);
            }
            return true;
        }
        if (c == '.') {
            p_++;
            out.kind = Param::Enum;
            while (p_ < e_ && *p_ != '.') out.str.push_back(*p_++);
            if (p_ < e_) p_++;
            return true;
        }
        if (c == '$' || c == '*') {
            p_++;
            out.kind = Param::None;
            return true;
        }
        if (c == '(') {
            out.kind = Param::List;
            return ParseList(out.list, depth);
        }
        if (c == '"') {  // binary
            p_++;
            while (p_ < e_ && *p_ != '"') p_++;
            if (p_ < e_) p_++;
            out.kind = Param::None;
            return true;
        }
        if (std::isalpha((unsigned char)c)) {
            out.kind = Param::Typed;
            out.str = Ident();
            return ParseList(out.list, depth);
        }
        out.kind = Param::Num;
        const char* s = p_;
        out.num = std::strtod(p_, (char**)&p_);
        if (p_ == s) return false;
        return true;
    }
    bool ParseList(std::vector<Param>& list, int depth) {
        if (!Eat('(')) return false;
        if (Eat(')')) return true;
        for (;;) {
            list.emplace_back();
            if (!ParseParam(list.back(), depth + 1)) return false;
            if (Eat(',')) continue;
            if (Eat(')')) return true;
            return false;
        }
    }
    // Parses an entity body: either TYPE(args) or ( TYPE(args) TYPE(args) ... )
    bool ParseEntity(std::vector<Part>& parts) {
        Ws();
        if (p_ < e_ && *p_ == '(') {
            p_++;
            for (;;) {
                Ws();
                if (Eat(')')) return true;
                Part part;
                part.type = Ident();
                if (part.type.empty() || !ParseList(part.args, 0)) return false;
                parts.push_back(std::move(part));
            }
        }
        Part part;
        part.type = Ident();
        if (part.type.empty() || !ParseList(part.args, 0)) return false;
        parts.push_back(std::move(part));
        return true;
    }

private:
    const char* p_;
    const char* e_;
};

// ---------------------------------------------------------------------------
// Entity index: records the text span of every #id=...; and parses lazily.
// ---------------------------------------------------------------------------
struct Span {
    uint32_t begin, end;
    std::string type;  // first type name (for complex entities, all names joined by ' ')
};

class StepFile {
public:
    bool Index(const char* d, size_t n) {
        d_ = d;
        const char* p = d;
        const char* e = d + n;
        // Skip to DATA section.
        const char* data = nullptr;
        for (const char* q = p; q + 5 <= e; q++) {
            if ((q == d || q[-1] == ';' || (unsigned char)q[-1] <= ' ') && std::memcmp(q, "DATA", 4) == 0) {
                const char* r = q + 4;
                while (r < e && (unsigned char)*r <= ' ') r++;
                if (r < e && (*r == ';' || *r == '(')) { data = r; break; }
            }
        }
        if (!data) return false;
        p = data;
        while (p < e) {
            // find next '#'
            while (p < e && *p != '#') {
                if (*p == '\'') { p++; while (p < e && *p != '\'') p++; }
                else if (p + 1 < e && p[0] == '/' && p[1] == '*') { p += 2; while (p + 1 < e && !(p[0] == '*' && p[1] == '/')) p++; p++; }
                if (p < e) p++;
            }
            if (p >= e) break;
            p++;
            char* endp;
            long id = std::strtol(p, &endp, 10);
            p = endp;
            while (p < e && (unsigned char)*p <= ' ') p++;
            if (p >= e || *p != '=') continue;
            p++;
            const char* body = p;
            // find terminating ';' outside strings
            while (p < e && *p != ';') {
                if (*p == '\'') { p++; while (p < e && *p != '\'') p++; }
                if (p < e) p++;
            }
            Span sp;
            sp.begin = (uint32_t)(body - d);
            sp.end = (uint32_t)(p - d);
            // type name(s)
            const char* q = body;
            while (q < p && (unsigned char)*q <= ' ') q++;
            if (q < p && *q == '(') {
                // complex: collect top-level identifiers
                int depth = 0;
                for (const char* r = q + 1; r < p; r++) {
                    if (*r == '\'') { r++; while (r < p && *r != '\'') r++; continue; }
                    if (*r == '(') depth++;
                    else if (*r == ')') depth--;
                    else if (depth == 0 && std::isalpha((unsigned char)*r)) {
                        const char* s = r;
                        while (r < p && (std::isalnum((unsigned char)*r) || *r == '_')) r++;
                        if (!sp.type.empty()) sp.type.push_back(' ');
                        sp.type.append(s, r);
                        r--;
                    }
                }
                for (char& c : sp.type) c = (char)std::toupper((unsigned char)c);
                sp.type = " " + sp.type + " ";
            } else {
                const char* s = q;
                while (q < p && (std::isalnum((unsigned char)*q) || *q == '_')) q++;
                sp.type.assign(s, q);
                for (char& c : sp.type) c = (char)std::toupper((unsigned char)c);
            }
            spans_[(int)id] = std::move(sp);
            if (p < e) p++;
        }
        return !spans_.empty();
    }

    const std::string* Type(int id) const {
        auto it = spans_.find(id);
        return it == spans_.end() ? nullptr : &it->second.type;
    }
    bool IsComplex(int id) const {
        auto t = Type(id);
        return t && !t->empty() && (*t)[0] == ' ';
    }
    bool HasPart(int id, const char* name) const {
        auto t = Type(id);
        if (!t) return false;
        if ((*t)[0] != ' ') return *t == name;
        return t->find(std::string(" ") + name + " ") != std::string::npos;
    }

    // Parsed entity, cached.
    const std::vector<Part>* Get(int id) {
        auto c = cache_.find(id);
        if (c != cache_.end()) return c->second.get();
        auto it = spans_.find(id);
        if (it == spans_.end()) return nullptr;
        auto parts = std::make_unique<std::vector<Part>>();
        Parser ps(d_ + it->second.begin, d_ + it->second.end);
        if (!ps.ParseEntity(*parts)) parts->clear();
        auto* r = parts.get();
        cache_[id] = std::move(parts);
        return r;
    }
    // Args of a simple entity, or of the named part of a complex entity.
    const std::vector<Param>* Args(int id, const char* part = nullptr) {
        auto* parts = Get(id);
        if (!parts || parts->empty()) return nullptr;
        if (!part) return &(*parts)[0].args;
        for (auto& pt : *parts)
            if (pt.type == part) return &pt.args;
        return nullptr;
    }

    template <class F>
    void ForEach(F f) const {
        for (auto& kv : spans_) f(kv.first, kv.second.type);
    }

private:
    const char* d_ = nullptr;
    std::unordered_map<int, Span> spans_;
    std::unordered_map<int, std::unique_ptr<std::vector<Part>>> cache_;
};

// ---------------------------------------------------------------------------
// Geometry helpers
// ---------------------------------------------------------------------------
struct Frame {
    Vec3 o, x{1, 0, 0}, y{0, 1, 0}, z{0, 0, 1};
    Vec3 ToWorld(const Vec3& p) const { return o + x * p.x + y * p.y + z * p.z; }
    Vec3 ToLocal(const Vec3& p) const {
        Vec3 d = p - o;
        return {Dot(d, x), Dot(d, y), Dot(d, z)};
    }
};

// Affine transform: rotation rows + translation.
struct Xform {
    double m[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    Vec3 Apply(const Vec3& p) const {
        return {m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3],
                m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3],
                m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3]};
    }
    static Xform FromFrame(const Frame& f) {
        Xform r;
        const Vec3* c[3] = {&f.x, &f.y, &f.z};
        for (int j = 0; j < 3; j++) {
            r.m[0][j] = c[j]->x; r.m[1][j] = c[j]->y; r.m[2][j] = c[j]->z;
        }
        r.m[0][3] = f.o.x; r.m[1][3] = f.o.y; r.m[2][3] = f.o.z;
        return r;
    }
    static Xform InverseFrame(const Frame& f) {  // orthonormal inverse
        Xform r;
        const Vec3* c[3] = {&f.x, &f.y, &f.z};
        for (int i = 0; i < 3; i++) {
            r.m[i][0] = c[i]->x; r.m[i][1] = c[i]->y; r.m[i][2] = c[i]->z;
            r.m[i][3] = -Dot(*c[i], f.o);
        }
        return r;
    }
    Xform operator*(const Xform& b) const {
        Xform r;
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 4; j++) {
                double s = (j == 3) ? m[i][3] : 0;
                for (int k = 0; k < 3; k++) s += m[i][k] * b.m[k][j];
                r.m[i][j] = s;
            }
        }
        return r;
    }
};

class Builder {
public:
    Builder(StepFile& f) : f_(f) {}

    bool Point(int id, Vec3& out) {
        auto* a = f_.Args(id);
        if (!a) return false;
        const std::string* t = f_.Type(id);
        if (*t == "VERTEX_POINT") return a->size() >= 2 && (*a)[1].kind == Param::Ref && Point((*a)[1].ref, out);
        if (*t != "CARTESIAN_POINT" || a->size() < 2 || (*a)[1].kind != Param::List) return false;
        const auto& l = (*a)[1].list;
        double c[3] = {0, 0, 0};
        for (size_t i = 0; i < l.size() && i < 3; i++) c[i] = l[i].num;
        out = {c[0], c[1], c[2]};
        return true;
    }
    bool Direction(int id, Vec3& out) {
        auto* a = f_.Args(id);
        if (!a) return false;
        const std::string* t = f_.Type(id);
        if (*t == "VECTOR") return a->size() >= 2 && (*a)[1].kind == Param::Ref && Direction((*a)[1].ref, out);
        if (*t != "DIRECTION" || a->size() < 2 || (*a)[1].kind != Param::List) return false;
        const auto& l = (*a)[1].list;
        double c[3] = {0, 0, 0};
        for (size_t i = 0; i < l.size() && i < 3; i++) c[i] = l[i].num;
        out = Norm(Vec3(c[0], c[1], c[2]));
        return Len(out) > 0;
    }
    bool Placement(int id, Frame& f) {
        auto* a = f_.Args(id);
        const std::string* t = f_.Type(id);
        if (!a || !t || a->size() < 2) return false;
        if (*t != "AXIS2_PLACEMENT_3D" && *t != "AXIS1_PLACEMENT" && *t != "AXIS2_PLACEMENT_2D") return false;
        f = Frame();
        if ((*a)[1].kind != Param::Ref || !Point((*a)[1].ref, f.o)) return false;
        Vec3 z(0, 0, 1), x(1, 0, 0);
        bool hasX = false;
        if (*t == "AXIS2_PLACEMENT_2D") {
            if (a->size() > 2 && (*a)[2].kind == Param::Ref) hasX = Direction((*a)[2].ref, x);
        } else {
            if (a->size() > 2 && (*a)[2].kind == Param::Ref) Direction((*a)[2].ref, z);
            if (a->size() > 3 && (*a)[3].kind == Param::Ref) hasX = Direction((*a)[3].ref, x);
        }
        if (!hasX || Len(Cross(z, x)) < 1e-9) {
            x = std::fabs(z.x) < 0.9 ? Vec3(1, 0, 0) : Vec3(0, 1, 0);
        }
        x = Norm(x - z * Dot(x, z));
        f.z = z;
        f.x = x;
        f.y = Cross(z, x);
        return true;
    }

    // --- Curves -------------------------------------------------------------
    // Samples a curve between p0 and p1 (p0 == p1 means a closed curve).
    void SampleCurve(int curve, const Vec3& p0, const Vec3& p1, bool sense, std::vector<Vec3>& out, int depth = 0) {
        out.clear();
        const std::string* t = f_.Type(curve);
        if (!t || depth > 8) { out = {p0, p1}; return; }
        double tol = 1e-7 * (1 + Len(p0) + Len(p1));
        bool closed = Len(p1 - p0) <= tol;
        if (*t == "SURFACE_CURVE" || *t == "SEAM_CURVE" || *t == "BOUNDED_SURFACE_CURVE" || *t == "TRIMMED_CURVE") {
            auto* a = f_.Args(curve);
            if (a && a->size() > 1 && (*a)[1].kind == Param::Ref) {
                SampleCurve((*a)[1].ref, p0, p1, sense, out, depth + 1);
                return;
            }
        }
        if (*t == "CIRCLE" || *t == "ELLIPSE") {
            auto* a = f_.Args(curve);
            Frame fr;
            if (a && a->size() >= 3 && (*a)[1].kind == Param::Ref && Placement((*a)[1].ref, fr)) {
                double r1 = (*a)[2].num, r2 = (*t == "ELLIPSE" && a->size() >= 4) ? (*a)[3].num : r1;
                if (r1 > 0 && r2 > 0) {
                    auto ang = [&](const Vec3& p) {
                        Vec3 l = fr.ToLocal(p);
                        return std::atan2(l.y / r2, l.x / r1);
                    };
                    double a0 = ang(p0), a1 = ang(p1);
                    double sweep;
                    if (closed) sweep = sense ? 2 * kPi : -2 * kPi;
                    else if (sense) { sweep = a1 - a0; while (sweep <= 0) sweep += 2 * kPi; }
                    else { sweep = a1 - a0; while (sweep >= 0) sweep -= 2 * kPi; }
                    int n = std::max(4, (int)std::ceil(std::fabs(sweep) / (2 * kPi) * 72));
                    for (int i = 0; i <= n; i++) {
                        double u = a0 + sweep * i / n;
                        out.push_back(fr.ToWorld(Vec3(r1 * std::cos(u), r2 * std::sin(u), 0)));
                    }
                    out.front() = p0;
                    out.back() = p1;
                    return;
                }
            }
        }
        if (*t == "POLYLINE") {
            auto* a = f_.Args(curve);
            std::vector<Vec3> pts;
            if (a && a->size() >= 2 && (*a)[1].kind == Param::List)
                for (auto& pr : (*a)[1].list) {
                    Vec3 p;
                    if (pr.kind == Param::Ref && Point(pr.ref, p)) pts.push_back(p);
                }
            if (pts.size() >= 2) { Slice(pts, p0, p1, closed, out); return; }
        }
        std::vector<Vec3> pts;
        if (SampleBSpline(curve, pts) && pts.size() >= 2) {
            Slice(pts, p0, p1, closed, out);
            return;
        }
        out = {p0, p1};  // LINE and anything unsupported
    }

    // Cuts the part of a densely sampled curve that runs from p0 to p1.
    static void Slice(const std::vector<Vec3>& pts, const Vec3& p0, const Vec3& p1, bool closed, std::vector<Vec3>& out) {
        auto nearest = [&](const Vec3& p) {
            size_t best = 0;
            double bd = 1e300;
            for (size_t i = 0; i < pts.size(); i++) {
                double d = Len(pts[i] - p);
                if (d < bd) { bd = d; best = i; }
            }
            return best;
        };
        if (closed) {
            out = pts;
        } else {
            size_t i0 = nearest(p0), i1 = nearest(p1);
            out.clear();
            if (i0 <= i1) for (size_t i = i0; i <= i1; i++) out.push_back(pts[i]);
            else for (size_t i = i0 + 1; i-- > i1;) out.push_back(pts[i]);
            if (out.size() < 2) out = {p0, p1};
        }
        out.front() = p0;
        out.back() = p1;
    }

    bool SampleBSpline(int curve, std::vector<Vec3>& pts) {
        const std::vector<Param>* base = nullptr;
        const std::vector<Param>* knotsP = nullptr;
        const std::vector<Param>* weightsP = nullptr;
        size_t ctrlIdx, knotOff;
        const std::string* t = f_.Type(curve);
        if (!t) return false;
        if (f_.IsComplex(curve)) {
            base = f_.Args(curve, "B_SPLINE_CURVE");
            knotsP = f_.Args(curve, "B_SPLINE_CURVE_WITH_KNOTS");
            weightsP = f_.Args(curve, "RATIONAL_B_SPLINE_CURVE");
            ctrlIdx = 1;  // (degree, ctrl, form, closed, self)
            knotOff = 0;  // (mults, knots, spec)
            if (!base || base->size() < 2) return false;
        } else if (*t == "B_SPLINE_CURVE_WITH_KNOTS" || *t == "BEZIER_CURVE" || *t == "QUASI_UNIFORM_CURVE" ||
                   *t == "UNIFORM_CURVE" || *t == "B_SPLINE_CURVE") {
            base = f_.Args(curve);
            if (!base || base->size() < 3) return false;
            ctrlIdx = 2;  // ('', degree, ctrl, ...)
            if (*t == "B_SPLINE_CURVE_WITH_KNOTS") { knotsP = base; knotOff = 6; }
        } else {
            return false;
        }
        int degree = (int)(*base)[ctrlIdx - 1].num;
        if ((*base)[ctrlIdx].kind != Param::List) return false;
        std::vector<Vec3> ctrl;
        for (auto& pr : (*base)[ctrlIdx].list) {
            Vec3 p;
            if (pr.kind == Param::Ref && Point(pr.ref, p)) ctrl.push_back(p);
        }
        size_t n = ctrl.size();
        if (n < 2 || degree < 1) return false;
        if (degree >= (int)n) degree = (int)n - 1;
        std::vector<double> w(n, 1.0);
        if (weightsP && !weightsP->empty() && (*weightsP)[0].kind == Param::List && (*weightsP)[0].list.size() == n)
            for (size_t i = 0; i < n; i++) w[i] = (*weightsP)[0].list[i].num;
        // Expand knot vector.
        std::vector<double> U;
        if (knotsP && knotsP->size() >= knotOff + 2 && (*knotsP)[knotOff].kind == Param::List &&
            (*knotsP)[knotOff + 1].kind == Param::List) {
            auto& mul = (*knotsP)[knotOff].list;
            auto& kn = (*knotsP)[knotOff + 1].list;
            for (size_t i = 0; i < mul.size() && i < kn.size(); i++)
                for (int m = 0; m < (int)mul[i].num && U.size() < 100000; m++) U.push_back(kn[i].num);
        }
        if (U.size() != n + degree + 1) {
            // Fall back to a clamped uniform knot vector.
            U.clear();
            int inner = (int)n - degree - 1;
            for (int i = 0; i <= degree; i++) U.push_back(0);
            for (int i = 1; i <= inner; i++) U.push_back((double)i / (inner + 1));
            for (int i = 0; i <= degree; i++) U.push_back(1);
        }
        double u0 = U[degree], u1 = U[n];
        if (!(u1 > u0)) return false;
        int samples = (int)std::min<size_t>(400, std::max<size_t>(16, n * 8));
        std::vector<Vec3> d(degree + 1);
        std::vector<double> dw(degree + 1);
        for (int s = 0; s <= samples; s++) {
            double u = u0 + (u1 - u0) * s / samples;
            size_t k = degree;
            while (k + 1 < n && U[k + 1] <= u) k++;
            for (int j = 0; j <= degree; j++) {
                size_t idx = k - degree + j;
                d[j] = ctrl[idx] * w[idx];
                dw[j] = w[idx];
            }
            for (int r = 1; r <= degree; r++) {
                for (int j = degree; j >= r; j--) {
                    size_t i = k - degree + j;
                    double den = U[i + degree - r + 1] - U[i];
                    double a = den > 0 ? (u - U[i]) / den : 0;
                    d[j] = d[j - 1] * (1 - a) + d[j] * a;
                    dw[j] = dw[j - 1] * (1 - a) + dw[j] * a;
                }
            }
            pts.push_back(dw[degree] != 0 ? d[degree] * (1.0 / dw[degree]) : d[degree]);
        }
        return true;
    }

    // Sampled EDGE_CURVE polyline from its start to its end vertex (cached).
    const std::vector<Vec3>* Edge(int id) {
        auto it = edges_.find(id);
        if (it != edges_.end()) return &it->second;
        auto& out = edges_[id];
        auto* a = f_.Args(id);
        const std::string* t = f_.Type(id);
        if (!a || !t || *t != "EDGE_CURVE" || a->size() < 5) return &out;
        Vec3 p0, p1;
        if ((*a)[1].kind != Param::Ref || (*a)[2].kind != Param::Ref || !Point((*a)[1].ref, p0) || !Point((*a)[2].ref, p1))
            return &out;
        bool sense = (*a)[4].kind != Param::Enum || (*a)[4].str != "F";
        if ((*a)[3].kind == Param::Ref) SampleCurve((*a)[3].ref, p0, p1, sense, out);
        else out = {p0, p1};
        newEdges_.push_back(id);
        return &out;
    }

    // Collects the 3D loop polyline of an EDGE_LOOP / POLY_LOOP.
    bool Loop(int id, std::vector<Vec3>& pts) {
        pts.clear();
        auto* a = f_.Args(id);
        const std::string* t = f_.Type(id);
        if (!a || !t || a->size() < 2 || (*a)[1].kind != Param::List) {
            if (t && *t == "VERTEX_LOOP") return false;
            return false;
        }
        if (*t == "POLY_LOOP") {
            for (auto& pr : (*a)[1].list) {
                Vec3 p;
                if (pr.kind == Param::Ref && Point(pr.ref, p)) pts.push_back(p);
            }
            for (size_t i = 0; i < pts.size(); i++) loopEdges_.push_back({pts[i], pts[(i + 1) % pts.size()]});
            return pts.size() >= 3;
        }
        if (*t != "EDGE_LOOP") return false;
        for (auto& oe : (*a)[1].list) {
            if (oe.kind != Param::Ref) continue;
            auto* oa = f_.Args(oe.ref);
            const std::string* ot = f_.Type(oe.ref);
            if (!oa || !ot) continue;
            int edgeId = oe.ref;
            bool fwd = true;
            if (*ot == "ORIENTED_EDGE" && oa->size() >= 5 && (*oa)[3].kind == Param::Ref) {
                edgeId = (*oa)[3].ref;
                fwd = !((*oa)[4].kind == Param::Enum && (*oa)[4].str == "F");
            }
            const std::vector<Vec3>* e = Edge(edgeId);
            if (!e || e->size() < 2) continue;
            if (fwd) pts.insert(pts.end(), e->begin(), e->end() - 1);
            else for (size_t i = e->size(); i-- > 1;) pts.push_back((*e)[i]);
        }
        return pts.size() >= 2;
    }

    // --- Faces --------------------------------------------------------------
    void Face(int id, Scene& local) {
        auto* a = f_.Args(id);
        if (!a || a->size() < 3 || (*a)[1].kind != Param::List) return;
        std::vector<std::vector<Vec3>> loops;
        for (auto& b : (*a)[1].list) {
            if (b.kind != Param::Ref) continue;
            auto* ba = f_.Args(b.ref);
            if (!ba || ba->size() < 2 || (*ba)[1].kind != Param::Ref) continue;
            std::vector<Vec3> pts;
            if (Loop((*ba)[1].ref, pts) && pts.size() >= 3) loops.push_back(std::move(pts));
        }
        for (int e : newEdges_) {
            auto& pl = edges_[e];
            for (size_t i = 0; i + 1 < pl.size(); i++) local.AddLine(pl[i], pl[i + 1]);
        }
        newEdges_.clear();
        for (auto& s : loopEdges_) local.AddLine(s.first, s.second);
        loopEdges_.clear();
        if (loops.empty()) return;

        int surf = (*a)[2].kind == Param::Ref ? (*a)[2].ref : 0;
        const std::string* st = f_.Type(surf);
        Frame fr;
        bool revolved = st && (*st == "CYLINDRICAL_SURFACE" || *st == "CONICAL_SURFACE" || *st == "TOROIDAL_SURFACE" ||
                               *st == "SPHERICAL_SURFACE");
        if (revolved) {
            auto* sa = f_.Args(surf);
            if (!sa || sa->size() < 2 || (*sa)[1].kind != Param::Ref || !Placement((*sa)[1].ref, fr)) revolved = false;
        }
        std::vector<std::vector<double>> uv;  // 2 doubles per point
        if (revolved && *st != "SPHERICAL_SURFACE" && ParamRevolved(loops, fr, *st == "TOROIDAL_SURFACE", uv)) {
            // ok
        } else {
            ParamPlanar(loops, uv);
        }
        Triangulate(loops, uv, local);
    }

    // Projects loops onto their best-fit plane (Newell normal).
    static void ParamPlanar(const std::vector<std::vector<Vec3>>& loops, std::vector<std::vector<double>>& uv) {
        Vec3 n(0, 0, 0), c(0, 0, 0);
        size_t cnt = 0;
        for (auto& l : loops) {
            for (size_t i = 0; i < l.size(); i++) {
                const Vec3& p = l[i];
                const Vec3& q = l[(i + 1) % l.size()];
                n.x += (p.y - q.y) * (p.z + q.z);
                n.y += (p.z - q.z) * (p.x + q.x);
                n.z += (p.x - q.x) * (p.y + q.y);
                c = c + p;
                cnt++;
            }
        }
        n = Norm(n);
        if (Len(n) == 0) n = Vec3(0, 0, 1);
        Vec3 x = std::fabs(n.x) < 0.9 ? Vec3(1, 0, 0) : Vec3(0, 1, 0);
        x = Norm(x - n * Dot(x, n));
        Vec3 y = Cross(n, x);
        uv.clear();
        for (auto& l : loops) {
            uv.emplace_back();
            for (auto& p : l) { uv.back().push_back(Dot(p, x)); uv.back().push_back(Dot(p, y)); }
        }
    }

    // (angle * radius, height) parameterisation for surfaces of revolution.
    // Loops that wrap all the way around the axis are joined into a band.
    static bool ParamRevolved(std::vector<std::vector<Vec3>>& loops, const Frame& fr, bool torus,
                              std::vector<std::vector<double>>& uv) {
        double rad = 0;
        size_t cnt = 0;
        for (auto& l : loops)
            for (auto& p : l) {
                Vec3 q = fr.ToLocal(p);
                rad += std::sqrt(q.x * q.x + q.y * q.y);
                cnt++;
            }
        rad = cnt ? rad / cnt : 1;
        if (rad <= 0) return false;
        if (torus) return false;  // fall back to planar projection
        std::vector<std::vector<double>> th(loops.size());
        std::vector<int> winding;
        for (size_t li = 0; li < loops.size(); li++) {
            double prev = 0;
            for (size_t i = 0; i < loops[li].size(); i++) {
                Vec3 q = fr.ToLocal(loops[li][i]);
                double a = std::atan2(q.y, q.x);
                if (i > 0) {
                    while (a - prev > kPi) a -= 2 * kPi;
                    while (a - prev < -kPi) a += 2 * kPi;
                }
                th[li].push_back(a);
                prev = a;
            }
            double net = th[li].back() - th[li].front();
            double last = std::atan2(fr.ToLocal(loops[li].front()).y, fr.ToLocal(loops[li].front()).x);
            (void)last;
            // closing segment back to the start
            double a0 = th[li].front();
            double close = a0 - th[li].back();
            while (close > kPi) close -= 2 * kPi;
            while (close < -kPi) close += 2 * kPi;
            net += close;
            if (std::fabs(net) > kPi) winding.push_back((int)li);
        }
        auto h = [&](const Vec3& p) { return fr.ToLocal(p).z; };
        if (winding.empty()) {
            uv.clear();
            for (size_t li = 0; li < loops.size(); li++) {
                uv.emplace_back();
                for (size_t i = 0; i < loops[li].size(); i++) {
                    uv.back().push_back(th[li][i] * rad);
                    uv.back().push_back(h(loops[li][i]));
                }
            }
            return true;
        }
        if (winding.size() != 2) return false;
        // Build one band polygon: loop A forward, then loop B in the opposite angular direction.
        int A = winding[0], B = winding[1];
        auto dirOf = [&](int li) { return th[li].back() > th[li].front() ? 1.0 : -1.0; };
        std::vector<Vec3> pa = loops[A], pb = loops[B];
        std::vector<double> ta = th[A], tb = th[B];
        if (dirOf(A) < 0) { std::reverse(pa.begin(), pa.end()); std::reverse(ta.begin(), ta.end()); }
        if (dirOf(B) > 0) { std::reverse(pb.begin(), pb.end()); std::reverse(tb.begin(), tb.end()); }
        // pa runs with increasing angle; close it fully around.
        pa.push_back(pa.front());
        ta.push_back(ta.front() + 2 * kPi);
        double endA = ta.back();
        // rotate pb so it starts at the point angularly nearest endA
        size_t best = 0;
        double bd = 1e300;
        for (size_t i = 0; i < tb.size(); i++) {
            double d = std::fmod(std::fabs(tb[i] - endA), 2 * kPi);
            d = std::min(d, 2 * kPi - d);
            if (d < bd) { bd = d; best = i; }
        }
        std::rotate(pb.begin(), pb.begin() + best, pb.end());
        std::vector<Vec3> band = pa;
        std::vector<double> buv;
        for (size_t i = 0; i < pa.size(); i++) { buv.push_back(ta[i] * rad); buv.push_back(h(pa[i])); }
        // unwrap pb starting near endA, decreasing
        double prev = endA;
        std::vector<double> tbu;
        for (size_t i = 0; i <= pb.size(); i++) {
            const Vec3& p = pb[i % pb.size()];
            Vec3 q = fr.ToLocal(p);
            double a = std::atan2(q.y, q.x);
            while (a - prev > kPi) a -= 2 * kPi;
            while (a - prev < -kPi) a += 2 * kPi;
            band.push_back(p);
            buv.push_back(a * rad);
            buv.push_back(h(p));
            prev = a;
        }
        std::vector<std::vector<Vec3>> newLoops = {band};
        uv.clear();
        uv.push_back(buv);
        for (size_t li = 0; li < loops.size(); li++) {
            if ((int)li == A || (int)li == B) continue;
            newLoops.push_back(loops[li]);
            uv.emplace_back();
            for (size_t i = 0; i < loops[li].size(); i++) {
                uv.back().push_back(th[li][i] * rad);
                uv.back().push_back(h(loops[li][i]));
            }
        }
        loops.swap(newLoops);
        return true;
    }

    // Ear-clipping triangulation of the outer loop with holes bridged in.
    static void Triangulate(const std::vector<std::vector<Vec3>>& loops, const std::vector<std::vector<double>>& uv,
                            Scene& out) {
        struct V { double x, y; Vec3 p; };
        std::vector<std::vector<V>> polys;
        for (size_t li = 0; li < loops.size(); li++) {
            std::vector<V> poly;
            for (size_t i = 0; i < loops[li].size(); i++) {
                V v{uv[li][2 * i], uv[li][2 * i + 1], loops[li][i]};
                if (!poly.empty() && std::fabs(poly.back().x - v.x) < 1e-12 && std::fabs(poly.back().y - v.y) < 1e-12) continue;
                poly.push_back(v);
            }
            while (poly.size() > 1 && std::fabs(poly.back().x - poly[0].x) < 1e-12 && std::fabs(poly.back().y - poly[0].y) < 1e-12)
                poly.pop_back();
            if (poly.size() >= 3) polys.push_back(std::move(poly));
        }
        if (polys.empty()) return;
        auto area = [](const std::vector<V>& p) {
            double a = 0;
            for (size_t i = 0; i < p.size(); i++) {
                const V& q = p[i];
                const V& r = p[(i + 1) % p.size()];
                a += q.x * r.y - r.x * q.y;
            }
            return a * 0.5;
        };
        size_t outer = 0;
        for (size_t i = 1; i < polys.size(); i++)
            if (std::fabs(area(polys[i])) > std::fabs(area(polys[outer]))) outer = i;
        std::vector<V> poly = polys[outer];
        if (area(poly) < 0) std::reverse(poly.begin(), poly.end());
        // Holes: sort by max x descending, bridge each to the outer polygon.
        std::vector<std::vector<V>> holes;
        for (size_t i = 0; i < polys.size(); i++) {
            if (i == outer) continue;
            auto h = polys[i];
            if (area(h) > 0) std::reverse(h.begin(), h.end());
            holes.push_back(std::move(h));
        }
        auto maxX = [](const std::vector<V>& p) {
            size_t b = 0;
            for (size_t i = 1; i < p.size(); i++) if (p[i].x > p[b].x) b = i;
            return b;
        };
        std::sort(holes.begin(), holes.end(), [&](const std::vector<V>& a, const std::vector<V>& b) {
            return a[maxX(a)].x > b[maxX(b)].x;
        });
        for (auto& h : holes) {
            size_t hi = maxX(h);
            const V& m = h[hi];
            // Find nearest edge of poly hit by a ray to +x.
            double bestX = 1e300;
            size_t bestI = (size_t)-1;
            for (size_t i = 0; i < poly.size(); i++) {
                const V& a = poly[i];
                const V& b = poly[(i + 1) % poly.size()];
                if ((a.y > m.y) == (b.y > m.y)) continue;
                double x = a.x + (m.y - a.y) * (b.x - a.x) / (b.y - a.y);
                if (x >= m.x && x < bestX) {
                    bestX = x;
                    bestI = a.x > b.x ? i : (i + 1) % poly.size();
                }
            }
            if (bestI == (size_t)-1) {
                // pick nearest vertex
                double bd = 1e300;
                for (size_t i = 0; i < poly.size(); i++) {
                    double d = (poly[i].x - m.x) * (poly[i].x - m.x) + (poly[i].y - m.y) * (poly[i].y - m.y);
                    if (d < bd) { bd = d; bestI = i; }
                }
            }
            std::vector<V> merged(poly.begin(), poly.begin() + bestI + 1);
            for (size_t k = 0; k <= h.size(); k++) merged.push_back(h[(hi + k) % h.size()]);
            merged.push_back(poly[bestI]);
            merged.insert(merged.end(), poly.begin() + bestI + 1, poly.end());
            poly.swap(merged);
        }
        // Ear clipping.
        size_t n = poly.size();
        std::vector<size_t> idx(n);
        for (size_t i = 0; i < n; i++) idx[i] = i;
        auto cross = [&](size_t a, size_t b, size_t c) {
            return (poly[b].x - poly[a].x) * (poly[c].y - poly[a].y) - (poly[b].y - poly[a].y) * (poly[c].x - poly[a].x);
        };
        auto inside = [&](size_t p, size_t a, size_t b, size_t c) {
            return cross(a, b, p) >= 0 && cross(b, c, p) >= 0 && cross(c, a, p) >= 0;
        };
        size_t guard = 0;
        size_t i = 0;
        while (idx.size() > 3 && guard < n * n + 100) {
            guard++;
            size_t m = idx.size();
            size_t ia = idx[(i + m - 1) % m], ib = idx[i % m], ic = idx[(i + 1) % m];
            bool ear = cross(ia, ib, ic) > 0;
            if (ear) {
                for (size_t k = 0; k < m && ear; k++) {
                    size_t p = idx[k];
                    if (p == ia || p == ib || p == ic) continue;
                    const V& pv = poly[p];
                    if ((pv.x == poly[ia].x && pv.y == poly[ia].y) || (pv.x == poly[ib].x && pv.y == poly[ib].y) ||
                        (pv.x == poly[ic].x && pv.y == poly[ic].y))
                        continue;
                    if (inside(p, ia, ib, ic)) ear = false;
                }
            }
            if (ear || guard > n * 4 + 100) {
                // emit (forced when stuck to avoid infinite loops)
                if (cross(ia, ib, ic) != 0) out.AddTri(poly[ia].p, poly[ib].p, poly[ic].p);
                idx.erase(idx.begin() + (i % m));
                if (i > 0) i--;
                if (guard > n * 4 + 100) guard = 0, n = idx.size();
            } else {
                i = (i + 1) % m;
            }
        }
        if (idx.size() == 3) out.AddTri(poly[idx[0]].p, poly[idx[1]].p, poly[idx[2]].p);
    }

    StepFile& f_;
    std::unordered_map<int, std::vector<Vec3>> edges_;
    std::vector<int> newEdges_;
    std::vector<std::pair<Vec3, Vec3>> loopEdges_;
};

bool IsFace(const std::string& t) { return t == "ADVANCED_FACE" || t == "FACE_SURFACE"; }

// Union-find for representations linked without a transform.
struct DSU {
    std::unordered_map<int, int> p;
    int Find(int x) {
        auto it = p.find(x);
        if (it == p.end()) { p[x] = x; return x; }
        if (it->second == x) return x;
        int r = Find(it->second);
        p[x] = r;
        return r;
    }
    void Unite(int a, int b) { p[Find(a)] = Find(b); }
};

}  // namespace

bool LooksLikeStep(const uint8_t* d, size_t n) {
    size_t lim = n < 1024 ? n : 1024;
    for (size_t i = 0; i + 12 <= lim; i++)
        if (std::memcmp(d + i, "ISO-10303-21", 12) == 0) return true;
    return false;
}

bool LoadStep(const uint8_t* data, size_t len, Scene& scene) {
    if (len > 0xFFFFFFF0u) return false;
    StepFile f;
    if (!f.Index((const char*)data, len)) return false;
    Builder b(f);

    // Representations and their items.
    std::vector<int> reps;
    f.ForEach([&](int id, const std::string& t) {
        if (t.size() >= 14 && t.compare(t.size() - 14, 14, "REPRESENTATION") == 0) reps.push_back(id);
    });
    DSU dsu;
    struct Link { int child, parent; Xform x; };
    std::vector<Link> links;
    std::unordered_map<int, std::vector<int>> faceOwner;  // rep -> faces
    std::unordered_set<int> seenFace;

    for (int r : reps) {
        dsu.Find(r);
        auto* a = f.Args(r);
        if (!a || a->size() < 2 || (*a)[1].kind != Param::List) continue;
        // DFS from items to faces, without following placements/mapped items.
        std::vector<int> stack;
        for (auto& it : (*a)[1].list)
            if (it.kind == Param::Ref) stack.push_back(it.ref);
        std::unordered_set<int> visited;
        while (!stack.empty()) {
            int e = stack.back();
            stack.pop_back();
            if (!visited.insert(e).second) continue;
            const std::string* t = f.Type(e);
            if (!t) continue;
            if (IsFace(*t)) { faceOwner[r].push_back(e); continue; }
            if (*t == "MAPPED_ITEM") {
                auto* ma = f.Args(e);
                if (!ma || ma->size() < 3 || (*ma)[1].kind != Param::Ref) continue;
                auto* rm = f.Args((*ma)[1].ref);  // REPRESENTATION_MAP(origin, rep)
                if (!rm || rm->size() < 2 || (*rm)[1].kind != Param::Ref) continue;
                Frame o, tg;
                Xform x;
                if ((*rm)[0].kind == Param::Ref && b.Placement((*rm)[0].ref, o) && (*ma)[2].kind == Param::Ref &&
                    b.Placement((*ma)[2].ref, tg))
                    x = Xform::FromFrame(tg) * Xform::InverseFrame(o);
                links.push_back({(*rm)[1].ref, r, x});
                continue;
            }
            static const char* stopAt[] = {"AXIS2_PLACEMENT_3D", "CARTESIAN_POINT", "STYLED_ITEM", "DIRECTION"};
            bool stop = false;
            for (auto s : stopAt) stop = stop || *t == s;
            if (stop) continue;
            if (*t == "MANIFOLD_SOLID_BREP" || *t == "BREP_WITH_VOIDS" || *t == "SHELL_BASED_SURFACE_MODEL" ||
                *t == "CLOSED_SHELL" || *t == "OPEN_SHELL" || *t == "FACETED_BREP" || *t == "ORIENTED_CLOSED_SHELL" ||
                *t == "FACE_BASED_SURFACE_MODEL" || *t == "CONNECTED_FACE_SET") {
                auto* ea = f.Args(e);
                if (!ea) continue;
                std::function<void(const Param&)> push = [&](const Param& p) {
                    if (p.kind == Param::Ref) stack.push_back(p.ref);
                    else if (p.kind == Param::List) for (auto& q : p.list) push(q);
                };
                for (size_t k = 1; k < ea->size(); k++) push((*ea)[k]);
            }
        }
    }

    // Relationships between representations.
    f.ForEach([&](int id, const std::string& t) {
        if (t.find("REPRESENTATION_RELATIONSHIP") == std::string::npos) return;
        const std::vector<Param>* rr = f.IsComplex(id) ? f.Args(id, "REPRESENTATION_RELATIONSHIP") : f.Args(id);
        if (!rr || rr->size() < 4 || (*rr)[2].kind != Param::Ref || (*rr)[3].kind != Param::Ref) return;
        int r1 = (*rr)[2].ref, r2 = (*rr)[3].ref;
        const std::vector<Param>* tr = f.IsComplex(id) ? f.Args(id, "REPRESENTATION_RELATIONSHIP_WITH_TRANSFORMATION")
                                                       : nullptr;
        if (!f.IsComplex(id) && t == "REPRESENTATION_RELATIONSHIP_WITH_TRANSFORMATION") tr = nullptr;
        if (tr && !tr->empty() && (*tr)[0].kind == Param::Ref) {
            Xform x;
            auto* idt = f.Args((*tr)[0].ref);  // ITEM_DEFINED_TRANSFORMATION('', '', ax1, ax2)
            Frame f1, f2;
            if (idt && idt->size() >= 4 && (*idt)[2].kind == Param::Ref && (*idt)[3].kind == Param::Ref &&
                b.Placement((*idt)[2].ref, f1) && b.Placement((*idt)[3].ref, f2))
                x = Xform::FromFrame(f2) * Xform::InverseFrame(f1);
            links.push_back({r1, r2, x});
        } else {
            dsu.Unite(r1, r2);
        }
    });

    // Group-level graph: child group -> list of (parent group, transform).
    std::unordered_map<int, std::vector<std::pair<int, Xform>>> parents;
    for (auto& l : links) {
        int c = dsu.Find(l.child), p = dsu.Find(l.parent);
        if (c != p) parents[c].push_back({p, l.x});
    }
    std::unordered_map<int, std::vector<Xform>> worldCache;
    const size_t kMaxInstances = 5000;
    std::function<const std::vector<Xform>&(int, int)> world = [&](int g, int depth) -> const std::vector<Xform>& {
        auto it = worldCache.find(g);
        if (it != worldCache.end()) return it->second;
        auto& res = worldCache[g];  // placeholder guards against cycles
        auto pit = parents.find(g);
        if (pit == parents.end() || depth > 32) {
            res.push_back(Xform());
            return res;
        }
        std::vector<Xform> acc;
        for (auto& pr : pit->second) {
            const auto& pw = world(pr.first, depth + 1);
            for (auto& w : pw) {
                if (acc.size() >= kMaxInstances) break;
                acc.push_back(w * pr.second);
            }
        }
        if (acc.empty()) acc.push_back(Xform());
        worldCache[g] = acc;
        return worldCache[g];
    };

    // Tessellate each representation's faces once and instance them.
    std::unordered_map<int, Scene> groupMesh;
    for (auto& kv : faceOwner) {
        Scene& local = groupMesh[dsu.Find(kv.first)];
        for (int face : kv.second) {
            if (!seenFace.insert(face).second) continue;
            b.Face(face, local);
        }
    }
    if (groupMesh.empty()) {
        // No representation structure found: tessellate every face directly.
        Scene& local = groupMesh[0];
        f.ForEach([&](int id, const std::string& t) {
            if (IsFace(t)) b.Face(id, local);
        });
    }
    size_t budget = 30u * 1000 * 1000;  // float budget to bound memory
    for (auto& kv : groupMesh) {
        const Scene& local = kv.second;
        if (local.Empty()) continue;
        const auto& ws = world(kv.first, 0);
        for (const Xform& w : ws) {
            if (scene.tris.size() + scene.lines.size() > budget) break;
            for (size_t i = 0; i + 8 < local.tris.size(); i += 9) {
                Vec3 p[3];
                for (int k = 0; k < 3; k++)
                    p[k] = w.Apply(Vec3(local.tris[i + 3 * k], local.tris[i + 3 * k + 1], local.tris[i + 3 * k + 2]));
                scene.AddTri(p[0], p[1], p[2]);
            }
            for (size_t i = 0; i + 5 < local.lines.size(); i += 6) {
                scene.AddLine(w.Apply(Vec3(local.lines[i], local.lines[i + 1], local.lines[i + 2])),
                              w.Apply(Vec3(local.lines[i + 3], local.lines[i + 4], local.lines[i + 5])));
            }
        }
    }
    return !scene.Empty();
}

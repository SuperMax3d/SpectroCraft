// SpectroCraft Mobile - sound map: small linear algebra (vectors, rotations,
// rigid poses, dense symmetric solvers). Header-only, no JUCE dependency.
//
// Conventions used throughout the sound map code:
//   - A Pose maps points of a source frame into a target frame: x' = R x + t.
//     camFromWorld maps world points into the camera frame; worldFromTag maps
//     tag points into the world.
//   - Camera frame (OpenCV): x right in the picture, y down, z forward.
//   - Tag frame: origin at the tag's centre, x right and y up as printed,
//     z out of the printed face (toward whoever reads it).
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace scm::smap
{
inline constexpr double kPi = 3.14159265358979323846;

struct Vec3
{
    double x = 0.0, y = 0.0, z = 0.0;

    constexpr Vec3() = default;
    constexpr Vec3 (double a, double b, double c) : x (a), y (b), z (c) {}

    double& operator[] (int i) noexcept { return i == 0 ? x : i == 1 ? y : z; }
    double operator[] (int i) const noexcept { return i == 0 ? x : i == 1 ? y : z; }

    Vec3 operator+ (const Vec3& o) const noexcept { return { x + o.x, y + o.y, z + o.z }; }
    Vec3 operator- (const Vec3& o) const noexcept { return { x - o.x, y - o.y, z - o.z }; }
    Vec3 operator- () const noexcept { return { -x, -y, -z }; }
    Vec3 operator* (double s) const noexcept { return { x * s, y * s, z * s }; }
    Vec3 operator/ (double s) const noexcept { return { x / s, y / s, z / s }; }
    Vec3& operator+= (const Vec3& o) noexcept { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-= (const Vec3& o) noexcept { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator*= (double s) noexcept { x *= s; y *= s; z *= s; return *this; }

    double dot (const Vec3& o) const noexcept { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross (const Vec3& o) const noexcept { return { y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x }; }
    double norm() const noexcept { return std::sqrt (dot (*this)); }
    Vec3 normalised() const noexcept { const double n = norm(); return n > 0.0 ? *this / n : Vec3 {}; }
};

inline Vec3 operator* (double s, const Vec3& v) noexcept { return v * s; }

struct Mat3
{
    // row-major
    std::array<double, 9> m { 1, 0, 0, 0, 1, 0, 0, 0, 1 };

    static Mat3 identity() noexcept { return {}; }
    static Mat3 zero() noexcept { Mat3 r; r.m.fill (0.0); return r; }
    static Mat3 fromRows (const Vec3& a, const Vec3& b, const Vec3& c) noexcept
    {
        Mat3 r;
        r.m = { a.x, a.y, a.z, b.x, b.y, b.z, c.x, c.y, c.z };
        return r;
    }
    static Mat3 fromColumns (const Vec3& a, const Vec3& b, const Vec3& c) noexcept
    {
        Mat3 r;
        r.m = { a.x, b.x, c.x, a.y, b.y, c.y, a.z, b.z, c.z };
        return r;
    }

    double& operator() (int r, int c) noexcept { return m[(size_t) (r * 3 + c)]; }
    double operator() (int r, int c) const noexcept { return m[(size_t) (r * 3 + c)]; }

    Vec3 row (int r) const noexcept { return { (*this) (r, 0), (*this) (r, 1), (*this) (r, 2) }; }
    Vec3 col (int c) const noexcept { return { (*this) (0, c), (*this) (1, c), (*this) (2, c) }; }

    Vec3 operator* (const Vec3& v) const noexcept
    {
        return { m[0] * v.x + m[1] * v.y + m[2] * v.z,
                 m[3] * v.x + m[4] * v.y + m[5] * v.z,
                 m[6] * v.x + m[7] * v.y + m[8] * v.z };
    }
    Mat3 operator* (const Mat3& o) const noexcept
    {
        Mat3 r = zero();
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                for (int k = 0; k < 3; ++k)
                    r (i, j) += (*this) (i, k) * o (k, j);
        return r;
    }
    Mat3 operator* (double s) const noexcept { Mat3 r = *this; for (auto& v : r.m) v *= s; return r; }
    Mat3 operator+ (const Mat3& o) const noexcept { Mat3 r = *this; for (size_t i = 0; i < 9; ++i) r.m[i] += o.m[i]; return r; }
    Mat3 transposed() const noexcept
    {
        Mat3 r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                r (i, j) = (*this) (j, i);
        return r;
    }
    double determinant() const noexcept
    {
        return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
    }
};

inline Mat3 skew (const Vec3& v) noexcept
{
    Mat3 r = Mat3::zero();
    r (0, 1) = -v.z; r (0, 2) = v.y;
    r (1, 0) = v.z;  r (1, 2) = -v.x;
    r (2, 0) = -v.y; r (2, 1) = v.x;
    return r;
}

/** Rotation of angle |w| about w (Rodrigues). */
inline Mat3 expRotation (const Vec3& w) noexcept
{
    const double th = w.norm();
    const Mat3 K = skew (w);
    if (th < 1e-12)
        return Mat3::identity() + K;
    const double a = std::sin (th) / th, b = (1.0 - std::cos (th)) / (th * th);
    return Mat3::identity() + K * a + (K * K) * b;
}

/** Inverse of expRotation (angle in [0, pi]). */
inline Vec3 logRotation (const Mat3& R) noexcept
{
    const double c = std::clamp ((R (0, 0) + R (1, 1) + R (2, 2) - 1.0) * 0.5, -1.0, 1.0);
    const double th = std::acos (c);
    const Vec3 v { R (2, 1) - R (1, 2), R (0, 2) - R (2, 0), R (1, 0) - R (0, 1) };
    if (th < 1e-9)
        return v * 0.5;
    if (th > kPi - 1e-6)
    {
        // near pi: axis from the largest diagonal entry of (R + I) / 2
        int i = 0;
        if (R (1, 1) > R (i, i)) i = 1;
        if (R (2, 2) > R (i, i)) i = 2;
        Vec3 axis;
        const double d = std::sqrt (std::max (0.0, (R (i, i) + 1.0) * 0.5));
        axis[i] = d;
        for (int j = 0; j < 3; ++j)
            if (j != i) axis[j] = (R (i, j) + R (j, i)) * 0.25 / std::max (d, 1e-12);
        axis = axis.normalised();
        if (axis.dot (v) < 0.0) axis = -axis;
        return axis * th;
    }
    return v * (th / (2.0 * std::sin (th)));
}

/** Angle (radians) of the rotation A^T B. */
inline double rotationAngle (const Mat3& A, const Mat3& B) noexcept
{
    return logRotation (A.transposed() * B).norm();
}

struct Pose
{
    Mat3 R;
    Vec3 t;

    Vec3 operator* (const Vec3& p) const noexcept { return R * p + t; }
    Pose operator* (const Pose& o) const noexcept { return { R * o.R, R * o.t + t }; }
    Pose inverse() const noexcept { const Mat3 Rt = R.transposed(); return { Rt, -(Rt * t) }; }

    /** The pose moved by a small step: rotation exp(dw) on the left, translation + dt. */
    Pose perturbed (const Vec3& dw, const Vec3& dt) const noexcept { return { expRotation (dw) * R, t + dt }; }
};

/** Eigen-decomposition of a symmetric n x n matrix (row-major, size n*n) by
    cyclic Jacobi rotations. Eigenvalues ascending; vectors[k] is the unit
    eigenvector of values[k]. */
struct SymEigen
{
    std::vector<double> values;
    std::vector<std::vector<double>> vectors;
};

inline SymEigen symmetricEigen (std::vector<double> a, int n)
{
    std::vector<double> v ((size_t) (n * n), 0.0);
    for (int i = 0; i < n; ++i) v[(size_t) (i * n + i)] = 1.0;
    auto A = [&a, n] (int r, int c) -> double& { return a[(size_t) (r * n + c)]; };
    auto V = [&v, n] (int r, int c) -> double& { return v[(size_t) (r * n + c)]; };
    for (int sweep = 0; sweep < 100; ++sweep)
    {
        double off = 0.0;
        for (int p = 0; p < n; ++p)
            for (int q = p + 1; q < n; ++q) off += A (p, q) * A (p, q);
        if (off < 1e-30) break;
        for (int p = 0; p < n; ++p)
            for (int q = p + 1; q < n; ++q)
            {
                if (std::abs (A (p, q)) < 1e-300) continue;
                const double theta = (A (q, q) - A (p, p)) / (2.0 * A (p, q));
                const double t = (theta >= 0.0 ? 1.0 : -1.0) / (std::abs (theta) + std::sqrt (theta * theta + 1.0));
                const double c = 1.0 / std::sqrt (t * t + 1.0), s = t * c;
                for (int k = 0; k < n; ++k)
                {
                    const double akp = A (k, p), akq = A (k, q);
                    A (k, p) = c * akp - s * akq;
                    A (k, q) = s * akp + c * akq;
                }
                for (int k = 0; k < n; ++k)
                {
                    const double apk = A (p, k), aqk = A (q, k);
                    A (p, k) = c * apk - s * aqk;
                    A (q, k) = s * apk + c * aqk;
                }
                for (int k = 0; k < n; ++k)
                {
                    const double vkp = V (k, p), vkq = V (k, q);
                    V (k, p) = c * vkp - s * vkq;
                    V (k, q) = s * vkp + c * vkq;
                }
            }
    }
    std::vector<int> order ((size_t) n);
    for (int i = 0; i < n; ++i) order[(size_t) i] = i;
    std::sort (order.begin(), order.end(), [&] (int x, int y) { return A (x, x) < A (y, y); });
    SymEigen r;
    for (int k : order)
    {
        r.values.push_back (A (k, k));
        std::vector<double> col ((size_t) n);
        for (int i = 0; i < n; ++i) col[(size_t) i] = V (i, k);
        r.vectors.push_back (std::move (col));
    }
    return r;
}

/** Closest rotation to M (Frobenius norm; the R maximising trace(R^T M)),
    by Horn's quaternion method: a proper rotation even when M is singular
    (points in a plane) or a reflection. */
inline Mat3 nearestRotation (const Mat3& M)
{
    // S = M^T, the cross-covariance of Horn's formulation
    const double Sxx = M (0, 0), Sxy = M (1, 0), Sxz = M (2, 0);
    const double Syx = M (0, 1), Syy = M (1, 1), Syz = M (2, 1);
    const double Szx = M (0, 2), Szy = M (1, 2), Szz = M (2, 2);
    const std::vector<double> N {
        Sxx + Syy + Szz, Syz - Szy,        Szx - Sxz,        Sxy - Syx,
        Syz - Szy,       Sxx - Syy - Szz,  Sxy + Syx,        Szx + Sxz,
        Szx - Sxz,       Sxy + Syx,        -Sxx + Syy - Szz, Syz + Szy,
        Sxy - Syx,       Szx + Sxz,        Syz + Szy,        -Sxx - Syy + Szz };
    const auto e = symmetricEigen (N, 4);
    const auto& q = e.vectors.back();   // largest eigenvalue
    const double w = q[0], x = q[1], y = q[2], z = q[3];
    Mat3 R;
    R.m = { 1 - 2 * (y * y + z * z), 2 * (x * y - w * z),     2 * (x * z + w * y),
            2 * (x * y + w * z),     1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
            2 * (x * z - w * y),     2 * (y * z + w * x),     1 - 2 * (x * x + y * y) };
    return R;
}

/** Solves A x = b for a symmetric positive definite n x n matrix (row-major)
    by Cholesky. Returns false when A is not positive definite. */
inline bool choleskySolve (std::vector<double> A, std::vector<double>& x, const std::vector<double>& b, int n)
{
    for (int j = 0; j < n; ++j)
    {
        double d = A[(size_t) (j * n + j)];
        for (int k = 0; k < j; ++k) d -= A[(size_t) (j * n + k)] * A[(size_t) (j * n + k)];
        if (! (d > 0.0)) return false;
        d = std::sqrt (d);
        A[(size_t) (j * n + j)] = d;
        for (int i = j + 1; i < n; ++i)
        {
            double s = A[(size_t) (i * n + j)];
            for (int k = 0; k < j; ++k) s -= A[(size_t) (i * n + k)] * A[(size_t) (j * n + k)];
            A[(size_t) (i * n + j)] = s / d;
        }
    }
    x.assign ((size_t) n, 0.0);
    std::vector<double> y ((size_t) n, 0.0);
    for (int i = 0; i < n; ++i)
    {
        double s = b[(size_t) i];
        for (int k = 0; k < i; ++k) s -= A[(size_t) (i * n + k)] * y[(size_t) k];
        y[(size_t) i] = s / A[(size_t) (i * n + i)];
    }
    for (int i = n - 1; i >= 0; --i)
    {
        double s = y[(size_t) i];
        for (int k = i + 1; k < n; ++k) s -= A[(size_t) (k * n + i)] * x[(size_t) k];
        x[(size_t) i] = s / A[(size_t) (i * n + i)];
    }
    return true;
}

/** Huber weight for a residual norm r (IRLS): 1 inside delta, delta / r beyond. */
inline double huberWeight (double r, double delta) noexcept
{
    return r <= delta ? 1.0 : delta / r;
}

inline double median (std::vector<double> v)
{
    if (v.empty()) return 0.0;
    const size_t mid = v.size() / 2;
    std::nth_element (v.begin(), v.begin() + (long) mid, v.end());
    double m = v[mid];
    if (v.size() % 2 == 0)
    {
        std::nth_element (v.begin(), v.begin() + (long) mid - 1, v.end());
        m = 0.5 * (m + v[mid - 1]);
    }
    return m;
}
} // namespace scm::smap

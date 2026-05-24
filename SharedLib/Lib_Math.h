#pragma once
// Lib_Math.h — Generic, header-only math utilities.
//
// Design:
//   The previous version of this file wrapped SDK::FVector / SDK::FQuat /
//   etc. with custom inheriting + union types. That worked on UE 4.x where
//   the SDK uses 32-bit floats, but the union layout (with hardcoded `float`
//   X/Y/Z/W members) broke on UE 5.x where LWC made those types double-
//   backed — every brace-init became a narrowing conversion.
//
//   This version is fully generic: every function is a template over a
//   small set of concepts (Vec3, Vec4, Quat, Rotator, LinearColor, ...).
//   No SDK types are referenced; no narrowing happens because the scalar
//   type is deduced from the caller's input.
//
//   Pass SDK::FVector directly. The library doesn't care whether its
//   components are `float`, `double`, or anything else — only that the
//   type has the expected `.X` / `.Y` / `.Z` members and is constructible
//   from a brace-init list of three scalars (which all Dumper-7 vector
//   types are, on both UE 4 and UE 5).
//
//   All functions return the same vector/quat type that came in, with the
//   scalar type deduced via the `scalar_t<V>` alias.

#include <cmath>
#include <concepts>
#include <algorithm>
#include <type_traits>

namespace Math
{

// =========================================================================
// Concepts
// =========================================================================

template<typename T>
concept Vec2 = requires(T v) { v.X; v.Y; };

template<typename T>
concept Vec3 = requires(T v) { v.X; v.Y; v.Z; };

template<typename T>
concept Vec4 = requires(T v) { v.X; v.Y; v.Z; v.W; };

template<typename T>
concept Quat = Vec4<T>;   // X, Y, Z, W layout — same shape as a 4-vec

template<typename T>
concept Rotator = requires(T r) { r.Pitch; r.Yaw; r.Roll; };

template<typename T>
concept LinearColor = requires(T c) { c.R; c.G; c.B; c.A; };

// =========================================================================
// Scalar-type extraction
// =========================================================================
//
// scalar_t<V> deduces the floating-point type used by a vector's components.
// e.g. for SDK::FVector on UE 4.27 this is `float`, on UE 5.6 it is `double`.

template<typename V>
using scalar_t = std::remove_cvref_t<decltype(std::declval<V>().X)>;

template<typename R>
using rotator_scalar_t = std::remove_cvref_t<decltype(std::declval<R>().Pitch)>;

// =========================================================================
// Constants
// =========================================================================
//
// Declared as `float` for the legacy float-precision use case. Inside the
// templates below they're cast to the deduced scalar type — promoting
// float→double is lossless, so UE 5 LWC vectors still get full precision
// on their operands (only the constant itself is float-precise, which is
// more than enough for tolerance/PI-style values).

constexpr float Pi         = 3.14159265358979323846f;
constexpr float TwoPi      = Pi * 2.f;
constexpr float HalfPi     = Pi * 0.5f;
constexpr float DegToRad   = Pi / 180.f;
constexpr float RadToDeg   = 180.f / Pi;
constexpr float SmallNum   = 1e-8f;
constexpr float KindaSmall = 1e-4f;

// =========================================================================
// Scalar utilities
// =========================================================================

template<typename T> constexpr T ToRadians(T deg) noexcept { return deg * T(DegToRad); }
template<typename T> constexpr T ToDegrees(T rad) noexcept { return rad * T(RadToDeg); }

template<typename T>
constexpr T Lerp(T a, T b, T t) noexcept { return a + (b - a) * t; }

template<typename T>
constexpr T Clamp(T v, T lo, T hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

template<typename T> constexpr T Clamp01(T v) noexcept { return Clamp(v, T(0), T(1)); }
template<typename T> constexpr T Abs    (T v) noexcept { return v < T(0) ? -v : v; }
template<typename T> constexpr T Sign   (T v) noexcept { return v < T(0) ? T(-1) : (v > T(0) ? T(1) : T(0)); }
template<typename T> constexpr T Square (T v) noexcept { return v * v; }

// =========================================================================
// Vec3
// =========================================================================

template<Vec3 V>
constexpr scalar_t<V> Dot(const V& a, const V& b) noexcept
{
    return a.X * b.X + a.Y * b.Y + a.Z * b.Z;
}

template<Vec3 V>
constexpr scalar_t<V> SizeSquared(const V& v) noexcept
{
    return v.X * v.X + v.Y * v.Y + v.Z * v.Z;
}

template<Vec3 V>
scalar_t<V> Size(const V& v) noexcept { return std::sqrt(SizeSquared(v)); }

template<Vec3 V>
constexpr scalar_t<V> DistSq(const V& a, const V& b) noexcept
{
    using T = scalar_t<V>;
    const T dx = b.X - a.X, dy = b.Y - a.Y, dz = b.Z - a.Z;
    return dx*dx + dy*dy + dz*dz;
}

template<Vec3 V>
scalar_t<V> Dist(const V& a, const V& b) noexcept { return std::sqrt(DistSq(a, b)); }

template<Vec3 V>
constexpr V Cross(const V& a, const V& b) noexcept
{
    return V{ a.Y * b.Z - a.Z * b.Y,
              a.Z * b.X - a.X * b.Z,
              a.X * b.Y - a.Y * b.X };
}

template<Vec3 V>
V GetNormalized(const V& v) noexcept
{
    using T = scalar_t<V>;
    const T sz = Size(v);
    return sz > T(SmallNum) ? V{ v.X / sz, v.Y / sz, v.Z / sz } : V{};
}

template<Vec3 V>
V Reflect(const V& v, const V& n) noexcept
{
    using T = scalar_t<V>;
    const T d = Dot(v, n);
    return V{ v.X - T(2) * d * n.X,
              v.Y - T(2) * d * n.Y,
              v.Z - T(2) * d * n.Z };
}

template<Vec3 V>
V ProjectOnTo(const V& v, const V& onto) noexcept
{
    using T = scalar_t<V>;
    const T sq = Dot(onto, onto);
    if (sq < T(SmallNum)) return V{};
    const T s = Dot(v, onto) / sq;
    return V{ onto.X * s, onto.Y * s, onto.Z * s };
}

template<Vec3 V>
constexpr V ComponentMin(const V& a, const V& b) noexcept
{
    return V{ a.X < b.X ? a.X : b.X,
              a.Y < b.Y ? a.Y : b.Y,
              a.Z < b.Z ? a.Z : b.Z };
}

template<Vec3 V>
constexpr V ComponentMax(const V& a, const V& b) noexcept
{
    return V{ a.X > b.X ? a.X : b.X,
              a.Y > b.Y ? a.Y : b.Y,
              a.Z > b.Z ? a.Z : b.Z };
}

template<Vec3 V>
constexpr V Abs(const V& v) noexcept
{
    using T = scalar_t<V>;
    return V{ v.X < T(0) ? -v.X : v.X,
              v.Y < T(0) ? -v.Y : v.Y,
              v.Z < T(0) ? -v.Z : v.Z };
}

template<Vec3 V>
scalar_t<V> AngleBetween(const V& a, const V& b) noexcept
{
    using T = scalar_t<V>;
    const T sa = Size(a), sb = Size(b);
    if (sa < T(SmallNum) || sb < T(SmallNum)) return T(0);
    const T c = Clamp<T>(Dot(a, b) / (sa * sb), T(-1), T(1));
    return ToDegrees(std::acos(c));
}

template<Vec3 V>
scalar_t<V> AngleBetweenNormals(const V& a, const V& b) noexcept
{
    using T = scalar_t<V>;
    return ToDegrees(std::acos(Clamp<T>(Dot(a, b), T(-1), T(1))));
}

template<Vec3 V>
constexpr V Lerp(const V& a, const V& b, scalar_t<V> t) noexcept
{
    return V{ Lerp(a.X, b.X, t),
              Lerp(a.Y, b.Y, t),
              Lerp(a.Z, b.Z, t) };
}

// Rodrigues — rotate v around axis (need not be unit) by angle in degrees.
template<Vec3 V>
V RotateAngleAxis(const V& v, scalar_t<V> angleDeg, const V& axis) noexcept
{
    using T = scalar_t<V>;
    const T rad = ToRadians(angleDeg);
    const T c = std::cos(rad), s = std::sin(rad);
    const V n = GetNormalized(axis);
    const V cr = Cross(n, v);
    const T d = Dot(n, v);
    const T omc = T(1) - c;
    return V{ v.X * c + cr.X * s + n.X * d * omc,
              v.Y * c + cr.Y * s + n.Y * d * omc,
              v.Z * c + cr.Z * s + n.Z * d * omc };
}

// =========================================================================
// Quaternion
// =========================================================================

template<Quat Q>
constexpr scalar_t<Q> QuatLengthSq(const Q& q) noexcept
{
    return q.X*q.X + q.Y*q.Y + q.Z*q.Z + q.W*q.W;
}

template<Quat Q>
scalar_t<Q> QuatLength(const Q& q) noexcept { return std::sqrt(QuatLengthSq(q)); }

template<Quat Q>
Q QuatNormalize(const Q& q) noexcept
{
    using T = scalar_t<Q>;
    const T ls = QuatLengthSq(q);
    if (ls < T(SmallNum)) return Q{ T(0), T(0), T(0), T(1) };
    const T inv = T(1) / std::sqrt(ls);
    return Q{ q.X * inv, q.Y * inv, q.Z * inv, q.W * inv };
}

// Inverse of a UNIT quaternion (conjugate). For non-unit quats, divide by
// QuatLengthSq afterwards.
template<Quat Q>
constexpr Q QuatInverse(const Q& q) noexcept { return Q{ -q.X, -q.Y, -q.Z, q.W }; }

// Hamilton product: a ⊗ b
template<Quat Q>
constexpr Q QuatMultiply(const Q& a, const Q& b) noexcept
{
    return Q{ a.W*b.X + a.X*b.W + a.Y*b.Z - a.Z*b.Y,
              a.W*b.Y - a.X*b.Z + a.Y*b.W + a.Z*b.X,
              a.W*b.Z + a.X*b.Y - a.Y*b.X + a.Z*b.W,
              a.W*b.W - a.X*b.X - a.Y*b.Y - a.Z*b.Z };
}

// Rotate a 3-vector by a quaternion using the Rodrigues optimization:
//   v' = v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v)
// Scalar type is deduced from V (not Q) so the returned vector keeps
// its caller's precision even if Q uses a different one.
template<Quat Q, Vec3 V>
V QuatRotateVector(const Q& q, const V& v) noexcept
{
    using T = scalar_t<V>;
    const V qv{ static_cast<T>(q.X), static_cast<T>(q.Y), static_cast<T>(q.Z) };
    const V inner{ Cross(qv, v).X + static_cast<T>(q.W) * v.X,
                   Cross(qv, v).Y + static_cast<T>(q.W) * v.Y,
                   Cross(qv, v).Z + static_cast<T>(q.W) * v.Z };
    const V outer = Cross(qv, inner);
    return V{ v.X + T(2) * outer.X,
              v.Y + T(2) * outer.Y,
              v.Z + T(2) * outer.Z };
}

template<Quat Q>
Q QuatSlerp(const Q& a, const Q& b, scalar_t<Q> t) noexcept
{
    using T = scalar_t<Q>;
    T d = a.X*b.X + a.Y*b.Y + a.Z*b.Z + a.W*b.W;
    Q B = b;
    if (d < T(0)) { B = Q{ -b.X, -b.Y, -b.Z, -b.W }; d = -d; }
    // Near-collinear: fall back to nlerp to avoid div by sin(0).
    if (d > T(1) - T(KindaSmall))
        return QuatNormalize(Q{ Lerp<T>(a.X, B.X, t),
                                Lerp<T>(a.Y, B.Y, t),
                                Lerp<T>(a.Z, B.Z, t),
                                Lerp<T>(a.W, B.W, t) });
    const T theta = std::acos(d);
    const T sinT  = std::sin(theta);
    const T wa    = std::sin((T(1) - t) * theta) / sinT;
    const T wb    = std::sin(t * theta) / sinT;
    return Q{ a.X * wa + B.X * wb,
              a.Y * wa + B.Y * wb,
              a.Z * wa + B.Z * wb,
              a.W * wa + B.W * wb };
}

template<Quat Q, Vec3 V>
Q QuatFromAxisAngle(const V& axis, scalar_t<V> deg) noexcept
{
    using T = scalar_t<V>;
    const T half = ToRadians(deg) * T(0.5);
    const T s    = std::sin(half);
    const V n    = GetNormalized(axis);
    return Q{ n.X * s, n.Y * s, n.Z * s, std::cos(half) };
}

// =========================================================================
// Rotator <-> Vector / Quat
// =========================================================================

// Unreal pitch/yaw convention: X = forward, Y = right, Z = up.
template<Vec3 V, Rotator R>
V RotatorToVector(const R& r) noexcept
{
    using T = scalar_t<V>;
    const T cp = static_cast<T>(std::cos(ToRadians<rotator_scalar_t<R>>(r.Pitch)));
    const T sp = static_cast<T>(std::sin(ToRadians<rotator_scalar_t<R>>(r.Pitch)));
    const T cy = static_cast<T>(std::cos(ToRadians<rotator_scalar_t<R>>(r.Yaw)));
    const T sy = static_cast<T>(std::sin(ToRadians<rotator_scalar_t<R>>(r.Yaw)));
    return V{ cp * cy, cp * sy, sp };
}

template<Rotator R, Vec3 V>
R VectorToRotator(const V& v) noexcept
{
    using TR = rotator_scalar_t<R>;
    using TV = scalar_t<V>;
    const TV sz = Size(v);
    const TR yaw   = static_cast<TR>(ToDegrees<TV>(std::atan2(v.Y, v.X)));
    const TR pitch = sz > TV(SmallNum)
                     ? static_cast<TR>(ToDegrees<TV>(std::asin(v.Z / sz)))
                     : TR(0);
    return R{ pitch, yaw, TR(0) };
}

// Unreal rotator → quaternion (ZYX Tait–Bryan, half-angle form)
template<Quat Q, Rotator R>
Q RotatorToQuat(const R& r) noexcept
{
    using T  = scalar_t<Q>;
    using TR = rotator_scalar_t<R>;
    const T halfP = static_cast<T>(ToRadians<TR>(r.Pitch) * TR(0.5));
    const T halfY = static_cast<T>(ToRadians<TR>(r.Yaw)   * TR(0.5));
    const T halfR = static_cast<T>(ToRadians<TR>(r.Roll)  * TR(0.5));
    const T cp = std::cos(halfP), sp = std::sin(halfP);
    const T cy = std::cos(halfY), sy = std::sin(halfY);
    const T cr = std::cos(halfR), sr = std::sin(halfR);
    return Q{ cr * sp * cy - sr * cp * sy,
              cr * cp * sy + sr * sp * cy,
              sr * cp * cy - cr * sp * sy,
              cr * cp * cy + sr * sp * sy };
}

// Rotator helpers ─────────────────────────────────────────────────────────
//
// ClampAxis: normalize an angle in degrees to [0, 360).
// NormalizeAxis: normalize an angle in degrees to (-180, 180].

template<typename T>
T ClampAxis(T angle) noexcept
{
    angle = std::fmod(angle, T(360));
    if (angle < T(0)) angle += T(360);
    return angle;
}

template<typename T>
T NormalizeAxis(T angle) noexcept
{
    angle = ClampAxis(angle);
    if (angle > T(180)) angle -= T(360);
    return angle;
}

// =========================================================================
// LinearColor
// =========================================================================

template<LinearColor C, typename T = float>
constexpr C LerpColor(const C& a, const C& b, T t) noexcept
{
    return C{ Lerp<T>(static_cast<T>(a.R), static_cast<T>(b.R), t),
              Lerp<T>(static_cast<T>(a.G), static_cast<T>(b.G), t),
              Lerp<T>(static_cast<T>(a.B), static_cast<T>(b.B), t),
              Lerp<T>(static_cast<T>(a.A), static_cast<T>(b.A), t) };
}

} // namespace Math

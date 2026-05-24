#pragma once
// Lib_Math.h — Enhanced math types + Math:: utility namespace.

#include <cmath>
#include <algorithm>
#include "SDK/SDK/CoreUObject_structs.hpp"

struct FVector : SDK::FVector
{
    using SDK::FVector::FVector;

    constexpr FVector(const SDK::FVector& v) noexcept : SDK::FVector(v.X, v.Y, v.Z) {}
    constexpr FVector(SDK::FVector&& v)      noexcept : SDK::FVector(v.X, v.Y, v.Z) {}

    constexpr FVector Cross(const FVector& o) const noexcept
    {
        return { Y*o.Z - Z*o.Y,  Z*o.X - X*o.Z,  X*o.Y - Y*o.X };
    }

    constexpr float SizeSquared() const noexcept { return X*X + Y*Y + Z*Z; }

    FVector Reflect    (const FVector& n)    const noexcept;
    FVector ProjectOnTo(const FVector& onto) const noexcept;
};

static_assert(sizeof(::FVector) == sizeof(SDK::FVector),
    "FVector wrapper size mismatch — do not add data members");

struct FRotator : SDK::FRotator
{
    using SDK::FRotator::FRotator;

    constexpr FRotator(const SDK::FRotator& r) noexcept : SDK::FRotator(r.Pitch, r.Yaw, r.Roll) {}
    constexpr FRotator(SDK::FRotator&& r)      noexcept : SDK::FRotator(r.Pitch, r.Yaw, r.Roll) {}
};

static_assert(sizeof(::FRotator) == sizeof(SDK::FRotator),
    "FRotator wrapper size mismatch — do not add data members");

struct FLinearColor : SDK::FLinearColor
{
    FLinearColor()                                          noexcept { R = G = B = 0.f; A = 1.f; }
    FLinearColor(float r, float g, float b, float a = 1.f) noexcept { R = r; G = g; B = b; A = a; }
    FLinearColor(const SDK::FLinearColor& c)                noexcept { R = c.R; G = c.G; B = c.B; A = c.A; }
};

static_assert(sizeof(::FLinearColor) == sizeof(SDK::FLinearColor),
    "FLinearColor wrapper size mismatch — do not add data members");

#pragma warning(push)
#pragma warning(disable: 4201)

struct FVector2D
{
    union {
        SDK::FVector2D sdk;
        struct { float X, Y; };
    };

    FVector2D()                        noexcept { X = Y = 0.f; }
    FVector2D(float x, float y)        noexcept { X = x; Y = y; }
    FVector2D(const SDK::FVector2D& s) noexcept { X = s.X; Y = s.Y; }

    operator SDK::FVector2D&()             noexcept { return sdk; }
    operator const SDK::FVector2D&() const noexcept { return sdk; }

    FVector2D  operator+ (const FVector2D& o) const noexcept { return sdk + o.sdk; }
    FVector2D  operator- (const FVector2D& o) const noexcept { return sdk - o.sdk; }
    FVector2D  operator* (float s)            const noexcept { return sdk * s; }
    FVector2D  operator/ (float s)            const noexcept { return sdk / s; }
    FVector2D  operator* (const FVector2D& o) const noexcept { return sdk * o.sdk; }
    FVector2D& operator+=(const FVector2D& o)       noexcept { sdk += o.sdk; return *this; }
    FVector2D& operator-=(const FVector2D& o)       noexcept { sdk -= o.sdk; return *this; }
    FVector2D& operator*=(float s)                  noexcept { sdk *= s;     return *this; }
    FVector2D& operator/=(float s)                  noexcept { sdk /= s;     return *this; }
    bool       operator==(const FVector2D& o) const noexcept { return X == o.X && Y == o.Y; }
    bool       operator!=(const FVector2D& o) const noexcept { return X != o.X || Y != o.Y; }

    float     Dot          (const FVector2D& o) const noexcept { return sdk.Dot(o.sdk); }
    float     Magnitude    ()                   const noexcept { return sdk.Magnitude(); }
    float     SizeSquared  ()                   const noexcept { return X*X + Y*Y; }
    FVector2D GetNormalized()                   const noexcept { return sdk.GetNormalized(); }
    bool      IsZero       ()                   const noexcept { return sdk.IsZero(); }
};

static_assert(sizeof(::FVector2D) == sizeof(SDK::FVector2D) && alignof(::FVector2D) == alignof(SDK::FVector2D),
    "FVector2D wrapper layout mismatch");

struct alignas(16) FQuat
{
    union {
        SDK::FQuat sdk;
        struct { float X, Y, Z, W; };
    };

    FQuat()                                   noexcept { X = Y = Z = 0.f; W = 1.f; }
    FQuat(float x, float y, float z, float w) noexcept { X = x; Y = y; Z = z; W = w; }
    FQuat(const SDK::FQuat& q)                noexcept { X = q.X; Y = q.Y; Z = q.Z; W = q.W; }

    operator SDK::FQuat&()             noexcept { return sdk; }
    operator const SDK::FQuat&() const noexcept { return sdk; }

    // Hamilton product
    FQuat operator*(const FQuat& b) const noexcept
    {
        return { W*b.X + X*b.W + Y*b.Z - Z*b.Y,
                 W*b.Y - X*b.Z + Y*b.W + Z*b.X,
                 W*b.Z + X*b.Y - Y*b.X + Z*b.W,
                 W*b.W - X*b.X - Y*b.Y - Z*b.Z };
    }

    bool  operator==(const FQuat& o) const noexcept { return X==o.X && Y==o.Y && Z==o.Z && W==o.W; }
    bool  operator!=(const FQuat& o) const noexcept { return !(*this == o); }

    FQuat Inverse()  const noexcept { return { -X, -Y, -Z, W }; } // unit quat only
    float LengthSq() const noexcept { return X*X + Y*Y + Z*Z + W*W; }
    float Length()   const noexcept { return std::sqrt(LengthSq()); }
};

static_assert(sizeof(::FQuat) == sizeof(SDK::FQuat) && alignof(::FQuat) == alignof(SDK::FQuat),
    "FQuat wrapper layout mismatch");

struct alignas(16) FVector4
{
    union {
        SDK::FVector4 sdk;
        struct { float X, Y, Z, W; };
    };

    FVector4()                                   noexcept { X = Y = Z = W = 0.f; }
    FVector4(float x, float y, float z, float w) noexcept { X = x; Y = y; Z = z; W = w; }
    FVector4(const ::FVector& v, float w = 0.f)    noexcept { X=v.X; Y=v.Y; Z=v.Z; W=w; }
    FVector4(const SDK::FVector4& v)             noexcept { X=v.X; Y=v.Y; Z=v.Z; W=v.W; }

    operator SDK::FVector4&()             noexcept { return sdk; }
    operator const SDK::FVector4&() const noexcept { return sdk; }

    FVector4 operator+ (const FVector4& o) const noexcept { return {X+o.X, Y+o.Y, Z+o.Z, W+o.W}; }
    FVector4 operator- (const FVector4& o) const noexcept { return {X-o.X, Y-o.Y, Z-o.Z, W-o.W}; }
    FVector4 operator* (float s)           const noexcept { return {X*s,   Y*s,   Z*s,   W*s  }; }
    FVector4 operator- ()                  const noexcept { return {-X,    -Y,    -Z,    -W   }; }
    bool     operator==(const FVector4& o) const noexcept { return X==o.X&&Y==o.Y&&Z==o.Z&&W==o.W; }

    ::FVector XYZ() const noexcept { return { X, Y, Z }; }
};

static_assert(sizeof(::FVector4) == sizeof(SDK::FVector4) && alignof(::FVector4) == alignof(SDK::FVector4),
    "FVector4 wrapper layout mismatch");

struct alignas(16) FPlane
{
    union {
        SDK::FPlane sdk;
        struct { float X, Y, Z, W; };
    };

    FPlane()                                   noexcept { X = Y = Z = W = 0.f; }
    FPlane(float x, float y, float z, float w) noexcept { X = x; Y = y; Z = z; W = w; }
    FPlane(const ::FVector& n, float d)          noexcept { X=n.X; Y=n.Y; Z=n.Z; W=d; }
    FPlane(const SDK::FPlane& p)               noexcept { X=p.X; Y=p.Y; Z=p.Z; W=p.W; }

    operator SDK::FPlane&()             noexcept { return sdk; }
    operator const SDK::FPlane&() const noexcept { return sdk; }

    ::FVector Normal()               const noexcept { return { X, Y, Z }; }
    float   PlaneDot(const SDK::FVector& v) const noexcept { return X*v.X + Y*v.Y + Z*v.Z - W; }
};

static_assert(sizeof(::FPlane) == sizeof(SDK::FPlane) && alignof(::FPlane) == alignof(SDK::FPlane),
    "FPlane wrapper layout mismatch");

struct FColor
{
    union {
        SDK::FColor sdk;
        struct { unsigned char B, G, R, A; };
    };

    FColor()                                           noexcept { B = G = R = 0; A = 255; }
    FColor(unsigned char r, unsigned char g,
           unsigned char b, unsigned char a = 255)     noexcept { R=r; G=g; B=b; A=a; }
    FColor(const SDK::FColor& c)                       noexcept { B=c.B; G=c.G; R=c.R; A=c.A; }

    operator SDK::FColor&()             noexcept { return sdk; }
    operator const SDK::FColor&() const noexcept { return sdk; }

    bool         operator==(const FColor& o) const noexcept { return B==o.B&&G==o.G&&R==o.R&&A==o.A; }
    bool         operator!=(const FColor& o) const noexcept { return !(*this == o); }

    ::FLinearColor ToLinear() const noexcept { return { R/255.f, G/255.f, B/255.f, A/255.f }; }
    unsigned int ToARGB()   const noexcept { return (unsigned int)((A<<24)|(R<<16)|(G<<8)|B); }
};

static_assert(sizeof(::FColor) == sizeof(SDK::FColor) && alignof(::FColor) == alignof(SDK::FColor),
    "FColor wrapper layout mismatch");

struct FIntPoint
{
    union {
        SDK::FIntPoint sdk;
        struct { int X, Y; };
    };

    FIntPoint()                        noexcept { X = Y = 0; }
    FIntPoint(int x, int y)            noexcept { X = x; Y = y; }
    FIntPoint(const SDK::FIntPoint& p) noexcept { X = p.X; Y = p.Y; }

    operator SDK::FIntPoint&()             noexcept { return sdk; }
    operator const SDK::FIntPoint&() const noexcept { return sdk; }

    FIntPoint operator+ (const FIntPoint& o) const noexcept { return {X+o.X, Y+o.Y}; }
    FIntPoint operator- (const FIntPoint& o) const noexcept { return {X-o.X, Y-o.Y}; }
    FIntPoint operator* (int s)              const noexcept { return {X*s,   Y*s  }; }
    bool      operator==(const FIntPoint& o) const noexcept { return X==o.X && Y==o.Y; }
    bool      operator!=(const FIntPoint& o) const noexcept { return X!=o.X || Y!=o.Y; }
};

static_assert(sizeof(::FIntPoint) == sizeof(SDK::FIntPoint) && alignof(::FIntPoint) == alignof(SDK::FIntPoint),
    "FIntPoint wrapper layout mismatch");

struct FIntVector
{
    union {
        SDK::FIntVector sdk;
        struct { int X, Y, Z; };
    };

    FIntVector()                         noexcept { X = Y = Z = 0; }
    FIntVector(int x, int y, int z)      noexcept { X = x; Y = y; Z = z; }
    FIntVector(const SDK::FIntVector& v) noexcept { X = v.X; Y = v.Y; Z = v.Z; }

    operator SDK::FIntVector&()             noexcept { return sdk; }
    operator const SDK::FIntVector&() const noexcept { return sdk; }

    FIntVector operator+ (const FIntVector& o) const noexcept { return {X+o.X, Y+o.Y, Z+o.Z}; }
    FIntVector operator- (const FIntVector& o) const noexcept { return {X-o.X, Y-o.Y, Z-o.Z}; }
    FIntVector operator* (int s)               const noexcept { return {X*s,   Y*s,   Z*s  }; }
    bool       operator==(const FIntVector& o) const noexcept { return X==o.X&&Y==o.Y&&Z==o.Z; }
    bool       operator!=(const FIntVector& o) const noexcept { return !(*this == o); }
};

static_assert(sizeof(::FIntVector) == sizeof(SDK::FIntVector) && alignof(::FIntVector) == alignof(SDK::FIntVector),
    "FIntVector wrapper layout mismatch");

#pragma warning(pop)

struct FMatrix
{
    SDK::FMatrix sdk{};

    FMatrix() = default;
    explicit FMatrix(const SDK::FMatrix& m) noexcept { sdk = m; }

    operator SDK::FMatrix&()             noexcept { return sdk; }
    operator const SDK::FMatrix&() const noexcept { return sdk; }

    ::FPlane GetXPlane() const noexcept { return sdk.XPlane; }
    ::FPlane GetYPlane() const noexcept { return sdk.YPlane; }
    ::FPlane GetZPlane() const noexcept { return sdk.ZPlane; }
    ::FPlane GetWPlane() const noexcept { return sdk.WPlane; }

    static FMatrix Identity() noexcept
    {
        FMatrix m{};
        m.sdk.XPlane.X = 1.f;
        m.sdk.YPlane.Y = 1.f;
        m.sdk.ZPlane.Z = 1.f;
        m.sdk.WPlane.W = 1.f;
        return m;
    }
};

static_assert(sizeof(::FMatrix) == sizeof(SDK::FMatrix),
    "FMatrix wrapper size mismatch");

struct FTransform
{
    SDK::FTransform sdk{};

    FTransform() = default;
    FTransform(const ::FQuat& rot, const ::FVector& trans,
               const ::FVector& scale = ::FVector(1.f, 1.f, 1.f)) noexcept
    {
        sdk.Rotation    = rot.sdk;
        sdk.Translation = trans;
        sdk.Scale3D     = scale;
    }
    explicit FTransform(const SDK::FTransform& t) noexcept { sdk = t; }

    operator SDK::FTransform&()             noexcept { return sdk; }
    operator const SDK::FTransform&() const noexcept { return sdk; }

    ::FQuat   GetRotation()    const noexcept { return sdk.Rotation; }
    ::FVector GetTranslation() const noexcept { return sdk.Translation; }
    ::FVector GetScale3D()     const noexcept { return sdk.Scale3D; }

    void SetRotation   (const ::FQuat& q)   noexcept { sdk.Rotation    = q.sdk; }
    void SetTranslation(const ::FVector& v) noexcept { sdk.Translation = v; }
    void SetScale3D    (const ::FVector& v) noexcept { sdk.Scale3D     = v; }

    static FTransform Identity() noexcept
    {
        FTransform t{};
        t.sdk.Rotation.W                          = 1.f;
        t.sdk.Scale3D.X = t.sdk.Scale3D.Y = t.sdk.Scale3D.Z = 1.f;
        return t;
    }
};

static_assert(sizeof(::FTransform) == sizeof(SDK::FTransform),
    "FTransform wrapper size mismatch");

struct FBox
{
    SDK::FBox sdk{};

    FBox() = default;
    FBox(const ::FVector& min, const ::FVector& max) noexcept
    {
        sdk.Min = min; sdk.Max = max; sdk.IsValid = 1;
    }
    explicit FBox(const SDK::FBox& b) noexcept { sdk = b; }

    operator SDK::FBox&()             noexcept { return sdk; }
    operator const SDK::FBox&() const noexcept { return sdk; }

    ::FVector GetMin()    const noexcept { return sdk.Min; }
    ::FVector GetMax()    const noexcept { return sdk.Max; }
    bool    IsValid()   const noexcept { return sdk.IsValid != 0; }
    ::FVector GetCenter() const noexcept
    {
        return { (sdk.Min.X + sdk.Max.X) * 0.5f,
                 (sdk.Min.Y + sdk.Max.Y) * 0.5f,
                 (sdk.Min.Z + sdk.Max.Z) * 0.5f };
    }
    ::FVector GetExtent() const noexcept
    {
        return { (sdk.Max.X - sdk.Min.X) * 0.5f,
                 (sdk.Max.Y - sdk.Min.Y) * 0.5f,
                 (sdk.Max.Z - sdk.Min.Z) * 0.5f };
    }
    bool Contains(const ::FVector& v) const noexcept
    {
        return v.X >= sdk.Min.X && v.X <= sdk.Max.X &&
               v.Y >= sdk.Min.Y && v.Y <= sdk.Max.Y &&
               v.Z >= sdk.Min.Z && v.Z <= sdk.Max.Z;
    }
};

static_assert(sizeof(::FBox) == sizeof(SDK::FBox),
    "FBox wrapper size mismatch");

struct FBox2D
{
    SDK::FBox2D sdk{};

    FBox2D() = default;
    FBox2D(const ::FVector2D& min, const ::FVector2D& max) noexcept
    {
        sdk.Min = min.sdk; sdk.Max = max.sdk; sdk.bIsValid = 1;
    }
    explicit FBox2D(const SDK::FBox2D& b) noexcept { sdk = b; }

    operator SDK::FBox2D&()             noexcept { return sdk; }
    operator const SDK::FBox2D&() const noexcept { return sdk; }

    SDK::FVector2D GetMin()    const noexcept { return sdk.Min; }
    SDK::FVector2D GetMax()    const noexcept { return sdk.Max; }
    bool      IsValid()   const noexcept { return sdk.bIsValid != 0; }
    SDK::FVector2D GetCenter() const noexcept
    {
        return { (sdk.Min.X + sdk.Max.X) * 0.5f,
                 (sdk.Min.Y + sdk.Max.Y) * 0.5f };
    }
    bool Contains(const SDK::FVector2D& v) const noexcept
    {
        return v.X >= sdk.Min.X && v.X <= sdk.Max.X &&
               v.Y >= sdk.Min.Y && v.Y <= sdk.Max.Y;
    }
};

static_assert(sizeof(::FBox2D) == sizeof(SDK::FBox2D),
    "FBox2D wrapper size mismatch");

struct FTwoVectors
{
    SDK::FTwoVectors sdk{};

    FTwoVectors() = default;
    FTwoVectors(const SDK::FVector& v1, const SDK::FVector& v2) noexcept
    {
        sdk.v1 = v1; sdk.v2 = v2;
    }
    explicit FTwoVectors(const SDK::FTwoVectors& t) noexcept { sdk = t; }

    operator SDK::FTwoVectors&()             noexcept { return sdk; }
    operator const SDK::FTwoVectors&() const noexcept { return sdk; }

    SDK::FVector GetV1() const noexcept { return sdk.v1; }
    SDK::FVector GetV2() const noexcept { return sdk.v2; }
    void    SetV1(const SDK::FVector& v) noexcept { sdk.v1 = v; }
    void    SetV2(const SDK::FVector& v) noexcept { sdk.v2 = v; }
};

static_assert(sizeof(::FTwoVectors) == sizeof(SDK::FTwoVectors),
    "FTwoVectors wrapper size mismatch");

// ── FBoxSphereBounds ──────────────────────────────────────────────────────────

struct FBoxSphereBounds
{
    SDK::FBoxSphereBounds sdk{};

    FBoxSphereBounds() = default;
    FBoxSphereBounds(const SDK::FVector& origin, const SDK::FVector& boxExtent, float sphereRadius) noexcept
    {
        sdk.Origin = origin; sdk.BoxExtent = boxExtent; sdk.SphereRadius = sphereRadius;
    }
    explicit FBoxSphereBounds(const SDK::FBoxSphereBounds& b) noexcept { sdk = b; }

    operator SDK::FBoxSphereBounds&()             noexcept { return sdk; }
    operator const SDK::FBoxSphereBounds&() const noexcept { return sdk; }

    SDK::FVector GetOrigin()       const noexcept { return sdk.Origin; }
    SDK::FVector GetBoxExtent()    const noexcept { return sdk.BoxExtent; }
    float   GetSphereRadius() const noexcept { return sdk.SphereRadius; }
    SDK::FBox    GetBox()          const noexcept
    {
        return { { sdk.Origin.X - sdk.BoxExtent.X,
                   sdk.Origin.Y - sdk.BoxExtent.Y,
                   sdk.Origin.Z - sdk.BoxExtent.Z },
                 { sdk.Origin.X + sdk.BoxExtent.X,
                   sdk.Origin.Y + sdk.BoxExtent.Y,
                   sdk.Origin.Z + sdk.BoxExtent.Z } };
    }
};

static_assert(sizeof(::FBoxSphereBounds) == sizeof(SDK::FBoxSphereBounds),
    "FBoxSphereBounds wrapper size mismatch");

// ─────────────────────────────────────────────────────────────────────────────
// Math:: utility namespace
// ─────────────────────────────────────────────────────────────────────────────
//
// Trivial bodies are inline here.  Non-trivial bodies live in Lib_Math.cpp.

namespace Math
{
    // ── Constants ─────────────────────────────────────────────────────────────

    inline constexpr float Pi         = 3.14159265358979323846f;
    inline constexpr float TwoPi      = Pi * 2.f;
    inline constexpr float HalfPi     = Pi * 0.5f;
    inline constexpr float DegToRad   = Pi / 180.f;
    inline constexpr float RadToDeg   = 180.f / Pi;
    inline constexpr float SmallNum   = 1e-8f;
    inline constexpr float KindaSmall = 1e-4f;

    // ── Scalar ────────────────────────────────────────────────────────────────

    inline float ToRadians (float deg)            noexcept { return deg * DegToRad; }
    inline float ToDegrees (float rad)            noexcept { return rad * RadToDeg; }
    inline float Lerp      (float a, float b, float t) noexcept { return a + (b-a)*t; }
    inline float Clamp     (float v, float lo, float hi) noexcept
    {
        return v < lo ? lo : (v > hi ? hi : v);
    }
    inline float Clamp01   (float v)              noexcept { return Clamp(v, 0.f, 1.f); }
    inline float Abs       (float v)              noexcept { return v < 0.f ? -v : v; }
    inline float Sign      (float v)              noexcept { return v < 0.f ? -1.f : (v > 0.f ? 1.f : 0.f); }
    inline float Square    (float v)              noexcept { return v * v; }
    inline float Sqrt      (float v)              noexcept { return std::sqrt(v); }
    inline float FMod      (float a, float b)     noexcept { return std::fmod(a, b); }
    inline float Pow       (float base, float e)  noexcept { return std::pow(base, e); }
    inline float Sin       (float rad)            noexcept { return std::sin(rad); }
    inline float Cos       (float rad)            noexcept { return std::cos(rad); }
    inline float Tan       (float rad)            noexcept { return std::tan(rad); }
    inline float Asin      (float v)              noexcept { return std::asin(Clamp(v,-1.f,1.f)); }
    inline float Acos      (float v)              noexcept { return std::acos(Clamp(v,-1.f,1.f)); }
    inline float Atan2     (float y, float x)     noexcept { return std::atan2(y, x); }

    inline float ClampAxis   (float angle) noexcept { return SDK::FRotator::ClampAxis(angle); }
    inline float NormalizeAxis(float angle) noexcept { return SDK::FRotator::NormalizeAxis(angle); }

    // ── FVector ───────────────────────────────────────────────────────────────

    inline float   Dot        (const FVector& a, const FVector& b) noexcept { return a.Dot(b); }
    inline float   Size       (const FVector& v)                   noexcept { return v.Magnitude(); }
    inline float   SizeSquared(const FVector& v)                   noexcept { return v.SizeSquared(); }
    inline float   Dist       (const FVector& a, const FVector& b) noexcept { return a.GetDistanceTo(b); }
    inline float   DistSq     (const FVector& a, const FVector& b) noexcept
    {
        const FVector d = b - a; return d.SizeSquared();
    }
    inline FVector GetNormalized(const FVector& v) noexcept { return FVector(v.GetNormalized()); }
    inline FVector Lerp(const FVector& a, const FVector& b, float t) noexcept
    {
        return FVector(a) + (FVector(b) - FVector(a)) * t;
    }

    FVector Cross           (FVector a, FVector b)                  noexcept;
    FVector Reflect         (FVector v, FVector n)                  noexcept;
    FVector ProjectOnTo     (FVector v, FVector onto)               noexcept;
    FVector RotateAngleAxis (FVector v, float angleDeg, FVector axis);
    FVector ComponentMin    (FVector a, FVector b)                  noexcept;
    FVector ComponentMax    (FVector a, FVector b)                  noexcept;
    FVector Abs             (FVector v)                             noexcept;
    float   AngleBetween        (FVector a, FVector b);
    float   AngleBetweenNormals (FVector a, FVector b)             noexcept;

    // ── FRotator ──────────────────────────────────────────────────────────────

    inline FRotator ClampRotator    (FRotator r) noexcept { r.Clamp();     return r; }
    inline FRotator NormalizeRotator(FRotator r) noexcept { r.Normalize(); return r; }
    inline FRotator Lerp(FRotator a, FRotator b, float t) noexcept
    {
        return { Lerp(a.Pitch, b.Pitch, t),
                 Lerp(a.Yaw,   b.Yaw,   t),
                 Lerp(a.Roll,  b.Roll,  t) };
    }

    FVector  RotatorToVector(FRotator r) noexcept;
    FRotator VectorToRotator(FVector v)  noexcept;
    FQuat    RotatorToQuat  (FRotator r) noexcept;

    // ── FQuat ─────────────────────────────────────────────────────────────────

    FQuat    QuatNormalize    (FQuat q)                   noexcept;
    inline FQuat QuatInverse  (FQuat q)                   noexcept { return q.Inverse(); }
    FQuat    QuatMultiply     (FQuat a, FQuat b)          noexcept;
    FVector  QuatRotateVector (FQuat q, FVector v)        noexcept;
    FQuat    QuatSlerp        (FQuat a, FQuat b, float t) noexcept;
    FQuat    QuatFromAxisAngle(FVector axis, float deg)   noexcept;
    FRotator QuatToRotator    (FQuat q)                   noexcept;

    // ── FLinearColor ──────────────────────────────────────────────────────────

    inline FLinearColor LerpColor(FLinearColor a, FLinearColor b, float t) noexcept
    {
        return { Lerp(a.R, b.R, t),
                 Lerp(a.G, b.G, t),
                 Lerp(a.B, b.B, t),
                 Lerp(a.A, b.A, t) };
    }

} // namespace Math

inline FVector FVector::Reflect    (const FVector& n)    const noexcept { return Math::Reflect(*this, n); }
inline FVector FVector::ProjectOnTo(const FVector& onto) const noexcept { return Math::ProjectOnTo(*this, onto); }

#include "Lib_Math.h"
// Lib_Math.cpp — Non-trivial implementations for Math:: and FVector members.

// ─────────────────────────────────────────────────────────────────────────────
// FVector member functions
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// Math:: implementations
// ─────────────────────────────────────────────────────────────────────────────

namespace Math
{

// ── FVector ───────────────────────────────────────────────────────────────────

FVector Cross(FVector a, FVector b) noexcept
{
    return { a.Y*b.Z - a.Z*b.Y,
             a.Z*b.X - a.X*b.Z,
             a.X*b.Y - a.Y*b.X };
}

FVector Reflect(FVector v, FVector n) noexcept
{
    // r = v - 2(v·n)n   (n should be unit)
    return FVector(v) - FVector(n) * (2.f * v.Dot(n));
}

FVector ProjectOnTo(FVector v, FVector onto) noexcept
{
    const float sq = onto.Dot(onto);
    if (sq < SmallNum)
        return {};
    return FVector(onto) * (v.Dot(onto) / sq);
}

FVector RotateAngleAxis(FVector v, float angleDeg, FVector axis)
{
    const FQuat q = QuatFromAxisAngle(axis, angleDeg);
    return QuatRotateVector(q, v);
}

FVector ComponentMin(FVector a, FVector b) noexcept
{
    return { a.X < b.X ? a.X : b.X,
             a.Y < b.Y ? a.Y : b.Y,
             a.Z < b.Z ? a.Z : b.Z };
}

FVector ComponentMax(FVector a, FVector b) noexcept
{
    return { a.X > b.X ? a.X : b.X,
             a.Y > b.Y ? a.Y : b.Y,
             a.Z > b.Z ? a.Z : b.Z };
}

FVector Abs(FVector v) noexcept
{
    return { v.X < 0.f ? -v.X : v.X,
             v.Y < 0.f ? -v.Y : v.Y,
             v.Z < 0.f ? -v.Z : v.Z };
}

float AngleBetween(FVector a, FVector b)
{
    const float sa = a.Magnitude();
    const float sb = b.Magnitude();
    if (sa < SmallNum || sb < SmallNum)
        return 0.f;
    const float cosA = Clamp(a.Dot(b) / (sa * sb), -1.f, 1.f);
    return ToDegrees(std::acos(cosA));
}

float AngleBetweenNormals(FVector a, FVector b) noexcept
{
    return ToDegrees(std::acos(Clamp(a.Dot(b), -1.f, 1.f)));
}

// ── FRotator ──────────────────────────────────────────────────────────────────

FVector RotatorToVector(FRotator r) noexcept
{
    const float pitch = r.Pitch * DegToRad;
    const float yaw   = r.Yaw   * DegToRad;
    const float cp = std::cos(pitch);
    return FVector{
        cp * std::cos(yaw),
        cp * std::sin(yaw),
        std::sin(pitch)
    };
}

// Rotator from a direction vector (roll = 0).
// Mirrors UKismetMathLibrary::MakeRotFromX.
FRotator VectorToRotator(FVector v) noexcept
{
    FRotator rot{};
    rot.Roll = 0.f;
    if (v.X == 0.f && v.Y == 0.f)
    {
        rot.Yaw   = 0.f;
        rot.Pitch = v.Z > 0.f ? 90.f : -90.f;
    }
    else
    {
        rot.Yaw   = ToDegrees(std::atan2(v.Y, v.X));
        rot.Pitch = ToDegrees(std::atan2(v.Z, std::sqrt(v.X*v.X + v.Y*v.Y)));
    }
    return rot;
}

// Rotator → Quat using the same half-angle formula as UE4's FRotator::Quaternion().
FQuat RotatorToQuat(FRotator r) noexcept
{
    const float half = 0.5f * DegToRad;
    const float sp = std::sin(r.Pitch * half),  cp = std::cos(r.Pitch * half);
    const float sy = std::sin(r.Yaw   * half),  cy = std::cos(r.Yaw   * half);
    const float sr = std::sin(r.Roll  * half),  cr = std::cos(r.Roll  * half);

    return {  cr*sp*sy - sr*cp*cy,
             -cr*sp*cy - sr*cp*sy,
              cr*cp*sy - sr*sp*cy,
              cr*cp*cy + sr*sp*sy };
}

// ── FQuat ─────────────────────────────────────────────────────────────────────

FQuat QuatNormalize(FQuat q) noexcept
{
    const float len = std::sqrt(q.X*q.X + q.Y*q.Y + q.Z*q.Z + q.W*q.W);
    if (len < SmallNum)
        return {};  // default ctor: identity (0,0,0,1)
    const float inv = 1.f / len;
    return { q.X*inv, q.Y*inv, q.Z*inv, q.W*inv };
}

FQuat QuatMultiply(FQuat a, FQuat b) noexcept
{
    // Hamilton product
    return { a.W*b.X + a.X*b.W + a.Y*b.Z - a.Z*b.Y,
             a.W*b.Y - a.X*b.Z + a.Y*b.W + a.Z*b.X,
             a.W*b.Z + a.X*b.Y - a.Y*b.X + a.Z*b.W,
             a.W*b.W - a.X*b.X - a.Y*b.Y - a.Z*b.Z };
}

// Rotate vector v by unit quaternion q.
// Uses the optimised form: v + 2*q.W*(qv × v) + 2*(qv × (qv × v))
FVector QuatRotateVector(FQuat q, FVector v) noexcept
{
    const FVector qv{ q.X, q.Y, q.Z };
    const FVector uv  = qv.Cross(v);
    const FVector uuv = qv.Cross(uv);
    return v + (uv * (2.f * q.W)) + (uuv * 2.f);
}

// Quaternion spherical linear interpolation.
FQuat QuatSlerp(FQuat a, FQuat b, float t) noexcept
{
    float dot = a.X*b.X + a.Y*b.Y + a.Z*b.Z + a.W*b.W;

    // Ensure shortest path
    if (dot < 0.f)
    {
        b.X = -b.X; b.Y = -b.Y; b.Z = -b.Z; b.W = -b.W;
        dot = -dot;
    }

    // Very close — lerp to avoid numerical instability
    if (dot > 0.9995f)
    {
        return QuatNormalize({ a.X + t*(b.X - a.X),
                               a.Y + t*(b.Y - a.Y),
                               a.Z + t*(b.Z - a.Z),
                               a.W + t*(b.W - a.W) });
    }

    const float theta0 = std::acos(dot);
    const float theta  = theta0 * t;
    const float sinT0  = std::sin(theta0);
    const float s0     = std::cos(theta) - dot * std::sin(theta) / sinT0;
    const float s1     = std::sin(theta) / sinT0;

    return { s0*a.X + s1*b.X,
             s0*a.Y + s1*b.Y,
             s0*a.Z + s1*b.Z,
             s0*a.W + s1*b.W };
}

// Build a quaternion representing `angleDeg` rotation around `axis`.
FQuat QuatFromAxisAngle(FVector axis, float angleDeg) noexcept
{
    const float half = angleDeg * DegToRad * 0.5f;
    const float s = std::sin(half);
    const float c = std::cos(half);
    const FVector an = axis.GetNormalized();
    return { an.X * s, an.Y * s, an.Z * s, c };
}

// Quat → Rotator using the same formula as UE4's FQuat::Rotator().
FRotator QuatToRotator(FQuat q) noexcept
{
    constexpr float SINGULARITY_THRESHOLD = 0.4999995f;

    const float SingularityTest = q.Z*q.X - q.W*q.Y;
    const float YawY = 2.f*(q.W*q.Z + q.X*q.Y);
    const float YawX = 1.f - 2.f*(q.Y*q.Y + q.Z*q.Z);

    FRotator rot{};
    if (SingularityTest < -SINGULARITY_THRESHOLD)
    {
        rot.Pitch = -90.f;
        rot.Yaw   = ToDegrees(std::atan2(YawY, YawX));
        rot.Roll  = NormalizeAxis(-rot.Yaw - 2.f * ToDegrees(std::atan2(q.X, q.W)));
    }
    else if (SingularityTest > SINGULARITY_THRESHOLD)
    {
        rot.Pitch = 90.f;
        rot.Yaw   = ToDegrees(std::atan2(YawY, YawX));
        rot.Roll  = NormalizeAxis(rot.Yaw - 2.f * ToDegrees(std::atan2(q.X, q.W)));
    }
    else
    {
        rot.Pitch = ToDegrees(std::asin(2.f * SingularityTest));
        rot.Yaw   = ToDegrees(std::atan2(YawY, YawX));
        rot.Roll  = ToDegrees(std::atan2(-2.f*(q.W*q.X + q.Y*q.Z),
                                          1.f - 2.f*(q.X*q.X + q.Y*q.Y)));
    }
    return rot;
}

} // namespace Math

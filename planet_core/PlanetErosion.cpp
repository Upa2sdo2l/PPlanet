// PlanetErosion.cpp
//
//   Erosion filter and Phacelle noise copyright (c) 2025 Rune Skovbo Johansen.
//   This Source Code Form is subject to the terms of the Mozilla Public
//   License, v. 2.0. If a copy of the MPL was not distributed with this
//   file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Sphere port; see PlanetErosion.h for what changed.
#include "PlanetErosion.h"
#include <algorithm>
#include <cmath>

namespace PlanetErosion
{

using PlanetCore::Vec3d;

namespace
{
    constexpr double TAU = 6.28318530717959;

    inline double Clamp01(double T) { return std::clamp(T, 0.0, 1.0); }
    inline double Mix(double A, double B, double T) { return A + (B - A) * T; }
    inline double Sign(double V) { return (V > 0.0) ? 1.0 : ((V < 0.0) ? -1.0 : 0.0); }

    inline double PowInv(double T, double Power) { return 1.0 - std::pow(1.0 - Clamp01(T), Power); }
    inline double EaseOut(double T) { const double V = 1.0 - Clamp01(T); return 1.0 - V * V; }
    inline double SmoothStart(double T, double Smoothing)
    {
        if (T >= Smoothing) return T - 0.5 * Smoothing;
        return 0.5 * T * T / Smoothing;
    }
    inline Vec3d SafeNormalize(const Vec3d& N)
    {
        const double L = N.Length();
        return (L > 1e-10) ? N / L : N;
    }

    // pcg3d (Jarzynski & Olano 2020): integer-only, identical on CPU and GPU.
    inline void Pcg3d(uint32_t& X, uint32_t& Y, uint32_t& Z)
    {
        X = X * 1664525u + 1013904223u;
        Y = Y * 1664525u + 1013904223u;
        Z = Z * 1664525u + 1013904223u;
        X += Y * Z; Y += Z * X; Z += X * Y;
        X ^= X >> 16u; Y ^= Y >> 16u; Z ^= Z >> 16u;
        X += Y * Z; Y += Z * X; Z += X * Y;
    }

    // Random cell-point offset in [-1, 1]^3 for an integer lattice cell.
    inline Vec3d Hash3(int64_t CX, int64_t CY, int64_t CZ, uint32_t Seed)
    {
        uint32_t X = (uint32_t)CX ^ Seed, Y = (uint32_t)CY, Z = (uint32_t)CZ;
        Pcg3d(X, Y, Z);
        constexpr double Inv = 1.0 / 4294967295.0;
        return Vec3d(X * Inv * 2.0 - 1.0, Y * Inv * 2.0 - 1.0, Z * Inv * 2.0 - 1.0);
    }

    struct Phacelle { double C = 0.0, S = 0.0; Vec3d Side; };

    // Stripes aligned with NormDir (a unit tangent vector), sampled from the
    // 4x4x4 lattice cells around P (in cell units). Up is the sphere normal:
    // the wave varies along Up x NormDir, i.e. across the slope.
    Phacelle PhacelleNoise3(const Vec3d& P, const Vec3d& Up, const Vec3d& NormDir,
                            double Freq, double Offset, double Normalization, uint32_t Seed)
    {
        const Vec3d SideDir = Vec3d::Cross(Up, NormDir) * (Freq * TAU);
        Offset *= TAU;

        const double FX = std::floor(P.X), FY = std::floor(P.Y), FZ = std::floor(P.Z);
        const Vec3d  Frac(P.X - FX, P.Y - FY, P.Z - FZ);

        double PX = 0.0, PY = 0.0, WeightSum = 0.0;
        for (int i = -1; i <= 2; ++i)
        for (int j = -1; j <= 2; ++j)
        for (int k = -1; k <= 2; ++k)
        {
            const Vec3d Rand = Hash3((int64_t)FX + i, (int64_t)FY + j, (int64_t)FZ + k, Seed) * 0.5;
            const Vec3d V = Vec3d(Frac.X - i - Rand.X, Frac.Y - j - Rand.Y, Frac.Z - k - Rand.Z);

            double W = std::exp(-Vec3d::Dot(V, V) * 2.0);
            W = std::max(0.0, W - 0.01111);
            WeightSum += W;

            const double Wave = Vec3d::Dot(V, SideDir) + Offset;
            PX += std::cos(Wave) * W;
            PY += std::sin(Wave) * W;
        }

        Phacelle Out;
        if (WeightSum <= 0.0) { Out.Side = SideDir; return Out; }
        const double IX = PX / WeightSum, IY = PY / WeightSum;
        const double Magnitude = std::max(1.0 - Normalization, std::sqrt(IX * IX + IY * IY));
        Out.C = IX / Magnitude;
        Out.S = IY / Magnitude;
        Out.Side = SideDir;
        return Out;
    }
}

int OctavesForSpacing(const Params& Prm, double VertexSpacingMetres)
{
    // Octave i has stripes ScaleMetres / 2^i apart; keep those >= 2 spacings.
    if (VertexSpacingMetres <= 0.0) return Prm.Octaves;
    const double Ratio = Prm.ScaleMetres / (2.0 * VertexSpacingMetres);
    if (Ratio < 1.0) return 0;
    return std::clamp((int)std::floor(std::log2(Ratio)) + 1, 0, Prm.Octaves);
}

Result Erode(const Vec3d& P, const Vec3d& Up, const Vec3d& Gradient, double FadeTarget,
             double Mask, const Params& Prm, int MaxOctaves)
{
    Result Out;
    const int Octaves = std::min(MaxOctaves, Prm.Octaves);
    if (Mask <= 0.0 || Octaves <= 0) return Out;

    double Strength = Prm.Strength * Prm.ScaleMetres;
    FadeTarget = std::clamp(FadeTarget, -1.0, 1.0);

    double Height = 0.0;                 // accumulated delta (input height is 0 here)
    Vec3d  Slope  = Gradient;
    double Freq   = 1.0 / (Prm.ScaleMetres * Prm.CellScale);
    const double SlopeLength = std::max(Gradient.Length(), 1e-10);
    double Magnitude = 0.0;
    double RoundingMult = 1.0;

    const double RoundingForInput = Mix(Prm.Rounding[1], Prm.Rounding[0], Clamp01(FadeTarget + 0.5)) * Prm.Rounding[2];
    double CombiMask = EaseOut(SmoothStart(SlopeLength * Prm.Onset[0], RoundingForInput * Prm.Onset[0]));

    double RidgeMapCombiMask  = EaseOut(SlopeLength * Prm.Onset[2]);
    double RidgeMapFadeTarget = FadeTarget;

    Vec3d GullySlope = Gradient + ((Gradient / SlopeLength) * Prm.AssumedSlope[0] - Gradient) * Prm.AssumedSlope[1];

    for (int i = 0; i < Octaves; ++i)
    {
        const Phacelle Ph = PhacelleNoise3(P * Freq, Up, SafeNormalize(GullySlope),
                                           Prm.CellScale, 0.25, Prm.Normalization, Prm.Seed + (uint32_t)i * 0x9E3779B9u);
        // Derivative direction in metres; negated because slopes point down.
        const Vec3d Side = Ph.Side * (-Freq);
        const double Sloping = std::fabs(Ph.S);

        GullySlope = GullySlope + Side * (Sign(Ph.S) * Strength * Prm.GullyWeight);

        const double GullyH = Ph.C;
        const Vec3d  GullyS = Side * Ph.S;

        const double FadedH = Mix(FadeTarget, GullyH * Prm.GullyWeight, CombiMask);
        const Vec3d  FadedS = GullyS * (Prm.GullyWeight * CombiMask);

        Height    += FadedH * Strength;
        Slope      = Slope + FadedS * Strength;
        Magnitude += Strength;

        FadeTarget = FadedH;

        const double RoundingForOctave = Mix(Prm.Rounding[1], Prm.Rounding[0], Clamp01(Ph.C + 0.5)) * RoundingMult;
        const double NewMask = EaseOut(SmoothStart(Sloping * Prm.Onset[1], RoundingForOctave * Prm.Onset[1]));
        CombiMask = PowInv(CombiMask, Prm.Detail) * NewMask;

        RidgeMapFadeTarget = Mix(RidgeMapFadeTarget, GullyH, RidgeMapCombiMask);
        RidgeMapCombiMask *= EaseOut(Sloping * Prm.Onset[3]);

        Strength     *= Prm.Gain;
        Freq         *= Prm.Lacunarity;
        RoundingMult *= Prm.Rounding[3];
    }

    Out.DeltaHeight = (Height + Prm.HeightOffset * Magnitude) * Mask;
    Out.DeltaSlope  = (Slope - Gradient) * Mask;
    Out.RidgeMap    = RidgeMapFadeTarget * (1.0 - RidgeMapCombiMask);
    return Out;
}

} // namespace PlanetErosion

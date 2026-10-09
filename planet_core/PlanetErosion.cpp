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
    constexpr double PHACELLE_CUTOFF_D2 = 2.2499;   // same constant in the shader

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

            // exp(-2 d^2) - 0.01111 <= 0 beyond d^2 = 2.2499: most of the 64
            // cells, so skip them before the exp and the sin/cos.
            const double D2 = Vec3d::Dot(V, V);
            if (D2 >= PHACELLE_CUTOFF_D2) continue;
            const double W = std::max(0.0, std::exp(-D2 * 2.0) - 0.01111);
            WeightSum += W;

            const double Wave = Vec3d::Dot(V, SideDir) + Offset;
            PX += std::cos(Wave) * W;
            PY += std::sin(Wave) * W;
        }

        Phacelle Out;
        if (WeightSum <= 0.0) { Out.Side = SideDir; return Out; }
        const double IX = PX / WeightSum, IY = PY / WeightSum;
        const double Len = std::max(1.0 - Normalization, std::sqrt(IX * IX + IY * IY));
        Out.C = IX / Len;
        Out.S = IY / Len;
        Out.Side = SideDir;
        return Out;
    }
}

bool IsEnabled(const Params& Prm)
{
    return Prm.Strength > 0.0 && Prm.ScaleMetres > 0.0 && Prm.Octaves > 0;
}

double Magnitude(const Params& Prm)
{
    double Sum = 0.0, S = Prm.Strength * Prm.ScaleMetres;
    for (int i = 0; i < Prm.Octaves; ++i) { Sum += S; S *= Prm.Gain; }
    return Sum;
}

void DeltaRange(const Params& Prm, double& OutLo, double& OutHi)
{
    // Each octave adds FadedH * Strength_i with |FadedH| <= 1.
    const double M = IsEnabled(Prm) ? Magnitude(Prm) : 0.0;
    OutLo = (Prm.HeightOffset - 1.0) * M;
    OutHi = (Prm.HeightOffset + 1.0) * M;
}

Result Erode(const Vec3d& Up, double RadiusMetres, const Vec3d& Gradient, double FadeTarget,
             double Mask, const Params& Prm)
{
    Result Out;
    if (Mask <= 0.0 || !IsEnabled(Prm)) return Out;

    const Vec3d P = Up * RadiusMetres;
    double Strength = Prm.Strength * Prm.ScaleMetres;
    FadeTarget = std::clamp(FadeTarget, -1.0, 1.0);

    double Height = 0.0;                 // accumulated delta (input height is 0 here)
    double Freq   = 1.0 / (Prm.ScaleMetres * Prm.CellScale);
    const double SlopeLength = std::max(Gradient.Length(), 1e-10);
    double TotalStrength = 0.0;
    double RoundingMult = 1.0;

    const double RoundingForInput = Mix(Prm.Rounding[1], Prm.Rounding[0], Clamp01(FadeTarget + 0.5)) * Prm.Rounding[2];
    double CombiMask = EaseOut(SmoothStart(SlopeLength * Prm.Onset[0], RoundingForInput * Prm.Onset[0]));

    double RidgeMapCombiMask  = EaseOut(SlopeLength * Prm.Onset[2]);
    double RidgeMapFadeTarget = FadeTarget;

    Vec3d GullySlope = Gradient + ((Gradient / SlopeLength) * Prm.AssumedSlope[0] - Gradient) * Prm.AssumedSlope[1];

    for (int i = 0; i < Prm.Octaves; ++i)
    {
        const Phacelle Ph = PhacelleNoise3(P * Freq, Up, SafeNormalize(GullySlope),
                                           Prm.CellScale, 0.25, Prm.Normalization, Prm.Seed + (uint32_t)i * 0x9E3779B9u);
        // Derivative direction in metres; negated because slopes point down.
        const Vec3d Side = Ph.Side * (-Freq);
        const double Sloping = std::fabs(Ph.S);

        GullySlope = GullySlope + Side * (Sign(Ph.S) * Strength * Prm.GullyWeight);

        const double GullyH = Ph.C;
        const double FadedH = Mix(FadeTarget, GullyH * Prm.GullyWeight, CombiMask);

        Height    += FadedH * Strength;
        TotalStrength += Strength;

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

    Out.DeltaHeight = (Height + Prm.HeightOffset * TotalStrength) * std::clamp(Mask, 0.0, 1.0);
    Out.RidgeMap    = RidgeMapFadeTarget * (1.0 - RidgeMapCombiMask);
    return Out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Gradients
// ─────────────────────────────────────────────────────────────────────────────
namespace
{
    // The four sample directions of the central difference at Dir.
    void GradientSampleDirs(const Vec3d& Dir, double StepRad, Vec3d& T1, Vec3d& T2, Vec3d Out[4])
    {
        PlanetCore::NoiseGraph::TangentBasis(Dir, T1, T2);
        Out[0] = (Dir + T1 * StepRad).Normalised();
        Out[1] = (Dir - T1 * StepRad).Normalised();
        Out[2] = (Dir + T2 * StepRad).Normalised();
        Out[3] = (Dir - T2 * StepRad).Normalised();
    }

    Vec3d GradientFromSamples(const Vec3d& T1, const Vec3d& T2, const double H[4], double StepMetres)
    {
        const double Inv = 1.0 / (2.0 * StepMetres);
        return T1 * ((H[0] - H[1]) * Inv) + T2 * ((H[2] - H[3]) * Inv);
    }
}

Vec3d GradientAt(const PlanetCore::NoiseGraph& Graph, const Vec3d& Dir, const Params& Prm)
{
    const double R    = Graph.GetParams().PlanetRadiusMetres;
    const double Step = Prm.GradientStepMetres();
    Vec3d T1, T2, D[4];
    GradientSampleDirs(Dir, Step / R, T1, T2, D);

    float X[4], Y[4], Z[4];
    for (int k = 0; k < 4; ++k) { X[k] = (float)D[k].X; Y[k] = (float)D[k].Y; Z[k] = (float)D[k].Z; }
    double H[4];
    Graph.EvaluateBaseHeightBatch(X, Y, Z, 4, H);
    return GradientFromSamples(T1, T2, H, Step);
}

double DeltaAt(const PlanetCore::NoiseGraph& Graph, const Vec3d& Dir, const Params& Prm)
{
    if (!IsEnabled(Prm) || !Graph.IsValid()) return 0.0;
    const PlanetCore::Surface S = Graph.SampleSurface(Dir);
    float Mask = 0.f, Fade = 0.f;
    Graph.ErosionMaskFade(S.Continent, S.Mountain, Mask, Fade);
    if (Mask <= 0.f) return 0.0;
    const Vec3d G = GradientAt(Graph, Dir, Prm);
    return Erode(Dir, Graph.GetParams().PlanetRadiusMetres, G, Fade, Mask, Prm).DeltaHeight;
}

void BuildChunkInputs(const PlanetCore::NoiseGraph& Graph, const PlanetCore::FChunkKey& Key,
                      const PlanetCore::HaloGrid& Halo, const Params& Prm, ChunkInputs& Out)
{
    using PlanetCore::Vec3d;
    constexpr int32_t G  = PlanetCore::HaloGrid::Side;            // 67
    constexpr int32_t N  = PlanetCore::PLANET_VERTS_PER_SIDE;     // 65
    constexpr int32_t SubStep = 4;
    // Subgrid nodes at chunk vertex -4, 0, 4, ..., 64, 68 (19 per side): the
    // halo ring is interpolated, not extrapolated, and the nodes of two
    // same-LOD neighbours coincide (B's node 0 is A's node 64), so both
    // chunks interpolate the halo and the first inner ring identically and
    // their border normals agree.
    constexpr int32_t SubMin = -1, SubMax = (N - 1) / SubStep + 1;   // node index range -1 .. 17
    constexpr int32_t SubCount = SubMax - SubMin + 1;                // 19

    for (int32_t i = 0; i < ChunkInputs::Count; ++i)
    {
        Graph.ErosionMaskFade(Halo.Continent[i], Halo.Mountain[i], Out.Mask[i], Out.Fade[i]);
        Out.Gradient[i] = Vec3d();
    }
    if (!IsEnabled(Prm)) return;

    // Direction of chunk vertex (x, y), also outside the halo grid: the same
    // parametrisation a neighbour chunk uses for its own vertex there (UV is
    // dyadic, so both compute bit-identical directions on the same face).
    auto DirOfVertex = [&](int32_t x, int32_t y) -> Vec3d
    {
        if (x >= -1 && x <= N && y >= -1 && y <= N) return Halo.Dir[(x + 1) + (y + 1) * G];
        double U, V;
        PlanetCore::ChunkVertexUV(Key, x + 1, y + 1, U, V);
        return PlanetCore::CubeFaceDirection(Key.Face, U, V);
    };

    static thread_local std::vector<Vec3d>   Points;    // directions of exact points
    static thread_local std::vector<int32_t> Slots;     // halo-grid index, or -1 (node off the grid)
    static thread_local std::vector<float>   X, Y, Z;
    static thread_local std::vector<double>  H;
    static thread_local std::vector<Vec3d>   T1s, T2s;
    Points.clear(); Slots.clear();
    Vec3d NodeGrad[SubCount * SubCount];

    for (int32_t ny = SubMin; ny <= SubMax; ++ny)
        for (int32_t nx = SubMin; nx <= SubMax; ++nx)
        {
            const int32_t x = nx * SubStep, y = ny * SubStep;
            Points.push_back(DirOfVertex(x, y));
            const bool bOnGrid = (x >= -1 && x <= N && y >= -1 && y <= N);
            Slots.push_back(bOnGrid ? (x + 1) + (y + 1) * G : -1);
        }
    for (int32_t y = 0; y < N; ++y)
        for (int32_t x = 0; x < N; ++x)
        {
            const bool bBorder = (x == 0) || (y == 0) || (x == N - 1) || (y == N - 1);
            const bool bNode   = (x % SubStep == 0) && (y % SubStep == 0);
            if (!bBorder || bNode) continue;
            Points.push_back(Halo.Dir[(x + 1) + (y + 1) * G]);
            Slots.push_back((x + 1) + (y + 1) * G);
        }

    const double R    = Graph.GetParams().PlanetRadiusMetres;
    const double Step = Prm.GradientStepMetres();
    const size_t P    = Points.size();
    X.resize(P * 4); Y.resize(P * 4); Z.resize(P * 4); H.resize(P * 4);
    T1s.resize(P); T2s.resize(P);
    for (size_t p = 0; p < P; ++p)
    {
        Vec3d D[4];
        GradientSampleDirs(Points[p], Step / R, T1s[p], T2s[p], D);
        for (int k = 0; k < 4; ++k)
        {
            X[p * 4 + k] = (float)D[k].X; Y[p * 4 + k] = (float)D[k].Y; Z[p * 4 + k] = (float)D[k].Z;
        }
    }
    Graph.EvaluateBaseHeightBatch(X.data(), Y.data(), Z.data(), (int32_t)(P * 4), H.data());

    static thread_local std::vector<uint8_t> IsExact;
    IsExact.assign((size_t)ChunkInputs::Count, 0);
    for (size_t p = 0; p < P; ++p)
    {
        const Vec3d Gr = GradientFromSamples(T1s[p], T2s[p], &H[p * 4], Step);
        if (p < (size_t)(SubCount * SubCount)) NodeGrad[p] = Gr;
        if (Slots[p] >= 0)
        {
            Out.Gradient[Slots[p]] = Gr;
            IsExact[(size_t)Slots[p]] = 1;
        }
    }

    // ── Everything else: bilinear between subgrid nodes, then projected onto
    //    the point's tangent plane.
    for (int32_t gy = 0; gy < G; ++gy)
        for (int32_t gx = 0; gx < G; ++gx)
        {
            const int32_t g = gx + gy * G;
            if (IsExact[(size_t)g]) continue;
            const int32_t x = gx - 1, y = gy - 1;                         // chunk vertex coords
            const int32_t ix = (int32_t)std::floor((double)x / SubStep);  // node index, -1 .. 16
            const int32_t iy = (int32_t)std::floor((double)y / SubStep);
            const double  fx = (double)(x - ix * SubStep) / SubStep;
            const double  fy = (double)(y - iy * SubStep) / SubStep;
            auto Node = [&](int32_t a, int32_t b) -> const Vec3d& { return NodeGrad[(a - SubMin) + (b - SubMin) * SubCount]; };
            const Vec3d A = Node(ix, iy) * (1.0 - fx) + Node(ix + 1, iy) * fx;
            const Vec3d B = Node(ix, iy + 1) * (1.0 - fx) + Node(ix + 1, iy + 1) * fx;
            const Vec3d Gr = A * (1.0 - fy) + B * fy;
            const Vec3d& Dir = Halo.Dir[g];
            Out.Gradient[g] = Gr - Dir * Vec3d::Dot(Gr, Dir);
        }
}

void ErodeChunk(const PlanetCore::HaloGrid& Halo, const ChunkInputs& In, const Params& Prm,
                double RadiusMetres, double* OutDelta65)
{
    constexpr int32_t G = PlanetCore::HaloGrid::Side;
    constexpr int32_t N = PlanetCore::PLANET_VERTS_PER_SIDE;
    for (int32_t y = 0; y < N; ++y)
        for (int32_t x = 0; x < N; ++x)
        {
            const int32_t g = (x + 1) + (y + 1) * G;
            OutDelta65[x + y * N] =
                Erode(Halo.Dir[g], RadiusMetres, In.Gradient[g], In.Fade[g], In.Mask[g], Prm).DeltaHeight;
        }
}

} // namespace PlanetErosion

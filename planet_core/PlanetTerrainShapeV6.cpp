// PlanetTerrainShapeV6.cpp
// Terrain shape V6: NoiseGraph's V6 members (graph, evaluation, height bound).
// The formulas are those of the terrain lab (lab6.cpp, mode 1), kept in double
// where the lab used double, so the harness can compare the two point by point.
#include "PlanetTerrainShapeV6.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace PlanetCore
{

// No namespace-wide using-directive: in a unity build it would leak into the
// next .cpp of the same blob. The functions below open it locally.
namespace
{
    inline double V6SStep(double A, double B, double X)
    {
        const double T = std::clamp((X - A) / (B - A), 0.0, 1.0);
        return T * T * (3.0 - 2.0 * T);
    }

    // Base of the region thresholds for MountainAmount (0.5 = the tuned values).
    inline void V6RegionThresholds(const ShapeV6Params& V, double& T0, double& T1)
    {
        const double Shift = (0.5 - (double)V.MountainAmount) * 1.2;
        T0 = PlanetTerrainShapeV6::V6_REGION_T0 + Shift;
        T1 = PlanetTerrainShapeV6::V6_REGION_T1 + Shift;
    }

    inline int32_t V6Octaves(const ShapeV6Params& V)
    {
        return std::clamp(V.RidgeOctaves, 1, 10);
    }

    inline double V6RidgeWavelength(const ShapeV6Params& V)
    {
        return std::max(1000.0, (double)V.RidgeWavelength);
    }

    inline double V6RegionScale(const ShapeV6Params& V)
    {
        return (V.RegionScale > 0.05f) ? (double)V.RegionScale : 1.0;
    }

    // Lookup tables with linear interpolation (NoiseGraph::V6Lut): one table
    // per function, V6_LUT_N intervals. Measured in the sandbox: heights
    // within 3.6 cm of the exact functions (mean 0.1 mm), mountain chunks
    // ~20 % cheaper. Every chunk uses the same tables, so shared vertices
    // still agree exactly.
    constexpr int32_t V6_LUT_N    = 2048;
    constexpr int32_t V6_LUT_SIZE = V6_LUT_N + 2;
    // POWSHARPX: x^RidgeSharpness on [0, 2] (mountain blocks: R is not clamped at 1).
    enum : int32_t { V6_LUT_POW12 = 0, V6_LUT_POW13, V6_LUT_POWSHARP, V6_LUT_TANH, V6_LUT_POWSHARPX, V6_LUT_COUNT };
    constexpr double V6_TANH_RANGE = 6.0;   // tanh(6) = 0.99999

    inline double V6Lookup(const float* Table, double Lo, double Scale, double X)
    {
        const double T = (X - Lo) * Scale;
        if (!(T > 0.0)) return Table[0];
        if (T >= (double)V6_LUT_N) return Table[V6_LUT_N];
        const int32_t I = (int32_t)T;
        const double F = T - (double)I;
        return Table[I] + (Table[I + 1] - Table[I]) * F;
    }
    // x in [0, 1]
    inline double V6Pow(const float* Table, double X) { return V6Lookup(Table, 0.0, (double)V6_LUT_N, X); }
    inline double V6Tanh(const float* Table, double X)
    {
        return V6Lookup(Table, -V6_TANH_RANGE, (double)V6_LUT_N / (2.0 * V6_TANH_RANGE), X);
    }
    // x in [0, 2]
    inline double V6PowX(const float* Table, double X) { return V6Lookup(Table, 0.0, 0.5 * (double)V6_LUT_N, X); }

    // Mountain blocks.
    constexpr int32_t V6_BLOCK_FBM    = -1;       // plain fBm of the same octaves: the reference distribution
    constexpr int32_t V6_BLOCK_KNOTS  = 1024;     // knots of the block -> fBm map
    constexpr int32_t V6_BLOCK_POINTS = 131072;   // sphere samples the map is measured on
    constexpr double  V6_UBER_EPS     = 0.02;     // finite-difference step, share of the octave's wavelength
    // Sum of the coarser octaves -> 0..1 "height of the massif so far" (fBm
    // units: the 1st..99th percentile of the normalised sum is about +-0.55).
    inline double V6HeightSoFar(double S, double ASum) { return std::clamp((S / ASum + 0.55) / 1.1, 0.0, 1.0); }
}

void NoiseGraph::BuildV6()
{
    using namespace PlanetTerrainShapeV6;
    static_assert(V6_MAX_RIDGE_OCTAVES >= 10, "ridge octaves");
    const ShapeV6Params& V = Params.ShapeV6;
    const double R  = Params.PlanetRadiusMetres;
    const double RS = V6RegionScale(V);
    const double L  = V6RidgeWavelength(V);
    auto Seed = [&](int32_t Layer) { return Params.LayerSeed(Layer); };
    auto Fbm = [&](double ScaleMetres, int32_t SeedValue, int32_t Octaves, float Lacunarity = 2.0f, float Gain = 0.5f) -> FastNoise::SmartNode<>
    {
        auto Src = FastNoise::New<FastNoise::Simplex>();
        Src->SetScale((float)(ScaleMetres / R)); Src->SetSeedOffset(SeedValue);
        auto F = FastNoise::New<FastNoise::FractalFBm>();
        F->SetSource(Src); F->SetOctaveCount(Octaves); F->SetGain(Gain); F->SetLacunarity(Lacunarity);
        return F;
    };

    V6Field[V6_Rough]       = Fbm(150000.0, Seed(Layer_V6Rough), 2);
    V6Field[V6_Swell]       = Fbm(12000.0,  Seed(Layer_V6Swell), 3);
    V6Field[V6_Hills]       = Fbm(3000.0,   Seed(Layer_V6Hills), 4, 2.1f, 0.5f);
    V6Field[V6_Hummock]     = Fbm(400.0,    Seed(Layer_V6Hummock), 3, 2.2f, 0.5f);
    V6Field[V6_RegionF]     = Fbm(300000.0 * RS, Seed(Layer_V6RegionF), 2);
    V6Field[V6_RegionG]     = Fbm(300000.0 * RS * 0.8, Seed(Layer_V6RegionG), 2);
    V6Field[V6_Activity]    = Fbm(1200000.0 * RS, Seed(Layer_V6Activity), 2);
    V6Field[V6_RegionWarpX] = Fbm(500000.0 * RS, Seed(Layer_V6RegionWarpX), 2);
    V6Field[V6_RegionWarpY] = Fbm(500000.0 * RS, Seed(Layer_V6RegionWarpY), 2);
    V6Field[V6_RegionWarpZ] = Fbm(500000.0 * RS, Seed(Layer_V6RegionWarpZ), 2);
    const double RW = 90000.0 * (L / 110000.0);
    V6Field[V6_RidgeWarpX]  = Fbm(RW, Seed(Layer_V6RidgeWarpX), 2);
    V6Field[V6_RidgeWarpY]  = Fbm(RW, Seed(Layer_V6RidgeWarpY), 2);
    V6Field[V6_RidgeWarpZ]  = Fbm(RW, Seed(Layer_V6RidgeWarpZ), 2);
    for (int32_t o = 0; o < V6_MAX_RIDGE_OCTAVES; ++o)
    {
        auto Src = FastNoise::New<FastNoise::Simplex>();
        Src->SetScale((float)(L / std::pow(2.0, o) / R));
        Src->SetSeedOffset(Seed(Layer_V6Ridge) + o * 137);
        V6Ridge[o] = Src;
    }

    // Lookup tables of the per-point math.
    const double Sharp = std::max(0.1, (double)V.RidgeSharpness);
    V6Lut.assign((size_t)(V6_LUT_COUNT * V6_LUT_SIZE), 0.f);
    for (int32_t i = 0; i < V6_LUT_SIZE; ++i)
    {
        const double X01 = std::min(1.0, (double)i / (double)V6_LUT_N);
        const double XT  = -V6_TANH_RANGE + 2.0 * V6_TANH_RANGE * (double)i / (double)V6_LUT_N;
        V6Lut[(size_t)(V6_LUT_POW12    * V6_LUT_SIZE + i)] = (float)std::pow(X01, 1.2);
        V6Lut[(size_t)(V6_LUT_POW13    * V6_LUT_SIZE + i)] = (float)std::pow(X01, 1.3);
        V6Lut[(size_t)(V6_LUT_POWSHARP * V6_LUT_SIZE + i)] = (float)std::pow(X01, Sharp);
        V6Lut[(size_t)(V6_LUT_TANH     * V6_LUT_SIZE + i)] = (float)std::tanh(XT);
        V6Lut[(size_t)(V6_LUT_POWSHARPX * V6_LUT_SIZE + i)] = (float)std::pow(std::min(2.0, 2.0 * (double)i / (double)V6_LUT_N), Sharp);
    }
    // The block map needs the std devs of the warp fields: BuildShape builds it.
    V6BlockMap.clear();
    V6BlockRMax = 1.0;
}

// The octave sum of a mountain block (Hybrid, Uber, or V6_BLOCK_FBM for the
// reference distribution), normalised by the amplitude sum of ALL octaves:
// the base height (Used < octaves) is the same sum without its fine octaves.
// Formulas of the terrain lab (lab7.cpp, V7_BLOCK=hybrid|uber).
void NoiseGraph::V6BlockSum(const float* X, const float* Y, const float* Z, int32_t Count, int32_t Used,
                            int32_t Block, double* S) const
{
    using namespace PlanetTerrainShapeV6;
    const ShapeV6Params& V = Params.ShapeV6;
    const double R = Params.PlanetRadiusMetres;
    const int32_t Oct = V6Octaves(V);
    const double  L   = V6RidgeWavelength(V);
    Used = std::clamp(Used, 0, Oct);
    double ASumAll = 0.0;
    {
        double a = 1.0;
        for (int32_t o = 0; o < Oct; ++o, a *= 0.5) ASumAll += a;
    }
    for (int32_t i = 0; i < Count; ++i) S[i] = 0.0;

    static thread_local std::vector<float> T, PX, PY, PZ, QX, QY, QZ, N1, N2;
    static thread_local std::vector<double> Am, Dx, Dy, A1, A2, A3, B1, B2, B3;
    if ((int32_t)T.size() < Count)
    {
        for (std::vector<float>* A : {&T, &PX, &PY, &PZ, &QX, &QY, &QZ, &N1, &N2}) A->resize((size_t)Count);
        for (std::vector<double>* A : {&Am, &Dx, &Dy, &A1, &A2, &A3, &B1, &B2, &B3}) A->resize((size_t)Count);
    }
    const int32_t HStart = std::max(1, V.HybridStartOctave);
    const double HLo = (double)V.HybridLow, HHi = std::max((double)V.HybridHigh, (double)V.HybridLow + 1e-3);
    // ridged with the mean and spread of the noise itself, so blending does not lift the tops
    auto Ridge = [&](double n) { return ((1.0 - std::fabs(n)) - V6RidgeMean) / V6RidgeSd * V6NoiseSd; };

    if (Block == V6_BLOCK_FBM || Block == (int32_t)EMountainBlock::Hybrid)
    {
        double a = 1.0, asum = 0.0;
        for (int32_t o = 0; o < Used; ++o, a *= 0.5)
        {
            Gen(V6Ridge[o], T.data(), Count, X, Y, Z);
            for (int32_t i = 0; i < Count; ++i)
            {
                const double n = (double)T[i];
                double v = n;
                if (Block != V6_BLOCK_FBM && o >= HStart && asum > 0.0)
                {
                    const double w = V6SStep(HLo, HHi, V6HeightSoFar(S[i], asum));
                    v = n + w * (Ridge(n) - n);
                }
                S[i] += a * v;
            }
            asum += a;
        }
    }
    else if (Block == (int32_t)EMountainBlock::Uber)
    {
        // a mild domain warp: the ranges' warp field with its own amplitude
        const double Aw = std::max(0.0, (double)V.UberWarp) / R;
        if (Aw > 0.0)
        {
            Gen(V6Field[V6_RidgeWarpX], QX.data(), Count, X, Y, Z);
            Gen(V6Field[V6_RidgeWarpY], QY.data(), Count, X, Y, Z);
            Gen(V6Field[V6_RidgeWarpZ], QZ.data(), Count, X, Y, Z);
            for (int32_t i = 0; i < Count; ++i)
            {
                PX[i] = (float)(X[i] + Aw * QX[i] / V6Sd[V6_RidgeWarpX]);
                PY[i] = (float)(Y[i] + Aw * QY[i] / V6Sd[V6_RidgeWarpY]);
                PZ[i] = (float)(Z[i] + Aw * QZ[i] / V6Sd[V6_RidgeWarpZ]);
            }
        }
        else
        {
            std::memcpy(PX.data(), X, sizeof(float) * (size_t)Count);
            std::memcpy(PY.data(), Y, sizeof(float) * (size_t)Count);
            std::memcpy(PZ.data(), Z, sizeof(float) * (size_t)Count);
        }
        // tangent basis per point (a function of the point alone: shared vertices agree)
        for (int32_t i = 0; i < Count; ++i)
        {
            const Vec3d p(PX[i], PY[i], PZ[i]);
            const Vec3d up = std::fabs(p.Z) < 0.9 ? Vec3d(0, 0, 1) : Vec3d(1, 0, 0);
            const Vec3d a = Vec3d::Cross(up, p).Normalised(), b = Vec3d::Cross(p, a);
            A1[i] = a.X; A2[i] = a.Y; A3[i] = a.Z; B1[i] = b.X; B2[i] = b.Y; B3[i] = b.Z;
            Am[i] = 1.0; Dx[i] = 0.0; Dy[i] = 0.0;
        }
        const double Sharp = (double)V.UberSharpness, Slope = std::max(0.0, (double)V.UberSlopeErosion);
        const double Alt = std::clamp((double)V.UberAltitudeErosion, 0.0, 1.0);
        double asum = 0.0;
        for (int32_t o = 0; o < Used; ++o)
        {
            const double h = V6_UBER_EPS * (L / std::pow(2.0, o)) / R;
            Gen(V6Ridge[o], T.data(), Count, PX.data(), PY.data(), PZ.data());
            for (int32_t i = 0; i < Count; ++i)
            {
                QX[i] = (float)(PX[i] + h * A1[i]); QY[i] = (float)(PY[i] + h * A2[i]); QZ[i] = (float)(PZ[i] + h * A3[i]);
            }
            Gen(V6Ridge[o], N1.data(), Count, QX.data(), QY.data(), QZ.data());
            for (int32_t i = 0; i < Count; ++i)
            {
                QX[i] = (float)(PX[i] + h * B1[i]); QY[i] = (float)(PY[i] + h * B2[i]); QZ[i] = (float)(PZ[i] + h * B3[i]);
            }
            Gen(V6Ridge[o], N2.data(), Count, QX.data(), QY.data(), QZ.data());
            for (int32_t i = 0; i < Count; ++i)
            {
                double sh;
                if (V.UberRidgesByHeight) sh = (o >= HStart && asum > 0.0) ? V6SStep(HLo, HHi, V6HeightSoFar(S[i], asum)) : 0.0;
                else                      sh = (o >= 1) ? Sharp : 0.0;
                const double n = (double)T[i];
                const double v = n + sh * (Ridge(n) - n);
                // slope erosion follows the smooth part of the octave: the ridge
                // crease has a derivative jump, which would make the height jump too
                Dx[i] += ((double)N1[i] - n) / V6_UBER_EPS;
                Dy[i] += ((double)N2[i] - n) / V6_UBER_EPS;
                S[i] += Am[i] * v / (1.0 + Slope * (Dx[i] * Dx[i] + Dy[i] * Dy[i]));
            }
            asum += std::pow(0.5, o);
            for (int32_t i = 0; i < Count; ++i)
                Am[i] *= 0.5 * (1.0 - Alt * (1.0 - V6HeightSoFar(S[i], asum)));   // altitude erosion
        }
    }
    for (int32_t i = 0; i < Count; ++i) S[i] /= ASumAll;
}

// Block sum -> R (0 at the valley floors .. ~1 at the tops, more on the
// highest peaks): through the map onto the fBm distribution, then as fBm.
double NoiseGraph::V6BlockR(double Sum) const
{
    const int32_t K = (int32_t)V6BlockMap.size() - 1;
    if (K < 1) return 0.0;
    double u = (Sum - V6BlockLo) / (V6BlockHi - V6BlockLo) * (double)K;
    u = std::clamp(u, -0.1 * (double)K, 1.1 * (double)K);   // beyond the samples: the end slope, 10 % at most
    double m;
    if (u <= 0.0)            m = V6BlockMap[0] + (V6BlockMap[1] - V6BlockMap[0]) * u;
    else if (u >= (double)K) m = V6BlockMap[K] + (V6BlockMap[K] - V6BlockMap[K - 1]) * (u - (double)K);
    else
    {
        const int32_t I = (int32_t)u;
        m = V6BlockMap[I] + (V6BlockMap[I + 1] - V6BlockMap[I]) * (u - (double)I);
    }
    const double x = (m - V6BlockQ01) / (V6BlockQ99 - V6BlockQ01);
    // soft floor, no ceiling: no flat valley floors, no flat summits
    return x < 0.05 ? 0.05 - 0.05 * std::tanh((0.05 - x) / 0.05) : x;
}

void NoiseGraph::BuildV6Blocks()
{
    using namespace PlanetTerrainShapeV6;
    V6BlockMap.clear();
    V6BlockRMax = 1.0;
    const int32_t Block = (int32_t)Params.ShapeV6.Block;
    if (Block == (int32_t)EMountainBlock::Musgrave) return;

    const int32_t NP = V6_BLOCK_POINTS;
    std::vector<float> X(NP), Y(NP), Z(NP);
    for (int32_t i = 0; i < NP; ++i)
    {
        const double z = 1.0 - 2.0 * (i + 0.5) / NP, r = std::sqrt(1.0 - z * z), ph = i * 2.399963229728653;
        X[i] = (float)(r * std::cos(ph)); Y[i] = (float)(r * std::sin(ph)); Z[i] = (float)z;
    }
    // one octave: spread of n, mean and spread of its ridge 1 - |n|
    {
        std::vector<float> T(NP);
        Gen(V6Ridge[0], T.data(), NP, X.data(), Y.data(), Z.data());
        double s1 = 0, s2 = 0, r1 = 0, r2 = 0;
        for (float v : T) { s1 += v; s2 += (double)v * v; const double r = 1.0 - std::fabs((double)v); r1 += r; r2 += r * r; }
        V6NoiseSd   = std::sqrt(std::max(1e-12, s2 / NP - (s1 / NP) * (s1 / NP)));
        V6RidgeMean = r1 / NP;
        V6RidgeSd   = std::sqrt(std::max(1e-12, r2 / NP - V6RidgeMean * V6RidgeMean));
    }
    const int32_t Oct = V6Octaves(Params.ShapeV6);
    std::vector<double> Sf(NP), Sb(NP);
    V6BlockSum(X.data(), Y.data(), Z.data(), NP, Oct, V6_BLOCK_FBM, Sf.data());
    V6BlockSum(X.data(), Y.data(), Z.data(), NP, Oct, Block, Sb.data());
    std::sort(Sf.begin(), Sf.end());
    std::sort(Sb.begin(), Sb.end());
    V6BlockQ01 = Sf[(size_t)(0.01 * NP)];
    V6BlockQ99 = Sf[(size_t)(0.99 * NP)];
    V6BlockLo  = Sb.front();
    V6BlockHi  = std::max(Sb.back(), Sb.front() + 1e-9);
    V6BlockMap.resize((size_t)V6_BLOCK_KNOTS + 1);
    for (int32_t k = 0; k <= V6_BLOCK_KNOTS; ++k)
    {
        const double v = V6BlockLo + (V6BlockHi - V6BlockLo) * (double)k / (double)V6_BLOCK_KNOTS;
        // fractional rank of v among the block's samples -> the fBm sample of that rank
        const size_t u = (size_t)(std::lower_bound(Sb.begin(), Sb.end(), v) - Sb.begin());
        double q;
        if (u == 0) q = 0.0;
        else if (u >= (size_t)NP) q = (double)(NP - 1);
        else q = (double)(u - 1) + (v - Sb[u - 1]) / std::max(1e-12, Sb[u] - Sb[u - 1]);
        const size_t q0 = std::min((size_t)q, (size_t)NP - 2);
        V6BlockMap[(size_t)k] = Sf[q0] + (Sf[q0 + 1] - Sf[q0]) * (q - (double)q0);
    }
    V6BlockRMax = std::max(1.0, V6BlockR(V6BlockHi + 0.1 * (V6BlockHi - V6BlockLo)));
}

double NoiseGraph::MaxHeightEstimateV6() const
{
    using namespace PlanetTerrainShapeV6;
    // Every term of EvaluateTerrainV6 at its largest; std-normalised fields
    // stay under 4.5.
    const ShapeParams& S = Params.Shape;
    const ShapeV6Params& V = Params.ShapeV6;
    const double Base   = S.LowlandHeight + S.InteriorRise * 1.5;
    const double Plains = (8.0 + 35.0) * 4.5 + (4.0 + 90.0) + (1.5 + 5.0) * 4.5;
    const double Detail = std::fabs(S.DetailPlains) + std::fabs(S.DetailMountains);
    // Mountain blocks: R is not clamped at 1 (V6BlockRMax bounds it).
    double Mountains = std::max(0.0, (double)V.MountainMaxHeight);
    if (V.Block != EMountainBlock::Musgrave && V6BlockRMax > 1.0)
    {
        const double Floor = std::clamp((double)V.RidgeFloor, 0.0, 1.0);
        Mountains *= Floor + (1.0 - Floor) * std::pow(std::min(V6BlockRMax, 2.0), std::max(0.1, (double)V.RidgeSharpness));
    }
    return Base + Plains + Detail + std::max(0.0, (double)V.Pedestal) + Mountains;
}

void NoiseGraph::EvaluateTerrainV6(const float* X, const float* Y, const float* Z, int32_t Count, bool bDetail,
                                   double* OutHeight, float* OutC, float* OutM,
                                   float* OutInland, float* OutMask, float* OutFade) const
{
    using namespace PlanetTerrainShapeV6;
    // Called by EvaluateTerrain, which has checked bValid and Count.
    if (V6Lut.size() != (size_t)(V6_LUT_COUNT * V6_LUT_SIZE)) return;   // BuildV6 not run
    const ShapeParams&   SP = Params.Shape;
    const ShapeV6Params& V  = Params.ShapeV6;
    const double R = Params.PlanetRadiusMetres;

    static thread_local std::vector<float> Cn, Rg, Sw, Hl, Hm, Dt, Bx, By, Bz, Bf, Bg, Ac, Rx, Ry, Rz, T, XW, YW, ZW;
    static thread_local std::vector<double> Sum, Wgt, Amp, Wide;
    if ((int32_t)Cn.size() < Count)
    {
        for (std::vector<float>* A : {&Cn, &Rg, &Sw, &Hl, &Hm, &Dt, &Bx, &By, &Bz, &Bf, &Bg, &Ac, &Rx, &Ry, &Rz, &T, &XW, &YW, &ZW})
            A->resize((size_t)Count);
        for (std::vector<double>* A : {&Sum, &Wgt, &Amp, &Wide}) A->resize((size_t)Count);
    }
    auto Field = [&](int32_t F, std::vector<float>& Out, const float* PX, const float* PY, const float* PZ)
    {
        Gen(V6Field[F], Out.data(), Count, PX, PY, PZ);
    };

    // ── Large scale: continent ──────────────────────────────────────────────
    Gen(ContinentWarp, Cn.data(), Count, X, Y, Z);
    if (OutC) std::memcpy(OutC, Cn.data(), sizeof(float) * (size_t)Count);
    bool bAnyLand = false;
    for (int32_t i = 0; i < Count; ++i) bAnyLand |= (Cn[i] - SeaLevel) >= 0.f;

    // Hills: plains and the sea floor.
    Field(V6_Hills, Hl, X, Y, Z);
    const double SdHills = V6Sd[V6_Hills];
    const double ShelfDepth = SP.ShelfDepth, SlopeDepth = (double)SP.AbyssDepth - (double)SP.ShelfDepth;

    auto Sea = [&](int32_t i, double Above)
    {
        const double d = -Above;
        const double Depth = ShelfDepth * V6SStep(0.0, 0.03, d) + SlopeDepth * V6SStep(0.03, 0.16, d);
        if (OutHeight) OutHeight[i] = -Depth + 60.0 * ((double)Hl[i] / SdHills) * V6SStep(0.02, 0.1, d);
        if (OutM)      OutM[i] = 0.f;
        if (OutInland) OutInland[i] = (float)Above;
        if (OutMask)   OutMask[i] = 0.f;
        if (OutFade)   OutFade[i] = 0.f;
    };
    if (!bAnyLand)
    {
        // Batches entirely at sea skip every land field (never changes a result).
        for (int32_t i = 0; i < Count; ++i) Sea(i, (double)(Cn[i] - SeaLevel));
        return;
    }

    // ── Large scale: roughness, mountain regions (warped), activity ──────────
    Field(V6_Rough, Rg, X, Y, Z);
    const double RS = V6RegionScale(V);
    Field(V6_RegionWarpX, Bx, X, Y, Z);
    Field(V6_RegionWarpY, By, X, Y, Z);
    Field(V6_RegionWarpZ, Bz, X, Y, Z);
    {
        const double Ab = V6_REGION_WARP_METRES * RS / R;
        for (int32_t i = 0; i < Count; ++i)
        {
            XW[i] = (float)(X[i] + Ab * Bx[i] / V6Sd[V6_RegionWarpX]);
            YW[i] = (float)(Y[i] + Ab * By[i] / V6Sd[V6_RegionWarpY]);
            ZW[i] = (float)(Z[i] + Ab * Bz[i] / V6Sd[V6_RegionWarpZ]);
        }
    }
    Field(V6_RegionF, Bf, XW.data(), YW.data(), ZW.data());
    Field(V6_RegionG, Bg, XW.data(), YW.data(), ZW.data());
    Field(V6_Activity, Ac, X, Y, Z);

    const float* LutPow12    = &V6Lut[(size_t)(V6_LUT_POW12    * V6_LUT_SIZE)];
    const float* LutPow13    = &V6Lut[(size_t)(V6_LUT_POW13    * V6_LUT_SIZE)];
    const float* LutPowSharp = &V6Lut[(size_t)(V6_LUT_POWSHARP * V6_LUT_SIZE)];
    const float* LutTanh     = &V6Lut[(size_t)(V6_LUT_TANH     * V6_LUT_SIZE)];
    const float* LutPowSharpX = &V6Lut[(size_t)(V6_LUT_POWSHARPX * V6_LUT_SIZE)];

    double T0, T1;
    V6RegionThresholds(V, T0, T1);
    const double Hmax = std::max(0.0, (double)V.MountainMaxHeight);
    const double LS = std::max(1e-3, (double)LandScale);
    bool bAnyMountain = false;
    for (int32_t i = 0; i < Count; ++i)
    {
        Amp[i] = 0.0; Wide[i] = 0.0;
        const double A = Cn[i] - SeaLevel;
        if (A < 0.0) continue;
        const double a = std::clamp(A / LS, 0.0, 1.5);
        const double Inland = V6SStep(0.02, 0.15, a);              // no big mountains right at the surf
        const double f = Bf[i] / V6Sd[V6_RegionF] + V6_ACTIVITY * Ac[i] / V6Sd[V6_Activity];
        const double g = Bg[i] / V6Sd[V6_RegionG];
        const double Reg = V6SStep(T0, T1, f);
        Amp[i]  = Hmax * V6Pow(LutPow12, Reg) * (0.35 + 0.65 * V6SStep(-1.2, 1.2, g)) * Inland;
        Wide[i] = V6SStep(T0 - 0.7, T1, f);
        bAnyMountain |= Amp[i] > 0.0;
    }

    // ── Ranges: ridged multifractal (Musgrave) with a moderate warp ──────────
    // Octaves 0 .. V6_LARGE_RIDGE_OCTAVES-1 are large scale (the future map
    // stores Sum and Wgt after them), the rest small scale. The base height
    // (bDetail = false, the erosion filter's steering) stops at the octaves of
    // V6_BASE_MIN_RIDGE_WAVELENGTH. Batches without mountains skip the ranges:
    // there Amp = 0 makes them irrelevant, so no result changes.
    const int32_t Oct = V6Octaves(V);
    const double  Lw  = V6RidgeWavelength(V);
    int32_t Used = Oct;
    if (!bDetail)
    {
        Used = 1;
        while (Used < Oct && Lw / std::pow(2.0, Used) >= V6_BASE_MIN_RIDGE_WAVELENGTH) ++Used;
    }
    double ASum = 0.0;
    {
        double a = 1.0;
        for (int32_t o = 0; o < Oct; ++o, a *= 0.5) ASum += a;
    }
    for (int32_t i = 0; i < Count; ++i) { Sum[i] = 0.0; Wgt[i] = 1.0; }
    const bool bBlock = V.Block != EMountainBlock::Musgrave && !V6BlockMap.empty();
    if (bAnyMountain && bBlock)
    {
        V6BlockSum(X, Y, Z, Count, Used, (int32_t)V.Block, Sum.data());
    }
    else if (bAnyMountain)
    {
        Field(V6_RidgeWarpX, Rx, X, Y, Z);
        Field(V6_RidgeWarpY, Ry, X, Y, Z);
        Field(V6_RidgeWarpZ, Rz, X, Y, Z);
        const double Ar = std::max(0.0, (double)V.RidgeWarp) / R;
        for (int32_t i = 0; i < Count; ++i)
        {
            XW[i] = (float)(X[i] + Ar * Rx[i] / V6Sd[V6_RidgeWarpX]);
            YW[i] = (float)(Y[i] + Ar * Ry[i] / V6Sd[V6_RidgeWarpY]);
            ZW[i] = (float)(Z[i] + Ar * Rz[i] / V6Sd[V6_RidgeWarpZ]);
        }
        double a = 1.0;
        for (int32_t o = 0; o < Used; ++o, a *= 0.5)
        {
            Gen(V6Ridge[o], T.data(), Count, XW.data(), YW.data(), ZW.data());
            for (int32_t i = 0; i < Count; ++i)
            {
                double r = 1.0 - std::sqrt((double)T[i] * T[i] + 0.0004);
                r *= r;
                Sum[i] += a * r * Wgt[i];
                Wgt[i] = std::clamp(r * 2.0, 0.0, 1.0);
            }
        }
    }

    // ── Small scale: swells, hummocks, detail ────────────────────────────────
    Field(V6_Swell, Sw, X, Y, Z);
    if (bDetail)
    {
        Field(V6_Hummock, Hm, X, Y, Z);
        Gen(DetailFractal, Dt.data(), Count, X, Y, Z);
    }
    else
    {
        std::fill(Hm.begin(), Hm.begin() + Count, 0.f);
        std::fill(Dt.begin(), Dt.begin() + Count, 0.f);
    }

    const double Floor = std::clamp((double)V.RidgeFloor, 0.0, 1.0);
    const double Ped   = (double)V.Pedestal;
    for (int32_t i = 0; i < Count; ++i)
    {
        const double A = Cn[i] - SeaLevel;
        if (A < 0.0) { Sea(i, A); continue; }
        const double a      = std::clamp(A / LS, 0.0, 1.5);
        const double Coast  = V6SStep(0.0, 0.04, a);
        const double Inland = V6SStep(0.02, 0.15, a);
        const double Base   = (double)SP.LowlandHeight + (double)SP.InteriorRise * a;
        const double rf     = V6SStep(-1.0, 1.2, Rg[i] / V6Sd[V6_Rough]);
        double H = Base + (8.0 + 35.0 * rf) * (Sw[i] / V6Sd[V6_Swell])
                 + (4.0 + 90.0 * V6Pow(LutPow13, rf)) * V6Tanh(LutTanh, Hl[i] / SdHills * 0.8) * Coast
                 + (1.5 + 5.0 * rf) * (Hm[i] / V6Sd[V6_Hummock]);
        // No hard clamp at the top: a clamp pins a quarter of the land to 1.0
        // (flat plateaus); normalised by the real range of the octave sum.
        double Rn, Shape;
        if (bBlock) { Rn = V6BlockR(Sum[i]); Shape = V6PowX(LutPowSharpX, Rn); }
        else        { Rn = std::clamp(Sum[i] / (ASum * 0.85), 0.0, 1.0); Shape = V6Pow(LutPowSharp, Rn); }
        const double M  = Amp[i] * (Floor + (1.0 - Floor) * Shape);
        H += Ped * Wide[i] * Inland * Coast;      // piedmont / regional uplift
        H += M * Coast;
        const double MtnMask = V6SStep(100.0, 900.0, M);
        H += (double)Dt[i] * ((double)SP.DetailPlains + (double)SP.DetailMountains * MtnMask);

        if (OutHeight) OutHeight[i] = H;
        if (OutM)      OutM[i] = bBlock ? (float)(Amp[i] > 0.0 ? std::min(Rn, 1.0) : 0.0) : (float)Rn;
        if (OutInland) OutInland[i] = (float)(Base / 12000.0);
        if (OutMask)   OutMask[i] = (float)(MtnMask * std::clamp(H / 300.0, 0.0, 1.0));
        // fade (valleys -1 .. peaks +1) for the erosion filter: blocks take it
        // from the height in the massif, as the filter's author suggests
        if (OutFade)   OutFade[i] = (float)((bBlock ? std::clamp(2.0 * Rn - 1.0, -1.0, 1.0)
                                                    : 0.3 * std::clamp(2.0 * std::sqrt(Rn) - 1.0, -1.0, 1.0)) * (M > 0.0 ? 1.0 : 0.0));
    }
}

} // namespace PlanetCore

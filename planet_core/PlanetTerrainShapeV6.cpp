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
    return Base + Plains + Detail + std::max(0.0, (double)V.Pedestal) + std::max(0.0, (double)V.MountainMaxHeight);
}

void NoiseGraph::EvaluateTerrainV6(const float* X, const float* Y, const float* Z, int32_t Count, bool bDetail,
                                   double* OutHeight, float* OutC, float* OutM,
                                   float* OutInland, float* OutMask, float* OutFade) const
{
    using namespace PlanetTerrainShapeV6;
    // Called by EvaluateTerrain, which has checked bValid and Count.
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
        Amp[i]  = Hmax * std::pow(Reg, 1.2) * (0.35 + 0.65 * V6SStep(-1.2, 1.2, g)) * Inland;
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
    if (bAnyMountain)
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
    const double Sharp = std::max(0.1, (double)V.RidgeSharpness);
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
                 + (4.0 + 90.0 * std::pow(rf, 1.3)) * std::tanh(Hl[i] / SdHills * 0.8) * Coast
                 + (1.5 + 5.0 * rf) * (Hm[i] / V6Sd[V6_Hummock]);
        // No hard clamp at the top: a clamp pins a quarter of the land to 1.0
        // (flat plateaus); normalised by the real range of the octave sum.
        const double Rn = std::clamp(Sum[i] / (ASum * 0.85), 0.0, 1.0);
        const double M  = Amp[i] * (Floor + (1.0 - Floor) * std::pow(Rn, Sharp));
        H += Ped * Wide[i] * Inland * Coast;      // piedmont / regional uplift
        H += M * Coast;
        const double MtnMask = V6SStep(100.0, 900.0, M);
        H += (double)Dt[i] * ((double)SP.DetailPlains + (double)SP.DetailMountains * MtnMask);

        if (OutHeight) OutHeight[i] = H;
        if (OutM)      OutM[i] = (float)Rn;
        if (OutInland) OutInland[i] = (float)(Base / 12000.0);
        if (OutMask)   OutMask[i] = (float)(MtnMask * std::clamp(H / 300.0, 0.0, 1.0));
        if (OutFade)   OutFade[i] = (float)(0.3 * std::clamp(2.0 * std::sqrt(Rn) - 1.0, -1.0, 1.0) * (M > 0.0 ? 1.0 : 0.0));
    }
}

} // namespace PlanetCore

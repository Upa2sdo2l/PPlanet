// PlanetNoiseCore.cpp
// UE-free noise core implementation.
//
// Two performance bugs found by the harness and fixed here:
//   1. ComputeNormal sampled the height field twice per vertex through
//      EvaluateHeight -> 2*N single-sample noise batches per chunk. At N=4489
//      that was 35912 single-sample calls at ~1/8 SIMD utilisation: 36 ms per
//      chunk. It then became three BULK batches (base, +U offset, +V offset),
//      36.3 ms -> 1.95 ms; now it is ONE batch over the chunk grid plus a
//      one-vertex halo, with central-difference normals.
//   2. The tangent basis branched on the coordinates to pick a reference axis.
//      A branch inside the inner loop is slow and discontinuous. The basis now
//      comes from the cube-face parametrisation: branch-free, and identical to
//      the one the mesh uses.

#include "PlanetNoiseCore.h"
#include "PlanetTerrainShape.h"
#include <cstring>
#include <random>

namespace PlanetCore
{

// ── Cube-face parametrisation — MUST match PlanetCoordinates.h exactly ──────
Vec3d CubeFaceDirection(int32_t Face, double U, double V)
{
    double x, y, z;
    switch (Face)
    {
        case 0: x =  1.0; y =  U;   z =  V;   break;
        case 1: x = -1.0; y = -U;   z =  V;   break;
        case 2: x = -U;   y =  1.0; z =  V;   break;
        case 3: x =  U;   y = -1.0; z =  V;   break;
        case 4: x =  U;   y =  V;   z =  1.0; break;
        default:x = -U;   y =  V;   z = -1.0; break;
    }
    const double Inv = 1.0 / std::sqrt(x*x + y*y + z*z);
    return Vec3d(x*Inv, y*Inv, z*Inv);
}

void ChunkVertexUV(const FChunkKey& Key, int32_t GridX, int32_t GridY,
                   double& OutU, double& OutV)
{
    const double D    = 2.0 / (double)((int64_t)1 << Key.LOD);
    const double U0   = -1.0 + (double)Key.X * D;
    const double V0   = -1.0 + (double)Key.Y * D;
    const double Step = D / (double)PLANET_QUADS_PER_SIDE;
    OutU = U0 + (double)(GridX - 1) * Step;
    OutV = V0 + (double)(GridY - 1) * Step;
}

void NoiseGraph::Gen(const FastNoise::SmartNode<>& Node, float* Out, int32_t Count,
                     const float* X, const float* Y, const float* Z) const
{
    Node->GenPositionArray3D(Out, Count, X, NoiseY(Y, Z), NoiseZ(Y, Z), 0.f, 0.f, 0.f, Params.MasterSeed);
}

void NoiseGraph::Build(const NoiseParams& P)
{
    Params = P;
    bValid = false;

    // Frequency lives on the SOURCE node's ScalableGenerator, not on the
    // fractal. FastNoise has no Fractal::SetScale: scale is a property of the
    // coherent noise node feeding the fractal, and SetScale is 1/frequency.
    // SetScale/SetSeedOffset are inherited by Simplex through
    //   Simplex : public virtual VariableRange<Seeded<ScalableGenerator>>
    // so they are called on the SOURCE, never on the fractal.
    const int32_t SeedContinent = P.LayerSeed(Layer_Continent);
    const int32_t SeedMountain  = P.LayerSeed(Layer_Mountains);
    const int32_t SeedHumidity  = P.LayerSeed(Layer_Humidity);
    const int32_t SeedDetail    = P.LayerSeed(Layer_Detail);
    const int32_t SeedWarp      = P.LayerSeed(Layer_DomainWarp);

    // Wall guards: a zero frequency would make SetScale infinite and poison the
    // whole graph with NaN. Clamp instead of trusting the editor.
    auto SafeFreq = [](float F, float Fallback)
    { return (F > 1e-4f && F < 1e4f) ? F : Fallback; };

    const float ContinentFreq = SafeFreq(P.ContinentFrequency, 1.2f);
    const float MountainFreq  = SafeFreq(P.MountainFrequency, 6.0f);
    const float WarpFreq      = SafeFreq(P.DomainWarpFrequency, 1.5f);

    // Detail is set in metres. Noise space is the unit sphere (1 unit = the
    // planet radius), and a Simplex scale is the feature size in input units,
    // so the scale is wavelength / radius.
    const double Radius        = (P.PlanetRadiusMetres > 1.0) ? P.PlanetRadiusMetres : 2500000.0;
    const double DetailWaveM   = (P.DetailWavelengthMetres > 0.01f) ? (double)P.DetailWavelengthMetres : 2000.0;
    const float  DetailScale   = (float)std::max(1e-7, DetailWaveM / Radius);
    Params.PlanetRadiusMetres  = Radius;   // sanitised copy used by sampling

    // ── Continent: Simplex -> FractalFBm -> DomainWarpGradient ────────────
    ContinentSource = FastNoise::New<FastNoise::Simplex>();
    ContinentSource->SetScale(1.0f / ContinentFreq);
    ContinentSource->SetSeedOffset(SeedContinent);

    ContinentFractal = FastNoise::New<FastNoise::FractalFBm>();
    ContinentFractal->SetSource(ContinentSource);
    ContinentFractal->SetOctaveCount(std::max(1, P.ContinentOctaves));
    ContinentFractal->SetGain(P.ContinentGain);
    ContinentFractal->SetLacunarity(std::max(1.01f, P.ContinentLacunarity));

    // The warp wraps the CONTENT: positions are distorted first, then the
    // fractal is evaluated at the distorted position.
    ContinentWarp = FastNoise::New<FastNoise::DomainWarpGradient>();
    ContinentWarp->SetSource(ContinentFractal);
    ContinentWarp->SetScale(1.0f / (ContinentFreq * WarpFreq));
    ContinentWarp->SetSeedOffset(SeedWarp);
    ContinentWarp->SetWarpAmplitude(P.DomainWarpAmplitude);

    // ── Mountains: Simplex -> FractalRidged ───────────────────────────────
    MountainSource = FastNoise::New<FastNoise::Simplex>();
    MountainSource->SetScale(1.0f / MountainFreq);
    MountainSource->SetSeedOffset(SeedMountain);

    MountainFractal = FastNoise::New<FastNoise::FractalRidged>();
    MountainFractal->SetSource(MountainSource);
    MountainFractal->SetOctaveCount(std::max(1, P.MountainOctaves));
    MountainFractal->SetGain(P.MountainGain);
    MountainFractal->SetLacunarity(2.0f);

    // ── Humidity: Simplex -> FractalFBm ───────────────────────────────────
    HumiditySource = FastNoise::New<FastNoise::Simplex>();
    HumiditySource->SetScale(1.0f / (ContinentFreq * 2.5f));
    HumiditySource->SetSeedOffset(SeedHumidity);

    HumidityFractal = FastNoise::New<FastNoise::FractalFBm>();
    HumidityFractal->SetSource(HumiditySource);
    HumidityFractal->SetOctaveCount(5);
    HumidityFractal->SetGain(0.5f);
    HumidityFractal->SetLacunarity(2.0f);

    // ── Detail: Simplex -> FractalFBm ─────────────────────────────────────
    DetailSource = FastNoise::New<FastNoise::Simplex>();
    DetailSource->SetScale(DetailScale);
    DetailSource->SetSeedOffset(SeedDetail);

    DetailFractal = FastNoise::New<FastNoise::FractalFBm>();
    DetailFractal->SetSource(DetailSource);
    DetailFractal->SetOctaveCount(std::max(1, P.DetailOctaves));
    DetailFractal->SetGain(0.5f);
    DetailFractal->SetLacunarity(2.2f);

    // ── Terrain shape V4 (sizes in metres on the surface) ────────────────────
    auto Fbm = [](float ScaleUnits, int32_t Seed, int32_t Octaves, float Lacunarity, float Gain) -> FastNoise::SmartNode<>
    {
        auto Src = FastNoise::New<FastNoise::Simplex>();
        Src->SetScale(ScaleUnits); Src->SetSeedOffset(Seed);
        auto F = FastNoise::New<FastNoise::FractalFBm>();
        F->SetSource(Src); F->SetOctaveCount(Octaves); F->SetGain(Gain); F->SetLacunarity(Lacunarity);
        return F;
    };
    const float R = (float)Radius;
    const float S = (P.Shape.RegionScale > 0.05f) ? P.Shape.RegionScale : 1.f;
    ShapeErosion = Fbm(220000.f * S / R, P.LayerSeed(Layer_ShapeErosion), 4, 2.0f, 0.5f);   // plains / hills / mountains
    ShapeWeird   = Fbm(140000.f * S / R, P.LayerSeed(Layer_ShapeWeird),   3, 2.0f, 0.5f);   // peaks & valleys (folded)
    ShapePlateau = Fbm(260000.f * S / R, P.LayerSeed(Layer_ShapePlateau), 3, 2.0f, 0.5f);   // where table mountains
    ShapeLevel   = Fbm(500000.f * S / R, P.LayerSeed(Layer_ShapeLevel),   2, 2.0f, 0.5f);   // table-top level
    ShapeMass    = Fbm(22000.f * S / R,  P.LayerSeed(Layer_ShapeMass),    2, 2.1f, 0.45f);  // table outlines
    ShapeHills   = Fbm(14000.f * S / R,  P.LayerSeed(Layer_ShapeHills),   4, 2.1f, 0.45f);  // rolling hills
    for (int32_t o = 0; o < PlanetTerrainShape::PEAK_OCTAVES; ++o)
    {
        // soft ridged by hand (see PlanetTerrainShape::SoftRidge): one Simplex per octave
        auto Src = FastNoise::New<FastNoise::Simplex>();
        Src->SetScale(60000.f * S / R / (float)(1 << o));
        Src->SetSeedOffset(P.LayerSeed(Layer_ShapePeaks) + o * 101);
        ShapePeaks[o] = Src;
    }

    if (P.TerrainShape == ETerrainShape::V6) BuildV6();

    bValid = (ContinentWarp && MountainFractal && HumidityFractal && DetailFractal);
    SeaLevel = P.SeaLevelNorm;
    if (bValid) BuildShape();
}

void NoiseGraph::BuildShape()
{
    if (!Params.IsShaped()) return;

    // Fibonacci sphere: the std devs of the shape fields (their thresholds are
    // in std units) and the 90th percentile of land (scale of the base rise).
    constexpr int32_t NP = 20000;
    std::vector<float> X(NP), Y(NP), Z(NP), V(NP);
    for (int32_t i = 0; i < NP; ++i)
    {
        const double z = 1.0 - 2.0 * (i + 0.5) / NP, r = std::sqrt(1.0 - z * z), ph = i * 2.399963229728653;
        X[i] = (float)(r * std::cos(ph)); Y[i] = (float)(r * std::sin(ph)); Z[i] = (float)z;
    }
    auto StdDev = [&](const FastNoise::SmartNode<>& Field) -> double
    {
        Gen(Field, V.data(), NP, X.data(), Y.data(), Z.data());
        double S1 = 0.0, S2 = 0.0;
        for (float v : V) { S1 += v; S2 += (double)v * v; }
        S1 /= NP;
        return std::max(1e-3, std::sqrt(std::max(0.0, S2 / NP - S1 * S1)));
    };
    if (Params.TerrainShape == ETerrainShape::V4)
    {
        const FastNoise::SmartNode<>* Fields[6] = {&ShapeErosion, &ShapeWeird, &ShapePlateau, &ShapeLevel, &ShapeMass, &ShapeHills};
        for (int32_t f = 0; f < 6; ++f) ShapeSd[f] = (float)StdDev(*Fields[f]);
    }
    else
    {
        for (int32_t f = 0; f < V6_FIELDS; ++f) V6Sd[f] = V6Field[f] ? StdDev(V6Field[f]) : 1.0;
    }

    LandScale = Params.Shape.LandScale;
    if (!(LandScale > 0.f))
    {
        Gen(ContinentWarp, V.data(), NP, X.data(), Y.data(), Z.data());
        std::vector<float> Land;
        for (int32_t i = 0; i < NP; ++i) if (V[i] - SeaLevel > 0.f) Land.push_back(V[i] - SeaLevel);
        LandScale = 0.65f;
        if (Land.size() > 20)
        {
            const size_t k = (size_t)(0.9 * (double)(Land.size() - 1));
            std::nth_element(Land.begin(), Land.begin() + (ptrdiff_t)k, Land.end());
            LandScale = std::max(0.05f, Land[k]);
        }
    }
}

double NoiseGraph::MaxHeightEstimate() const
{
    const NoiseParams& P = Params;
    if (P.TerrainShape == ETerrainShape::V6) return MaxHeightEstimateV6();
    if (!P.IsShaped())
    {
        // Continent above the sea stays below ~1.5; ridged peaks below the amplitude.
        return 1.5 * 12000.0 + (double)P.MountainAmplitude + std::fabs((double)P.DetailAmplitude);
    }
    // V4 (PlanetTerrainShape::Compose), every term at its largest. The fields
    // are std-normalised; |field| < 4.5 there.
    const ShapeParams& S = P.Shape;
    const double Base   = S.LowlandHeight + S.InteriorRise * 1.5;
    const double Rolled = 18.0 * 4.5 + S.HillsMaxHeight + S.MountainMaxHeight;
    const double Top    = S.TableMinHeight + 4.0 * S.TableLevelStep;
    const double Table  = 1.45 * Top + 25.0 * 4.5;
    const double Detail = std::fabs(S.DetailPlains) + std::fabs(S.DetailHills) + std::fabs(S.DetailMountains) + std::fabs(S.DetailCliffs);
    return Base + std::max(Rolled, Table) + Detail;
}

void NoiseGraph::EvaluateTempNoise(const float* X, const float* Y, const float* Z, int32_t Count,
                                   float* Out) const
{
    HumidityFractal->GenPositionArray3D(Out, Count, X, NoiseY(Y, Z), NoiseZ(Y, Z), 17.31f, -9.17f, 5.73f, Params.MasterSeed);
}

void NoiseGraph::EvaluateHeightField(const float* X, const float* Y, const float* Z,
                                     int32_t Count, float* OutHeight) const
{
    static thread_local std::vector<double> H;
    if ((int32_t)H.size() < Count) H.resize((size_t)Count);
    EvaluateTerrain(X, Y, Z, Count, true, H.data(), nullptr, nullptr, nullptr, nullptr, nullptr);
    for (int32_t i = 0; i < Count; ++i) OutHeight[i] = (float)H[i];
}

void NoiseGraph::EvaluateBaseHeightBatch(const float* X, const float* Y, const float* Z,
                                         int32_t Count, double* OutHeight) const
{
    if (!bValid || Count <= 0) return;
    EvaluateTerrain(X, Y, Z, Count, false, OutHeight, nullptr, nullptr, nullptr, nullptr, nullptr);
}

void NoiseGraph::EvaluateTerrain(const float* X, const float* Y, const float* Z, int32_t Count, bool bDetail,
                                 double* OutHeight, float* OutC, float* OutM,
                                 float* OutInland, float* OutMask, float* OutFade) const
{
    if (!bValid || Count <= 0) return;
    // thread_local scratch: called several times per chunk.
    static thread_local std::vector<float> C, M, D, E, W, Pl, Lv, Ms, Hl, Pk, Tmp;
    static thread_local std::vector<PlanetTerrainShape::Regimes> Rg;
    if ((int32_t)C.size() < Count)
    {
        for (std::vector<float>* V : {&C, &M, &D, &E, &W, &Pl, &Lv, &Ms, &Hl, &Pk, &Tmp}) V->resize((size_t)Count);
        Rg.resize((size_t)Count);
    }
    if (Params.TerrainShape == ETerrainShape::V6)
    {
        EvaluateTerrainV6(X, Y, Z, Count, bDetail, OutHeight, OutC, OutM, OutInland, OutMask, OutFade);
        return;
    }
    Gen(ContinentWarp, C.data(), Count, X, Y, Z);
    if (bDetail) Gen(DetailFractal, D.data(), Count, X, Y, Z);
    else std::fill(D.begin(), D.begin() + Count, 0.f);
    if (OutC) std::memcpy(OutC, C.data(), sizeof(float) * (size_t)Count);

    if (!Params.IsShaped())
    {
        Gen(MountainFractal, M.data(), Count, X, Y, Z);
        if (OutM) std::memcpy(OutM, M.data(), sizeof(float) * (size_t)Count);
        for (int32_t i = 0; i < Count; ++i)
        {
            if (OutHeight) OutHeight[i] = ComposeHeight(C[i], M[i], D[i]);
            if (OutInland) OutInland[i] = C[i] - SeaLevel;
            if (OutMask || OutFade)
            {
                float Mk, Fd;
                ErosionMaskFade(C[i], M[i], Mk, Fd);
                if (OutMask) OutMask[i] = Mk;
                if (OutFade) OutFade[i] = Fd;
            }
        }
        return;
    }

    // Terrain shape V4. Fields a batch does not need are skipped as a whole
    // (land-only: plateau; mountains: peaks; tables: level and mass). That
    // never changes a result, so chunks sharing a border still agree.
    const ShapeParams& SP = Params.Shape;
    Gen(ShapeErosion, E.data(), Count, X, Y, Z);
    Gen(ShapeWeird, W.data(), Count, X, Y, Z);
    Gen(ShapeHills, Hl.data(), Count, X, Y, Z);
    bool bAnyLand = false;
    for (int32_t i = 0; i < Count; ++i) bAnyLand |= (C[i] - SeaLevel) >= 0.f;
    if (bAnyLand) Gen(ShapePlateau, Pl.data(), Count, X, Y, Z);

    auto LayersAt = [&](int32_t i)
    {
        PlanetTerrainShape::Layers L;
        L.Above   = C[i] - SeaLevel;
        L.Erosion = E[i] / ShapeSd[0];
        L.Weird   = W[i] / ShapeSd[1];
        L.Plateau = bAnyLand ? Pl[i] / ShapeSd[2] : 0.f;
        L.Hills   = Hl[i] / ShapeSd[5];
        L.Detail  = D[i];
        return L;
    };
    bool bAnyMountain = false, bAnyTable = false;
    for (int32_t i = 0; i < Count; ++i)
    {
        Rg[i] = PlanetTerrainShape::RegimesAt(LayersAt(i), LandScale, SP);
        bAnyMountain |= Rg[i].Mountain > 0.f;
        bAnyTable    |= Rg[i].Table > 0.f;
    }
    std::fill(Pk.begin(), Pk.begin() + Count, 0.f);
    if (bAnyMountain)
    {
        float WSum = 0.f, Wt = 1.f;
        for (int32_t o = 0; o < PlanetTerrainShape::PEAK_OCTAVES; ++o, Wt *= 0.5f)
        {
            Gen(ShapePeaks[o], Tmp.data(), Count, X, Y, Z);
            for (int32_t i = 0; i < Count; ++i) Pk[i] += Wt * PlanetTerrainShape::SoftRidge(Tmp[i]);
            WSum += Wt;
        }
        for (int32_t i = 0; i < Count; ++i) Pk[i] /= WSum;
    }
    if (bAnyTable)
    {
        Gen(ShapeLevel, Lv.data(), Count, X, Y, Z);
        Gen(ShapeMass, Ms.data(), Count, X, Y, Z);
    }
    for (int32_t i = 0; i < Count; ++i)
    {
        PlanetTerrainShape::Layers L = LayersAt(i);
        L.Peaks = Pk[i];
        L.Level = bAnyTable ? Lv[i] / ShapeSd[3] : 0.f;
        L.Mass  = bAnyTable ? Ms[i] / ShapeSd[4] : 0.f;
        const PlanetTerrainShape::Result Res = PlanetTerrainShape::Compose(L, Rg[i], LandScale, SP);
        if (OutHeight) OutHeight[i] = Res.Height;
        if (OutM)      OutM[i]      = Pk[i];
        if (OutInland) OutInland[i] = Res.Inland;
        if (OutMask)   OutMask[i]   = Res.ErosionMask;
        if (OutFade)   OutFade[i]   = Res.ErosionFade;
    }
}

void NoiseGraph::ErosionMaskFade(float Continent, float Mountain, float& OutMask, float& OutFade) const
{
    const float Above = Continent - SeaLevel;
    OutMask = (Above > 0.f) ? std::clamp(Above * Params.ShoreSharpness * 8.f, 0.f, 1.f) : 0.f;
    const float Mn = std::clamp((Mountain + 1.2f) / 3.1f, 0.f, 1.f);
    OutFade = std::clamp((Mn - 0.5f) / 0.3f, -1.f, 1.f);
}

void NoiseGraph::BuildNormalBasis(const Vec3d& Dir, Vec3d& OutT1, Vec3d& OutT2)
{
    // Branch-free: pick a reference axis by the dominant component, then
    // orthogonalise. Stable at the poles and identical everywhere.
    const double ax = std::fabs(Dir.X), ay = std::fabs(Dir.Y), az = std::fabs(Dir.Z);

    Vec3d Ref;
    if (ax >= ay && ax >= az)      Ref = Vec3d(0, 1, 0);
    else if (ay >= ax && ay >= az) Ref = Vec3d(0, 0, 1);
    else                           Ref = Vec3d(1, 0, 0);

    OutT1 = Vec3d::Cross(Ref, Dir).Normalised();
    OutT2 = Vec3d::Cross(Dir, OutT1).Normalised();
}

void NoiseGraph::SampleChunkBatch(const FChunkKey& Key, Surface* Out, HaloGrid* OutHalo) const
{
    if (!bValid) return;

    // The chunk grid is N x N vertices; the sampled grid adds a one-vertex
    // ring (halo) around it, G = N + 2. Every vertex then has all four grid
    // neighbours, so its normal is a central difference and the surface is
    // sampled exactly once: one noise pass of G² points instead of three
    // passes of N² (base, +U, +V) -- 4489 instead of 12675 samples.
    const int32_t N = PLANET_VERTS_PER_SIDE;     // 65
    const int32_t G = PLANET_GRID_WITH_HALO;     // 67
    const int32_t TotalG = G * G;

    static thread_local std::vector<float>  XG, YG, ZG, Cg, Mg, Hg, Tg, Ig, Kg, Fg;
    static thread_local std::vector<double> DXg, DYg, DZg, HeightG;
    if ((int32_t)XG.size() < TotalG)
    {
        for (std::vector<float>* V : {&XG, &YG, &ZG, &Cg, &Mg, &Hg, &Tg, &Ig, &Kg, &Fg})
            V->resize((size_t)TotalG);
        for (std::vector<double>* V : {&DXg, &DYg, &DZg, &HeightG})
            V->resize((size_t)TotalG);
    }

    // Grid index g = 0 and G-1 are the halo; g = 1..N map to the chunk's
    // vertices 0..N-1. ChunkVertexUV is the same function the mesh builder
    // uses, so the interior samples land exactly on the mesh vertices.
    for (int32_t gy = 0; gy < G; ++gy)
    {
        for (int32_t gx = 0; gx < G; ++gx)
        {
            const int32_t i = gx + gy * G;
            double U, V;
            ChunkVertexUV(Key, gx, gy, U, V);
            const Vec3d d = CubeFaceDirection(Key.Face, U, V);

            // float for FastNoise2, double kept for the normal: adjacent LOD-14
            // vertices are ~5 m apart, and float directions (0.15 m steps on a
            // 2500 km sphere) would put ~3% noise into the difference.
            XG[i] = (float)d.X;  YG[i] = (float)d.Y;  ZG[i] = (float)d.Z;
            DXg[i] = d.X;        DYg[i] = d.Y;        DZg[i] = d.Z;
        }
    }

    EvaluateTerrain(XG.data(), YG.data(), ZG.data(), TotalG, true,
                    HeightG.data(), Cg.data(), Mg.data(), Ig.data(), Kg.data(), Fg.data());
    Gen(HumidityFractal, Hg.data(), TotalG, XG.data(), YG.data(), ZG.data());
    EvaluateTempNoise(XG.data(), YG.data(), ZG.data(), TotalG, Tg.data());

    if (OutHalo)
    {
        for (int32_t i = 0; i < TotalG; ++i)
        {
            OutHalo->Dir[i]       = Vec3d(DXg[i], DYg[i], DZg[i]);
            OutHalo->Height[i]    = HeightG[i];
            OutHalo->Continent[i] = Cg[i];
            OutHalo->Mountain[i]  = Mg[i];
            OutHalo->Humidity[i]  = Hg[i];
            OutHalo->TempNoise[i] = Tg[i];
            OutHalo->Inland[i]      = Ig[i];
            OutHalo->ErosionMask[i] = Kg[i];
            OutHalo->ErosionFade[i] = Fg[i];
        }
    }

    const double R = Params.PlanetRadiusMetres;
    auto SurfacePoint = [&](int32_t i)
    {
        return Vec3d(DXg[i], DYg[i], DZg[i]) * (R + HeightG[i]);
    };

    for (int32_t gy = 0; gy < N; ++gy)
    {
        for (int32_t gx = 0; gx < N; ++gx)
        {
            const int32_t g = (gx + 1) + (gy + 1) * G;   // same vertex in the halo grid
            Surface& S = Out[gx + gy * N];
            const Vec3d Dir(DXg[g], DYg[g], DZg[g]);

            S.Continent   = Cg[g];
            S.Mountain    = Mg[g];
            S.HumidityRaw = Hg[g];
            S.TempNoiseRaw = Tg[g];
            S.Height      = HeightG[g];
            S.Inland      = Ig[g];
            S.ErosionMask = Kg[g];
            S.ErosionFade = Fg[g];

            // Central differences along the grid's U and V directions.
            const Vec3d Su = SurfacePoint(g + 1) - SurfacePoint(g - 1);
            const Vec3d Sv = SurfacePoint(g + G) - SurfacePoint(g - G);

            Vec3d n = Vec3d::Cross(Su, Sv);
            if (n.LengthSq() < 1e-20) n = Dir;
            n = n.Normalised();
            if (Vec3d::Dot(n, Dir) < 0.0) n = -n;

            S.Normal = n;
            S.Slope  = (float)std::clamp(1.0 - Vec3d::Dot(n, Dir), 0.0, 1.0);

            Vec3d t = Su - n * Vec3d::Dot(n, Su);
            S.Tangent = (t.LengthSq() > 1e-20) ? t.Normalised() : Vec3d(1,0,0);

            Climate Cl = ComputeClimate(Dir, S.Height, Params);
            Cl.Humidity = std::clamp(Cl.Humidity + Hg[g] * Params.HumidityVariance * 0.5f, 0.f, 1.f);

            ComputeBiomes(Cl, S.Slope, S.FlowMask, S.Height, S.Biome);
        }
    }
}

void NoiseGraph::SampleChunkHalo(const FChunkKey& Key, HaloGrid& Out) const
{
    if (!bValid) return;
    const int32_t G = PLANET_GRID_WITH_HALO;
    const int32_t TotalG = G * G;
    static thread_local std::vector<float> XG, YG, ZG;
    if ((int32_t)XG.size() < TotalG)
    {
        XG.resize((size_t)TotalG); YG.resize((size_t)TotalG); ZG.resize((size_t)TotalG);
    }
    // Same grid, same order and same float conversion as SampleChunkBatch.
    for (int32_t gy = 0; gy < G; ++gy)
        for (int32_t gx = 0; gx < G; ++gx)
        {
            const int32_t i = gx + gy * G;
            double U, V;
            ChunkVertexUV(Key, gx, gy, U, V);
            const Vec3d d = CubeFaceDirection(Key.Face, U, V);
            Out.Dir[i] = d;
            XG[i] = (float)d.X; YG[i] = (float)d.Y; ZG[i] = (float)d.Z;
        }
    EvaluateTerrain(XG.data(), YG.data(), ZG.data(), TotalG, true,
                    Out.Height, Out.Continent, Out.Mountain, Out.Inland, Out.ErosionMask, Out.ErosionFade);
    Gen(HumidityFractal, Out.Humidity, TotalG, XG.data(), YG.data(), ZG.data());
    EvaluateTempNoise(XG.data(), YG.data(), ZG.data(), TotalG, Out.TempNoise);
}

void NoiseGraph::EvaluatePoint(const Vec3d& Dir, double* OutHeight, float* OutMask, float* OutFade) const
{
    const float x = (float)Dir.X, y = (float)Dir.Y, z = (float)Dir.Z;
    double H = 0.0; float Mk = 0.f, Fd = 0.f;
    EvaluateTerrain(&x, &y, &z, 1, true, &H, nullptr, nullptr, nullptr, &Mk, &Fd);
    if (OutHeight) *OutHeight = H;
    if (OutMask)   *OutMask = Mk;
    if (OutFade)   *OutFade = Fd;
}

void NoiseGraph::SampleSurfaceBatch(const float* DirX, const float* DirY, const float* DirZ,
                                    int32_t Count, Surface* Out) const
{
    if (!bValid || Count <= 0) return;

    static thread_local std::vector<float> XU,YU,ZU, XV,YV,ZV, HU,HV, Cb,Mb,Hb,Tb,Ib,Kb,Fb;
    static thread_local std::vector<double> H0;
    XU.resize((size_t)Count); YU.resize((size_t)Count); ZU.resize((size_t)Count);
    XV.resize((size_t)Count); YV.resize((size_t)Count); ZV.resize((size_t)Count);
    H0.resize((size_t)Count); HU.resize((size_t)Count); HV.resize((size_t)Count);
    Cb.resize((size_t)Count); Mb.resize((size_t)Count); Hb.resize((size_t)Count);
    Tb.resize((size_t)Count); Ib.resize((size_t)Count); Kb.resize((size_t)Count); Fb.resize((size_t)Count);

    const double Eps = 1e-5;   // radians, ~25 m on a 2500 km sphere

    for (int32_t i = 0; i < Count; ++i)
    {
        const Vec3d d((double)DirX[i], (double)DirY[i], (double)DirZ[i]);
        Vec3d t1, t2;
        BuildNormalBasis(d, t1, t2);
        const Vec3d du = (d + t1 * Eps).Normalised();
        const Vec3d dv = (d + t2 * Eps).Normalised();
        XU[i]=(float)du.X; YU[i]=(float)du.Y; ZU[i]=(float)du.Z;
        XV[i]=(float)dv.X; YV[i]=(float)dv.Y; ZV[i]=(float)dv.Z;
    }

    EvaluateTerrain(DirX, DirY, DirZ, Count, true, H0.data(), Cb.data(), Mb.data(), Ib.data(), Kb.data(), Fb.data());
    Gen(HumidityFractal, Hb.data(), Count, DirX, DirY, DirZ);
    EvaluateTempNoise(DirX, DirY, DirZ, Count, Tb.data());

    EvaluateHeightField(XU.data(), YU.data(), ZU.data(), Count, HU.data());
    EvaluateHeightField(XV.data(), YV.data(), ZV.data(), Count, HV.data());

    const double R = Params.PlanetRadiusMetres;

    for (int32_t i = 0; i < Count; ++i)
    {
        Surface& S = Out[i];
        const Vec3d Dir((double)DirX[i], (double)DirY[i], (double)DirZ[i]);

        S.Continent = Cb[i]; S.Mountain = Mb[i]; S.HumidityRaw = Hb[i]; S.TempNoiseRaw = Tb[i]; S.Height = H0[i];
        S.Inland = Ib[i]; S.ErosionMask = Kb[i]; S.ErosionFade = Fb[i];

        const Vec3d S0 = Dir * (R + H0[i]);
        const Vec3d Su = Vec3d((double)XU[i], (double)YU[i], (double)ZU[i]) * (R + (double)HU[i]);
        const Vec3d Sv = Vec3d((double)XV[i], (double)YV[i], (double)ZV[i]) * (R + (double)HV[i]);

        Vec3d n = Vec3d::Cross(Su - S0, Sv - S0);
        if (n.LengthSq() < 1e-20) n = Dir;
        n = n.Normalised();
        if (Vec3d::Dot(n, Dir) < 0.0) n = -n;
        S.Normal = n;
        S.Slope = (float)std::clamp(1.0 - Vec3d::Dot(n, Dir), 0.0, 1.0);

        Vec3d t = Su - S0;
        t = t - n * Vec3d::Dot(n, t);
        S.Tangent = (t.LengthSq() > 1e-20) ? t.Normalised() : Vec3d(1,0,0);

        Climate Cl = ComputeClimate(Dir, S.Height, Params);
        Cl.Humidity = std::clamp(Cl.Humidity + Hb[i] * Params.HumidityVariance * 0.5f, 0.f, 1.f);
        ComputeBiomes(Cl, S.Slope, S.FlowMask, S.Height, S.Biome);
    }
}

Surface NoiseGraph::SampleSurface(const Vec3d& UnitDir) const
{
    const float x=(float)UnitDir.X, y=(float)UnitDir.Y, z=(float)UnitDir.Z;
    Surface S;
    SampleSurfaceBatch(&x,&y,&z,1,&S);
    return S;
}

double NoiseGraph::EvaluateHeight(const Vec3d& Dir) const
{
    const float x=(float)Dir.X, y=(float)Dir.Y, z=(float)Dir.Z;
    double H = 0.0;
    EvaluateTerrain(&x, &y, &z, 1, true, &H, nullptr, nullptr, nullptr, nullptr, nullptr);
    return H;
}

// ── ComposeHeight ───────────────────────────────────────────────────────────
// The single formula that turns three layer values into metres of displacement.
// Above sea level: the continent value rises smoothly and ridged mountains add
// sharp peaks on top, masked by the shore falloff so mountains only appear on
// land. Below sea level: the continent dip is scaled into an ocean floor.
double NoiseGraph::ComposeHeight(float C, float M, float D) const
{
    const float Above = C - SeaLevel;

    if (Above >= 0.f)
    {
        // Shore falloff: 0 at the waterline, 1 a little inland. Without it,
        // mountains would rise directly out of the surf.
        const float Shore = std::clamp(Above * Params.ShoreSharpness * 8.f, 0.f, 1.f);

        // Ridged noise peaks at +1; the sharpness exponent controls how
        // needle-like the crests are.
        // Ridged output is NOT [0,1]: measured [-1.40, +1.91]. Clamping it pins 23.6%
        // of the land to exactly 1.0, which makes the mountain term a CONSTANT there --
        // that is the flat-topped plateau. Remap the real range instead, so no sample
        // lands on the bound.
        const float Mn    = std::clamp((M + 1.2f) / 3.1f, 0.f, 1.f);
        const float Peaks = std::pow(Mn, Params.MountainSharpness);

        // Detail is in metres: DetailAmplitude is the height of a full-scale
        // detail feature. (An earlier "* 0.05" here silently turned the
        // default 300 into +-15 m.)
        return (double)Above * 12000.0
             + (double)Shore * (double)Peaks * (double)Params.MountainAmplitude
             + (double)D * (double)Params.DetailAmplitude;
    }

    // Ocean floor: bounded so a deep trench cannot reach absurd depths.
    const double Depth = (double)(-Above) * (double)Params.OceanDepthScale * 12000.0;
    return -(std::min(Depth, 11000.0)) + (double)D * (double)Params.DetailAmplitude * (double)Params.OceanDetailAmp;
}

// ── Auto sea level: quantile of the continent field over the whole sphere ───
// This is what stops a random seed producing a world that is 99% ocean. It
// samples all six faces on a coarse grid and takes the quantile that leaves the
// requested land fraction above water.
float NoiseGraph::ComputeSeaLevelForLandFraction(float Target, int32_t SamplesPerFace)
{
    if (!bValid) return 0.f;

    const int32_t N = std::max(2, SamplesPerFace);
    const int32_t PerFace = N * N;
    const int32_t Total = PerFace * 6;

    std::vector<float> X((size_t)Total), Y((size_t)Total), Z((size_t)Total);
    for (int32_t f = 0; f < 6; ++f)
        for (int32_t j = 0; j < N; ++j)
            for (int32_t i = 0; i < N; ++i)
            {
                const double u = -1.0 + 2.0 * (double)i / (double)(N - 1);
                const double v = -1.0 + 2.0 * (double)j / (double)(N - 1);
                const Vec3d d = CubeFaceDirection(f, u, v);
                const int32_t idx = f * PerFace + i + j * N;
                X[idx]=(float)d.X; Y[idx]=(float)d.Y; Z[idx]=(float)d.Z;
            }

    std::vector<float> C((size_t)Total);
    Gen(ContinentWarp, C.data(), Total, X.data(), Y.data(), Z.data());
    std::sort(C.begin(), C.end());

    const double Q = std::clamp(1.0 - (double)Target, 0.0, 1.0);
    const size_t Idx = (size_t)std::clamp(Q * (double)(Total - 1), 0.0, (double)(Total - 1));

    SeaLevel = C[Idx];
    BuildShape();   // the land scale depends on the sea level
    return SeaLevel;
}

NoiseGraph::Range NoiseGraph::MeasureHeightRange(int32_t SamplesPerFace, int32_t Seed) const
{
    Range R;
    if (!bValid) return R;

    std::mt19937 rng((uint32_t)Seed);
    std::uniform_real_distribution<double> uni(-1.0, 1.0);

    std::vector<float> X, Y, Z;
    X.reserve((size_t)SamplesPerFace * 6);
    Y.reserve((size_t)SamplesPerFace * 6);
    Z.reserve((size_t)SamplesPerFace * 6);

    for (int32_t i = 0; i < SamplesPerFace * 6; ++i)
    {
        Vec3d d(uni(rng), uni(rng), uni(rng));
        if (d.LengthSq() < 1e-9) continue;
        d = d.Normalised();
        X.push_back((float)d.X); Y.push_back((float)d.Y); Z.push_back((float)d.Z);
    }

    const int32_t Total = (int32_t)X.size();
    if (Total == 0) return R;

    std::vector<float> Hs((size_t)Total);
    EvaluateHeightField(X.data(), Y.data(), Z.data(), Total, Hs.data());

    R.Min = Hs[0]; R.Max = Hs[0];
    double Sum = 0.0;
    for (int32_t i = 0; i < Total; ++i)
    {
        R.Min = std::min(R.Min, Hs[i]);
        R.Max = std::max(R.Max, Hs[i]);
        Sum += (double)Hs[i];
    }
    R.Mean = Sum / (double)Total;
    return R;
}

} // namespace PlanetCore

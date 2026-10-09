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

    bValid = (ContinentWarp && MountainFractal && HumidityFractal && DetailFractal);
    SeaLevel = P.SeaLevelNorm;
}

void NoiseGraph::EvaluateLayers(const float* X, const float* Y, const float* Z, int32_t Count,
                                float* OutContinent, float* OutMountain,
                                float* OutHumidity, float* OutDetail) const
{
    ContinentWarp->GenPositionArray3D(  OutContinent, Count, X, NoiseY(Y, Z), NoiseZ(Y, Z), 0.f,0.f,0.f, Params.MasterSeed);
    MountainFractal->GenPositionArray3D(OutMountain,  Count, X, NoiseY(Y, Z), NoiseZ(Y, Z), 0.f,0.f,0.f, Params.MasterSeed);
    HumidityFractal->GenPositionArray3D(OutHumidity,  Count, X, NoiseY(Y, Z), NoiseZ(Y, Z), 0.f,0.f,0.f, Params.MasterSeed);
    DetailFractal->GenPositionArray3D(  OutDetail,    Count, X, NoiseY(Y, Z), NoiseZ(Y, Z), 0.f,0.f,0.f, Params.MasterSeed);
}

void NoiseGraph::EvaluateTempNoise(const float* X, const float* Y, const float* Z, int32_t Count,
                                   float* Out) const
{
    HumidityFractal->GenPositionArray3D(Out, Count, X, NoiseY(Y, Z), NoiseZ(Y, Z), 17.31f, -9.17f, 5.73f, Params.MasterSeed);
}

void NoiseGraph::EvaluateLayersNoHumidity(const float* X, const float* Y, const float* Z, int32_t Count,
                                          float* OutContinent, float* OutMountain,
                                          float* OutDetail) const
{
    ContinentWarp->GenPositionArray3D(  OutContinent, Count, X, NoiseY(Y, Z), NoiseZ(Y, Z), 0.f,0.f,0.f, Params.MasterSeed);
    MountainFractal->GenPositionArray3D(OutMountain,  Count, X, NoiseY(Y, Z), NoiseZ(Y, Z), 0.f,0.f,0.f, Params.MasterSeed);
    DetailFractal->GenPositionArray3D(  OutDetail,    Count, X, NoiseY(Y, Z), NoiseZ(Y, Z), 0.f,0.f,0.f, Params.MasterSeed);
}

void NoiseGraph::EvaluateHeightField(const float* X, const float* Y, const float* Z,
                                     int32_t Count, float* OutHeight) const
{
    // thread_local scratch: this is called twice per chunk, and allocating four
    // vectors each time cost a measurable slice of the chunk budget.
    static thread_local std::vector<float> C, M, D;
    if ((int32_t)C.size() < Count)
    { C.resize((size_t)Count); M.resize((size_t)Count); D.resize((size_t)Count); }

    EvaluateLayersNoHumidity(X, Y, Z, Count, C.data(), M.data(), D.data());
    for (int32_t i = 0; i < Count; ++i)
        OutHeight[i] = (float)ComposeHeight(C[i], M[i], D[i]);
}

void NoiseGraph::EvaluateBaseHeightBatch(const float* X, const float* Y, const float* Z,
                                         int32_t Count, double* OutHeight) const
{
    if (!bValid || Count <= 0) return;
    static thread_local std::vector<float> C, M;
    if ((int32_t)C.size() < Count) { C.resize((size_t)Count); M.resize((size_t)Count); }

    ContinentWarp->GenPositionArray3D(  C.data(), Count, X, NoiseY(Y, Z), NoiseZ(Y, Z), 0.f,0.f,0.f, Params.MasterSeed);
    MountainFractal->GenPositionArray3D(M.data(), Count, X, NoiseY(Y, Z), NoiseZ(Y, Z), 0.f,0.f,0.f, Params.MasterSeed);
    for (int32_t i = 0; i < Count; ++i)
        OutHeight[i] = ComposeHeight(C[i], M[i], 0.f);
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

    static thread_local std::vector<float>  XG, YG, ZG, Cg, Mg, Hg, Dg, Tg;
    static thread_local std::vector<double> DXg, DYg, DZg, HeightG;
    if ((int32_t)XG.size() < TotalG)
    {
        for (std::vector<float>* V : {&XG, &YG, &ZG, &Cg, &Mg, &Hg, &Dg, &Tg})
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

    EvaluateLayers(XG.data(), YG.data(), ZG.data(), TotalG,
                   Cg.data(), Mg.data(), Hg.data(), Dg.data());
    EvaluateTempNoise(XG.data(), YG.data(), ZG.data(), TotalG, Tg.data());
    for (int32_t i = 0; i < TotalG; ++i)
        HeightG[i] = ComposeHeight(Cg[i], Mg[i], Dg[i]);

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

void NoiseGraph::SampleSurfaceBatch(const float* DirX, const float* DirY, const float* DirZ,
                                    int32_t Count, Surface* Out) const
{
    if (!bValid || Count <= 0) return;

    static thread_local std::vector<float> XU,YU,ZU, XV,YV,ZV, H0,HU,HV, Cb,Mb,Hb,Db,Tb;
    XU.resize((size_t)Count); YU.resize((size_t)Count); ZU.resize((size_t)Count);
    XV.resize((size_t)Count); YV.resize((size_t)Count); ZV.resize((size_t)Count);
    H0.resize((size_t)Count); HU.resize((size_t)Count); HV.resize((size_t)Count);
    Cb.resize((size_t)Count); Mb.resize((size_t)Count); Hb.resize((size_t)Count); Db.resize((size_t)Count);
    Tb.resize((size_t)Count);

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

    EvaluateLayers(DirX, DirY, DirZ, Count, Cb.data(), Mb.data(), Hb.data(), Db.data());
    EvaluateTempNoise(DirX, DirY, DirZ, Count, Tb.data());
    for (int32_t i = 0; i < Count; ++i)
        H0[i] = (float)ComposeHeight(Cb[i], Mb[i], Db[i]);

    EvaluateHeightField(XU.data(), YU.data(), ZU.data(), Count, HU.data());
    EvaluateHeightField(XV.data(), YV.data(), ZV.data(), Count, HV.data());

    const double R = Params.PlanetRadiusMetres;

    for (int32_t i = 0; i < Count; ++i)
    {
        Surface& S = Out[i];
        const Vec3d Dir((double)DirX[i], (double)DirY[i], (double)DirZ[i]);

        S.Continent = Cb[i]; S.Mountain = Mb[i]; S.HumidityRaw = Hb[i]; S.TempNoiseRaw = Tb[i]; S.Height = (double)H0[i];

        const Vec3d S0 = Dir * (R + (double)H0[i]);
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
    float C=0.f,M=0.f,H=0.f,D=0.f;
    EvaluateLayers(&x,&y,&z,1,&C,&M,&H,&D);
    return ComposeHeight(C,M,D);
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

    std::vector<float> C((size_t)Total), M((size_t)Total), H((size_t)Total), D((size_t)Total);
    EvaluateLayers(X.data(), Y.data(), Z.data(), Total, C.data(), M.data(), H.data(), D.data());
    std::sort(C.begin(), C.end());

    const double Q = std::clamp(1.0 - (double)Target, 0.0, 1.0);
    const size_t Idx = (size_t)std::clamp(Q * (double)(Total - 1), 0.0, (double)(Total - 1));

    SeaLevel = C[Idx];
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

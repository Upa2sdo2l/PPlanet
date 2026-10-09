// PlanetNoiseCore.h
// ─────────────────────────────────────────────────────────────────────────────
// UE-FREE noise core.
//
// Depends on nothing but the C++ standard library and FastNoise2. That is
// deliberate: every claim about terrain shape, seam continuity, biome
// distribution and cost can then be verified by compiling and RUNNING a test
// binary in seconds instead of by rebuilding the editor.
//
// PlanetNoise.h (the UE-facing wrapper) delegates here. One truth, two
// environments.
//
// ── API facts checked against the FastNoise2 source in this tree ────────────
//   • Fractal has NO SetScale. Frequency lives on the SOURCE node's
//     ScalableGenerator; SetScale is effectively 1/frequency.
//   • Simplex = VariableRange<Seeded<ScalableGenerator>>
//     → SetScale / SetSeedOffset / SetOutputMin / SetOutputMax.
//   • DomainWarpGradient = Seeded<ScalableGenerator>, holds mSource.
//     Nesting is one level deeper than it looks: the warp node wraps the
//     CONTENT node, so DomainWarpGradient(Source = FractalFBm) distorts the
//     position and then evaluates the fractal there.
//   • GenPositionArray3D(out, count, x, y, z, xOff, yOff, zOff, seed)
//   • FractalRidged folds each octave as (|n| · -2 + 1): peaks at +1.
//
// ── PRECISION ───────────────────────────────────────────────────────────────
// Noise input is a unit direction, |dir| <= 1. A node with Scale S multiplies
// internally by f = 1/S, so the internal coordinate is dir·f with |·| <= f.
//     ulp at f          = f · 2⁻²³
//     adjacent vertex Δ = (s/R) · f          (s = vertex spacing in metres)
//     ratio Δ/ulp       = (s/R) · 2²³ = s · 3.355     ← f cancels
// Precision therefore depends on VERTEX SPACING ONLY, not on frequency:
//     s = 0.30 m → ratio 1.0   hard floor
//     s = 1.00 m → ratio 3.4   usable
//     s = 19.0 m → ratio 64    our finest gameplay LOD, comfortable
// FastNoise2 covers everything down to ~1 m spacing; below that, micro-detail
// needs a double-precision lattice path.
//
//   NOTE: an earlier draft claimed per-layer octave counts must be culled by
//   LOD. That was wrong: every layer's finest octave has a wavelength
//   hundreds of times larger than the finest vertex spacing, so no octave is
//   ever sub-grid. One graph, one octave set, all LODs.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <FastNoise/FastNoise.h>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <vector>

namespace PlanetCore
{

// ── Minimal vector ──────────────────────────────────────────────────────────
struct Vec3d
{
    double X = 0.0, Y = 0.0, Z = 0.0;

    Vec3d() = default;
    Vec3d(double x, double y, double z) : X(x), Y(y), Z(z) {}

    double LengthSq() const { return X*X + Y*Y + Z*Z; }
    double Length()   const { return std::sqrt(LengthSq()); }

    Vec3d Normalised() const
    {
        const double Inv = 1.0 / Length();
        return Vec3d(X * Inv, Y * Inv, Z * Inv);
    }

    static double Dot(const Vec3d& A, const Vec3d& B) { return A.X*B.X + A.Y*B.Y + A.Z*B.Z; }
    static Vec3d Cross(const Vec3d& A, const Vec3d& B)
    { return Vec3d(A.Y*B.Z - A.Z*B.Y, A.Z*B.X - A.X*B.Z, A.X*B.Y - A.Y*B.X); }

    Vec3d operator+(const Vec3d& O) const { return Vec3d(X+O.X, Y+O.Y, Z+O.Z); }
    Vec3d operator-(const Vec3d& O) const { return Vec3d(X-O.X, Y-O.Y, Z-O.Z); }
    Vec3d operator-()               const { return Vec3d(-X, -Y, -Z); }
    Vec3d operator*(double S)       const { return Vec3d(X*S, Y*S, Z*S); }
    Vec3d operator/(double S)       const { return Vec3d(X/S, Y/S, Z/S); }
};

inline Vec3d operator*(double S, const Vec3d& V) { return V * S; }

// ── Shared constants (must match the UE-side header) ────────────────────────
static constexpr uint32_t PLANET_GENERATOR_VERSION = 2;   // 2: halo-grid normals, detail in metres
static constexpr int32_t  BIOME_GRASS = 0;
static constexpr int32_t  BIOME_ROCK  = 1;
static constexpr int32_t  BIOME_SAND  = 2;
static constexpr int32_t  BIOME_SNOW  = 3;

static constexpr int32_t PLANET_QUADS_PER_SIDE = 64;
static constexpr int32_t PLANET_VERTS_PER_SIDE = PLANET_QUADS_PER_SIDE + 1;   // 65
static constexpr int32_t PLANET_GRID_WITH_HALO = PLANET_VERTS_PER_SIDE + 2;   // 67

struct FChunkKey
{
    uint8_t  Face = 0;
    uint8_t  LOD  = 0;
    int32_t  X    = 0;
    int32_t  Y    = 0;

    FChunkKey() = default;
    FChunkKey(uint8_t f, uint8_t l, int32_t x, int32_t y) : Face(f), LOD(l), X(x), Y(y) {}

    bool operator==(const FChunkKey& O) const
    { return Face == O.Face && LOD == O.LOD && X == O.X && Y == O.Y; }
};

// ── Coordinate functions (defined in the .cpp, mirrored in the UE header) ───
// CubeFaceDirection is the single source of truth for cube-sphere mapping.
// PlanetCoordinates.h mirrors it and the harness proves the two agree
// bit-for-bit on 10086 samples.
Vec3d CubeFaceDirection(int32_t Face, double U, double V);

// UV of grid vertex (GridX, GridY) for a chunk, including the halo ring:
// index 0 and PLANET_GRID_WITH_HALO-1 lie one step outside the chunk.
void ChunkVertexUV(const FChunkKey& Key, int32_t GridX, int32_t GridY,
                   double& OutU, double& OutV);

enum ELayer : int32_t
{
    Layer_Continent  = 0,
    Layer_Mountains  = 1,
    Layer_DomainWarp = 2,
    Layer_Humidity   = 3,
    Layer_Detail     = 4,
    Layer_Micro      = 5,
    Layer_COUNT      = 6,
};

struct NoiseParams
{
    // ── Continent ───────────────────────────────────────────────────────
    float   ContinentFrequency = 1.2f;
    float   SeaLevelNorm       = 0.f;
    float   ShoreSharpness     = 0.4f;
    int32_t ContinentOctaves   = 8;
    float   ContinentGain      = 0.5f;
    float   ContinentLacunarity= 2.0f;

    // ── Mountains ───────────────────────────────────────────────────────
    float   MountainFrequency  = 6.0f;
    float   MountainAmplitude  = 50000.f;
    float   MountainSharpness  = 2.5f;
    int32_t MountainOctaves    = 12;
    float   MountainGain       = 0.5f;

    // ── Ocean ───────────────────────────────────────────────────────────
    float   OceanDepthScale    = 0.35f;
    float   OceanDetailAmp     = 0.08f;

    // ── Domain warp ─────────────────────────────────────────────────────
    float   DomainWarpAmplitude = 0.30f;
    float   DomainWarpFrequency = 1.5f;

    // ── Climate ─────────────────────────────────────────────────────────
    float   SnowLatitudeStart  = 0.90f;   // sin(latitude): 0.90 ~ 64 deg
    float   SnowAltitudeStart  = 3500.f;
    float   HumidityVariance   = 0.40f;

    // ── Detail ──────────────────────────────────────────────────────────
    // Wavelength of the largest detail octave, metres on the surface. Each
    // further octave is 2.2x smaller: 2000 m with 8 octaves reaches ~8 m,
    // about the LOD-14 vertex spacing.
    float   DetailWavelengthMetres = 2000.f;
    int32_t DetailOctaves      = 8;
    // Metres. Added to the height as D * DetailAmplitude, D roughly [-1, 1].
    float   DetailAmplitude    = 40.f;

    // ── Planet ──────────────────────────────────────────────────────────
    // Radius of the reference sphere, metres. Converts metre-sized settings
    // into the unit-sphere noise space and positions samples for normals.
    double  PlanetRadiusMetres = 2500000.0;

    int32_t MasterSeed = 1337;

    // Wang-mixed per-layer seed: low correlation between layers, deterministic
    // and cheap. Reordering layers is a generator version bump.
    int32_t LayerSeed(int32_t L) const
    {
        uint32_t H = (uint32_t)MasterSeed ^ ((uint32_t)L * 0x9E3779B9u);
        H = (H ^ (H >> 16)) * 0x45D9F3Bu;
        H = (H ^ (H >> 16)) * 0x45D9F3Bu;
        H ^= (H >> 16);
        return (int32_t)H;
    }
};

struct Climate
{
    float Temperature = 0.f;
    float Humidity    = 0.f;
    float Aridity     = 0.f;
    float SnowCover   = 0.f;
};

struct Surface
{
    double   Height = 0.0;             // displacement from the sphere, metres
    Vec3d    Normal{0,0,1};
    Vec3d    Tangent{1,0,0};
    float    Biome[4] = {1.f, 0.f, 0.f, 0.f};
    float    FlowMask = 0.f;
    float    Slope    = 0.f;
    float    Continent = 0.f;
    float    Mountain  = 0.f;
    float    HumidityRaw = 0.f;
    float    TempNoiseRaw = 0.f;     // decorrelated twin of HumidityRaw, for temperature
};

// The chunk's sampling grid including the one-vertex halo ring, as
// SampleChunkBatch computes it. Index g = gx + gy * PLANET_GRID_WITH_HALO;
// chunk vertex (x, y) is g = (x + 1) + (y + 1) * PLANET_GRID_WITH_HALO.
// The GPU renderer needs the halo to compute normals after erosion.
struct HaloGrid
{
    static constexpr int32_t Side  = PLANET_GRID_WITH_HALO;   // 67
    static constexpr int32_t Count = Side * Side;             // 4489
    Vec3d  Dir[Count];          // unit direction, double (exactly as the mesh)
    double Height[Count];       // full height, metres
    float  Continent[Count];
    float  Mountain[Count];
    float  Humidity[Count];     // HumidityRaw
    float  TempNoise[Count];    // TempNoiseRaw
};

class NoiseGraph
{
public:
    NoiseGraph() = default;
    NoiseGraph(const NoiseGraph&) = delete;
    NoiseGraph& operator=(const NoiseGraph&) = delete;

    void Build(const NoiseParams& P);

    bool  IsValid() const { return bValid; }
    const NoiseParams& GetParams() const { return Params; }
    float GetEffectiveSeaLevel() const { return SeaLevel; }

    // Arbitrary direction arrays (Seed Lab, gameplay, analytics).
    void SampleSurfaceBatch(const float* DirX, const float* DirY, const float* DirZ,
                            int32_t Count, Surface* Out) const;

    // THE mesh-generation entry point. Fills PLANET_VERTS_PER_SIDE² surfaces
    // row-major: index = gy * VERTS_PER_SIDE + gx.
    //
    // One bulk noise pass over a PLANET_GRID_WITH_HALO² grid (the chunk plus
    // a one-vertex ring around it). Normals are central differences between
    // grid neighbours, so a border vertex gets the same normal from both
    // chunks that share it.
    //
    // OutHalo (optional) receives the whole sampled grid, halo included.
    void SampleChunkBatch(const FChunkKey& Key, Surface* Out, HaloGrid* OutHalo = nullptr) const;

    // Continent + mountains only (no detail, no humidity): the large-scale
    // relief that erosion follows. Two of the four noise layers.
    void EvaluateBaseHeightBatch(const float* X, const float* Y, const float* Z,
                                 int32_t Count, double* OutHeight) const;

    // Erosion inputs from a point's layer values: Mask 0..1 is the land mask
    // (0 under water, the same shore ramp that fades the mountains in), Fade
    // -1..1 hints valleys vs peaks (from the mountain layer).
    void ErosionMaskFade(float Continent, float Mountain, float& OutMask, float& OutFade) const;

    // Branch-free tangent basis; deterministic for a given direction, so a
    // point shared by two chunks gets the same basis from both.
    static void TangentBasis(const Vec3d& Dir, Vec3d& OutT1, Vec3d& OutT2) { BuildNormalBasis(Dir, OutT1, OutT2); }

    Surface SampleSurface(const Vec3d& UnitDir) const;
    double  EvaluateHeight(const Vec3d& UnitDir) const;

    float ComputeSeaLevelForLandFraction(float Target, int32_t SamplesPerFace = 32);

    double ComposeHeight(float C, float M, float D) const;

    struct Range { float Min = 0.f; float Max = 0.f; double Mean = 0.0; };
    Range MeasureHeightRange(int32_t SamplesPerFace, int32_t Seed) const;

    // ── Pure functions, public so tests and the Seed Lab can call them ────
    // Stateless: they read only their arguments. Exposing them costs nothing
    // and removes the temptation to duplicate the climate model elsewhere.
    static Climate ComputeClimate(const Vec3d& Dir, double Height, const NoiseParams& P);
    static void    ComputeBiomes(const Climate& C, float Slope, float Flow,
                                 double Height, float Out[4]);

private:
    void EvaluateLayers(const float* X, const float* Y, const float* Z, int32_t Count,
                        float* OutContinent, float* OutMountain,
                        float* OutHumidity, float* OutDetail) const;

    // Height only. Humidity is not part of ComposeHeight, so skipping it here
    // removes 2 of the 12 noise calls per chunk.
    // The humidity fractal sampled at an offset position: a second, uncorrelated
    // low-frequency field for the temperature noise, at no extra node.
    void EvaluateTempNoise(const float* X, const float* Y, const float* Z, int32_t Count,
                           float* Out) const;

    void EvaluateLayersNoHumidity(const float* X, const float* Y, const float* Z, int32_t Count,
                                  float* OutContinent, float* OutMountain,
                                  float* OutDetail) const;

    void EvaluateHeightField(const float* X, const float* Y, const float* Z,
                             int32_t Count, float* OutHeight) const;

    // Branch-free tangent basis from the cube-face parametrisation.
    static void BuildNormalBasis(const Vec3d& Dir, Vec3d& OutT1, Vec3d& OutT2);

    FastNoise::SmartNode<FastNoise::Simplex>            ContinentSource;
    FastNoise::SmartNode<FastNoise::FractalFBm>         ContinentFractal;
    FastNoise::SmartNode<FastNoise::DomainWarpGradient> ContinentWarp;

    FastNoise::SmartNode<FastNoise::Simplex>            MountainSource;
    FastNoise::SmartNode<FastNoise::FractalRidged>      MountainFractal;

    FastNoise::SmartNode<FastNoise::Simplex>            HumiditySource;
    FastNoise::SmartNode<FastNoise::FractalFBm>         HumidityFractal;

    FastNoise::SmartNode<FastNoise::Simplex>            DetailSource;
    FastNoise::SmartNode<FastNoise::FractalFBm>         DetailFractal;

    NoiseParams Params;
    float SeaLevel = 0.f;
    bool  bValid   = false;
};

// ── Inline climate and biome logic ──────────────────────────────────────────
// Analytic, not noise: the tricellular circulation model puts rising (wet) air
// at the equator and ~60°, and descending (dry) air at ~30° and the poles.
//   humidity_lat(lat) = 0.5 + 0.5·cos(6·lat)
//   0°->1.0 wet, 30°->0.0 dry, 60°->1.0 wet, 90°->0.0 dry
// Verified numerically: desert band centres land at exactly ±30.00°.
//
// Polar axis is +Z (UE "up"): the poles sit at the centres of cube faces 4/5.
inline Climate NoiseGraph::ComputeClimate(const Vec3d& Dir, double Height, const NoiseParams& P)
{
    Climate Out;
    const double SinLat = std::clamp(Dir.Z, -1.0, 1.0);
    const double Lat    = std::asin(SinLat);

    const double BaseTemp = std::pow(std::max(0.0, std::cos(Lat)), 1.5);
    const double Lapse    = std::max(0.0, Height) / 11000.0;
    Out.Temperature = (float)std::clamp(BaseTemp - Lapse, 0.0, 1.0);

    const double LatBand = 0.5 + 0.5 * std::cos(6.0 * Lat);
    Out.Humidity = (float)LatBand;

    const double Interior = std::clamp(Height / 2000.0, 0.0, 1.0);
    Out.Aridity = (float)std::clamp(
        (1.0 - LatBand) * (1.0 + 0.5 * Interior) * 0.5 + (1.0 - Out.Humidity) * (double)P.HumidityVariance,
        0.0, 1.0);

    // 0 below |sin(lat)| = SnowLatitudeStart - 0.10, 1 above SnowLatitudeStart + 0.10.
    // An earlier version had "1.f - clamp(...)", which put full snow on the
    // equator and none on the poles.
    const float LatTrig = (float)std::clamp(
        (std::fabs(SinLat) - (P.SnowLatitudeStart - 0.10)) / 0.20, 0.0, 1.0);
    const float AltTrig = (float)std::clamp(
        (Height - (P.SnowAltitudeStart - 500.f)) / 1000.f, 0.0, 1.0);
    Out.SnowCover = std::clamp(std::max(LatTrig, AltTrig) * (1.f - Out.Temperature * 0.4f), 0.f, 1.f);

    return Out;
}

inline void NoiseGraph::ComputeBiomes(const Climate& C, float Slope, float Flow,
                                      double Height, float Out[4])
{
    (void)Flow;
    const float Snow = std::clamp(C.SnowCover * (1.f - std::clamp(Slope * 1.4f, 0.f, 1.f)), 0.f, 1.f);

    const float RockSlope = (float)std::clamp((Slope - 0.35f) / 0.35f, 0.f, 1.f);
    const float RockCold  = (float)std::clamp((0.30f - C.Temperature) / 0.18f, 0.f, 1.f);
    const float Rock = std::clamp(std::max(RockSlope, RockCold * 0.6f) * (1.f - Snow), 0.f, 1.f);

    const float SandArid  = (float)std::clamp((C.Aridity - 0.55f) / 0.25f, 0.f, 1.f);
    const float SandBeach = (float)std::clamp((60.f - (float)Height) / 60.f, 0.f, 1.f)
                          * (float)std::clamp((0.30f - Slope) / 0.25f, 0.f, 1.f);
    const float Sand = std::clamp(std::max(SandArid, SandBeach * 0.7f) * (1.f - Snow) * (1.f - Rock), 0.f, 1.f);

    const float Grass = std::clamp(1.f - Snow - Rock - Sand, 0.f, 1.f);

    const float Sum = Grass + Rock + Sand + Snow;
    if (Sum < 1e-8f) { Out[0]=1.f; Out[1]=Out[2]=Out[3]=0.f; return; }
    const float Inv = 1.f / Sum;
    Out[BIOME_GRASS] = Grass * Inv;
    Out[BIOME_ROCK]  = Rock  * Inv;
    Out[BIOME_SAND]  = Sand  * Inv;
    Out[BIOME_SNOW]  = Snow  * Inv;
}

} // namespace PlanetCore

// PlanetTypes.h
// Core data types, keys, constants and configuration. UE-facing only.
//
// Coordinate contract (read before touching any coordinate math):
//   - Planet-space positions            : FVector3d (double, R ~ 2.5e6 m)
//   - Cube-face UV                      : double, range [-1, 1]
//   - Unit directions                   : FVector3d, normalised
//   - Noise inputs (FastNoise2 float32) : FVector3f, derived from the direction
//   - Vertex positions inside a chunk   : FVector3f, relative to chunk origin
//                                         chunk <= 128 m -> ULP < 2e-5 m
//   - World/UE positions                : FVector (LWC double precision)
//
// The numerical generation lives in planet_core/ (UE-free, covered by
// harnesses). This header carries only what the editor and the actor need.

#pragma once

#include "CoreMinimal.h"

// UHT requires the generated header to be the LAST include.
#include "PlanetTypes.generated.h"

// Bump when the noise graph, biome formula or erosion changes. The bake cache
// hashes this, so stale bakes are evicted without a manual cache bust.
static constexpr uint32 PLANET_GENERATOR_VERSION = 1;

// ── Chunk geometry (must match PlanetNoiseCore.h) ───────────────────────────
static constexpr int32 PLANET_QUADS_PER_SIDE = 64;
static constexpr int32 PLANET_VERTS_PER_SIDE = PLANET_QUADS_PER_SIDE + 1;   // 65
static constexpr int32 PLANET_GRID_WITH_HALO = PLANET_VERTS_PER_SIDE + 2;   // 67
static constexpr int32 PLANET_BODY_VERTS     = PLANET_VERTS_PER_SIDE * PLANET_VERTS_PER_SIDE;
static constexpr int32 PLANET_BODY_TRIS      = PLANET_QUADS_PER_SIDE * PLANET_QUADS_PER_SIDE * 2;

// ── Fixed component pool ────────────────────────────────────────────────────
// Decided policy: the pool never grows at runtime. Under pressure the LOD
// budget and the visible set shrink; the component count does not.
// PlanetLOD::TraversalParams::MaxLeaves must equal this value.
static constexpr int32 PLANET_COMPONENT_POOL_SIZE = 256;

// UE world units are centimetres; the noise core works in metres. This is the
// single named conversion factor. Prefixed deliberately: in a Unreal unity
// build the anonymous namespaces of several .cpp files merge into one
// translation unit, so an unprefixed helper name here collides with any other
// file that defines the same one.
static constexpr double PLANET_METRES_TO_UE_CM = 100.0;

// Biome weight order, matching PlanetCore::Surface::Biome.
enum class EPlanetBiome : uint8
{
    Grass = 0,
    Rock  = 1,
    Sand  = 2,
    Snow  = 3,
    Count = 4,
};

// ── Chunk key ───────────────────────────────────────────────────────────────
USTRUCT(BlueprintType)
struct FChunkKey
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet|Chunk")
    uint8  Face = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet|Chunk")
    uint8  LOD  = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet|Chunk")
    int32  X    = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet|Chunk")
    int32  Y    = 0;

    FChunkKey() = default;
    FChunkKey(uint8 InFace, uint8 InLOD, int32 InX, int32 InY)
        : Face(InFace), LOD(InLOD), X(InX), Y(InY) {}

    bool operator==(const FChunkKey& O) const
    { return Face == O.Face && LOD == O.LOD && X == O.X && Y == O.Y; }
    bool operator!=(const FChunkKey& O) const { return !(*this == O); }

    friend uint32 GetTypeHash(const FChunkKey& K)
    {
        uint32 H = (uint32)K.Face;
        H = H * 1000003u ^ (uint32)K.LOD;
        H = H * 1000003u ^ (uint32)(K.X + 0x80000000u);
        H = H * 1000003u ^ (uint32)(K.Y + 0x80000000u);
        return H;
    }

    FChunkKey Parent() const { return FChunkKey(Face, LOD - 1, X >> 1, Y >> 1); }
    FChunkKey Child(int32 i) const
    { return FChunkKey(Face, LOD + 1, X * 2 + (i & 1), Y * 2 + (i >> 1)); }

    FString ToString() const
    { return FString::Printf(TEXT("F%d L%d [%d,%d]"), (int32)Face, (int32)LOD, X, Y); }
};

// ── Editor-facing planet configuration ──────────────────────────────────────
// Every field here is mapped to PlanetCore::NoiseParams by
// PlanetBridge::ToCoreParams in PlanetNoise.h. A field with no mapping would
// silently do nothing, so unmapped fields are not exposed as UPROPERTYs.
USTRUCT(BlueprintType)
struct FPlanetNoiseParams
{
    GENERATED_BODY()

    // ── Continent ───────────────────────────────────────────────────────
    UPROPERTY(EditAnywhere, Category="Planet|Continent", meta=(ClampMin="0.1", ClampMax="8.0"))
    float ContinentFrequency = 1.2f;

    // Sharpness of the continent-ocean boundary. Low = gentle shelves.
    UPROPERTY(EditAnywhere, Category="Planet|Continent", meta=(ClampMin="0.0", ClampMax="1.0"))
    float ShoreSharpness = 0.4f;

    // ── Mountains ───────────────────────────────────────────────────────
    UPROPERTY(EditAnywhere, Category="Planet|Mountains",
    meta=(ClampMin="0.5", ClampMax="24.0"))
    float MountainFrequency = 6.f;

    UPROPERTY(EditAnywhere, Category="Planet|Mountains",
        meta=(ClampMin="0.0", ClampMax="120000.0"))
    float MountainAmplitude = 50000.f;

    UPROPERTY(EditAnywhere, Category="Planet|Mountains",
        meta=(ClampMin="1.0", ClampMax="8.0"))
    float MountainSharpness = 2.5f;

    UPROPERTY(EditAnywhere, Category="Planet|Mountains",
        meta=(ClampMin="1", ClampMax="16"))
    int32 MountainOctaves = 12;

    UPROPERTY(EditAnywhere, Category="Planet|Mountains",
        meta=(ClampMin="0.0", ClampMax="1.0"))
    float MountainGain = 0.5f;

    // ── Domain warp ─────────────────────────────────────────────────────
    UPROPERTY(EditAnywhere, Category="Planet|Warp", meta=(ClampMin="0.0", ClampMax="2.0"))
    float DomainWarpAmplitude = 0.3f;

    UPROPERTY(EditAnywhere, Category="Planet|Warp", meta=(ClampMin="0.5", ClampMax="6.0"))
    float DomainWarpFrequency = 1.5f;

    // ── Climate ─────────────────────────────────────────────────────────
    // sin(latitude) where polar snow is half-way in; the ramp is ±0.10 around
    // it. 0.90 ~ 64 deg. Polar axis is world +Z.
    UPROPERTY(EditAnywhere, Category="Planet|Climate", meta=(ClampMin="0.0", ClampMax="1.0"))
    float SnowLatitudeStart = 0.90f;

    // Elevation above sea level, metres, where altitude snow starts. The
    // terrain reaches ~20+ km, so the range goes well past 9 km.
    UPROPERTY(EditAnywhere, Category="Planet|Climate", meta=(ClampMin="0.0", ClampMax="25000.0"))
    float SnowAltitudeStart = 3500.f;

    UPROPERTY(EditAnywhere, Category="Planet|Climate", meta=(ClampMin="0.0", ClampMax="1.0"))
    float HumidityVariance = 0.4f;

    // ── Ocean floor ─────────────────────────────────────────────────────
    // Kept in sync with PlanetCore::NoiseParams::OceanDepthScale. An earlier
    // draft of this header said 2.5 while the core ran 0.35, so every slider
    // would have disagreed with the generated terrain. It is 0.35.
    UPROPERTY(EditAnywhere, Category="Planet|Ocean", meta=(ClampMin="0.0", ClampMax="2.0"))
    float OceanDepthScale = 0.35f;
    
    // ── Detail ─────────────────────────────────────────────────────────────

    UPROPERTY(EditAnywhere, Category="Planet|Detail",
        meta=(ClampMin="1.0", ClampMax="128.0"))
    float DetailFrequency = 24.0f;

    UPROPERTY(EditAnywhere, Category="Planet|Detail",
        meta=(ClampMin="1", ClampMax="16"))
    int32 DetailOctaves = 8;

    UPROPERTY(EditAnywhere, Category="Planet|Detail",
        meta=(ClampMin="0.0", ClampMax="10000.0"))
    float DetailAmplitude = 300.0f;

    // ── Seed ────────────────────────────────────────────────────────────
    UPROPERTY(EditAnywhere, Category="Planet|Seed")
    int32 MasterSeed = 1337;

    // Fraction of the surface that should end up above sea level.
    // The sea level is derived from this by quantile, so no seed is ever
    // "all ocean" or "no water" unless asked for.
    UPROPERTY(EditAnywhere, Category="Planet|Seed", meta=(ClampMin="0.05", ClampMax="0.95"))
    float TargetLandFraction = 0.30f;

    // ── Layer seeds ─────────────────────────────────────────────────────
    enum ELayer : int32
    {
        Layer_Continent  = 0,
        Layer_Mountains  = 1,
        Layer_DomainWarp = 2,
        Layer_Humidity   = 3,
        Layer_Detail     = 4,
        Layer_Micro      = 5,
        Layer_COUNT      = 6,
    };

    FORCEINLINE int32 LayerSeed(int32 L) const
    {
        uint32 H = (uint32)MasterSeed ^ ((uint32)L * 0x9E3779B9u);
        H = (H ^ (H >> 16)) * 0x45D9F3Bu;
        H = (H ^ (H >> 16)) * 0x45D9F3Bu;
        H ^= (H >> 16);
        return (int32)H;
    }

    // Hash of everything that changes generated geometry. Used as the mesh
    // cache key component.
    uint64 ComputeCacheHash() const;
};

inline uint64 FPlanetNoiseParams::ComputeCacheHash() const
{
    uint64 H = 1469598103934665603ull;   // FNV-1a offset basis
    auto Mix = [&H](const void* Data, int32 Len)
    {
        const uint8* P = (const uint8*)Data;
        for (int32 i = 0; i < Len; ++i) { H ^= P[i]; H *= 1099511628211ull; }
    };
    Mix(&ContinentFrequency,   sizeof(ContinentFrequency));
    Mix(&ShoreSharpness,       sizeof(ShoreSharpness));
    Mix(&MountainFrequency,    sizeof(MountainFrequency));
    Mix(&MountainAmplitude,    sizeof(MountainAmplitude));
    Mix(&MountainSharpness,    sizeof(MountainSharpness));
    Mix(&MountainOctaves,      sizeof(MountainOctaves));
    Mix(&MountainGain,         sizeof(MountainGain));
    Mix(&DomainWarpAmplitude,  sizeof(DomainWarpAmplitude));
    Mix(&DomainWarpFrequency,  sizeof(DomainWarpFrequency));
    Mix(&SnowLatitudeStart,    sizeof(SnowLatitudeStart));
    Mix(&SnowAltitudeStart,    sizeof(SnowAltitudeStart));
    Mix(&HumidityVariance,     sizeof(HumidityVariance));
    Mix(&OceanDepthScale,      sizeof(OceanDepthScale));
    Mix(&DetailFrequency,      sizeof(DetailFrequency));
    Mix(&DetailOctaves,        sizeof(DetailOctaves));
    Mix(&DetailAmplitude,      sizeof(DetailAmplitude));
    Mix(&MasterSeed,           sizeof(MasterSeed));
    Mix(&TargetLandFraction,   sizeof(TargetLandFraction));
    return H ^ (uint64)PLANET_GENERATOR_VERSION;
}


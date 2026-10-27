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
static constexpr uint32 PLANET_GENERATOR_VERSION = 6;   // 6: terrain shape V6; 4: V4; 2: halo-grid normals, detail in metres

// ── Chunk geometry (must match PlanetNoiseCore.h) ───────────────────────────
static constexpr int32 PLANET_QUADS_PER_SIDE = 64;
static constexpr int32 PLANET_VERTS_PER_SIDE = PLANET_QUADS_PER_SIDE + 1;   // 65
static constexpr int32 PLANET_GRID_WITH_HALO = PLANET_VERTS_PER_SIDE + 2;   // 67
static constexpr int32 PLANET_BODY_VERTS     = PLANET_VERTS_PER_SIDE * PLANET_VERTS_PER_SIDE;
static constexpr int32 PLANET_BODY_TRIS      = PLANET_QUADS_PER_SIDE * PLANET_QUADS_PER_SIDE * 2;

// ── Fixed component pool ────────────────────────────────────────────────────
// Decided policy: the pool never grows at runtime.
// The LOD selection may use at most PLANET_LOD_LEAF_BUDGET chunks. The extra
// slots hold chunks that are no longer desired but stay on screen until the
// chunks replacing them are built ("lingering", see PlanetStreaming), so the
// pool must be larger than the leaf budget. 64 spare slots ~ 15 MB of mesh.
static constexpr int32 PLANET_LOD_LEAF_BUDGET     = 256;
static constexpr int32 PLANET_COMPONENT_POOL_SIZE = 320;
static_assert(PLANET_COMPONENT_POOL_SIZE > PLANET_LOD_LEAF_BUDGET,
              "the pool needs headroom above the leaf budget for lingering chunks");

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

// Terrain formula (PlanetCore::ETerrainShape).
UENUM(BlueprintType)
enum class EPlanetTerrainShape : uint8
{
    // Continent * 12 km + ridged mountains on all land (the first formula).
    Original UMETA(DisplayName="Original"),
    // Regions of plains, hills and mountains, table mountains (planet_core/PlanetTerrainShape.h).
    V4       UMETA(DisplayName="V4: regions and table mountains"),
    // Warped mountain regions with ridged ranges and a piedmont (planet_core/PlanetTerrainShapeV6.h).
    V6       UMETA(DisplayName="V6: mountain belts"),
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

    // Fractal of the continent layer: more octaves = more ragged coasts and
    // islands; gain = how strong each finer octave is; lacunarity = how much
    // finer each octave is.
    UPROPERTY(EditAnywhere, Category="Planet|Continent", meta=(ClampMin="1", ClampMax="16"))
    int32 ContinentOctaves = 8;

    UPROPERTY(EditAnywhere, Category="Planet|Continent", meta=(ClampMin="0.0", ClampMax="1.0"))
    float ContinentGain = 0.5f;

    UPROPERTY(EditAnywhere, Category="Planet|Continent", meta=(ClampMin="1.01", ClampMax="4.0"))
    float ContinentLacunarity = 2.0f;

    // Sharpness of the continent-ocean boundary. Low = gentle shelves.
    UPROPERTY(EditAnywhere, Category="Planet|Continent", meta=(ClampMin="0.0", ClampMax="1.0"))
    float ShoreSharpness = 0.4f;

    // ── Terrain shape ───────────────────────────────────────────────────
    // V6 (default): warped mountain regions with ridged ranges and a
    // piedmont, plains with swells and hills (planet_core/PlanetTerrainShapeV6.h).
    // V4: Minecraft-style regions of plains, hills and mountains, table
    // mountains (planet_core/PlanetTerrainShape.h). Original: continent *
    // 12 km + ridged mountains on all land; the Mountains section,
    // ShoreSharpness and OceanDepthScale apply only then. Continent settings
    // place the land in every shape.
    UPROPERTY(EditAnywhere, Category="Planet|Terrain Shape")
    EPlanetTerrainShape TerrainShape = EPlanetTerrainShape::V6;

    // Ground height at the coast and its rise towards the continent interiors (V4 and V6).
    UPROPERTY(EditAnywhere, Category="Planet|Terrain Shape", meta=(ClampMin="0.0", ClampMax="2000.0", EditCondition="TerrainShape != EPlanetTerrainShape::Original"))
    float LowlandHeightMetres = 25.f;

    UPROPERTY(EditAnywhere, Category="Planet|Terrain Shape", meta=(ClampMin="0.0", ClampMax="5000.0", EditCondition="TerrainShape != EPlanetTerrainShape::Original"))
    float InteriorRiseMetres = 220.f;

    // Ocean: shelf depth and shelf + continental slope (V4 and V6).
    UPROPERTY(EditAnywhere, Category="Planet|Terrain Shape", meta=(ClampMin="0.0", ClampMax="2000.0", EditCondition="TerrainShape != EPlanetTerrainShape::Original"))
    float ShelfDepthMetres = 120.f;

    UPROPERTY(EditAnywhere, Category="Planet|Terrain Shape", meta=(ClampMin="100.0", ClampMax="11000.0", EditCondition="TerrainShape != EPlanetTerrainShape::Original"))
    float AbyssDepthMetres = 3820.f;

    // ── Terrain shape V6 ────────────────────────────────────────────────
    // Size of the mountain regions: 1 = fields of ~300 km (warped by 70 km);
    // 0.5 = twice as many, smaller.
    UPROPERTY(EditAnywhere, Category="Planet|Terrain Shape V6", meta=(ClampMin="0.1", ClampMax="5.0", EditCondition="TerrainShape == EPlanetTerrainShape::V6"))
    float V6RegionScale = 1.f;

    // More / fewer mountain regions (0.5 = tuned default).
    UPROPERTY(EditAnywhere, Category="Planet|Terrain Shape V6", meta=(ClampMin="0.0", ClampMax="1.0", EditCondition="TerrainShape == EPlanetTerrainShape::V6"))
    float V6MountainAmount = 0.5f;

    // Relief of the highest ranges above their plain.
    UPROPERTY(EditAnywhere, Category="Planet|Terrain Shape V6", meta=(ClampMin="0.0", ClampMax="20000.0", EditCondition="TerrainShape == EPlanetTerrainShape::V6"))
    float V6MountainMaxHeightMetres = 7000.f;

    // Wavelength of the largest ridged octave: the spacing of the main ranges.
    UPROPERTY(EditAnywhere, Category="Planet|Terrain Shape V6", meta=(ClampMin="5000.0", ClampMax="500000.0", EditCondition="TerrainShape == EPlanetTerrainShape::V6"))
    float V6RidgeWavelengthMetres = 110000.f;

    // How far the ranges are bent (domain warp of the ridged field).
    UPROPERTY(EditAnywhere, Category="Planet|Terrain Shape V6", meta=(ClampMin="0.0", ClampMax="100000.0", EditCondition="TerrainShape == EPlanetTerrainShape::V6"))
    float V6RidgeWarpMetres = 14000.f;

    // Regional uplift (piedmont) around the ranges.
    UPROPERTY(EditAnywhere, Category="Planet|Terrain Shape V6", meta=(ClampMin="0.0", ClampMax="5000.0", EditCondition="TerrainShape == EPlanetTerrainShape::V6"))
    float V6PedestalMetres = 450.f;

    // Exponent on the ridged value: higher = narrower peaks, wider valleys.
    UPROPERTY(EditAnywhere, Category="Planet|Terrain Shape V6", meta=(ClampMin="0.3", ClampMax="4.0", EditCondition="TerrainShape == EPlanetTerrainShape::V6"))
    float V6RidgeSharpness = 1.4f;

    // Share of the relief every mountain area keeps, even in its valleys.
    UPROPERTY(EditAnywhere, Category="Planet|Terrain Shape V6", meta=(ClampMin="0.0", ClampMax="1.0", EditCondition="TerrainShape == EPlanetTerrainShape::V6"))
    float V6RidgeFloor = 0.22f;

    // Ridged octaves: 8 at 110 km reach ~0.9 km.
    UPROPERTY(EditAnywhere, Category="Planet|Terrain Shape V6", meta=(ClampMin="1", ClampMax="10", EditCondition="TerrainShape == EPlanetTerrainShape::V6"))
    int32 V6RidgeOctaves = 8;

    // ── Terrain shape V4 ────────────────────────────────────────────────
    // Size of the plains / hills / mountain regions: 1 = ~220 km, 0.5 = twice as many, smaller.
    UPROPERTY(EditAnywhere, Category="Planet|Terrain Shape V4", meta=(ClampMin="0.1", ClampMax="5.0", EditCondition="TerrainShape == EPlanetTerrainShape::V4"))
    float RegionScale = 1.f;

    // Share of mountain / hill regions and of table mountains (0.5 = tuned default).
    UPROPERTY(EditAnywhere, Category="Planet|Terrain Shape V4", meta=(ClampMin="0.0", ClampMax="1.0", EditCondition="TerrainShape == EPlanetTerrainShape::V4"))
    float MountainAmount = 0.5f;

    UPROPERTY(EditAnywhere, Category="Planet|Terrain Shape V4", meta=(ClampMin="0.0", ClampMax="1.0", EditCondition="TerrainShape == EPlanetTerrainShape::V4"))
    float HillsAmount = 0.5f;

    UPROPERTY(EditAnywhere, Category="Planet|Terrain Shape V4", meta=(ClampMin="0.0", ClampMax="1.0", EditCondition="TerrainShape == EPlanetTerrainShape::V4"))
    float TableAmount = 0.5f;

    // Hills above the plains: low where the peaks & valleys field is low, high on its peaks.
    UPROPERTY(EditAnywhere, Category="Planet|Terrain Shape V4", meta=(ClampMin="0.0", ClampMax="5000.0", EditCondition="TerrainShape == EPlanetTerrainShape::V4"))
    float HillsMinHeightMetres = 180.f;

    UPROPERTY(EditAnywhere, Category="Planet|Terrain Shape V4", meta=(ClampMin="0.0", ClampMax="5000.0", EditCondition="TerrainShape == EPlanetTerrainShape::V4"))
    float HillsMaxHeightMetres = 400.f;

    // Mountain massifs above their base, in valleys / on peaks.
    UPROPERTY(EditAnywhere, Category="Planet|Terrain Shape V4", meta=(ClampMin="0.0", ClampMax="20000.0", EditCondition="TerrainShape == EPlanetTerrainShape::V4"))
    float MountainMinHeightMetres = 600.f;

    UPROPERTY(EditAnywhere, Category="Planet|Terrain Shape V4", meta=(ClampMin="0.0", ClampMax="20000.0", EditCondition="TerrainShape == EPlanetTerrainShape::V4"))
    float MountainMaxHeightMetres = 3200.f;

    // Table-top levels above the base: Min, Min + Step, Min + 2 Step, Min + 3 Step.
    UPROPERTY(EditAnywhere, Category="Planet|Terrain Shape V4", meta=(ClampMin="0.0", ClampMax="10000.0", EditCondition="TerrainShape == EPlanetTerrainShape::V4"))
    float TableMinHeightMetres = 400.f;

    UPROPERTY(EditAnywhere, Category="Planet|Terrain Shape V4", meta=(ClampMin="0.0", ClampMax="5000.0", EditCondition="TerrainShape == EPlanetTerrainShape::V4"))
    float TableLevelStepMetres = 600.f;

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
    // Both in metres. These replace DetailFrequency / DetailAmplitude, whose
    // amplitude was secretly scaled by 0.05 and whose frequency put the
    // finest detail at ~2 km. New names on purpose: a value saved under the
    // old name is ignored instead of silently meaning metres.

    // Wavelength of the largest detail octave on the surface. Each further
    // octave is 2.2x smaller (2000 m, 8 octaves -> finest ~8 m).
    UPROPERTY(EditAnywhere, Category="Planet|Detail",
        meta=(ClampMin="10.0", ClampMax="200000.0"))
    float DetailWavelengthMetres = 2000.0f;

    UPROPERTY(EditAnywhere, Category="Planet|Detail",
        meta=(ClampMin="1", ClampMax="16"))
    int32 DetailOctaves = 8;

    // Height of a full-scale detail feature. 40 m gives roughly half the
    // local relief at walking scale and ~13% at 2 km.
    UPROPERTY(EditAnywhere, Category="Planet|Detail",
        meta=(ClampMin="0.0", ClampMax="2000.0"))
    float DetailAmplitudeMetres = 40.0f;

    // ── Seed ────────────────────────────────────────────────────────────
    UPROPERTY(EditAnywhere, Category="Planet|Seed")
    int32 MasterSeed = 1337;

    // On: continents are placed as Planet Noise Lab shows them on its map
    // (the lab's pole is the noise Y axis; the game's poles are on world Z).
    // Off: the original placement. Shapes are the same either way.
    UPROPERTY(EditAnywhere, Category="Planet|Seed")
    bool bNoiseLabOrientation = false;

    // Fraction of the surface that should end up above sea level.
    // The sea level is derived from this by quantile, so no seed is ever
    // "all ocean" or "no water" unless asked for.
    UPROPERTY(EditAnywhere, Category="Planet|Seed", meta=(ClampMin="0.05", ClampMax="0.95", EditCondition="bAutoSeaLevel"))
    float TargetLandFraction = 0.30f;

    // On: the sea level is chosen so that TargetLandFraction of the surface
    // is land. Off: SeaLevel is used as is (continent-noise value), which
    // keeps the exact coastlines of a hand-picked setting.
    UPROPERTY(EditAnywhere, Category="Planet|Seed")
    bool bAutoSeaLevel = true;

    UPROPERTY(EditAnywhere, Category="Planet|Seed", meta=(ClampMin="-1.0", ClampMax="1.0", EditCondition="!bAutoSeaLevel"))
    float SeaLevel = 0.0f;

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

// ── Erosion (GPU terrain renderer only) ─────────────────────────────────────
// Sphere port of the "Fast & Gorgeous" erosion filter (runevision, MPL 2.0):
// gullies that run down the slopes and branch, computed per chunk tile by a
// compute shader (PlanetErosion plugin). Mapped to PlanetErosion::Params by
// PlanetBridge::ToCoreErosion. Defaults were tuned in the harness for
// mountains of ~5-10 km; with much higher mountains raise ScaleMetres.
// Terrain shapes V4 / V6: erosion acts on the mountains (and V4's table
// escarpments) only; original terrain: on all land. V6 was tuned in the
// terrain lab with Strength 0.12 and HeightOffset -0.1 (ScaleMetres 4000,
// 8 octaves).
USTRUCT(BlueprintType)
struct FPlanetErosionSettings
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, Category="Erosion")
    bool bEnabled = true;

    // Width (and roughly depth) of the largest gullies, metres. Each further
    // octave is half as wide.
    UPROPERTY(EditAnywhere, Category="Erosion", meta=(ClampMin="200.0", ClampMax="50000.0"))
    float ScaleMetres = 4000.f;

    // Gully depth relative to ScaleMetres. 0.06 x 4000 m = ~240 m for the
    // largest octave. The main knob for how steep the mountains look: 0.06
    // keeps 90 % of mountain slopes under ~15 deg, 0.12 doubles that.
    UPROPERTY(EditAnywhere, Category="Erosion", meta=(ClampMin="0.0", ClampMax="1.0"))
    float Strength = 0.06f;

    // Each octave halves the gully width: 8 octaves at 4000 m reach ~31 m
    // gullies, the scale you see on foot. GPU cost is negligible either way.
    UPROPERTY(EditAnywhere, Category="Erosion", meta=(ClampMin="1", ClampMax="8"))
    int32 Octaves = 8;

    // 0 = only sharpen ridges, 1 = full gullies.
    UPROPERTY(EditAnywhere, Category="Erosion", meta=(ClampMin="0.0", ClampMax="1.0"))
    float GullyWeight = 0.5f;

    // Lower = fine gullies only on steep slopes; higher = fine gullies also
    // on gentle slopes and between the large ones.
    UPROPERTY(EditAnywhere, Category="Erosion", meta=(ClampMin="0.1", ClampMax="4.0"))
    float Detail = 2.0f;

    // -1 = erosion only lowers the terrain, +1 = only raises it. -0.2 keeps
    // valley floors near their height (-0.5 dug them below the sea: lakes).
    UPROPERTY(EditAnywhere, Category="Erosion", meta=(ClampMin="-1.0", ClampMax="1.0"))
    float HeightOffset = -0.2f;

    // Step of the slope measurement that steers the gullies, as a fraction of
    // ScaleMetres. Small values follow every small ridge and look noisy.
    UPROPERTY(EditAnywhere, Category="Erosion", meta=(ClampMin="0.05", ClampMax="1.0"))
    float GradientStepFraction = 0.25f;

    UPROPERTY(EditAnywhere, Category="Erosion")
    int32 Seed = 0;

    // Slope (m/m) below which the gullies fade out near crests. Keeps ridge
    // lines clean: without it the gullies of the two sides meet on the
    // crest in a comb of teeth. 0 = the original filter; 0.3 rounds summits.
    UPROPERTY(EditAnywhere, Category="Erosion", meta=(ClampMin="0.0", ClampMax="1.0"))
    float CrestSoftness = 0.15f;
};

// ── Biomes (GPU terrain renderer) ───────────────────────────────────────────
// Climate and biome model of planet_core/PlanetBiomes.h, mapped by
// PlanetBridge::ToCoreBiomes. Ten Earth biomes from temperature and moisture;
// the material blends eight surface layers (Sand, DryGrass, Grass, Forest,
// Jungle, Tundra, Snow, Rock). Defaults for the V4 terrain (Earth-like heights).
USTRUCT(BlueprintType)
struct FPlanetBiomeSettings
{
    GENERATED_BODY()

    // Sea-level temperature at the equator and at the poles, deg C.
    UPROPERTY(EditAnywhere, Category="Biomes|Climate", meta=(ClampMin="-50.0", ClampMax="60.0"))
    float EquatorTempC = 28.f;

    UPROPERTY(EditAnywhere, Category="Biomes|Climate", meta=(ClampMin="-80.0", ClampMax="40.0"))
    float PoleTempC = -28.f;

    // Cooling per km of altitude. Earth's 6.5 suits the V4 terrain; the
    // original terrain (relief ~10x Earth's) needs ~3 or its highlands freeze.
    UPROPERTY(EditAnywhere, Category="Biomes|Climate", meta=(ClampMin="0.0", ClampMax="10.0"))
    float LapseCPerKm = 6.5f;

    // Share of the continental plateau height ignored for temperature: 0 =
    // highlands freeze, 1 = only mountains above the plateau get colder.
    UPROPERTY(EditAnywhere, Category="Biomes|Climate", meta=(ClampMin="0.0", ClampMax="1.0"))
    float PlateauDiscount = 0.7f;

    UPROPERTY(EditAnywhere, Category="Biomes|Climate", meta=(ClampMin="0.0", ClampMax="20.0"))
    float TempNoiseC = 5.f;

    // Share of noise in moisture (the rest: latitude bands, wet at 0 and 60
    // deg, dry at 30 and 90 deg).
    UPROPERTY(EditAnywhere, Category="Biomes|Climate", meta=(ClampMin="0.0", ClampMax="1.0"))
    float MoistureNoise = 0.45f;

    // Moisture lost deep inland (deserts in continental interiors).
    UPROPERTY(EditAnywhere, Category="Biomes|Climate", meta=(ClampMin="0.0", ClampMax="1.0"))
    float InlandDryness = 0.25f;

    UPROPERTY(EditAnywhere, Category="Biomes|Surface", meta=(ClampMin="0.0", ClampMax="500.0"))
    float BeachHeightMetres = 25.f;

    UPROPERTY(EditAnywhere, Category="Biomes|Surface", meta=(ClampMin="0.0", ClampMax="5000.0"))
    float SwampMaxHeightMetres = 300.f;

    // Slope (1 - cos of the angle) where rock starts and where it is full:
    // 0.30 ~ 46 deg, 0.55 ~ 63 deg.
    UPROPERTY(EditAnywhere, Category="Biomes|Surface", meta=(ClampMin="0.0", ClampMax="1.0"))
    float RockSlopeStart = 0.30f;

    UPROPERTY(EditAnywhere, Category="Biomes|Surface", meta=(ClampMin="0.0", ClampMax="1.0"))
    float RockSlopeFull = 0.55f;

    // Flats colder than this are snow-covered (mountain tops, polar ice).
    UPROPERTY(EditAnywhere, Category="Biomes|Surface", meta=(ClampMin="-60.0", ClampMax="20.0"))
    float SnowTempC = -10.f;
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
    Mix(&DetailWavelengthMetres, sizeof(DetailWavelengthMetres));
    Mix(&DetailOctaves,          sizeof(DetailOctaves));
    Mix(&DetailAmplitudeMetres,  sizeof(DetailAmplitudeMetres));
    Mix(&MasterSeed,           sizeof(MasterSeed));
    Mix(&TargetLandFraction,   sizeof(TargetLandFraction));
    Mix(&ContinentOctaves,     sizeof(ContinentOctaves));
    Mix(&ContinentGain,        sizeof(ContinentGain));
    Mix(&ContinentLacunarity,  sizeof(ContinentLacunarity));
    Mix(&bAutoSeaLevel,        sizeof(bAutoSeaLevel));
    Mix(&SeaLevel,             sizeof(SeaLevel));
    Mix(&bNoiseLabOrientation, sizeof(bNoiseLabOrientation));
    Mix(&TerrainShape,            sizeof(TerrainShape));
    Mix(&RegionScale,             sizeof(RegionScale));
    Mix(&MountainAmount,          sizeof(MountainAmount));
    Mix(&HillsAmount,             sizeof(HillsAmount));
    Mix(&TableAmount,             sizeof(TableAmount));
    Mix(&LowlandHeightMetres,     sizeof(LowlandHeightMetres));
    Mix(&InteriorRiseMetres,      sizeof(InteriorRiseMetres));
    Mix(&HillsMinHeightMetres,    sizeof(HillsMinHeightMetres));
    Mix(&HillsMaxHeightMetres,    sizeof(HillsMaxHeightMetres));
    Mix(&MountainMinHeightMetres, sizeof(MountainMinHeightMetres));
    Mix(&MountainMaxHeightMetres, sizeof(MountainMaxHeightMetres));
    Mix(&TableMinHeightMetres,    sizeof(TableMinHeightMetres));
    Mix(&TableLevelStepMetres,    sizeof(TableLevelStepMetres));
    Mix(&ShelfDepthMetres,        sizeof(ShelfDepthMetres));
    Mix(&AbyssDepthMetres,        sizeof(AbyssDepthMetres));
    Mix(&V6RegionScale,             sizeof(V6RegionScale));
    Mix(&V6MountainAmount,          sizeof(V6MountainAmount));
    Mix(&V6MountainMaxHeightMetres, sizeof(V6MountainMaxHeightMetres));
    Mix(&V6RidgeWavelengthMetres,   sizeof(V6RidgeWavelengthMetres));
    Mix(&V6RidgeWarpMetres,         sizeof(V6RidgeWarpMetres));
    Mix(&V6PedestalMetres,          sizeof(V6PedestalMetres));
    Mix(&V6RidgeSharpness,          sizeof(V6RidgeSharpness));
    Mix(&V6RidgeFloor,              sizeof(V6RidgeFloor));
    Mix(&V6RidgeOctaves,            sizeof(V6RidgeOctaves));
    return H ^ (uint64)PLANET_GENERATOR_VERSION;
}


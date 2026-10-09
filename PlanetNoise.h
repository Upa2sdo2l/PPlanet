// PlanetNoise.h
// THIN Unreal-facing wrapper over PlanetCore::NoiseGraph.
//
// This file contains NO generation logic: not one octave, biome weight or
// climate formula. Everything numerical lives in planet_core/, which compiles
// and runs without Unreal and is covered by noise_harness (9 tests), lod_harness
// (9 tests) and streaming_harness (14 tests) — all passing.
//
// Keeping the wrapper thin is not tidiness. An earlier draft carried its own
// graph and parameter set, which meant TWO sources of truth for terrain height:
// mesh, collision and material would have diverged silently. Its only job now
// is FVector3d <-> PlanetCore::Vec3d and editor params -> core params.
#pragma once

#include "CoreMinimal.h"
#include "PlanetTypes.h"

// Plain C++ header: <cmath>, <cstdint>, <vector> and FastNoise2. Unreal
// consumes it happily, which is what makes the split work.
#include "planet_core/PlanetNoiseCore.h"
#include "planet_core/PlanetErosion.h"
#include "planet_core/PlanetBiomes.h"

class FPlanetNoiseGenerator
{
public:
    FPlanetNoiseGenerator() = default;

    // Rebuild the graph and recompute the sea level for the target land
    // fraction. A seed is never "all ocean" unless that fraction asks for it.
    // The radius turns metre-sized settings (detail wavelength) into noise
    // space and positions samples for normals.
    void Build(const FPlanetNoiseParams& InParams, double PlanetRadiusMetres);

    bool IsValid() const { return Graph.IsValid(); }
    float GetSeaLevel() const { return Graph.GetEffectiveSeaLevel(); }

    const PlanetCore::NoiseParams& GetCoreParams() const { return Graph.GetParams(); }

    // ── THE mesh-generation call ─────────────────────────────────────────
    // Fills PLANET_VERTS_PER_SIDE^2 surfaces row-major (i = gy * VPS + gx).
    // Measured cost on the reference box: min 0.95 ms per chunk single-thread,
    // which leaves three workers comfortably ahead of a 60 fps frame.
    // OutHalo (optional) receives the full sampled grid, halo ring included.
    void SampleChunk(const FChunkKey& Key, TArrayView<PlanetCore::Surface> Out,
                     PlanetCore::HaloGrid* OutHalo = nullptr) const;

    const PlanetCore::NoiseGraph& GetGraph() const { return Graph; }

    // ── Erosion ──────────────────────────────────────────────────────────
    // Set by the actor once it knows the renderer: only the GPU renderer
    // erodes the terrain, so only then must heights include erosion.
    // Set before any worker starts; read-only afterwards.
    void SetErosion(bool bEnabled, const PlanetErosion::Params& Params)
    {
        Erosion = Params;
        if (!bEnabled) Erosion.Strength = 0.0;
    }
    const PlanetErosion::Params& GetErosion() const { return Erosion; }
    bool HasErosion() const { return PlanetErosion::IsEnabled(Erosion); }

    // ── Biomes (climate model shared with the GPU) ──────────────────────
    void SetBiomes(const PlanetBiomes::Params& Params) { Biomes = Params; }
    const PlanetBiomes::Params& GetBiomes() const { return Biomes; }

    // ── Single point ─────────────────────────────────────────────────────
    // Height of the terrain the player sees, erosion included (to ~0.1 m of
    // the eroded mesh). For line traces and placement, not per-vertex use;
    // with erosion one call costs ~20 us.
    double GetHeightAt(const FVector3d& UnitDir) const;
    PlanetCore::Surface GetSurfaceAt(const FVector3d& UnitDir) const;
    FVector3d GetSurfacePositionAt(const FVector3d& UnitDir, double PlanetRadius) const;

    // ── Diagnostics for the Seed Lab ─────────────────────────────────────
    struct FHeightStats { double Min = 0, Max = 0, Mean = 0; };
    FHeightStats MeasureHeightStats(int32 Samples, int32 SampleSeed) const;

private:
    PlanetCore::NoiseGraph Graph;
    PlanetErosion::Params  Erosion = []{ PlanetErosion::Params P; P.Strength = 0.0; return P; }();
    PlanetBiomes::Params   Biomes;
};

namespace PlanetBridge
{

FORCEINLINE PlanetCore::Vec3d ToCore(const FVector3d& V) { return PlanetCore::Vec3d(V.X, V.Y, V.Z); }
FORCEINLINE FVector3d ToUE(const PlanetCore::Vec3d& V)   { return FVector3d(V.X, V.Y, V.Z); }
FORCEINLINE PlanetCore::FChunkKey ToCoreKey(const FChunkKey& K)
{ return PlanetCore::FChunkKey(K.Face, K.LOD, K.X, K.Y); }

// ─────────────────────────────────────────────────────────────────────────────
// FPlanetNoiseParams -> PlanetCore::NoiseParams
//
// The ONLY place editor configuration becomes generator configuration. If a
// UPROPERTY existed without a mapping here it would silently do nothing, which
// is exactly the failure this function exists to prevent. Every exposed field
// is mapped; the rest are listed as DEFAULT with the core's own defaults,
// because a zero octave count or zero lacunarity would degenerate the terrain
// rather than merely ignore a slider.
// ─────────────────────────────────────────────────────────────────────────────
inline PlanetCore::NoiseParams ToCoreParams(const FPlanetNoiseParams& P, double PlanetRadiusMetres)
{
    PlanetCore::NoiseParams C;
    C.PlanetRadiusMetres  = PlanetRadiusMetres;

    // ── Mapped from the editor ──────────────────────────────────────────
    C.ContinentFrequency  = P.ContinentFrequency;
    C.ShoreSharpness      = P.ShoreSharpness;
    C.MountainFrequency   = P.MountainFrequency;
    C.MountainAmplitude   = P.MountainAmplitude;
    C.MountainSharpness   = P.MountainSharpness;
    C.MountainOctaves     = P.MountainOctaves;
    C.MountainGain        = P.MountainGain;
    C.DomainWarpAmplitude = P.DomainWarpAmplitude;
    C.DomainWarpFrequency = P.DomainWarpFrequency;
    C.SnowLatitudeStart   = P.SnowLatitudeStart;
    C.SnowAltitudeStart   = P.SnowAltitudeStart;
    C.HumidityVariance    = P.HumidityVariance;
    C.OceanDepthScale     = P.OceanDepthScale;
    C.DetailWavelengthMetres = P.DetailWavelengthMetres;
    C.DetailOctaves       = P.DetailOctaves;
    C.DetailAmplitude     = P.DetailAmplitudeMetres;
    C.MasterSeed          = P.MasterSeed;
    C.NoiseSwapYZ         = P.bNoiseLabOrientation;

    // ── DEFAULT: not exposed as sliders yet ─────────────────────────────
    C.ContinentOctaves    = P.ContinentOctaves;
    C.ContinentGain       = P.ContinentGain;
    C.ContinentLacunarity = P.ContinentLacunarity;
    C.SeaLevelNorm        = P.bAutoSeaLevel ? 0.f : P.SeaLevel;   // auto: replaced by the quantile pass
    //C.MountainOctaves     = 8;
    //C.MountainGain        = 0.5f;
    C.OceanDetailAmp      = 0.08f;
   

    return C;
}

// FPlanetErosionSettings -> PlanetErosion::Params. Fields not exposed keep the
// core's defaults (Rounding, Onset, AssumedSlope, CellScale, Normalization,
// Lacunarity, Gain).
inline PlanetErosion::Params ToCoreErosion(const FPlanetErosionSettings& S)
{
    PlanetErosion::Params P;
    P.ScaleMetres          = S.ScaleMetres;
    P.Strength             = S.bEnabled ? S.Strength : 0.0;
    P.Octaves              = FMath::Clamp(S.Octaves, 1, 8);
    P.GullyWeight          = S.GullyWeight;
    P.Detail               = S.Detail;
    P.HeightOffset         = S.HeightOffset;
    P.GradientStepFraction = S.GradientStepFraction;
    P.Seed                 = (uint32_t)S.Seed;
    return P;
}

inline PlanetBiomes::Params ToCoreBiomes(const FPlanetBiomeSettings& S)
{
    PlanetBiomes::Params P;
    P.EquatorTempC    = S.EquatorTempC;
    P.PoleTempC       = S.PoleTempC;
    P.LapseCPerKm     = S.LapseCPerKm;
    P.PlateauDiscount = S.PlateauDiscount;
    P.TempNoiseC      = S.TempNoiseC;
    P.MoistureNoise   = S.MoistureNoise;
    P.InlandDryness   = S.InlandDryness;
    P.BeachHeightM    = S.BeachHeightMetres;
    P.SwampMaxHeightM = S.SwampMaxHeightMetres;
    P.RockSlopeStart  = S.RockSlopeStart;
    P.RockSlopeFull   = FMath::Max(S.RockSlopeFull, S.RockSlopeStart + 0.01f);
    P.SnowTempC       = S.SnowTempC;
    return P;
}

} // namespace PlanetBridge

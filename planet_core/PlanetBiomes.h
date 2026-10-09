// PlanetBiomes.h
// UE-FREE climate and biome model (CPU reference). The GPU twin lives in
// GpuErosion/Shaders/PlanetErosionCommon.ush (PlanetBio_*) and is run against
// this file in the harness.
//
// Climate per point:
//   Temperature (deg C) = linear in cos(latitude) from the poles to the
//                         equator, minus a lapse rate per km of altitude,
//                         plus low-frequency noise.
//   Moisture (0..1)     = circulation bands (wet equator and 60 deg, dry 30
//                         deg and poles) mixed with humidity noise, drier
//                         deep inland (high continent value).
//
// Ten biomes from (temperature, moisture), Whittaker-style, with soft edges:
//   Ice, Tundra, Taiga, TemperateForest, Grassland, Desert, Savanna,
//   Rainforest, Swamp (warm, wet, low and flat), Beach (just above sea level).
//
// What the terrain material draws are eight SURFACE LAYERS, not biomes:
//   Sand, DryGrass, Grass, Forest, Jungle, Tundra, Snow, Rock
// Each biome is a fixed mix of layers; on top, steep slopes turn to rock and
// cold flats to snow. Layer weights are smooth, so bilinear filtering works;
// biomes are for gameplay and vegetation (BiomeAt gives the dominant one).
#pragma once

#include <cstdint>

namespace PlanetBiomes
{

enum EBiome : int32_t
{
    Biome_Ice = 0, Biome_Tundra, Biome_Taiga, Biome_TemperateForest, Biome_Grassland,
    Biome_Desert, Biome_Savanna, Biome_Rainforest, Biome_Swamp, Biome_Beach,
    Biome_COUNT
};

enum ELayer : int32_t
{
    Layer_Sand = 0, Layer_DryGrass, Layer_Grass, Layer_Forest,
    Layer_Jungle, Layer_Tundra, Layer_Snow, Layer_Rock,
    Layer_COUNT
};

struct Params
{
    double EquatorTempC     = 28.0;   // sea level, equator
    double PoleTempC        = -28.0;  // sea level, poles
    // Degrees per km of altitude. Earth is ~6.5, but this planet's relief is
    // ~10x Earth's (median land 4.8 km with 10 km mountains).
    double LapseCPerKm      = 3.0;
    // Share of the continental plateau (the continent layer's own height, not
    // the mountains) that does NOT count as altitude for temperature. With 0
    // every highland freezes; with 0.7 plateaus keep their latitude's biome
    // and only the mountains on them get cold and snowy.
    double PlateauDiscount  = 0.7;
    double TempNoiseC       = 5.0;    // amplitude of the temperature noise
    double MoistureNoise    = 0.45;   // share of noise in moisture (rest: latitude bands)
    double InlandDryness    = 0.25;   // moisture lost deep inland
    double BeachHeightM     = 25.0;   // sand below this height above sea
    double SwampMaxHeightM  = 300.0;  // swamps only in low, flat, wet places
    double RockSlopeStart   = 0.30;   // slope = 1 - cos(angle): 0.30 ~ 46 deg
    double RockSlopeFull    = 0.55;   //                          0.55 ~ 63 deg
    double SnowTempC        = -10.0;  // flats colder than this are snow-covered (tundra stays bare above it)
};

struct Climate
{
    double TemperatureC = 0.0;
    double Moisture     = 0.0;
};

// SinLat: Up.z (polar axis is +Z). HeightM: above sea level (negative under
// water). HumidityRaw, TempNoise: the noise layers, about -1..1. Inland:
// continent value above sea level (0 at the coast).
Climate ClimateAt(double SinLat, double HeightM, double HumidityRaw, double TempNoise,
                  double Inland, const Params& P);

// Soft biome weights, sum 1.
void BiomeWeights(const Climate& C, double HeightM, double Slope, const Params& P,
                  double Out[Biome_COUNT]);

// Surface layer weights, sum 1 (what the material blends).
void LayerWeights(const Climate& C, double HeightM, double Slope, const Params& P,
                  double Out[Layer_COUNT]);

// Dominant biome (vegetation, gameplay).
EBiome BiomeAt(const Climate& C, double HeightM, double Slope, const Params& P);

} // namespace PlanetBiomes

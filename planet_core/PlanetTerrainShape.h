// PlanetTerrainShape.h
// ─────────────────────────────────────────────────────────────────────────────
// UE-FREE. Terrain shape "V4", Minecraft-style: a few broad noise fields
// mapped to height by hand-tuned curves.
//
//   continentalness  the continent layer (land / sea, coast to interior)
//   erosion          ~220 km regions: low = mountains, middle = hills, high = plains
//   peaks & valleys  |weirdness| folded (Minecraft's PV): massifs on the peaks,
//                    valleys between them; bigger hills where it is high
//   plateau          where table mountains stand (flat tops at 4 levels, an
//                    escarpment with a talus apron, a rare second tier)
//
// Plains are flat with gentle swells; hills 180-400 m; mountains 600-3200 m
// above their base with soft ridged crests; table tops 400-2200 m. Erosion acts
// on the mountains and the table escarpments only.
//
// Tuned in the terrain lab (scratchpad terrain/lab4.cpp); the harness checks
// this file against it.
#pragma once

#include "PlanetNoiseCore.h"

namespace PlanetTerrainShape
{

// Noise values of one point. Std-normalised fields (divided by their measured
// standard deviation) except Above (continent value minus sea level) and
// Peaks (soft ridged, -1..1).
struct Layers
{
    float Above = 0.f;
    float Erosion = 0.f, Weird = 0.f, Plateau = 0.f, Level = 0.f, Mass = 0.f, Hills = 0.f;
    float Peaks = 0.f, Detail = 0.f;
};

// Regime weights of a point (0..1); computed first, so the caller knows which
// layers a batch needs (Peaks: Mountain > 0; Level and Mass: Table > 0).
struct Regimes
{
    float Land = 0.f;       // Above >= 0
    float Coast = 0.f;      // fades the regimes in a little inland
    float Mountain = 0.f, Hill = 0.f, Table = 0.f;
};

Regimes RegimesAt(const Layers& L, double LandScale, const PlanetCore::ShapeParams& P);

struct Result
{
    double Height = 0.0;       // metres above the sea
    float  Inland = 0.f;       // base height / 12000 on land (biome climate), Above at sea
    float  ErosionMask = 0.f;  // 0..1: mountains and table escarpments
    float  ErosionFade = 0.f;  // -1 valleys .. +1 peaks (small: see the .cpp)
};

Result Compose(const Layers& L, const Regimes& Rg, double LandScale, const PlanetCore::ShapeParams& P);

// Soft ridged value of one octave sample n (-1..1): 1 - 2 sqrt(n^2 + e^2).
// The plain |n| fold of FastNoise's ridged fractal leaves a sharp, nearly
// straight crease along n = 0.
inline float SoftRidge(float N) { return 1.f - 2.f * std::sqrt(N * N + 0.0064f); }

constexpr int32_t PEAK_OCTAVES = 5;

} // namespace PlanetTerrainShape

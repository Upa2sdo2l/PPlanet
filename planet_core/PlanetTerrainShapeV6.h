// PlanetTerrainShapeV6.h
// ─────────────────────────────────────────────────────────────────────────────
// UE-FREE. Terrain shape "V6": continuous mountain belts instead of massifs.
//
//   regions   a low-frequency field (~300 km) under a strong domain warp
//             (70 km on a 500 km wave), plus "tectonic activity" (~1200 km):
//             where it is high, mountain country; where it is low, plains
//   ranges    ridged multifractal (Musgrave) on a 110 km wave with a moderate
//             warp (14 km on a 90 km wave): ranges with main summits and a
//             common base, not separate bumps
//   piedmont  a regional uplift (pedestal) around the ranges
//   plains    base height rising inland, swells, hills and hummocks whose
//             amplitude follows a "roughness" field (~150 km)
//
// Tuned in the terrain lab (scratchpad terrain/lab6.cpp, mode 1); the harness
// checks this port against it. No table mountains (V4 has them).
//
// ── Large and small scale ────────────────────────────────────────────────────
// The future global planet map stores, per planet, everything coarser than
// ~10 km; chunks then add only the small scale. The fields are already split
// that way (see EvaluateTerrainV6):
//   large  continent, roughness, regions (with their warp), activity, the
//          ridge warp, and the first V6_LARGE_RIDGE_OCTAVES ridged octaves as
//          a running (sum, weight) pair; also the uplift and the plain level
//          the large-scale erosion will need (Amp and plain height)
//   small  swells, hills, hummocks, detail and the remaining ridged octaves
#pragma once

#include "PlanetNoiseCore.h"

namespace PlanetTerrainShapeV6
{

// Indices of NoiseGraph::V6Field (and V6Sd, their std devs).
enum EField : int32_t
{
    V6_Rough = 0,       // plains roughness, 150 km
    V6_Swell,           // swells, 12 km
    V6_Hills,           // hills, 3 km (also the sea floor)
    V6_Hummock,         // hummocks, 400 m
    V6_RegionF,         // mountain regions, 300 km (warped)
    V6_RegionG,         // segments along the regions, 240 km (warped)
    V6_Activity,        // tectonic activity, 1200 km
    V6_RegionWarpX, V6_RegionWarpY, V6_RegionWarpZ,   // 500 km
    V6_RidgeWarpX, V6_RidgeWarpY, V6_RidgeWarpZ,      // 90 km (scaled with the ridge wavelength)
    V6_FIELD_COUNT
};
static_assert(V6_FIELD_COUNT == PlanetCore::NoiseGraph::V6_FIELDS_PUBLIC, "field count");

// Ridged octaves with a wavelength of ~10 km and more (110, 55, 27.5, 13.75 km
// at the default wavelength): the part of the ranges the global map will hold.
constexpr int32_t V6_LARGE_RIDGE_OCTAVES = 4;

// Smallest ridged wavelength (metres) in the base height that steers the
// erosion filter: the steering gradient must be low-pass (PlanetErosion.h),
// so finer octaves only cost time there.
constexpr double V6_BASE_MIN_RIDGE_WAVELENGTH = 2000.0;

// Region thresholds of the default MountainAmount (0.5).
constexpr double V6_REGION_T0 = 0.0, V6_REGION_T1 = 0.9;
constexpr double V6_ACTIVITY  = 0.8;    // weight of the activity field in the regions
constexpr double V6_REGION_WARP_METRES = 70000.0;

} // namespace PlanetTerrainShapeV6

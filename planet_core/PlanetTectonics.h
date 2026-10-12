// PlanetTectonics.h
// ─────────────────────────────────────────────────────────────────────────────
// UE-FREE. The "V2+" terrain shape: Earth-like heights and mountain ranges
// where tectonic plates meet.
//
//   * Plates are seeded in continent interiors (continental plates) and ocean
//     basins (oceanic plates), so plate boundaries run along the continental
//     margins. Each plate rotates about its own Euler pole; most oceanic
//     plates move towards the nearest continent (subduction).
//   * What grows at a boundary depends on the two plates and their relative
//     motion:
//       continent | continent, converging: collision range + highlands (Himalaya)
//       continent | ocean,     converging: coastal range inland (Andes) + plateau
//       ocean     | continent, converging: trench
//       ocean     | ocean,     converging: island arc on the overriding plate, trench
//       continent | continent, diverging : rift valley
//       ocean     | ocean,     diverging : mid-ocean ridge with a wide flank
//       continent | ocean,     diverging : passive margin (nothing: Atlantic)
//   * Land height follows an Earth-like hypsometric curve of the continent
//     layer; plains are smooth, hills only where a low-frequency regime says
//     so; mountains only inside range masks. The ocean has a shelf, a
//     continental slope and an abyssal plain.
//
// Prototyped and tuned in the terrain lab (scratchpad terrain/lab.cpp,
// "v3"); the harness checks this file against it.
//
// Every random choice uses std::mt19937 (standardised) and hand-written
// distributions (std:: distributions differ between standard libraries), so
// the same seed gives the same plates with MSVC and GCC.
#pragma once

#include "PlanetNoiseCore.h"

namespace PlanetTectonics
{

using PlanetCore::Vec3d;

struct Plates
{
    std::vector<Vec3d>   Centre;        // unit vectors
    std::vector<Vec3d>   Velocity;      // Euler pole * angular speed
    std::vector<uint8_t> Continental;

    // Pts: sample directions (Fibonacci sphere); Above: continent value
    // minus sea level at each.
    void Build(uint32_t Seed, const std::vector<Vec3d>& Pts, const std::vector<float>& Above,
               const PlanetCore::TectonicParams& P);

    int32_t Num() const { return (int32_t)Centre.size(); }

    // For the plate p lies in (Ids[0]) and its next two neighbours (Ids[1],
    // Ids[2]): distance in metres to the two boundaries and the convergence
    // across each (-1..1, + = converging). Two boundaries, so features stay
    // continuous where the second-nearest plate changes.
    void Query(const Vec3d& p, double RadiusMetres, double OutDist[2], double OutConv[2], int32_t OutIds[3]) const;

    // The plates that can be among the three nearest of any of the Count
    // points (ascending index): a cap around the points, bounds by angle.
    // BoundaryAt over these gives exactly the result over all plates.
    void Candidates(const Vec3d* Points, int32_t Count, std::vector<int32_t>& Out) const;
};

// Masks of the boundary features at a (warped) point, 0..1.
struct Boundary
{
    float Range = 0, Foot = 0, Plateau = 0, Rift = 0, Arc = 0;
    float Ridge = 0, Trench = 0, Flank = 0;
};

// Cand / NCand: Plates::Candidates for a batch containing q (null: all plates).
Boundary BoundaryAt(const Plates& Pl, const Vec3d& q, double RadiusMetres, const PlanetCore::TectonicParams& P,
                    const int32_t* Cand = nullptr, int32_t NCand = 0);

// Noise layers of one point; Above = continent value minus sea level.
struct Layers
{
    float Above = 0, Mountain = 0, Detail = 0;
    float Uplift = 0, Hills = 0, HillRegime = 0, OldRanges = 0, Alpine = 0;
};

struct Result
{
    double Height = 0.0;       // metres above the sea
    float  Inland = 0.f;       // broad plateau height / 12000 on land (biome climate), Above at sea
    float  ErosionMask = 0.f;  // 0..1: only mountains erode
    float  ErosionFade = 0.f;  // -1 valleys .. +1 peaks
};

Result Compose(const Layers& L, const Boundary& B, double LandScale, const PlanetCore::TectonicParams& P);

// Compose reads the alpine layer only where this is true; elsewhere it
// need not be evaluated.
inline bool NeedsAlpine(const Boundary& B) { return B.Range > 0.f || B.Arc > 0.f; }

} // namespace PlanetTectonics

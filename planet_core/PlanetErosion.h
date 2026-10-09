// PlanetErosion.h
// UE-FREE port of the "Advanced Terrain Erosion Filter" (stacked faded
// gullies over Phacelle noise) to a sphere.
//
//   Erosion filter and Phacelle noise copyright (c) 2025 Rune Skovbo Johansen.
//   This Source Code Form is subject to the terms of the Mozilla Public
//   License, v. 2.0. If a copy of the MPL was not distributed with this
//   file, You can obtain one at https://mozilla.org/MPL/2.0/.
//   Original: https://blog.runevision.com/2026/03/fast-and-gorgeous-erosion-filter.html
//
// Because this file is a derivative of MPL-2.0 code, it stays under MPL 2.0
// (file-level copyleft: changes to THIS file must remain open; the rest of the
// project is not affected). Keep erosion code in this file / its shader twin.
//
// ── Changes from the 2D original ────────────────────────────────────────────
// * Phacelle noise runs on a 3D lattice (4x4x4 cells) around the point on
//   the sphere, and the stripe direction is taken in the sphere's tangent
//   plane. A 2D version per cube face would tear at the 12 cube edges.
// * Units are metres: positions, heights and the erosion scale. Slopes are
//   dimensionless (m/m), as in the original.
// * The gradient that steers the gullies is supplied by the caller and must
//   not depend on mesh resolution, or gullies would turn when the LOD changes.
// * All octaves at every LOD (see Erode): chunk edges of different LOD
//   must agree exactly.
// * The hash is an integer one (pcg3d), so the GPU twin can match it.
#pragma once

#include "PlanetNoiseCore.h"

namespace PlanetErosion
{

// Defaults tuned on this planet (R 2500 km, mountains 10 km) in the harness:
// the demo values (scale 2.5 km, strength 0.22) overwhelm the gentle relief
// and turn it into noise.
struct Params
{
    double ScaleMetres     = 4000.0;  // horizontal and vertical scale of the largest gullies
    double Strength        = 0.12;    // gully magnitude relative to the scale
    double GullyWeight     = 0.5;     // 0 = sharpen ridges only, 1 = full gullies
    double Detail          = 1.0;     // lower = fine gullies only on steep slopes
    double Rounding[4]     = {0.1, 0.0, 0.1, 2.0};   // ridges, creases, input multiplier, per-octave multiplier
    double Onset[4]        = {1.25, 1.25, 2.8, 1.5}; // input, octave, ridge-map input, ridge-map octave
    double AssumedSlope[2] = {0.7, 1.0};             // value, how much it overrides the real slope
    double CellScale       = 0.7;
    double Normalization   = 0.5;
    int    Octaves         = 5;
    double Lacunarity      = 2.0;
    double Gain            = 0.5;
    double HeightOffset    = -0.5;    // -1 only lowers, +1 only raises (times the total magnitude)
    uint32_t Seed          = 0;

    // Step of the central difference that gives the steering gradient, as a
    // fraction of ScaleMetres. It must be a LOW-PASS gradient: with a 30 m
    // step the 12-octave ridged mountains steer every gully a different way
    // and the result is cross-hatched noise; at 0.25 * scale the gullies
    // follow the large slopes and branch as in the original.
    double GradientStepFraction = 0.25;
    double GradientStepMetres() const { return ScaleMetres * GradientStepFraction; }
};

struct Result
{
    double DeltaHeight = 0.0;   // metres, already includes HeightOffset and the mask
    double RidgeMap    = 0.0;   // -1 creases .. +1 ridges (drainage, foliage)
};

// Up         unit radial direction of the point. The noise lattice is sampled
//            at Up * RadiusMetres, on the REFERENCE sphere: adding the terrain
//            height would shear the pattern along slopes.
// Gradient   tangent gradient of the base height (m/m), low-pass: a central
//            difference with step Prm.GradientStepMetres() of the base layers
//            (continent + mountains, no detail). See BuildChunkInputs.
// FadeTarget -1 in valleys .. +1 on peaks of the input terrain
// Mask       0..1 multiplier of the whole effect (land mask)
//
// Every octave is always evaluated, at every LOD: the height at a point must
// not depend on the chunk that samples it, or chunks of different LOD would
// crack apart along their shared edge. Normals are taken from the eroded
// mesh itself, so octaves finer than the vertex spacing do not sparkle.
Result Erode(const PlanetCore::Vec3d& Up, double RadiusMetres,
             const PlanetCore::Vec3d& Gradient, double FadeTarget, double Mask,
             const Params& Prm);

// Sum of all octave strengths, metres.
double Magnitude(const Params& Prm);

// Bounds of DeltaHeight at Mask = 1: [(HeightOffset - 1), (HeightOffset + 1)] * Magnitude.
void DeltaRange(const Params& Prm, double& OutLo, double& OutHi);

bool IsEnabled(const Params& Prm);

// ── Per chunk ───────────────────────────────────────────────────────────────
// Erosion inputs for every point of the chunk's halo grid (67 x 67).
//
// The gradient is exact (four base-height samples) on a 19 x 19 subgrid
// (every 4th vertex, from -4 to 68) and on EVERY border vertex, and bilinear
// in between.
//  * Border vertices are shared with neighbour chunks, possibly of another
//    LOD; there the gradient is a function of the point alone, so both
//    chunks erode the shared point identically: no cracks.
//  * The subgrid nodes of same-LOD neighbours coincide, so the halo ring and
//    the first inner ring are interpolated identically in both chunks, and
//    the normals of their shared border agree.
// Cost: 553 points x 4 samples of two noise layers per chunk (~0.25 ms).
struct ChunkInputs
{
    static constexpr int32_t Count = PlanetCore::HaloGrid::Count;
    PlanetCore::Vec3d Gradient[Count];
    float Mask[Count];
    float Fade[Count];
};

void BuildChunkInputs(const PlanetCore::NoiseGraph& Graph, const PlanetCore::FChunkKey& Key,
                      const PlanetCore::HaloGrid& Halo, const Params& Prm, ChunkInputs& Out);

// CPU twin of the GPU pass: DeltaHeight for the chunk's 65 x 65 vertices
// (row-major, as Surfaces). Reference for the harness and planet.Bench; the
// game reads the heights back from the GPU instead (~16 ms per chunk here).
void ErodeChunk(const PlanetCore::HaloGrid& Halo, const ChunkInputs& In, const Params& Prm,
                double RadiusMetres, double* OutDelta65);

// ── Single point (gameplay, camera height) ──────────────────────────────────
// Exact gradient at the point; matches the chunks to centimetres near the
// camera (interior chunk vertices use the interpolated gradient).
PlanetCore::Vec3d GradientAt(const PlanetCore::NoiseGraph& Graph, const PlanetCore::Vec3d& Dir,
                             const Params& Prm);
double DeltaAt(const PlanetCore::NoiseGraph& Graph, const PlanetCore::Vec3d& Dir, const Params& Prm);

} // namespace PlanetErosion

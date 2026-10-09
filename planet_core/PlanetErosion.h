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
// * MaxOctaves lets coarse LODs skip gullies narrower than their vertex
//   spacing (they would only alias); the morph hides the new octaves.
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
    double          DeltaHeight = 0.0;   // metres, already includes HeightOffset
    PlanetCore::Vec3d DeltaSlope;        // change of the tangent gradient (m/m)
    double          RidgeMap = 0.0;      // -1 creases .. +1 ridges (drainage, foliage)
};

// P          Up * PlanetRadiusMetres: the point on the REFERENCE sphere. Do not
//            add the terrain height, or the pattern shears along slopes.
// Up         unit radial direction at P
// Gradient   tangent gradient of the input height (m/m), central difference
//            with step Prm.GradientStepMetres() of the base layers
//            (continent + mountains, no detail): LOD-independent and low-pass
// FadeTarget -1 in valleys .. +1 on peaks of the input terrain
// Mask       0..1 multiplier of the whole effect (e.g. land mask)
// MaxOctaves cap from OctavesForSpacing, or Params.Octaves
Result Erode(const PlanetCore::Vec3d& P, const PlanetCore::Vec3d& Up,
             const PlanetCore::Vec3d& Gradient, double FadeTarget, double Mask,
             const Params& Prm, int MaxOctaves);

// Octaves whose gullies are at least two vertex spacings wide.
int OctavesForSpacing(const Params& Prm, double VertexSpacingMetres);

} // namespace PlanetErosion

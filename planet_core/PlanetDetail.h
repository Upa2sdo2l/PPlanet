// PlanetDetail.h
// UE-FREE selection of the DETAIL levels of the GPU terrain: "our own
// tessellation". Generated chunks stop at the actor's MaxLOD (BaseLOD): below
// that the noise and erosion run out of float precision, and new chunks would
// cost a CPU build each. Near the camera, a shown BaseLOD chunk is instead
// split further into derived chunks (up to BaseLOD + Levels) that build
// nothing: they are extra instances of the same grid mesh that read a
// sub-rectangle of the base chunk's atlas tile (bilinearly) and are pushed
// along the surface normal by the height maps of the terrain textures in the
// vertex shader (GpuErosion/Shaders/PlanetTerrainVertex.ush).
//
// Selection: best-first by distance, a node splits while its nearest point is
// closer than SplitDistanceFactor x its side, within a fixed instance budget.
// A split base keeps its atlas tile (the derived chunks read it) and hides its
// own instance.
//
// The displacement is camera-independent (shadow-map caches stay valid). It is
// filtered by the chunk's vertex spacing (texture mip per LOD), so chunks of
// different LOD displace the same point differently. Where they meet, the
// shader evaluates shared border points at the coarsest LOD around them: edge
// deltas (as for stitching) and corner deltas (CoarserCornerLevels, which also
// sees the diagonal neighbour) are in the instance data.
//
// Depends only on PlanetLOD.h and the C++ standard library.
#pragma once

#include "PlanetLOD.h"
#include <vector>

namespace PlanetDetail
{
using PlanetLOD::ChunkKey;
using PlanetLOD::KeySet;
using PlanetLOD::Vec3d;

// ── Instance data of the terrain material (all instances, generated and
//    derived). PerInstanceCustomData3Vector index = first float below.
//     0  atlas tile          1  morph start (s)     2..5 edge deltas U- U+ V- V+
//     6  SubX (base quads)   7  SubY                8  SubSize (64 = whole tile)
//     9  corner deltas, packed 3 bits each (c0 + 8 c1 + 64 c2 + 512 c3)
//    10  vertex spacing of this chunk's LOD, cm (function of the LOD only)
//    11  LOD - DisplacementMinLOD (displacement only where >= 0)
//    12..14  local X axis in world cm (instance rotation x scale, column)
//    15..17  local Y axis        18..20  local Z axis
//    21..23  frac(instance origin / TileNear)   24..26  frac(origin / TileMid)
static constexpr int32_t CUSTOM_DATA_FLOATS = 27;

struct Params
{
    int32_t BaseLOD             = 14;    // finest LOD of generated chunks (actor MaxLOD)
    int32_t Levels              = 4;     // detail levels below BaseLOD; 0 = off
    // Shown generated chunks this coarse may be split too: where the main
    // selection runs out of budget (cube corners, fast flight) the chunk
    // under the camera can be a level or two short of BaseLOD. A derived
    // chunk is at most 6 levels below its base (one base quad per grid step).
    int32_t MinBaseLOD          = 12;
    double  SplitDistanceFactor = 1.0;   // split while nearest distance < factor x side
    int32_t MaxInstances        = 160;   // derived instances
    double  Hysteresis          = 1.25;  // split nodes stay split until factor x this

    Vec3d   CameraPos       = {0, 0, 0}; // planet space, cm
    double  PlanetRadiusCm  = 2.5e8;
    double  SurfaceOffsetCm = 0.0;       // terrain height under the camera, cm

    // Split nodes of the previous selection (Result::SplitNodes).
    const KeySet* PreviouslySplit = nullptr;

    // Every shown generated chunk. A base with a FINER generated neighbour is
    // not split: that neighbour's edge carries finer data than the base tile
    // the derived chunks would interpolate, and the two edges would not meet.
    // (So split bases next to each other are always of the same LOD.)
    const KeySet* GeneratedShown = nullptr;
};

struct Base
{
    ChunkKey Key;    // shown generated chunk (LOD >= MinBaseLOD)
    int32_t  Slot;   // its slot / atlas tile
};

struct Leaf
{
    ChunkKey Key;
    int32_t  BaseIndex = -1;            // into the Bases array
    float    SubX = 0, SubY = 0;        // sub-rectangle of the base tile, in base quads
    float    SubSize = 64;
};

struct Result
{
    std::vector<Leaf>    Leaves;
    std::vector<uint8_t> BaseSplit;     // per base: hidden, covered by its leaves
    KeySet               SplitNodes;    // for the next frame's hysteresis
    bool                 bBudgetHit = false;
};

void Select(const Params& P, const std::vector<Base>& Bases, Result& Out);

// Sub-rectangle of the base tile covered by the derived chunk D under base B.
void SubRect(const ChunkKey& D, const ChunkKey& B, float& OutX, float& OutY, float& OutSize);

// How many levels coarser the coarsest chunk of Shown touching corner Corner
// of K is: 0 (U0,V0)  1 (U1,V0)  2 (U0,V1)  3 (U1,V1). Probes both edges next
// to the corner and the diagonal. Shown must not hold overlapping chunks.
int32_t CoarserCornerLevels(const ChunkKey& K, int32_t Corner, const KeySet& Shown);
float   PackedCornerDeltas(const ChunkKey& K, const KeySet& Shown);

// Vertex spacing of a chunk of LOD L at the centre of a cube face, cm. A
// function of the LOD only, so neighbours agree on it bit for bit.
double VertexSpacingCm(int32_t LOD, double PlanetRadiusCm);

// Everything of the 27 floats that depends on the chunk and its transform
// (not tile, morph or edge/corner deltas). The transform is the BASE tile's
// (derived chunks are drawn with it): axes as rows, scale per local axis, and
// the absolute world origin of the instance in cm (double).
struct TransformIn
{
    double AxisX[3], AxisY[3], AxisZ[3];
    double Scale[3];
    double OriginWorld[3];
};
void FillChunkData(float* Data, const ChunkKey& Key, const ChunkKey& BaseKey, const TransformIn& T,
                   double PlanetRadiusCm, int32_t DisplacementMinLOD, double TileNearCm, double TileMidCm);

} // namespace PlanetDetail

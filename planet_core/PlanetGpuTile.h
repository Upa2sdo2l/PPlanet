// PlanetGpuTile.h
// UE-FREE builder of the per-chunk data the GPU terrain renderer draws from.
//
// The GPU path draws every chunk as an instance of ONE 65x65 grid mesh. Each
// instance has a transform (rotation, translation, per-axis scale) that maps
// the unit box [-0.5, 0.5]^3 onto the chunk's bounding box, so engine culling
// sees tight bounds. The vertex shader then moves each grid vertex to its
// target position, which is stored in a texture atlas tile in the instance's
// LOCAL space:
//
//     world = Translation + Rotation * (Scale * Local)
//
// Storing targets in local space keeps every float in the shader small
// (|Local| <= 0.5); the planet-sized coordinate lives only in the instance
// transform, which the engine keeps in double precision.
//
// Depends on PlanetNoiseCore.h for Surface / CubeFaceDirection / ChunkVertexUV.
//
// ── Erosion (GPU) ───────────────────────────────────────────────────────────
// The atlas tiles are written by a compute shader, not uploaded: the CPU
// sends one GpuVertex per point of the chunk's 67 x 67 halo grid, the shader
// erodes every point, takes normals from the eroded grid (central
// differences, the halo gives the border vertices their outer neighbours),
// recomputes biomes from the eroded height and slope, and writes the
// position, normal and biome tiles. Shaders/PlanetErosionCommon.ush.
#pragma once

#include "PlanetNoiseCore.h"
#include "PlanetErosion.h"
#include <cstdint>

namespace PlanetGpu
{

static constexpr int32_t TILE_SIDE   = PlanetCore::PLANET_VERTS_PER_SIDE;   // 65
static constexpr int32_t TILE_TEXELS = TILE_SIDE * TILE_SIDE;               // 4225
static constexpr int32_t HALO_SIDE   = PlanetCore::PLANET_GRID_WITH_HALO;   // 67
static constexpr int32_t HALO_POINTS = HALO_SIDE * HALO_SIDE;               // 4489

// Layout shared with the shader (FPlanetVertexIn). 48 bytes.
struct GpuVertex
{
    float LocalBase[3];   // instance-local position before erosion
    float Height0;        // height before erosion, metres
    float PRef[3];        // Dir * radius, metres: erosion lattice and up vector
    float Fade;           // -1 valleys .. +1 peaks
    float Gradient[3];    // steering gradient, planet space, m/m
    float Mask;           // 0..1 land mask; 0 = no erosion
};
static_assert(sizeof(GpuVertex) == 48, "GpuVertex must match FPlanetVertexIn");

// Layout shared with the shader (FPlanetTileIn). 64 bytes.
struct GpuTileInfo
{
    float   AxisX[3];  int32_t AtlasTile;   // set when the tile is dispatched
    float   AxisY[3];  float   Pad0;
    float   AxisZ[3];  float   Pad1;
    float   ExtMetres[3]; float Pad2;       // instance scale, metres per local unit
};
static_assert(sizeof(GpuTileInfo) == 64, "GpuTileInfo must match FPlanetTileIn");

struct TileData
{
    // Halo grid, row-major (index = gy * 67 + gx); chunk vertex (x, y) is
    // (x + 1) + (y + 1) * 67.
    GpuVertex   Vertices[HALO_POINTS];
    GpuTileInfo Info;

    // Instance transform in planet space (centimetres). Axis rows form a
    // proper rotation (det +1): AxisZ = AxisX x AxisY. The box covers the
    // chunk before erosion AND every height erosion could give it, so the
    // eroded vertices stay inside the instance bounds.
    double  AxisX[3], AxisY[3], AxisZ[3];
    double  Translation[3];
    double  Scale[3];
};

// Builds the tile for Key from its halo grid (SampleChunkBatch) and the
// erosion inputs (BuildChunkInputs). Inputs may be null: no erosion (mask 0).
void BuildTile(const PlanetCore::FChunkKey& Key, const PlanetCore::HaloGrid& Halo,
               const PlanetErosion::ChunkInputs* Inputs, const PlanetErosion::Params& Erosion,
               double PlanetRadiusMetres, TileData& Out);

// Planet-space position (cm) of a local-space point under the tile transform.
void LocalToPlanet(const TileData& T, const double Local[3], double OutPlanet[3]);

} // namespace PlanetGpu

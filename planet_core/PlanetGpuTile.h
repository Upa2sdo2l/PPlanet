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
#pragma once

#include "PlanetNoiseCore.h"
#include <cstdint>

namespace PlanetGpu
{

static constexpr int32_t TILE_SIDE  = PlanetCore::PLANET_VERTS_PER_SIDE;   // 65
static constexpr int32_t TILE_TEXELS = TILE_SIDE * TILE_SIDE;               // 4225

struct TileData
{
    // Per vertex, row-major (index = gy * 65 + gx), gx along U, gy along V.
    float   Position[TILE_TEXELS][4];   // instance-local target in [-0.5, 0.5]^3, w = 0
    uint8_t Normal[TILE_TEXELS][4];     // planet-space normal * 0.5 + 0.5, a = 255
    uint8_t Biome[TILE_TEXELS][4];      // grass, rock, sand, snow (as the old vertex colour)

    // Instance transform in planet space (centimetres). Axis rows form a
    // proper rotation (det +1): AxisZ = AxisX x AxisY.
    double  AxisX[3], AxisY[3], AxisZ[3];
    double  Translation[3];
    double  Scale[3];
};

// Builds the tile for Key from its 65x65 surfaces (as SampleChunkBatch fills
// them). Vertex positions match FPlanetChunkMeshBuilder exactly: direction
// from ChunkVertexUV(gx + 1, gy + 1), radius PlanetRadiusMetres + Height.
void BuildTile(const PlanetCore::FChunkKey& Key, const PlanetCore::Surface* Surfaces,
               double PlanetRadiusMetres, TileData& Out);

// Planet-space position (cm) of a local-space point under the tile transform.
void LocalToPlanet(const TileData& T, const double Local[3], double OutPlanet[3]);

} // namespace PlanetGpu

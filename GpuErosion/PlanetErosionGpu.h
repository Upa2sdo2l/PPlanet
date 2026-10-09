// PlanetErosionGpu.h
// Interface of the PlanetErosion plugin (compute shaders of the planet).
//
// The game module fills a batch of chunk tiles on the game thread and hands
// it over; the plugin runs two compute passes on the render thread:
//   ErodeCS  erosion height for every point of every tile's 67 x 67 halo grid
//   WriteCS  eroded position, normal and biomes of every chunk vertex,
//            written straight into the three atlas render targets.
// and copies the erosion heights back to the CPU (asynchronously, 2-3 frames
// later): collision meshes use exactly the heights that are drawn, without
// computing erosion on the CPU.
//
// Layouts: one vertex = PlanetGpu::GpuVertex (48 bytes), one tile =
// PlanetGpu::GpuTileInfo (64 bytes); the shader declares the same structs.
// Bytes are passed as-is so the plugin does not depend on planet_core.
//
// Code and shaders live in the game repository (this folder); the plugin in
// <Project>/Plugins/PlanetErosion only includes PlanetErosionGpu.inl and maps
// the shader folder. See README.md.
#pragma once

#include "CoreMinimal.h"

class UTextureRenderTarget2D;

namespace PlanetErosionGpu
{
    static constexpr int32 HaloPoints     = 67 * 67;
    static constexpr int32 VertexBytes    = 48;
    static constexpr int32 TileBytes      = 64;

    struct FShaderParams
    {
        FVector4f P0       = FVector4f(0.f, 0.f, 0.f, 0.f);   // ScaleMetres, Strength, GullyWeight, Detail
        FVector4f Rounding = FVector4f(0.f, 0.f, 0.f, 0.f);
        FVector4f Onset    = FVector4f(0.f, 0.f, 0.f, 0.f);
        FVector4f P3       = FVector4f(0.f, 0.f, 0.f, 0.f);   // AssumedSlope value, override, CellScale, Normalization
        FVector4f P4       = FVector4f(0.f, 0.f, 0.f, 0.f);   // Lacunarity, Gain, HeightOffset, -
        int32     Octaves  = 0;                               // 0 = no erosion
        uint32    Seed     = 0;
        FVector4f Climate  = FVector4f(0.f, 0.f, 0.f, 0.f);   // SnowLatitudeStart, SnowAltitudeStart, HumidityVariance, -
    };

    struct FTileBatch
    {
        TArray<uint8> Vertices;   // NumTiles * HaloPoints * VertexBytes
        TArray<uint8> Tiles;      // NumTiles * TileBytes (AtlasTile filled in)
        TArray<int64> Tags;       // NumTiles caller ids, returned with the heights
        int32         NumTiles = 0;

        void Reset() { Vertices.Reset(); Tiles.Reset(); Tags.Reset(); NumTiles = 0; }
    };

    // Erosion heights of one tile, read back from the GPU.
    struct FHeightReadback
    {
        int64         Tag = 0;
        TArray<float> Delta;      // HaloPoints metres, halo-grid order (67 x 67)
    };

    // False below SM5 (no compute shaders compiled): use another renderer.
    PLANETEROSION_API bool IsSupported();

    // Enqueues both passes for the batch and the read-back of its heights.
    // Game thread. The textures must be created with bCanCreateUAV (positions
    // RGBA32F, normals and biomes RGBA8). Owner keys the read-backs.
    PLANETEROSION_API void Dispatch(const void* Owner,
                                    UTextureRenderTarget2D* PosAtlas,
                                    UTextureRenderTarget2D* NormalAtlas,
                                    UTextureRenderTarget2D* BiomeAtlas,
                                    int32 TilesPerRow,
                                    const FShaderParams& Params,
                                    FTileBatch&& Batch);

    // Game thread, once per frame: appends the heights that have arrived
    // since the last call (oldest first) and checks for new ones.
    PLANETEROSION_API void PollHeights(const void* Owner, TArray<FHeightReadback>& Out);

    // Drops every pending read-back of Owner (EndPlay).
    PLANETEROSION_API void ReleaseHeights(const void* Owner);
}

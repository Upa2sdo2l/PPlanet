// PlanetErosionGpu.h
// Interface of the PlanetErosion plugin (compute shaders of the planet).
//
// The game module fills a batch of chunk tiles on the game thread and hands
// it over; the plugin runs two compute passes on the render thread:
//   ErodeCS  erosion height for every point of every tile's 67 x 67 halo grid
//   WriteCS  eroded position, normal and biomes of every chunk vertex,
//            written straight into the three atlas render targets.
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
        int32         NumTiles = 0;

        void Reset() { Vertices.Reset(); Tiles.Reset(); NumTiles = 0; }
    };

    // False below SM5 (no compute shaders compiled): use another renderer.
    PLANETEROSION_API bool IsSupported();

    // Enqueues both passes for the batch. Game thread. The textures must be
    // created with bCanCreateUAV (positions RGBA32F, normals and biomes RGBA8).
    PLANETEROSION_API void Dispatch(UTextureRenderTarget2D* PosAtlas,
                                    UTextureRenderTarget2D* NormalAtlas,
                                    UTextureRenderTarget2D* BiomeAtlas,
                                    int32 TilesPerRow,
                                    const FShaderParams& Params,
                                    FTileBatch&& Batch);
}

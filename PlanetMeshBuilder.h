// PlanetMeshBuilder.h
// Surface[] -> RealtimeMesh stream set.
//
// Consumes PlanetCore::NoiseGraph::SampleChunkBatch output and produces a
// stream set ready for MeshSimple->CreateSectionGroup().
//
// Runs on a worker thread. Touches no UObject. Internals are reused across
// Build() calls so the large allocations happen once.
//
// ── Coordinate contract ─────────────────────────────────────────────────────
//   * Surface values are planet-space METRES (double, R = 2.5e6 m).
//   * UE uses centimetres. Vertex positions written to the RMC stream and the
//     component transform are therefore METRES * 100.
//   * Vertex positions are RELATIVE TO THE CHUNK ORIGIN, float32. At 64-128 m
//     chunks the float32 ULP is 8e-6 .. 1.5e-5 m before conversion, five orders
//     below the 0.5 m target resolution. The large coordinate reaches the
//     renderer only as the component transform.
//
// ── RMC API status ──────────────────────────────────────────────────────────
// TRealtimeMeshBuilderLocal template (from the TriAxis docs):
//     <IndexType=uint32,
//      TangentElementType=FPackedNormal,
//      TexCoordElementType=FVector2DHalf,
//      int32 NumTexCoords=1,          // 1..8
//      PolyGroupIndexType=uint16>
//
// Documented fluent chain used below:
//     Builder.EnableTangents() / EnableTexCoords() / EnableColors() / EnablePolyGroups()
//     Builder.ReserveNumVertices(N) / ReserveNumTriangles(N)
//     Builder.AddVertex(FVector3f)
//            .SetNormalAndTangent(FVector3f, FVector3f)
//            .SetColor(FColor)
//            .SetTexCoord(FVector2f)
//     Builder.AddTriangle(a, b, c, PolyGroup)
//
// Biome weights go in vertex COLOUR, not extra UV channels: a channel-indexed
// SetTexCoord is documented for the stream builder, not confirmed for the
// local fluent builder. FColor is documented and is exactly four channels, one
// per biome. The cost is 8-bit quantisation; if the multi-channel fluent form
// exists in this plugin revision, change ApplyBiomeWeights() and the material
// together — never one without the other.
#pragma once

#include "CoreMinimal.h"
#include "PlanetTypes.h"
#include "planet_core/PlanetNoiseCore.h"

namespace RealtimeMesh { struct FRealtimeMeshStreamSet; }

class FPlanetChunkMeshBuilder
{
public:
    FPlanetChunkMeshBuilder();
    ~FPlanetChunkMeshBuilder();

    FPlanetChunkMeshBuilder(const FPlanetChunkMeshBuilder&) = delete;
    FPlanetChunkMeshBuilder& operator=(const FPlanetChunkMeshBuilder&) = delete;

    // @param Key           chunk identity
    // @param Surfaces      PLANET_VERTS_PER_SIDE^2 surfaces, row-major
    // @param PlanetRadius  metres
    // @param OutChunkOrigin planet-space chunk centre for the component transform
    // @return false when Surfaces is null
    bool Build(const FChunkKey& Key,
               const PlanetCore::Surface* Surfaces,
               double PlanetRadius,
               FVector3d& OutChunkOrigin);

    // Valid until the next Build() on this instance.
    RealtimeMesh::FRealtimeMeshStreamSet& GetStreamSet();

    // Move the stream set out. Prefer this when handing the result to another
    // thread: it makes the ownership transfer explicit.
    RealtimeMesh::FRealtimeMeshStreamSet TakeStreamSet();

private:
    struct FImpl;
    TUniquePtr<FImpl> Impl;
};

// PlanetMeshBuilder.cpp
#include "PlanetMeshBuilder.h"

#include "RealtimeMeshSimple.h"
#include "RealtimeMeshComponent.h"

namespace
{
    constexpr int32 VPS  = PLANET_VERTS_PER_SIDE;   // 65
    constexpr int32 QUAD = PLANET_QUADS_PER_SIDE;   // 64
    constexpr int32 VTX  = VPS * VPS;               // 4225
    constexpr int32 TRI  = QUAD * QUAD * 2;         // 8192

    // Macro texture tiling: one UV unit per 256 m of arc. A LOD-12 chunk is
    // ~1.2 km across, so the pattern repeats about five times per chunk.
    // Deliberately NOT one repeat per chunk: that would lock the tiling to the
    // LOD grid and the repeat would become visible when the LOD changes.
    constexpr double MACRO_METRES_PER_UV = 256.0;

    // Biome weights -> vertex colour, one weight per channel in the order
    // Grass, Rock, Sand, Snow (PlanetCore::Surface::Biome).
    template<class TVert>
    FORCEINLINE void ApplyBiomeWeights(TVert& Vert, const PlanetCore::Surface& S)
    {
        auto Q = [](float V) -> uint8
        {
            return (uint8)(FMath::Clamp(V, 0.f, 1.f) * 255.f + 0.5f);
        };
        Vert.SetColor(FColor(Q(S.Biome[0]), Q(S.Biome[1]), Q(S.Biome[2]), Q(S.Biome[3])));
    }
}

struct FPlanetChunkMeshBuilder::FImpl
{
    RealtimeMesh::FRealtimeMeshStreamSet StreamSet;

    // Indices are identical for every chunk at this resolution, so they are
    // built once. Per chunk that turns 24576 index writes into a memcpy.
    TArray<uint32> Indices;
    bool bIndicesBuilt = false;

    void BuildIndicesOnce()
    {
        if (bIndicesBuilt) return;
        Indices.SetNumUninitialized(TRI * 3, EAllowShrinking::No);

        int32 w = 0;
        for (int32 gy = 0; gy < QUAD; ++gy)
        {
            for (int32 gx = 0; gx < QUAD; ++gx)
            {
                const uint32 v00 = (uint32)(gx     + gy       * VPS);
                const uint32 v10 = (uint32)(gx + 1 + gy       * VPS);
                const uint32 v01 = (uint32)(gx     + (gy + 1) * VPS);
                const uint32 v11 = (uint32)(gx + 1 + (gy + 1) * VPS);

                // Winding verified numerically to face outward on all six cube
                // faces under UE's left-handed rule. This is the old project's
                // order, empirically confirmed to render on this engine
                // revision; the alternative diagonal is also outward but
                // untested here.
                Indices[w++] = v00; Indices[w++] = v01; Indices[w++] = v10;
                Indices[w++] = v10; Indices[w++] = v01; Indices[w++] = v11;
            }
        }
        bIndicesBuilt = true;
    }
};

FPlanetChunkMeshBuilder::FPlanetChunkMeshBuilder() : Impl(MakeUnique<FImpl>()) {}
FPlanetChunkMeshBuilder::~FPlanetChunkMeshBuilder() = default;

RealtimeMesh::FRealtimeMeshStreamSet& FPlanetChunkMeshBuilder::GetStreamSet()
{
    check(Impl.IsValid());
    return Impl->StreamSet;
}

RealtimeMesh::FRealtimeMeshStreamSet FPlanetChunkMeshBuilder::TakeStreamSet()
{
    check(Impl.IsValid());
    RealtimeMesh::FRealtimeMeshStreamSet Out = MoveTemp(Impl->StreamSet);
    Impl->StreamSet = RealtimeMesh::FRealtimeMeshStreamSet();
    return Out;
}

bool FPlanetChunkMeshBuilder::Build(const FChunkKey& Key,
                                    const PlanetCore::Surface* Surfaces,
                                    double PlanetRadius,
                                    FVector3d& OutChunkOrigin)
{
    if (!Surfaces) return false;
    check(Impl.IsValid());
    Impl->BuildIndicesOnce();

    const PlanetCore::FChunkKey CoreKey(Key.Face, Key.LOD, Key.X, Key.Y);

    // ── Chunk origin: the chunk centre on the reference sphere ───────────
    // Using the centre (not a corner) keeps the float32 vertex offsets centred
    // on zero, so precision is spent symmetrically instead of on one side.
    {
        const double D    = 2.0 / (double)((int64)1 << Key.LOD);
        const double MidU = -1.0 + (double)Key.X * D + D * 0.5;
        const double MidV = -1.0 + (double)Key.Y * D + D * 0.5;
        const PlanetCore::Vec3d C = PlanetCore::CubeFaceDirection(Key.Face, MidU, MidV);
        OutChunkOrigin = FVector3d(C.X, C.Y, C.Z) * (PlanetRadius * PLANET_METRES_TO_UE_CM);
    }
    const PlanetCore::Vec3d O(OutChunkOrigin.X, OutChunkOrigin.Y, OutChunkOrigin.Z);

    Impl->StreamSet = RealtimeMesh::FRealtimeMeshStreamSet();

    RealtimeMesh::TRealtimeMeshBuilderLocal<uint32, FPackedNormal, FVector2f, 1>
        Builder(Impl->StreamSet);

    Builder.EnableTangents();
    Builder.EnableTexCoords();
    Builder.EnableColors();
    Builder.EnablePolyGroups();

    Builder.ReserveNumVertices(VTX);
    Builder.ReserveNumTriangles(TRI);

    for (int32 gy = 0; gy < VPS; ++gy)
    {
        for (int32 gx = 0; gx < VPS; ++gx)
        {
            const int32 i = gx + gy * VPS;
            const PlanetCore::Surface& S = Surfaces[i];

            // Radial direction recomputed from the SAME parametrisation the
            // core sampled with. The +1 accounts for ChunkVertexUV reserving
            // grid index 0 for the halo ring while the core's chunk grid starts
            // at the chunk corner. Sharing the function keeps the mesh welded
            // to the sampled heights.
            double U, V;
            PlanetCore::ChunkVertexUV(CoreKey, gx + 1, gy + 1, U, V);
            const PlanetCore::Vec3d R = PlanetCore::CubeFaceDirection(Key.Face, U, V);

            const PlanetCore::Vec3d World = R * (PlanetRadius + S.Height);

            const FVector3f Pos(
                (float)((World.X * PLANET_METRES_TO_UE_CM) - O.X),
                (float)((World.Y * PLANET_METRES_TO_UE_CM) - O.Y),
                (float)((World.Z * PLANET_METRES_TO_UE_CM) - O.Z));

            const FVector3f N((float)S.Normal.X,  (float)S.Normal.Y,  (float)S.Normal.Z);
            const FVector3f T((float)S.Tangent.X, (float)S.Tangent.Y, (float)S.Tangent.Z);

            // Planet-space macro UV: neighbours tile without knowing each other.
            const FVector2f MacroUV(
                (float)(World.X / MACRO_METRES_PER_UV),
                (float)(World.Z / MACRO_METRES_PER_UV));

            auto Vert = Builder.AddVertex(Pos);
            Vert.SetNormalAndTangent(N, T);
            Vert.SetTexCoord(MacroUV);
            ApplyBiomeWeights(Vert, S);
        }
    }

    for (int32 t = 0; t < TRI; ++t)
    {
        Builder.AddTriangle(Impl->Indices[t * 3],
                            Impl->Indices[t * 3 + 1],
                            Impl->Indices[t * 3 + 2],
                            0);
    }

    return true;
}

// ProceduralPlanetGpu.cpp
// The GPU terrain renderer of AProceduralPlanet.
//
// Every chunk slot owns:
//   * one instance of GridMesh (a 65x65 grid, local box [-0.5, 0.5]^3) in the
//     single instanced component TerrainISM; its transform maps that box onto
//     the chunk's bounding box, so engine culling sees tight bounds;
//   * one 65x65 tile in three atlas render targets: target vertex positions
//     in the instance's local space (RGBA32F), normals and biome weights
//     (RGBA8). The tiles are written by two compute passes (PlanetErosion
//     plugin, GpuErosion/): erosion of the chunk's halo grid, then position,
//     normal (from the eroded neighbours) and biomes per vertex.
// The material (MaterialHLSL/README.md) moves each grid vertex to its target,
// stitches edges against coarser neighbours and morphs splits in over time.
//
// Hidden slots keep their instance at zero scale: the instance count never
// changes, so instance index == slot index == atlas tile index, always.
//
// Instance transforms are stored by the engine in float relative to the
// component, so the component follows the camera (GpuRebaseIfNeeded).
#include "ProceduralPlanet.h"

#include "RealtimeMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "TextureResource.h"

namespace
{
    // Material parameter names. They must match the material exactly.
    const FName PARAM_POS_ATLAS     (TEXT("PlanetPosAtlas"));
    const FName PARAM_NORMAL_ATLAS  (TEXT("PlanetNormalAtlas"));
    const FName PARAM_BIOME_ATLAS   (TEXT("PlanetBiomeAtlas"));
    const FName PARAM_TILES_PER_ROW (TEXT("AtlasTilesPerRow"));
    const FName PARAM_ATLAS_TEXELS  (TEXT("AtlasTexels"));
    const FName PARAM_MORPH_SECONDS (TEXT("MorphSeconds"));

    // Per-instance custom data layout (the WPO node reads it as two float3):
    //   0 atlas tile   1 morph start time (s)   2..5 edge LOD deltas U- U+ V- V+
    constexpr int32 CUSTOM_DATA_FLOATS = 6;

    // Atlas written by compute shaders (UAV) and sampled by the material.
    UTextureRenderTarget2D* MakeAtlasTarget(UObject* Outer, int32 Size, EPixelFormat Format,
                                            TextureFilter Filter, const TCHAR* Name)
    {
        UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(Outer, FName(Name), RF_Transient);
        if (!RT) return nullptr;
        RT->bCanCreateUAV     = true;
        RT->bAutoGenerateMips = false;
        RT->Filter            = Filter;
        RT->AddressX          = TA_Clamp;
        RT->AddressY          = TA_Clamp;
        RT->ClearColor        = FLinearColor::Transparent;
        RT->InitCustomFormat(Size, Size, Format, /*bInForceLinearGamma*/ true);   // data, not colour
        RT->UpdateResourceImmediate(true);
        return RT;
    }

    FTransform TileTransform(const PlanetGpu::TileData& T)
    {
        const FVector X(T.AxisX[0], T.AxisX[1], T.AxisX[2]);
        const FVector Y(T.AxisY[0], T.AxisY[1], T.AxisY[2]);
        const FVector Z(T.AxisZ[0], T.AxisZ[1], T.AxisZ[2]);
        // Rows are the local axes: local X maps to X, and so on.
        const FMatrix Rot(FPlane(X, 0.0), FPlane(Y, 0.0), FPlane(Z, 0.0), FPlane(0.0, 0.0, 0.0, 1.0));
        return FTransform(FQuat(Rot),
                          FVector(T.Translation[0], T.Translation[1], T.Translation[2]),
                          FVector(T.Scale[0], T.Scale[1], T.Scale[2]));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Setup
// ─────────────────────────────────────────────────────────────────────────────
void AProceduralPlanet::InitGpuTerrain()
{
    bGPUTerrain = false;
    if (TerrainRenderer != EPlanetTerrainRenderer::GPUInstanced) return;

    if (!GPUTerrainMaterial)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Planet] GPU terrain: GPUTerrainMaterial is not set, using the RealtimeMesh renderer. ")
            TEXT("See MaterialHLSL/README.md."));
        return;
    }

    if (!PlanetErosionGpu::IsSupported())
    {
        UE_LOG(LogTemp, Warning, TEXT("[Planet] GPU terrain needs SM5 compute shaders; using the RealtimeMesh renderer."));
        return;
    }

    GridMesh = BuildGridMesh();
    if (!GridMesh)
    {
        UE_LOG(LogTemp, Error, TEXT("[Planet] GPU terrain: grid mesh build failed, using the RealtimeMesh renderer."));
        return;
    }

    const int32 SlotCount = Pool.Num();
    AtlasTilesPerRow = FMath::Max(1, FMath::CeilToInt(FMath::Sqrt((float)SlotCount)));
    AtlasTexels      = AtlasTilesPerRow * PlanetGpu::TILE_SIDE;

    // Positions are read with Load (exact texels); normals and biomes are
    // sampled bilinearly in the pixel shader.
    PosAtlas    = MakeAtlasTarget(this, AtlasTexels, PF_A32B32G32R32F, TF_Nearest,  TEXT("PlanetPosAtlas"));
    NormalAtlas = MakeAtlasTarget(this, AtlasTexels, PF_R8G8B8A8,      TF_Bilinear, TEXT("PlanetNormalAtlas"));
    BiomeAtlas  = MakeAtlasTarget(this, AtlasTexels, PF_R8G8B8A8,      TF_Bilinear, TEXT("PlanetBiomeAtlas"));
    if (!PosAtlas || !NormalAtlas || !BiomeAtlas)
    {
        UE_LOG(LogTemp, Error, TEXT("[Planet] GPU terrain: atlas textures could not be created, using the RealtimeMesh renderer."));
        return;
    }

    TerrainMID = UMaterialInstanceDynamic::Create(GPUTerrainMaterial, this);
    TerrainMID->SetTextureParameterValue(PARAM_POS_ATLAS,    PosAtlas);
    TerrainMID->SetTextureParameterValue(PARAM_NORMAL_ATLAS, NormalAtlas);
    TerrainMID->SetTextureParameterValue(PARAM_BIOME_ATLAS,  BiomeAtlas);
    TerrainMID->SetScalarParameterValue(PARAM_TILES_PER_ROW, (float)AtlasTilesPerRow);
    TerrainMID->SetScalarParameterValue(PARAM_ATLAS_TEXELS,  (float)AtlasTexels);
    TerrainMID->SetScalarParameterValue(PARAM_MORPH_SECONDS, MorphSeconds);

    TerrainISM = NewObject<UInstancedStaticMeshComponent>(this, TEXT("PlanetTerrainGPU"));
    TerrainISM->SetupAttachment(RootComponent);
    TerrainISM->SetUsingAbsoluteLocation(true);
    TerrainISM->SetUsingAbsoluteRotation(true);
    TerrainISM->SetUsingAbsoluteScale(true);
    TerrainISM->SetMobility(EComponentMobility::Movable);
    TerrainISM->SetStaticMesh(GridMesh);
    TerrainISM->SetMaterial(0, TerrainMID);
    TerrainISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    TerrainISM->SetCanEverAffectNavigation(false);
    TerrainISM->SetCastShadow(true);
    // A distance field of the flat grid would be wrong (the shape comes from
    // the material), and hardware ray tracing is not used.
    TerrainISM->bAffectDistanceFieldLighting = false;
    TerrainISM->bVisibleInRayTracing = false;
    // The material offsets vertices, which by default makes Virtual Shadow
    // Maps re-render the terrain's pages every frame. Its shape only changes
    // through instance updates (which invalidate anyway) and short morphs.
    TerrainISM->ShadowCacheInvalidationBehavior = EShadowCacheInvalidationBehavior::Static;
    TerrainISM->SetNumCustomDataFloats(CUSTOM_DATA_FLOATS);
    TerrainISM->RegisterComponent();

    GpuTerrainOrigin = LastCameraPosUE;
    TerrainISM->SetWorldLocation(GetActorLocation() + FVector(GpuTerrainOrigin));

    // One instance per slot, all hidden.
    const FTransform Hidden(FQuat::Identity, GetActorLocation(), FVector::ZeroVector);
    TArray<FTransform> Instances;
    Instances.Init(Hidden, SlotCount);
    TerrainISM->AddInstances(Instances, /*bShouldReturnIndices*/ false, /*bWorldSpace*/ true);

    GpuSlots.SetNum(SlotCount);
    for (int32 i = 0; i < SlotCount; ++i)
    {
        GpuSlots[i] = FGpuSlot();
        GpuSlots[i].CustomData[0] = (float)i;
        TerrainISM->SetCustomData(i, MakeArrayView(GpuSlots[i].CustomData, CUSTOM_DATA_FLOATS), false);
    }
    PendingTiles.Reset();
    TerrainISM->MarkRenderStateDirty();

    // Chunk meshes now only carry collision.
    for (URealtimeMeshComponent* Comp : Pool)
    {
        if (Comp) Comp->SetVisibility(false);
    }

    bGPUTerrain = true;
    UE_LOG(LogTemp, Log, TEXT("[Planet] GPU terrain: %d instances, atlas %dx%d (%d tiles per row)"),
           SlotCount, AtlasTexels, AtlasTexels, AtlasTilesPerRow);
}

// A 65x65 grid in the unit square, z = 0, except one vertex at z = -0.5 and
// one at z = +0.5 so the mesh bounds are the unit box. The material moves
// every vertex to its target anyway (offset = target - local position), so
// these two only set the bounds. Triangles use the same vertex order as
// FPlanetChunkMeshBuilder, so they face outward exactly as the RMC chunks do.
UStaticMesh* AProceduralPlanet::BuildGridMesh()
{
    const int32 Side = PlanetGpu::TILE_SIDE;
    const int32 Quads = Side - 1;

    FMeshDescription MeshDesc;
    FStaticMeshAttributes Attributes(MeshDesc);
    Attributes.Register();

    TVertexAttributesRef<FVector3f>          Positions     = Attributes.GetVertexPositions();
    TVertexInstanceAttributesRef<FVector3f>  Normals       = Attributes.GetVertexInstanceNormals();
    TVertexInstanceAttributesRef<FVector3f>  Tangents      = Attributes.GetVertexInstanceTangents();
    TVertexInstanceAttributesRef<float>      BinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
    TVertexInstanceAttributesRef<FVector2f>  UVs           = Attributes.GetVertexInstanceUVs();
    TPolygonGroupAttributesRef<FName>        SlotNames     = Attributes.GetPolygonGroupMaterialSlotNames();

    const FPolygonGroupID Group = MeshDesc.CreatePolygonGroup();
    SlotNames[Group] = FName(TEXT("Planet"));

    MeshDesc.ReserveNewVertices(Side * Side);
    MeshDesc.ReserveNewVertexInstances(Side * Side);
    MeshDesc.ReserveNewTriangles(Quads * Quads * 2);

    TArray<FVertexInstanceID> Instances;
    Instances.SetNum(Side * Side);

    for (int32 gy = 0; gy < Side; ++gy)
    {
        for (int32 gx = 0; gx < Side; ++gx)
        {
            const float Z = (gx == 0 && gy == 0) ? -0.5f : ((gx == Quads && gy == Quads) ? 0.5f : 0.f);

            const FVertexID V = MeshDesc.CreateVertex();
            Positions[V] = FVector3f((float)gx / (float)Quads - 0.5f, (float)gy / (float)Quads - 0.5f, Z);

            const FVertexInstanceID VI = MeshDesc.CreateVertexInstance(V);
            Normals[VI]       = FVector3f(0.f, 0.f, 1.f);
            Tangents[VI]      = FVector3f(1.f, 0.f, 0.f);
            BinormalSigns[VI] = 1.f;
            UVs.Set(VI, 0, FVector2f((float)gx / (float)Quads, (float)gy / (float)Quads));
            Instances[gx + gy * Side] = VI;
        }
    }

    for (int32 gy = 0; gy < Quads; ++gy)
    {
        for (int32 gx = 0; gx < Quads; ++gx)
        {
            const FVertexInstanceID V00 = Instances[gx     + gy       * Side];
            const FVertexInstanceID V10 = Instances[gx + 1 + gy       * Side];
            const FVertexInstanceID V01 = Instances[gx     + (gy + 1) * Side];
            const FVertexInstanceID V11 = Instances[gx + 1 + (gy + 1) * Side];

            const FVertexInstanceID T0[3] = {V00, V01, V10};
            const FVertexInstanceID T1[3] = {V10, V01, V11};
            MeshDesc.CreateTriangle(Group, MakeArrayView(T0, 3));
            MeshDesc.CreateTriangle(Group, MakeArrayView(T1, 3));
        }
    }

    UStaticMesh* Mesh = NewObject<UStaticMesh>(this, TEXT("PlanetGridMesh"), RF_Transient);
    Mesh->GetStaticMaterials().Add(FStaticMaterial(GPUTerrainMaterial, FName(TEXT("Planet")), FName(TEXT("Planet"))));

    UStaticMesh::FBuildMeshDescriptionsParams Params;
    Params.bFastBuild = true;
    Params.bBuildSimpleCollision = false;

    TArray<const FMeshDescription*> Descriptions;
    Descriptions.Add(&MeshDesc);
    if (!Mesh->BuildFromMeshDescriptions(Descriptions, Params))
    {
        return nullptr;
    }
    return Mesh;
}

// ─────────────────────────────────────────────────────────────────────────────
// Per chunk
// ─────────────────────────────────────────────────────────────────────────────
// Queues the slot's tile for this frame's compute dispatch (GpuFlushTiles).
void AProceduralPlanet::GpuUploadTile(int32 SlotIndex, PlanetGpu::TileData& Tile, uint32 Generation)
{
    if (!GpuSlots.IsValidIndex(SlotIndex)) return;

    Tile.Info.AtlasTile = SlotIndex;
    static_assert(sizeof(PlanetGpu::GpuVertex)   == PlanetErosionGpu::VertexBytes, "vertex layout");
    static_assert(sizeof(PlanetGpu::GpuTileInfo) == PlanetErosionGpu::TileBytes,   "tile layout");
    static_assert(PlanetGpu::HALO_POINTS         == PlanetErosionGpu::HaloPoints,  "halo grid");

    PendingTiles.Vertices.Append(reinterpret_cast<const uint8*>(Tile.Vertices), (int32)sizeof(Tile.Vertices));
    PendingTiles.Tiles.Append(reinterpret_cast<const uint8*>(&Tile.Info), (int32)sizeof(Tile.Info));
    PendingTiles.Tags.Add(((int64)SlotIndex << 32) | (int64)Generation);
    ++PendingTiles.NumTiles;

    FGpuSlot& G = GpuSlots[SlotIndex];
    G.PlanetTransform     = TileTransform(Tile);
    G.Heights.Reset();
    G.bHeightsReady       = !Generator.HasErosion();   // nothing to wait for without erosion
    G.HeightsRequestFrame = FrameCounter;
}

void AProceduralPlanet::GpuPollHeights()
{
    static TArray<PlanetErosionGpu::FHeightReadback> Arrived;   // game thread only
    Arrived.Reset();
    PlanetErosionGpu::PollHeights(this, Arrived);

    const std::vector<PlanetStreaming::Slot>& Slots = Scheduler.GetSlots();
    constexpr int32 N = PlanetGpu::TILE_SIDE;
    constexpr int32 G = PlanetGpu::HALO_SIDE;

    for (PlanetErosionGpu::FHeightReadback& R : Arrived)
    {
        const int32  SlotIndex  = (int32)(R.Tag >> 32);
        const uint32 Generation = (uint32)(R.Tag & 0xffffffffll);
        if (!GpuSlots.IsValidIndex(SlotIndex) || SlotIndex >= (int32)Slots.size()) continue;
        if (Slots[SlotIndex].Generation != Generation) continue;      // slot reused since
        FGpuSlot& S = GpuSlots[SlotIndex];
        if (S.bHeightsReady || R.Delta.Num() != PlanetGpu::HALO_POINTS) continue;

        // Halo grid (67x67) -> the chunk's own 65x65 vertices.
        S.Heights.SetNumUninitialized(N * N);
        for (int32 y = 0; y < N; ++y)
            FMemory::Memcpy(&S.Heights[y * N], &R.Delta[(y + 1) * G + 1], sizeof(float) * N);
        S.bHeightsReady = true;

        const int32 Latency = (int32)(FrameCounter - S.HeightsRequestFrame);
        ++PerfWindow.HeightsArrived;
        PerfWindow.HeightsLatencyFrames += Latency;
        PerfWindow.HeightsLatencyMax = FMath::Max(PerfWindow.HeightsLatencyMax, Latency);
    }

    // Safety net: heights normally arrive in 2-3 frames. If a read-back is
    // ever lost, give the chunk collision without erosion rather than none
    // (off by up to the erosion depth, but nothing falls through the world).
    constexpr uint64 GiveUpFrames = 300;
    for (int32 i = 0; i < GpuSlots.Num() && i < (int32)Slots.size(); ++i)
    {
        FGpuSlot& S = GpuSlots[i];
        if (S.bHeightsReady || Slots[i].State != PlanetStreaming::SlotState::Active) continue;
        if (FrameCounter - S.HeightsRequestFrame < GiveUpFrames) continue;
        UE_LOG(LogTemp, Warning, TEXT("[Planet] erosion heights of slot %d did not come back from the GPU; collision without erosion"), i);
        S.Heights.Reset();
        S.bHeightsReady = true;
    }
}

void AProceduralPlanet::GpuFlushTiles()
{
    if (PendingTiles.NumTiles <= 0) return;
    PerfWindow.GpuTiles += PendingTiles.NumTiles;
    const int32 Tiles = PendingTiles.NumTiles;
    PlanetErosionGpu::Dispatch(this, PosAtlas, NormalAtlas, BiomeAtlas, AtlasTilesPerRow,
                               GpuShaderParams, MoveTemp(PendingTiles));
    // The arrays went to the render thread; keep next frame's appends cheap.
    PendingTiles.Reset();
    PendingTiles.Vertices.Reserve(Tiles * PlanetErosionGpu::HaloPoints * PlanetErosionGpu::VertexBytes);
    PendingTiles.Tiles.Reserve(Tiles * PlanetErosionGpu::TileBytes);
    PendingTiles.Tags.Reserve(Tiles);
}

void AProceduralPlanet::UpdateGpuShaderParams()
{
    const PlanetErosion::Params& E = Generator.GetErosion();
    PlanetErosionGpu::FShaderParams& P = GpuShaderParams;
    P.P0       = FVector4f((float)E.ScaleMetres, (float)E.Strength, (float)E.GullyWeight, (float)E.Detail);
    P.Rounding = FVector4f((float)E.Rounding[0], (float)E.Rounding[1], (float)E.Rounding[2], (float)E.Rounding[3]);
    P.Onset    = FVector4f((float)E.Onset[0], (float)E.Onset[1], (float)E.Onset[2], (float)E.Onset[3]);
    P.P3       = FVector4f((float)E.AssumedSlope[0], (float)E.AssumedSlope[1], (float)E.CellScale, (float)E.Normalization);
    P.P4       = FVector4f((float)E.Lacunarity, (float)E.Gain, (float)E.HeightOffset, 0.f);
    P.Octaves  = PlanetErosion::IsEnabled(E) ? E.Octaves : 0;
    P.Seed     = E.Seed;

    const PlanetCore::NoiseParams& N = Generator.GetCoreParams();
    P.Climate  = FVector4f(N.SnowLatitudeStart, N.SnowAltitudeStart, N.HumidityVariance, 0.f);
}

FTransform AProceduralPlanet::GpuWorldTransform(int32 SlotIndex) const
{
    FTransform T = GpuSlots[SlotIndex].PlanetTransform;
    T.AddToTranslation(GetActorLocation());
    return T;
}

void AProceduralPlanet::GpuShowSlot(int32 SlotIndex, bool bShow, bool bMorphIn)
{
    FGpuSlot& G = GpuSlots[SlotIndex];
    G.bShown = bShow;

    if (!bShow)
    {
        const FTransform Hidden(FQuat::Identity, GetActorLocation(), FVector::ZeroVector);
        TerrainISM->UpdateInstanceTransform(SlotIndex, Hidden, /*bWorldSpace*/ true, /*bMarkRenderStateDirty*/ false, /*bTeleport*/ true);
        return;
    }

    // Edge deltas start at 0 and are set by GpuUpdateEdgeDeltas this frame.
    G.CustomData[0] = (float)SlotIndex;
    G.CustomData[1] = (bMorphIn && GetWorld()) ? (float)GetWorld()->GetTimeSeconds() : -1e6f;
    G.CustomData[2] = G.CustomData[3] = G.CustomData[4] = G.CustomData[5] = 0.f;

    TerrainISM->UpdateInstanceTransform(SlotIndex, GpuWorldTransform(SlotIndex), true, false, true);
    TerrainISM->SetCustomData(SlotIndex, MakeArrayView(G.CustomData, CUSTOM_DATA_FLOATS), false);
}

// For every shown chunk, how many levels coarser each edge neighbour is. The
// material slides edge vertices onto that neighbour's edge, so shown chunks of
// different LOD meet without cracks. Recomputed whenever the shown set
// changes; only changed instances are touched.
void AProceduralPlanet::GpuUpdateEdgeDeltas()
{
    const std::vector<PlanetStreaming::Slot>& Slots = Scheduler.GetSlots();

    PlanetLOD::KeySet Shown;
    Shown.reserve(GpuSlots.Num() * 2);
    for (int32 i = 0; i < GpuSlots.Num(); ++i)
        if (GpuSlots[i].bShown) Shown.insert(Slots[i].Key);

    for (int32 i = 0; i < GpuSlots.Num(); ++i)
    {
        FGpuSlot& G = GpuSlots[i];
        if (!G.bShown) continue;

        bool bChanged = false;
        for (int32 Side = 0; Side < 4; ++Side)
        {
            const float Delta = (float)FMath::Min(PlanetLOD::CoarserNeighbourLevels(Slots[i].Key, Side, Shown), 6);
            if (G.CustomData[2 + Side] != Delta)
            {
                G.CustomData[2 + Side] = Delta;
                bChanged = true;
            }
        }
        if (bChanged)
        {
            TerrainISM->SetCustomData(i, MakeArrayView(G.CustomData, CUSTOM_DATA_FLOATS), false);
        }
    }
}

void AProceduralPlanet::GpuRebaseIfNeeded(const FVector3d& CameraPos)
{
    if (FVector3d::Distance(CameraPos, GpuTerrainOrigin) < GPUTerrainRebaseDistanceMetres * PLANET_METRES_TO_UE_CM)
        return;

    GpuTerrainOrigin = CameraPos;
    TerrainISM->SetWorldLocation(GetActorLocation() + FVector(GpuTerrainOrigin));

    // Stored transforms are relative to the component: re-send them all.
    const FTransform Hidden(FQuat::Identity, GetActorLocation(), FVector::ZeroVector);
    for (int32 i = 0; i < GpuSlots.Num(); ++i)
    {
        TerrainISM->UpdateInstanceTransform(i, GpuSlots[i].bShown ? GpuWorldTransform(i) : Hidden, true, false, true);
    }
    TerrainISM->MarkRenderStateDirty();
}

// ─────────────────────────────────────────────────────────────────────────────
// Presentation: the GPU counterpart of SyncSlotPresentation.
// ─────────────────────────────────────────────────────────────────────────────
void AProceduralPlanet::SyncSlotPresentationGpu()
{
    const std::vector<PlanetStreaming::Slot>& Slots = Scheduler.GetSlots();
    const int32 Count = FMath::Min((int32)Slots.size(), GpuSlots.Num());

    bool bShownChanged = false;

    struct FCollisionChange { int32 SlotIndex; double Priority; bool bOn; };
    TArray<FCollisionChange, TInlineAllocator<32>> CollisionChanges;

    for (int32 i = 0; i < Count; ++i)
    {
        const PlanetStreaming::Slot& Slot = Slots[i];
        FGpuSlot& G = GpuSlots[i];

        if (Slot.State == PlanetStreaming::SlotState::Retiring)
        {
            // Hidden in the same frame its replacements are shown.
            if (G.bShown)
            {
                GpuShowSlot(i, false, false);
                bShownChanged = true;
            }
            if (SlotCollisionApplied[i] && Pool.IsValidIndex(i) && Pool[i])
            {
                Pool[i]->SetCollisionEnabled(ECollisionEnabled::NoCollision);
                SlotCollisionApplied[i] = 0;
            }
            continue;
        }
        if (Slot.State != PlanetStreaming::SlotState::Active) continue;

        if (G.bShown != Slot.bShown)
        {
            GpuShowSlot(i, Slot.bShown, Slot.bShown && Slot.bAppearedBySplit);
            bShownChanged = true;
        }

        if (Slot.bWorkerInFlight) continue;   // a collision mesh is on its way
        if (Slot.bNeedsCollision && !G.bHasCollisionMesh && G.bHeightsReady)
            CollisionChanges.Add({i, Slot.Priority, true});
        else if (!Slot.bNeedsCollision && G.bHasCollisionMesh)
            CollisionChanges.Add({i, Slot.Priority, false});
    }

    if (bShownChanged)
    {
        GpuUpdateEdgeDeltas();
        TerrainISM->MarkRenderStateDirty();
    }

    // Closest first; each "on" launches a collision-mesh job for the slot.
    CollisionChanges.Sort([](const FCollisionChange& A, const FCollisionChange& B)
    {
        return A.Priority < B.Priority;
    });
    const int32 Budget = FMath::Min(CollisionChanges.Num(), FMath::Max(1, MaxCollisionChangesPerFrame));
    for (int32 c = 0; c < Budget; ++c)
    {
        if (CollisionChanges[c].bOn) LaunchCollisionMeshJob(CollisionChanges[c].SlotIndex);
        else                         RemoveCollisionMesh(CollisionChanges[c].SlotIndex);
    }
}

// ProceduralPlanet.h
// The thin UE owner of a FIXED pool of chunk slots.
//
// Two renderers (TerrainRenderer):
//   GPUInstanced  : every chunk is one instance of a shared 65x65 grid mesh in
//                   a single instanced component; the material moves vertices
//                   to positions stored in an atlas texture, stitches LOD seams
//                   and morphs splits. RMC components only carry collision, for
//                   chunks within CollisionDistanceMetres. The atlas tiles are
//                   written by compute shaders (PlanetErosion plugin): erosion,
//                   normals and biomes per tile.
//   RealtimeMesh  : the original path, one visible RMC component per chunk.
//
// POLICY: the pool has PLANET_COMPONENT_POOL_SIZE slots and never grows. The
// LOD selection uses at most PLANET_LOD_LEAF_BUDGET of them; the rest hold
// chunks kept on screen until their replacements are built. Predictable VRAM,
// predictable frame cost, no unbounded growth.
//
// Division of labour:
//   Game thread   : traversal, scheduler reconciliation, RMC commit, transforms
//   Worker threads: noise sampling (FPlanetNoiseGenerator::SampleChunk) and
//                   mesh building (FPlanetChunkMeshBuilder)
//   UE-free cores : LOD traversal and the streaming state machine, both covered
//                   by harnesses that run in seconds without the editor
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformAtomics.h"
#include "Async/Future.h"
#include "PlanetTypes.h"
#include "PlanetNoise.h"
#include "PlanetMeshBuilder.h"
#include "PlanetCoordinates.h"
#include "planet_core/PlanetLOD.h"
#include "planet_core/PlanetStreaming.h"
#include "planet_core/PlanetGpuTile.h"
#include "GpuErosion/PlanetErosionGpu.h"
#include "ProceduralPlanet.generated.h"

class URealtimeMeshComponent;
class URealtimeMeshSimple;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UInstancedStaticMeshComponent;
class UStaticMesh;
class UTexture2D;
class UTextureRenderTarget2D;

UENUM()
enum class EPlanetTerrainRenderer : uint8
{
    // One instanced draw for the whole terrain, displaced in the material.
    // Needs GPUTerrainMaterial; falls back to RealtimeMesh without it.
    GPUInstanced,
    // One visible RMC component per chunk (the original renderer).
    RealtimeMesh,
};

UCLASS()
class FASTNOISETEST_API AProceduralPlanet : public AActor
{
    GENERATED_BODY()

public:
    AProceduralPlanet();

    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    // ── Configuration ───────────────────────────────────────────────────

    // Planet radius in metres. A 2500 km body gives a realistic horizon curve
    // without the LOD tree needing absurd depth.
    UPROPERTY(EditAnywhere, Category="Planet", meta=(ClampMin="100000.0"))
    double PlanetRadiusMetres = 2500000.0;

    UPROPERTY(EditAnywhere, Category="Planet")
    FPlanetNoiseParams NoiseParams;

    // Material of the RealtimeMesh renderer (biome weights in vertex colour).
    UPROPERTY(EditAnywhere, Category="Planet")
    TObjectPtr<UMaterialInterface> PlanetMaterial;

    // ── GPU terrain ─────────────────────────────────────────────────────
    UPROPERTY(EditAnywhere, Category="Planet|GPU Terrain")
    EPlanetTerrainRenderer TerrainRenderer = EPlanetTerrainRenderer::GPUInstanced;

    // Material built as described in MaterialHLSL/README.md. Without it the
    // planet falls back to the RealtimeMesh renderer and says so in the log.
    UPROPERTY(EditAnywhere, Category="Planet|GPU Terrain")
    TObjectPtr<UMaterialInterface> GPUTerrainMaterial;

    // Seconds over which a chunk that replaced its parent morphs from the
    // parent's shape to its own. 0 = no morph (instant swap).
    UPROPERTY(EditAnywhere, Category="Planet|GPU Terrain", meta=(ClampMin="0.0", ClampMax="5.0"))
    float MorphSeconds = 0.35f;

    // The instanced component is kept within this distance of the camera.
    // Instance transforms are stored in float relative to it, so a far origin
    // would open sub-millimetre gaps between chunks near the player.
    UPROPERTY(EditAnywhere, Category="Planet|GPU Terrain", meta=(ClampMin="100.0"))
    double GPUTerrainRebaseDistanceMetres = 4000.0;

    // Gullies and ridges of the erosion filter (GPU renderer only; the
    // RealtimeMesh renderer shows the terrain without erosion).
    UPROPERTY(EditAnywhere, Category="Planet|Erosion")
    FPlanetErosionSettings Erosion;

    // Worst-case mountain height above the sphere, used only for bounding
    // spheres and horizon culling. Over-estimating costs a little culling;
    // under-estimating pops chunks, so this is generous on purpose.
    UPROPERTY(EditAnywhere, Category="Planet", meta=(ClampMin="0.0"))
    double HeightMarginMetres = 60000.0;

    // ── View for LOD selection ──────────────────────────────────────────

    // When set, this is used instead of the local player camera. Useful for
    // a Seed Lab preview pawn or a fixed debug camera.
    UPROPERTY(EditAnywhere, Category="Planet|View")
    TObjectPtr<AActor> ViewOverride;

    UPROPERTY(EditAnywhere, Category="Planet|View", meta=(ClampMin="0.5", ClampMax="8.0"))
    double ErrorThresholdPixels = 1.0;

    UPROPERTY(EditAnywhere, Category="Planet|View", meta=(ClampMin="1", ClampMax="18"))
    int32 MaxLOD = 14;

    // Slack (metres) subtracted from the camera-to-chunk distance in the LOD
    // error. The distance is already measured from the terrain under the
    // camera, so keep this small: a few km makes every nearby chunk tie on
    // error and the leaf budget is then spent arbitrarily.
    UPROPERTY(EditAnywhere, Category="Planet|View", meta=(ClampMin="0.0", ClampMax="20000.0"))
    double LODReliefMetres = 0.0;

    // A chunk split last frame stays split until its error drops this many
    // times below the split point. Stops chunks at the edge of the leaf
    // budget from merging and re-splitting as the camera moves a few metres.
    // 1 = off.
    UPROPERTY(EditAnywhere, Category="Planet|View", meta=(ClampMin="1.0", ClampMax="3.0"))
    double LODHysteresis = 1.25;

    // ── Frame budgets ───────────────────────────────────────────────────
    // These bound work per frame. Together with the fixed pool they are what
    // keeps a fly-over from stalling.
    UPROPERTY(EditAnywhere, Category="Planet|Budget", meta=(ClampMin="0", ClampMax="512"))
    int32 MaxNewRequestsPerFrame = 8;

    UPROPERTY(EditAnywhere, Category="Planet|Budget", meta=(ClampMin="0", ClampMax="512"))
    int32 MaxCommitsPerFrame = 4;

    UPROPERTY(EditAnywhere, Category="Planet|Budget", meta=(ClampMin="0", ClampMax="512"))
    int32 MaxRetiresPerFrame = 8;

    // Chunks whose nearest point is within this distance of the camera get
    // collision, at any LOD. It is dropped again only beyond 1.25x this
    // distance, so a chunk at the boundary does not toggle.
    UPROPERTY(EditAnywhere, Category="Planet|Budget", meta=(ClampMin="0.0"))
    double CollisionDistanceMetres = 1500.0;

    // How many frames a new chunk must be on the pool before the chunk it
    // replaces is removed. Gives its async collision cook time to finish so
    // the ground does not vanish under the player during a split or merge.
    UPROPERTY(EditAnywhere, Category="Planet|Budget", meta=(ClampMin="0", ClampMax="120"))
    int32 ReplacementSettleFrames = 6;

    // Collision on/off changes applied to already-built chunks per frame
    // (each "on" builds a collision mesh and starts a cook).
    UPROPERTY(EditAnywhere, Category="Planet|Budget", meta=(ClampMin="1", ClampMax="64"))
    int32 MaxCollisionChangesPerFrame = 4;

    UPROPERTY(EditAnywhere, Category="Planet|Debug")
    bool bDrawDebugStats = false;

    // ── Seed Lab / diagnostics API ──────────────────────────────────────

    // Rebuild the noise graph and refresh the sea level. Safe to call when a
    // slider moves; the pool is reconciled on the next tick.
    UFUNCTION(BlueprintCallable, Category="Planet")
    void RebuildNoise();

    UFUNCTION(BlueprintCallable, Category="Planet")
    double GetHeightAtDirection(const FVector& UnitDir) const;
    
    UFUNCTION(BlueprintCallable, Category="Planet")
    double GetHeightAtWorldLocation(const FVector& WorldLocation) const;

    UFUNCTION(BlueprintCallable, Category="Planet")
    FVector GetSurfacePositionAtDirection(const FVector& UnitDir) const;

    // Fills Out with every currently active chunk key.
    UFUNCTION(BlueprintCallable, Category="Planet")
    void GetActiveChunks(TArray<FChunkKey>& Out) const;

    UFUNCTION(BlueprintCallable, Category="Planet")
    FString GetStreamingStatsString() const;

    // Synchronous per-stage benchmark on the game thread: builds Count chunks
    // at the given LOD around the camera and times noise, mesh build and the
    // RMC calls separately. Console: planet.Bench [Count] [LOD].
    // Returns the report; it is also printed on screen and to the log.
    UFUNCTION(BlueprintCallable, Category="Planet|Debug")
    FString RunBenchmark(int32 Count = 32, int32 LOD = 12);

private:
    // ── Internals ───────────────────────────────────────────────────────

    void CreatePool();

    void UpdateLODSelection(const FVector3d& CameraPos);
    void ReconcileStreaming(uint64 Frame, const FVector3d& CameraPos);
    void PumpWorkers();
    void ApplyReadyChunks();
    void ReleaseRetired();

    void ConfigureComponentForChunk(int32 SlotIndex, const FChunkKey& Key);

    PlanetStreaming::FrameBudget MakeFrameBudget() const;

    // ── GPU terrain (ProceduralPlanetGpu.cpp) ───────────────────────────
    // Creates the grid mesh, atlas textures, material instance and the
    // instanced component. Leaves bGPUTerrain false (RealtimeMesh renderer)
    // if the renderer is not selected or anything is missing.
    void InitGpuTerrain();
    UStaticMesh* BuildGridMesh();
    void GpuUploadTile(int32 SlotIndex, PlanetGpu::TileData& Tile);
    // Sends the tiles queued this frame to the compute shaders (one batch).
    void GpuFlushTiles();
    // Erosion and climate constants of the compute shaders, from the
    // generator's current settings.
    void UpdateGpuShaderParams();
    void GpuShowSlot(int32 SlotIndex, bool bShow, bool bMorphIn);
    void GpuUpdateEdgeDeltas();
    void GpuRebaseIfNeeded(const FVector3d& CameraPos);
    FTransform GpuWorldTransform(int32 SlotIndex) const;
    void SyncSlotPresentationGpu();

    // Collision mesh for a slot from its kept surfaces: on a worker, or
    // applied on the game thread when one comes back.
    void LaunchCollisionMeshJob(int32 SlotIndex);
    void ApplyCollisionMesh(int32 SlotIndex);
    void RemoveCollisionMesh(int32 SlotIndex);

    bool bGPUTerrain = false;

    UPROPERTY(Transient)
    TObjectPtr<UInstancedStaticMeshComponent> TerrainISM;

    UPROPERTY(Transient)
    TObjectPtr<UStaticMesh> GridMesh;

    UPROPERTY(Transient)
    TObjectPtr<UTextureRenderTarget2D> PosAtlas;

    UPROPERTY(Transient)
    TObjectPtr<UTextureRenderTarget2D> NormalAtlas;

    UPROPERTY(Transient)
    TObjectPtr<UTextureRenderTarget2D> BiomeAtlas;

    UPROPERTY(Transient)
    TObjectPtr<UMaterialInstanceDynamic> TerrainMID;

    // Per slot: the instance as last pushed to the instanced component.
    struct FGpuSlot
    {
        FTransform PlanetTransform = FTransform::Identity;   // planet space, cm
        float      CustomData[6] = {0.f, -1e6f, 0.f, 0.f, 0.f, 0.f};
        bool       bShown = false;
        bool       bHasCollisionMesh = false;
    };
    TArray<FGpuSlot> GpuSlots;

    // Tiles committed this frame, dispatched together by GpuFlushTiles.
    PlanetErosionGpu::FTileBatch  PendingTiles;
    PlanetErosionGpu::FShaderParams GpuShaderParams;

    // Where the instanced component sits, relative to the actor (cm).
    FVector3d GpuTerrainOrigin = FVector3d::ZeroVector;
    int32     AtlasTilesPerRow = 1;
    int32     AtlasTexels = 1;

    // Applies the scheduler's per-slot decisions to the components: shows or
    // hides built chunks (atomic swaps), hides retiring ones at once, and
    // turns collision on/off as chunks enter or leave CollisionDistanceMetres.
    void SyncSlotPresentation();

    // What each pool component currently has applied, so only changes are
    // pushed to UE. Indexed like Pool.
    TArray<uint8> SlotVisibleApplied;
    TArray<uint8> SlotCollisionApplied;

    // Publishes "stat Planet" values for this frame and draws the
    // planet.Stats overlay when that console variable is on.
    void UpdatePlanetStats();

    // Under-camera diagnostics, refreshed by UpdatePlanetStats.
    int32  NadirLOD = 0;
    bool   bNadirCollision = false;
    double CameraAboveTerrainM = 0.0;

    // Rolling one-second window behind the per-chunk averages in stat Planet.
    struct FPerfWindow
    {
        double WindowStart = 0.0;
        int32  Built = 0;
        double NoiseMs = 0.0, NoiseMax = 0.0;
        double BuildMs = 0.0, BuildMax = 0.0;
        int32  Committed = 0;
        double RMCMs = 0.0, RMCMax = 0.0;
        int32  GpuTiles = 0;                           // tiles sent to the compute shaders
        int32  CollisionBuilt = 0;                     // collision meshes built (GPU renderer)
        double CollisionErosionMs = 0.0, CollisionErosionMax = 0.0;   // CPU erosion for them

        // Game-thread stage time summed over the window's frames (ms).
        int32  Frames = 0;
        double TickMs = 0.0;
        double HeightMs = 0.0, LODMs = 0.0, ReconcileMs = 0.0, PumpMs = 0.0;
        double ApplyMs = 0.0, RemoveMs = 0.0, CreateMs = 0.0, ConfigMs = 0.0, ReleaseMs = 0.0;
        double CoverageMs = 0.0;
    };
    FPerfWindow PerfWindow;      // being accumulated
    FPerfWindow PerfPublished;   // last complete second, shown in stat Planet

    // Terrain height under the camera this frame (metres), from the LOD pass.
    double CameraTerrainHeightM = 0.0;

    FPlanetNoiseGenerator    Generator;
    PlanetStreaming::Scheduler Scheduler;

    UPROPERTY(Transient)
    TArray<TObjectPtr<URealtimeMeshComponent>> Pool;

    // Per-slot worker scratch. Worker tasks write here; the game thread reads
    // and clears on commit. Only one worker ever touches a given slot index.
    struct FSlotWork
    {
        // Full = sample the surface, then build the GPU tile and/or the mesh.
        // CollisionMesh = build the mesh from the Surfaces kept from the last
        // Full job (the chunk is already shown; the camera came close).
        enum class EJob : uint8 { Full, CollisionMesh };

        TArray<PlanetCore::Surface> Surfaces;
        TAtomic<bool>               bDone{false};
        uint32                      Generation = 0;
        FChunkKey                   Key;
        bool                        bBuilt = false;
        bool                        bNeedsCollision = false;

        EJob                        Job = EJob::Full;
        bool                        bBuildMesh = true;     // build the RMC stream set
        bool                        bMeshBuilt = false;
        // GPU renderer only: allocated per job (215 KB), freed once uploaded.
        TUniquePtr<PlanetGpu::TileData> Tile;
        bool                        bGpuTile = false;      // this job builds a GPU tile

        // The task writing into this slot, if any. EndPlay waits on it
        // before freeing the scratch the task points at.
        TFuture<void>               Future;

        // Worker-side timings of the last run, read by the game thread after
        // bDone (the release/acquire pair makes them visible).
        double                      NoiseMs = 0.0;
        double                      BuildMs = 0.0;
        double                      ErosionMs = 0.0;     // CPU erosion of a collision mesh
    };
    TArray<TUniquePtr<FSlotWork>> SlotWork;

    // One builder per worker keeps the stream-set buffers warm across chunks.
    TArray<TUniquePtr<FPlanetChunkMeshBuilder>> Builders;

    uint64   FrameCounter = 0;

    // Camera position for this frame, UE centimetres, relative to the actor.
    // Stored so worker launch and commit both judge collision distance from the
    // same value the traversal used.
    FVector3d LastCameraPosUE = FVector3d::ZeroVector;

    // Cached last selection so we can detect a stable view and stop re-issuing.
    PlanetLOD::Selection LastSelection;

    // Internal nodes of LastSelection: what was split last frame (hysteresis).
    PlanetLOD::KeySet LODPreviouslySplit;

    friend class FPlanetChunkWorker;
};

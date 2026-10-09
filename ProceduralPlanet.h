// ProceduralPlanet.h
// The thin UE owner of a FIXED pool of RMC components.
//
// POLICY: the pool has PLANET_COMPONENT_POOL_SIZE (64) slots and never grows.
// Under pressure PlanetLOD lowers the budget so the visible set fits; it does
// not allocate a 65th component. That is the whole point of the fixed pool:
// predictable VRAM, predictable frame cost, no unbounded growth.
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
#include "ProceduralPlanet.generated.h"

class URealtimeMeshComponent;
class URealtimeMeshSimple;
class UMaterialInterface;

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

    UPROPERTY(EditAnywhere, Category="Planet")
    TObjectPtr<UMaterialInterface> PlanetMaterial;

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

    // Distance beyond which collision is not requested. Collision cook is the
    // most expensive thing per chunk; a pedestrian never needs it 10 km away.
    UPROPERTY(EditAnywhere, Category="Planet|Budget", meta=(ClampMin="0.0"))
    double CollisionDistanceMetres = 4000.0;

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

        // Game-thread stage time summed over the window's frames (ms).
        int32  Frames = 0;
        double TickMs = 0.0;
        double HeightMs = 0.0, LODMs = 0.0, ReconcileMs = 0.0, PumpMs = 0.0;
        double ApplyMs = 0.0, RemoveMs = 0.0, CreateMs = 0.0, ConfigMs = 0.0, ReleaseMs = 0.0;
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
        TArray<PlanetCore::Surface> Surfaces;
        TAtomic<bool>               bDone{false};
        uint32                      Generation = 0;
        FChunkKey                   Key;
        bool                        bBuilt = false;
        bool                        bNeedsCollision = false;

        // The task writing into this slot, if any. EndPlay waits on it
        // before freeing the scratch the task points at.
        TFuture<void>               Future;

        // Worker-side timings of the last run, read by the game thread after
        // bDone (the release/acquire pair makes them visible).
        double                      NoiseMs = 0.0;
        double                      BuildMs = 0.0;
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

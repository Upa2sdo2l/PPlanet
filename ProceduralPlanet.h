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

private:
    // ── Internals ───────────────────────────────────────────────────────

    void CreatePool();

    void UpdateLODSelection(const FVector3d& CameraPos);
    void ReconcileStreaming(uint64 Frame, const FVector3d& CameraPos);
    void PumpWorkers();
    void ApplyReadyChunks();
    void ReleaseRetired();

    void ConfigureComponentForChunk(int32 SlotIndex, const FChunkKey& Key);

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

    friend class FPlanetChunkWorker;
};

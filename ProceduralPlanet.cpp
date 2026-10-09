// ProceduralPlanet.cpp
#include "ProceduralPlanet.h"

#include "RealtimeMeshSimple.h"
#include "RealtimeMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Async/Async.h"
#include "DrawDebugHelpers.h"

namespace
{
    inline FChunkKey FromLODKey(const PlanetLOD::ChunkKey& K)
    {
        return FChunkKey(K.Face, K.LOD, K.X, K.Y);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Worker task
// ─────────────────────────────────────────────────────────────────────────────

// Runs on the task graph. Reads only the generator (const, thread-safe) and
// its own slot scratch; writes only its own slot scratch. It never touches a
// UObject and never touches another slot.
class FPlanetChunkWorker
{
public:
    FPlanetChunkWorker(const FPlanetNoiseGenerator* InGen,
                       FPlanetChunkMeshBuilder* InBuilder,
                       AProceduralPlanet::FSlotWork* InWork,
                       uint32 InGeneration,
                       double InPlanetRadiusMetres)
        : Gen(InGen), Builder(InBuilder), Work(InWork)
        , Generation(InGeneration), PlanetRadius(InPlanetRadiusMetres) {}

    static TFuture<void> Launch(const FPlanetNoiseGenerator* Gen,
                                FPlanetChunkMeshBuilder* Builder,
                                AProceduralPlanet::FSlotWork* Work,
                                uint32 Generation,
                                double PlanetRadiusMetres)
    {
        return Async(EAsyncExecution::ThreadPool, [=]()
        {
            FPlanetChunkWorker Job(Gen, Builder, Work, Generation, PlanetRadiusMetres);
            Job.Run();
        });
    }

    void Run()
    {
        if (!Gen || !Gen->IsValid() || !Builder || !Work) return;

        // 1. Sample the surface. This is the expensive step: min 0.95 ms per
        //    chunk for 4225 vertices, measured on the reference box.
        TArrayView<PlanetCore::Surface> View(Work->Surfaces.GetData(),
                                             Work->Surfaces.Num());
        Gen->SampleChunk(Work->Key, View);

        // 2. Build the stream set with the worker's own builder instance.
        FVector3d Origin;
        Work->bBuilt = Builder->Build(Work->Key, Work->Surfaces.GetData(),
                                      PlanetRadius, Origin);

        // 3. Signal completion. The generation was captured at launch; if the
        //    slot has been reassigned since, the game thread discards this
        //    result instead of applying it to the wrong chunk.
        Work->Generation = Generation;
        Work->bDone.Store(true);
    }

private:
    const FPlanetNoiseGenerator* Gen;
    FPlanetChunkMeshBuilder*     Builder;
    AProceduralPlanet::FSlotWork* Work;
    uint32 Generation;
    double PlanetRadius;
};

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

AProceduralPlanet::AProceduralPlanet()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickGroup = TG_PostPhysics;
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

    Scheduler = PlanetStreaming::Scheduler(PLANET_COMPONENT_POOL_SIZE);
}

void AProceduralPlanet::BeginPlay()
{
    Super::BeginPlay();
    RebuildNoise();
    CreatePool();
}

void AProceduralPlanet::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    SlotWork.Reset();
    Builders.Reset();
    Pool.Reset();
    Super::EndPlay(EndPlayReason);
}

// ─────────────────────────────────────────────────────────────────────────────
// Pool creation: exactly PLANET_COMPONENT_POOL_SIZE components, preallocated.
// Nothing here ever creates a component after this point.
// ─────────────────────────────────────────────────────────────────────────────
void AProceduralPlanet::CreatePool()
{
    if (Pool.Num() > 0) return;

    Pool.Reserve(PLANET_COMPONENT_POOL_SIZE);
    SlotWork.Reserve(PLANET_COMPONENT_POOL_SIZE);
    Builders.Reserve(PLANET_COMPONENT_POOL_SIZE);

    for (int32 i = 0; i < PLANET_COMPONENT_POOL_SIZE; ++i)
    {
        URealtimeMeshComponent* Comp =
            NewObject<URealtimeMeshComponent>(this, URealtimeMeshComponent::StaticClass(),
                                              *FString::Printf(TEXT("PlanetChunk_%d"), i));
        if (!Comp) continue;

        Comp->SetupAttachment(RootComponent);
        Comp->RegisterComponent();
        Comp->SetMobility(EComponentMobility::Movable);

        URealtimeMeshSimple* MeshSimple = Comp->InitializeRealtimeMesh<URealtimeMeshSimple>();
        if (MeshSimple)
        {
            if (PlanetMaterial)
            {
                MeshSimple->SetupMaterialSlot(0, TEXT("Planet"), PlanetMaterial);
            }
            FRealtimeMeshCollisionConfiguration Collision;
            Collision.bUseComplexAsSimpleCollision = true;
            Collision.bUseAsyncCook = true;
            MeshSimple->SetCollisionConfig(Collision);
        }

        // Start hidden. A slot shows geometry only once it is Active.
        Comp->SetVisibility(false);
        Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);

        Pool.Add(Comp);

        TUniquePtr<FSlotWork> W = MakeUnique<FSlotWork>();
        W->Surfaces.SetNumUninitialized(PLANET_BODY_VERTS, EAllowShrinking::No);
        SlotWork.Add(MoveTemp(W));

        Builders.Add(MakeUnique<FPlanetChunkMeshBuilder>());
    }

    UE_LOG(LogTemp, Log, TEXT("[Planet] fixed pool created: %d components"), Pool.Num());
}

// ─────────────────────────────────────────────────────────────────────────────
// Noise
// ─────────────────────────────────────────────────────────────────────────────
void AProceduralPlanet::RebuildNoise()
{
    Generator.Build(NoiseParams);
}

double AProceduralPlanet::GetHeightAtDirection(const FVector& UnitDir) const
{
    return Generator.GetHeightAt(UnitDir);
}

double AProceduralPlanet::GetHeightAtWorldLocation(const FVector& WorldLocation) const
{
    const FVector3d Dir = FVector3d(WorldLocation) - FVector3d(GetActorLocation());
    // SizeSquared(), not LengthSquared(): UE's TVector API is Size()/SizeSquared().
    // Length()/LengthSquared() are the names used by PlanetCore::Vec3d, which is
    // a different type entirely.
    if (Dir.SizeSquared() < 1.0) return 0.0;
    return Generator.GetHeightAt(Dir.GetSafeNormal());
}

FVector AProceduralPlanet::GetSurfacePositionAtDirection(const FVector& UnitDir) const
{
    const FVector3d Unit = UnitDir.GetSafeNormal();
    const double H = Generator.GetHeightAt(Unit);
    // Metres -> UE centimetres; returned relative to the actor so it composes
    // with LWC without ever constructing a 2500 km world coordinate.
    return FVector(Unit * ((PlanetRadiusMetres + H) * PLANET_METRES_TO_UE_CM)) + FVector(GetActorLocation());
}

void AProceduralPlanet::GetActiveChunks(TArray<FChunkKey>& Out) const
{
    Out.Reset();
    Out.Reserve(LastSelection.Chunks.size());
    for (const PlanetLOD::ChunkKey& K : LastSelection.Chunks)
        Out.Add(FromLODKey(K));
}

// ─────────────────────────────────────────────────────────────────────────────
// Tick: our entire per-frame planet work.
// ─────────────────────────────────────────────────────────────────────────────
void AProceduralPlanet::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (Pool.Num() == 0) return;

    ++FrameCounter;

    // ── 1. Camera position, in UE centimetres relative to the actor ───────
    FVector CameraWorld = FVector::ZeroVector;
    if (ViewOverride)
    {
        CameraWorld = ViewOverride->GetActorLocation();
    }
    else if (UWorld* World = GetWorld())
    {
        if (APlayerController* PC = World->GetFirstPlayerController())
        {
            if (PC->PlayerCameraManager)
            {
                CameraWorld = PC->PlayerCameraManager->GetCameraLocation();
            }
        }
    }

    const FVector3d CameraRel = FVector3d(CameraWorld) - FVector3d(GetActorLocation());
    LastCameraPosUE = CameraRel;

    // ── 2. LOD selection (UE-free core) ──────────────────────────────────
    UpdateLODSelection(CameraRel);

    // ── 3. Reconcile against the fixed pool ──────────────────────────────
    ReconcileStreaming(FrameCounter, CameraRel);

    // ── 4. Hand ready work to workers, commit finished work ──────────────
    PumpWorkers();
    ApplyReadyChunks();
    ReleaseRetired();

#if !UE_BUILD_SHIPPING
    if (bDrawDebugStats && GEngine)
    {
        // Display stats as on-screen messages (always visible, camera-independent)
        const FString StatsText = GetStreamingStatsString();

        // Key -1 means it will be refreshed every frame at the same position
        // TimeToDisplay 0.0f means show only this frame (refreshed every tick)
        GEngine->AddOnScreenDebugMessage(-1, 0.0f, FColor::Green, StatsText, true, FVector2D(0.8f, 0.8f));
    }
#endif
}

void AProceduralPlanet::UpdateLODSelection(const FVector3d& CameraPos)
{
    PlanetLOD::TraversalParams P;
    P.CameraPos        = PlanetLOD::Vec3d{CameraPos.X, CameraPos.Y, CameraPos.Z};
    P.PlanetRadius     = PlanetRadiusMetres * PLANET_METRES_TO_UE_CM;
    P.HeightMargin     = HeightMarginMetres * PLANET_METRES_TO_UE_CM;
    P.ErrorThresholdPx = ErrorThresholdPixels;
    P.ReliefCm         = LODReliefMetres * PLANET_METRES_TO_UE_CM;

    // The error metric is measured from the terrain under the camera, not from
    // the reference sphere: terrain rises 20+ km, and measured from the bare
    // sphere a camera standing on a high plateau looks kilometres up, so the
    // chunk under it never refines to the collision LODs. One single-point
    // noise sample per frame.
    P.SurfaceOffsetCm = 0.0;
    if (CameraPos.SizeSquared() > 1.0)
    {
        const double TerrainHeightM = Generator.GetHeightAt(CameraPos.GetSafeNormal());
        P.SurfaceOffsetCm = TerrainHeightM * PLANET_METRES_TO_UE_CM;
    }
    P.MaxLOD           = (uint8_t)FMath::Clamp(MaxLOD, 1, 18);

    // THE fixed-pool link: the traversal can never return more leaves than the
    // pool can display. If a view needs more detail, ErrorThresholdPixels is
    // what gives, never the component count.
    P.MaxLeaves        = PLANET_COMPONENT_POOL_SIZE;
    P.bUseHorizonCull  = true;

    PlanetLOD::Traverse(P, LastSelection);
}

void AProceduralPlanet::ReconcileStreaming(uint64 Frame, const FVector3d& CameraPos)
{
    std::vector<PlanetStreaming::DesiredChunk> Desired;
    Desired.reserve(LastSelection.Chunks.size());

    // Collision only for the top 2 most detailed LODs (dynamic based on MaxLOD)
    const uint8 CollisionMinLOD = (MaxLOD >= 2) ? (uint8)(MaxLOD - 2) : 0;

    for (const PlanetLOD::ChunkKey& K : LastSelection.Chunks)
    {
        double U0, V0, Size;
        PlanetLOD::ChunkExtent(K, U0, V0, Size);
        const PlanetLOD::Vec3d Dir = PlanetLOD::CubeFaceDirection(
            K.Face, U0 + Size * 0.5, V0 + Size * 0.5);

        const double Cx = Dir.X * PlanetRadiusMetres * PLANET_METRES_TO_UE_CM;
        const double Cy = Dir.Y * PlanetRadiusMetres * PLANET_METRES_TO_UE_CM;
        const double Cz = Dir.Z * PlanetRadiusMetres * PLANET_METRES_TO_UE_CM;

        const double Dx = CameraPos.X - Cx;
        const double Dy = CameraPos.Y - Cy;
        const double Dz = CameraPos.Z - Cz;
        const double Dist = std::sqrt(Dx*Dx + Dy*Dy + Dz*Dz);

        PlanetStreaming::DesiredChunk D;
        D.Key             = K;
        D.Priority        = Dist;                       // lower = closer = kept
        D.bNeedsCollision = (K.LOD >= CollisionMinLOD); // collision for top-2 LOD only
        Desired.push_back(D);
    }

    PlanetStreaming::FrameBudget Budget;
    Budget.MaxNewWorkerRequests = MaxNewRequestsPerFrame;
    Budget.MaxCommits           = MaxCommitsPerFrame;
    Budget.MaxRetires           = MaxRetiresPerFrame;

    Scheduler.Reconcile(Desired, Frame, Budget);
}

// Distance from the camera to a chunk centre, in UE centimetres.
// Utilised by PumpWorkers() at launch time so collision distance is judged
// from the same camera position the traversal used.
static double ChunkDistanceToCamera(const PlanetLOD::ChunkKey& Key,
                                    double PlanetRadiusMetres,
                                    const FVector3d& CameraPos)
{
    double U0, V0, Size;
    PlanetLOD::ChunkExtent(Key, U0, V0, Size);
    const PlanetLOD::Vec3d Dir =
        PlanetLOD::CubeFaceDirection(Key.Face, U0 + Size * 0.5, V0 + Size * 0.5);

    const double Cx = Dir.X * PlanetRadiusMetres * PLANET_METRES_TO_UE_CM;
    const double Cy = Dir.Y * PlanetRadiusMetres * PLANET_METRES_TO_UE_CM;
    const double Cz = Dir.Z * PlanetRadiusMetres * PLANET_METRES_TO_UE_CM;

    const double Dx = CameraPos.X - Cx, Dy = CameraPos.Y - Cy, Dz = CameraPos.Z - Cz;
    return std::sqrt(Dx*Dx + Dy*Dy + Dz*Dz);
}

// ─────────────────────────────────────────────────────────────────────────────
// PumpWorkers: take queued assignments and launch tasks.
//
// MarkBuilding() immediately before launch is what makes a queued request safe.
// The slot may have been retired or reassigned while the request sat in the
// priority queue; MarkBuilding detects that before a worker is spent on it.
// ─────────────────────────────────────────────────────────────────────────────
void AProceduralPlanet::PumpWorkers()
{
    int32 Launched = 0;
    PlanetStreaming::WorkerRequest Req;

    while (Scheduler.PopWorkerRequest(Req))
    {
        if (!Scheduler.MarkBuilding(Req)) continue;   // stale: skip

        if (!SlotWork.IsValidIndex(Req.SlotIndex)) continue;
        if (!Builders.IsValidIndex(Req.SlotIndex)) continue;

        FSlotWork& W = *SlotWork[Req.SlotIndex];
        W.Key        = FromLODKey(Req.Key);
        W.Generation = Req.Generation;
        W.bBuilt     = false;
        W.bDone.Store(false);

        // Collision requirement was already determined by LOD in ReconcileStreaming
        W.bNeedsCollision = Req.bNeedsCollision;

        FPlanetChunkWorker::Launch(&Generator, Builders[Req.SlotIndex].Get(),
                                   &W, Req.Generation, PlanetRadiusMetres);

        if (++Launched >= MaxNewRequestsPerFrame) break;
    }
}

// PumpWorkers: take queued assignments and launch tasks.
// ApplyReadyChunks: commit finished work on the game thread.
//
// The generation check is the crux: a worker that finished after its slot was
// reassigned reports a stale generation, and applying it would show the wrong
// geometry in a component that now belongs to another chunk. It is dropped; the
// slot is not lost, just re-queued.
// ─────────────────────────────────────────────────────────────────────────────
void AProceduralPlanet::ApplyReadyChunks()
{
    int32 Committed = 0;

    for (int32 i = 0; i < SlotWork.Num() && Committed < MaxCommitsPerFrame; ++i)
    {
        FSlotWork& W = *SlotWork[i];

        // Acquire load: pairs with the worker's release store, so the stream
        // set writes above are visible here.
        if (!W.bDone.Load()) continue;

        PlanetStreaming::Completion C;
        C.Key        = PlanetLOD::ChunkKey{W.Key.Face, W.Key.LOD, W.Key.X, W.Key.Y};
        C.SlotIndex  = i;
        C.Generation = W.Generation;

        if (!Scheduler.AcceptCompletion(C) || !W.bBuilt)
        {
            W.bDone.Store(false);
            continue;
        }

        ConfigureComponentForChunk(i, W.Key);

        if (Pool.IsValidIndex(i) && Pool[i])
        {
            URealtimeMeshSimple* MeshSimple = Pool[i]->GetRealtimeMeshAs<URealtimeMeshSimple>();
            if (MeshSimple)
            {
                RealtimeMesh::FRealtimeMeshStreamSet Streams = Builders[i]->TakeStreamSet();
                const FRealtimeMeshSectionGroupKey GK =
                    FRealtimeMeshSectionGroupKey::Create(0, TEXT("Chunk"));

                MeshSimple->RemoveSectionGroup(GK);
                MeshSimple->CreateSectionGroup(GK, MoveTemp(Streams));

                const FRealtimeMeshSectionKey SK =
                    FRealtimeMeshSectionKey::CreateForPolyGroup(GK, 0);

                // Collision is cooked only for near chunks: it is the most
                // expensive per-chunk step and a pedestrian never needs it
                // kilometres away.
                const bool bWantCollision = W.bNeedsCollision;
                FRealtimeMeshSectionConfig Config;
                MeshSimple->UpdateSectionConfig(SK, Config, bWantCollision);

                Pool[i]->SetVisibility(true);
                Pool[i]->SetCollisionEnabled(bWantCollision
                    ? ECollisionEnabled::QueryAndPhysics
                    : ECollisionEnabled::NoCollision);
            }
        }

        Scheduler.MarkActive(C);
        W.bDone.Store(false);
        ++Committed;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ReleaseRetired: second half of the two-phase retirement.
//
// A slot is never reused while its old geometry might still be referenced. The
// scheduler hands back physical indices only here, after the component has been
// hidden and its collision disabled, which is what makes reuse safe.
// ─────────────────────────────────────────────────────────────────────────────
void AProceduralPlanet::ReleaseRetired()
{
    const std::vector<int32> Freed = Scheduler.DrainRetires(MaxRetiresPerFrame);
    for (int32 SlotIndex : Freed)
    {
        if (Pool.IsValidIndex(SlotIndex) && Pool[SlotIndex])
        {
            URealtimeMeshSimple* MeshSimple = Pool[SlotIndex]->GetRealtimeMeshAs<URealtimeMeshSimple>();
            if (MeshSimple)
            {
                MeshSimple->RemoveSectionGroup(
                    FRealtimeMeshSectionGroupKey::Create(0, TEXT("Chunk")));
            }
            Pool[SlotIndex]->SetVisibility(false);
            Pool[SlotIndex]->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        }
    }
}

void AProceduralPlanet::ConfigureComponentForChunk(int32 SlotIndex, const FChunkKey& Key)
{
    if (!Pool.IsValidIndex(SlotIndex) || !Pool[SlotIndex]) return;

    const FVector3d CentreM = PlanetCoordinates::GetChunkCentrePosition(Key, PlanetRadiusMetres);
    const FVector OriginUE = FVector(CentreM) * PLANET_METRES_TO_UE_CM + FVector(GetActorLocation());

    // The only place the large coordinate reaches the renderer, and it lives in
    // the component transform where LWC handles it.
    Pool[SlotIndex]->SetWorldLocation(OriginUE);
}

FString AProceduralPlanet::GetStreamingStatsString() const
{
    const PlanetStreaming::Stats S = Scheduler.GetStats();

    // LOD distribution: count chunks per LOD level
    TMap<uint8, int32> LODCounts;
    for (const PlanetLOD::ChunkKey& K : LastSelection.Chunks)
        LODCounts.FindOrAdd(K.LOD)++;

    // Build LOD stats string: "L0:6 L1:24 L12:128 L13:64"
    FString LODStats;
    TArray<uint8> LODs;
    LODCounts.GetKeys(LODs);
    LODs.Sort();
    for (uint8 L : LODs)
        LODStats += FString::Printf(TEXT("L%d:%d "), L, LODCounts[L]);

    return FString::Printf(
        TEXT("Pool %d | Active %d Ready %d Building %d Requested %d Free %d Retiring %d | ")
        TEXT("Desired %d Rejected %d Stale %d | LOD leaves %d visited %d budgetHit:%s | %s"),
        S.PoolSize, S.Active, S.Ready, S.Building, S.Requested, S.Free, S.Retiring,
        S.Desired, S.RejectedByCapacity, S.StaleCompletionsDropped,
        (int32)LastSelection.Chunks.size(), LastSelection.Visited,
        LastSelection.bBudgetHit ? TEXT("YES") : TEXT("NO"),
        *LODStats);
}


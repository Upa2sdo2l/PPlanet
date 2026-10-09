// ProceduralPlanet.cpp
#include "ProceduralPlanet.h"

#include "RealtimeMeshSimple.h"
#include "RealtimeMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Async/Async.h"
#include "DrawDebugHelpers.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Stats/Stats.h"

// ─────────────────────────────────────────────────────────────────────────────
// Two ways to see the planet's costs:
//   planet.Stats 1  — on-screen overlay, always available (simplest);
//   stat Planet     — the same numbers in UE's stats system. The group is
//                     registered on the planet's first tick, so it is missing
//                     from console autocomplete; type it in full.
//
// stat Planet (PIE or game, any non-Shipping build):
//
// Game-thread stages are cycle counters: their per-frame cost shows directly.
// Worker stages (noise, mesh build) run on pool threads, so they are timed by
// hand in the worker and shown as per-chunk averages over the last second,
// next to the per-chunk game-thread cost of the RMC calls for comparison.
// Not shown: render-thread GPU upload of a new section and the async collision
// cook; those are not on the game thread.
// ─────────────────────────────────────────────────────────────────────────────
DECLARE_STATS_GROUP(TEXT("Planet"), STATGROUP_Planet, STATCAT_Advanced);

// Game thread, per frame
DECLARE_CYCLE_STAT(TEXT("GT: Tick total"),                       STAT_Planet_Tick,          STATGROUP_Planet);
DECLARE_CYCLE_STAT(TEXT("GT: terrain height under camera"),      STAT_Planet_CameraHeight,  STATGROUP_Planet);
DECLARE_CYCLE_STAT(TEXT("GT: LOD traversal"),                    STAT_Planet_LOD,           STATGROUP_Planet);
DECLARE_CYCLE_STAT(TEXT("GT: streaming reconcile"),              STAT_Planet_Reconcile,     STATGROUP_Planet);
DECLARE_CYCLE_STAT(TEXT("GT: launch workers"),                   STAT_Planet_Pump,          STATGROUP_Planet);
DECLARE_CYCLE_STAT(TEXT("GT: commit chunks (all)"),              STAT_Planet_Apply,         STATGROUP_Planet);
DECLARE_CYCLE_STAT(TEXT("GT: RMC RemoveSectionGroup"),           STAT_Planet_RMCRemove,     STATGROUP_Planet);
DECLARE_CYCLE_STAT(TEXT("GT: RMC CreateSectionGroup"),           STAT_Planet_RMCCreate,     STATGROUP_Planet);
DECLARE_CYCLE_STAT(TEXT("GT: RMC UpdateSectionConfig+collision"),STAT_Planet_RMCConfig,     STATGROUP_Planet);
DECLARE_CYCLE_STAT(TEXT("GT: component transform/visibility"),   STAT_Planet_Component,     STATGROUP_Planet);
DECLARE_CYCLE_STAT(TEXT("GT: release retired slots"),            STAT_Planet_Release,       STATGROUP_Planet);

// Per chunk, averaged over the last second
DECLARE_FLOAT_COUNTER_STAT(TEXT("Chunk: worker noise, ms avg"),       STAT_Planet_NoiseAvg,  STATGROUP_Planet);
DECLARE_FLOAT_COUNTER_STAT(TEXT("Chunk: worker noise, ms max"),       STAT_Planet_NoiseMax,  STATGROUP_Planet);
DECLARE_FLOAT_COUNTER_STAT(TEXT("Chunk: worker mesh build, ms avg"),  STAT_Planet_BuildAvg,  STATGROUP_Planet);
DECLARE_FLOAT_COUNTER_STAT(TEXT("Chunk: worker mesh build, ms max"),  STAT_Planet_BuildMax,  STATGROUP_Planet);
DECLARE_FLOAT_COUNTER_STAT(TEXT("Chunk: GT commit (RMC+component), ms avg"), STAT_Planet_RMCAvg, STATGROUP_Planet);
DECLARE_FLOAT_COUNTER_STAT(TEXT("Chunk: GT commit (RMC+component), ms max"), STAT_Planet_RMCMax, STATGROUP_Planet);
DECLARE_DWORD_COUNTER_STAT(TEXT("Chunks built per second"),           STAT_Planet_BuiltPerSec,     STATGROUP_Planet);
DECLARE_DWORD_COUNTER_STAT(TEXT("Chunks committed per second"),       STAT_Planet_CommittedPerSec, STATGROUP_Planet);

// State
DECLARE_DWORD_COUNTER_STAT(TEXT("Camera: LOD of chunk under camera"),   STAT_Planet_NadirLOD,       STATGROUP_Planet);
DECLARE_DWORD_COUNTER_STAT(TEXT("Camera: collision on chunk under camera (1/0)"), STAT_Planet_NadirCollision, STATGROUP_Planet);
DECLARE_FLOAT_COUNTER_STAT(TEXT("Camera: height above terrain, m"),     STAT_Planet_CameraAGL,      STATGROUP_Planet);
DECLARE_DWORD_COUNTER_STAT(TEXT("LOD: leaves"),                         STAT_Planet_Leaves,         STATGROUP_Planet);
DECLARE_DWORD_COUNTER_STAT(TEXT("LOD: deepest level"),                  STAT_Planet_MaxLevel,       STATGROUP_Planet);
DECLARE_DWORD_COUNTER_STAT(TEXT("LOD: budget hit (1/0)"),               STAT_Planet_BudgetHit,      STATGROUP_Planet);
DECLARE_DWORD_COUNTER_STAT(TEXT("Pool: active"),                        STAT_Planet_Active,         STATGROUP_Planet);
DECLARE_DWORD_COUNTER_STAT(TEXT("Pool: waiting for a worker"),          STAT_Planet_Requested,      STATGROUP_Planet);
DECLARE_DWORD_COUNTER_STAT(TEXT("Pool: building"),                      STAT_Planet_Building,       STATGROUP_Planet);
DECLARE_DWORD_COUNTER_STAT(TEXT("Pool: retiring"),                      STAT_Planet_Retiring,       STATGROUP_Planet);
DECLARE_DWORD_COUNTER_STAT(TEXT("Pool: free"),                          STAT_Planet_Free,           STATGROUP_Planet);

namespace
{
    inline FChunkKey FromLODKey(const PlanetLOD::ChunkKey& K)
    {
        return FChunkKey(K.Face, K.LOD, K.X, K.Y);
    }

    inline double PlanetMsSince(uint64 StartCycles)
    {
        return FPlatformTime::ToMilliseconds64(FPlatformTime::Cycles64() - StartCycles);
    }

    // Adds the scope's wall time (ms) to an accumulator. Works in every build
    // configuration, unlike the stats system; feeds the planet.Stats overlay.
    struct FPlanetScopedMs
    {
        explicit FPlanetScopedMs(double& InAcc) : Acc(InAcc), Start(FPlatformTime::Cycles64()) {}
        ~FPlanetScopedMs() { Acc += PlanetMsSince(Start); }
        double& Acc;
        uint64  Start;
    };
}

// "planet.Stats 1" in the console: on-screen profiling overlay. Independent of
// the stats system, so it needs no "stat" command and shows in autocomplete.
static TAutoConsoleVariable<int32> CVarPlanetStats(
    TEXT("planet.Stats"),
    0,
    TEXT("1 = show the planet profiling overlay: per-chunk cost of noise, mesh build and ")
    TEXT("commit, game-thread time per stage, and the LOD/collision of the chunk under the camera."),
    ECVF_Default);

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
        if (!Work) return;
        if (!Gen || !Gen->IsValid() || !Builder)
        {
            // Still report completion: the scheduler keeps the slot reserved
            // until the game thread sees bDone, so a silent return here would
            // lose the slot for good.
            Work->bBuilt = false;
            Work->NoiseMs = 0.0;
            Work->BuildMs = 0.0;
            Work->Generation = Generation;
            Work->bDone.Store(true);
            return;
        }

        // 1. Sample the surface. This is the expensive step: min 0.95 ms per
        //    chunk for 4225 vertices, measured on the reference box.
        TArrayView<PlanetCore::Surface> View(Work->Surfaces.GetData(),
                                             Work->Surfaces.Num());
        const uint64 NoiseStart = FPlatformTime::Cycles64();
        Gen->SampleChunk(Work->Key, View);
        Work->NoiseMs = PlanetMsSince(NoiseStart);

        // 2. Build the stream set with the worker's own builder instance.
        FVector3d Origin;
        const uint64 BuildStart = FPlatformTime::Cycles64();
        Work->bBuilt = Builder->Build(Work->Key, Work->Surfaces.GetData(),
                                      PlanetRadius, Origin);
        Work->BuildMs = PlanetMsSince(BuildStart);

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
    // Workers hold raw pointers into SlotWork, Builders and Generator. Let
    // every running task finish before any of them is destroyed.
    for (const TUniquePtr<FSlotWork>& W : SlotWork)
    {
        if (W.IsValid() && W->Future.IsValid())
        {
            W->Future.Wait();
        }
    }

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
    Generator.Build(NoiseParams, PlanetRadiusMetres);
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

    SCOPE_CYCLE_COUNTER(STAT_Planet_Tick);
    FPlanetScopedMs TickTimer(PerfWindow.TickMs);

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

    UpdatePlanetStats();

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
    CameraTerrainHeightM = 0.0;
    if (CameraPos.SizeSquared() > 1.0)
    {
        SCOPE_CYCLE_COUNTER(STAT_Planet_CameraHeight);
        FPlanetScopedMs HeightTimer(PerfWindow.HeightMs);
        CameraTerrainHeightM = Generator.GetHeightAt(CameraPos.GetSafeNormal());
        P.SurfaceOffsetCm = CameraTerrainHeightM * PLANET_METRES_TO_UE_CM;
    }
    P.MaxLOD           = (uint8_t)FMath::Clamp(MaxLOD, 1, 18);

    // THE fixed-pool link: the traversal can never return more leaves than the
    // pool can display. If a view needs more detail, ErrorThresholdPixels is
    // what gives, never the component count.
    P.MaxLeaves        = PLANET_COMPONENT_POOL_SIZE;
    P.bUseHorizonCull  = true;

    P.PreviouslySplit  = &LODPreviouslySplit;
    P.HysteresisFactor = LODHysteresis;

    SCOPE_CYCLE_COUNTER(STAT_Planet_LOD);
    FPlanetScopedMs LODTimer(PerfWindow.LODMs);
    PlanetLOD::Traverse(P, LastSelection);
    LODPreviouslySplit = PlanetLOD::InternalNodesOf(LastSelection.Chunks);
}

void AProceduralPlanet::ReconcileStreaming(uint64 Frame, const FVector3d& CameraPos)
{
    SCOPE_CYCLE_COUNTER(STAT_Planet_Reconcile);
    FPlanetScopedMs ReconcileTimer(PerfWindow.ReconcileMs);

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
    SCOPE_CYCLE_COUNTER(STAT_Planet_Pump);
    FPlanetScopedMs PumpTimer(PerfWindow.PumpMs);

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

        // MarkBuilding guarantees no other worker owns this slot's scratch;
        // the scheduler keeps the slot reserved until WorkerFinished.
        W.Future = FPlanetChunkWorker::Launch(&Generator, Builders[Req.SlotIndex].Get(),
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
    SCOPE_CYCLE_COUNTER(STAT_Planet_Apply);
    FPlanetScopedMs ApplyTimer(PerfWindow.ApplyMs);

    int32 Committed = 0;

    for (int32 i = 0; i < SlotWork.Num() && Committed < MaxCommitsPerFrame; ++i)
    {
        FSlotWork& W = *SlotWork[i];

        // Acquire load: pairs with the worker's release store, so the stream
        // set writes above are visible here.
        if (!W.bDone.Load()) continue;

        // The worker has returned, whatever happens to its result below:
        // the slot's scratch may be reused (and a retired slot released).
        Scheduler.WorkerFinished(i);

        // Worker CPU time counts even when the result is dropped as stale:
        // it was spent either way.
        if (W.bBuilt)
        {
            ++PerfWindow.Built;
            PerfWindow.NoiseMs += W.NoiseMs;
            PerfWindow.BuildMs += W.BuildMs;
            PerfWindow.NoiseMax = FMath::Max(PerfWindow.NoiseMax, W.NoiseMs);
            PerfWindow.BuildMax = FMath::Max(PerfWindow.BuildMax, W.BuildMs);
        }

        PlanetStreaming::Completion C;
        C.Key        = PlanetLOD::ChunkKey{W.Key.Face, W.Key.LOD, W.Key.X, W.Key.Y};
        C.SlotIndex  = i;
        C.Generation = W.Generation;

        if (!Scheduler.AcceptCompletion(C) || !W.bBuilt)
        {
            W.bDone.Store(false);
            continue;
        }

        const uint64 CommitStart = FPlatformTime::Cycles64();

        {
            SCOPE_CYCLE_COUNTER(STAT_Planet_Component);
            ConfigureComponentForChunk(i, W.Key);
        }

        if (Pool.IsValidIndex(i) && Pool[i])
        {
            URealtimeMeshSimple* MeshSimple = Pool[i]->GetRealtimeMeshAs<URealtimeMeshSimple>();
            if (MeshSimple)
            {
                RealtimeMesh::FRealtimeMeshStreamSet Streams = Builders[i]->TakeStreamSet();
                const FRealtimeMeshSectionGroupKey GK =
                    FRealtimeMeshSectionGroupKey::Create(0, TEXT("Chunk"));

                {
                    SCOPE_CYCLE_COUNTER(STAT_Planet_RMCRemove);
                    FPlanetScopedMs RemoveTimer(PerfWindow.RemoveMs);
                    MeshSimple->RemoveSectionGroup(GK);
                }
                {
                    SCOPE_CYCLE_COUNTER(STAT_Planet_RMCCreate);
                    FPlanetScopedMs CreateTimer(PerfWindow.CreateMs);
                    MeshSimple->CreateSectionGroup(GK, MoveTemp(Streams));
                }

                const FRealtimeMeshSectionKey SK =
                    FRealtimeMeshSectionKey::CreateForPolyGroup(GK, 0);

                // Collision is cooked only for near chunks: it is the most
                // expensive per-chunk step and a pedestrian never needs it
                // kilometres away.
                const bool bWantCollision = W.bNeedsCollision;
                {
                    SCOPE_CYCLE_COUNTER(STAT_Planet_RMCConfig);
                    FPlanetScopedMs ConfigTimer(PerfWindow.ConfigMs);
                    FRealtimeMeshSectionConfig Config;
                    MeshSimple->UpdateSectionConfig(SK, Config, bWantCollision);
                }

                {
                    SCOPE_CYCLE_COUNTER(STAT_Planet_Component);
                    Pool[i]->SetVisibility(true);
                    Pool[i]->SetCollisionEnabled(bWantCollision
                        ? ECollisionEnabled::QueryAndPhysics
                        : ECollisionEnabled::NoCollision);
                }
            }
        }

        const double CommitMs = PlanetMsSince(CommitStart);
        ++PerfWindow.Committed;
        PerfWindow.RMCMs += CommitMs;
        PerfWindow.RMCMax = FMath::Max(PerfWindow.RMCMax, CommitMs);

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
    SCOPE_CYCLE_COUNTER(STAT_Planet_Release);
    FPlanetScopedMs ReleaseTimer(PerfWindow.ReleaseMs);

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


// ─────────────────────────────────────────────────────────────────────────────
// stat Planet
// ─────────────────────────────────────────────────────────────────────────────
void AProceduralPlanet::UpdatePlanetStats()
{
    // Close the one-second window that feeds the averages.
    ++PerfWindow.Frames;
    const double Now = FPlatformTime::Seconds();
    if (PerfWindow.WindowStart <= 0.0) PerfWindow.WindowStart = Now;
    if (Now - PerfWindow.WindowStart >= 1.0)
    {
        PerfPublished = PerfWindow;
        PerfWindow = FPerfWindow();
        PerfWindow.WindowStart = Now;
    }

    // ── Under-camera diagnostics ─────────────────────────────────────────
    // The chunk under the camera: its LOD, and whether its component has
    // collision switched on. Enabled means the cook was requested; with
    // async cooking the body can still be a few frames behind.
    NadirLOD = 0;                  // 0 also when no leaf was found
    bNadirCollision = false;
    const double CamDistCm = LastCameraPosUE.Size();
    CameraAboveTerrainM = CamDistCm / PLANET_METRES_TO_UE_CM - (PlanetRadiusMetres + CameraTerrainHeightM);
    if (CamDistCm > 1.0)
    {
        const FVector3d Dir = LastCameraPosUE / CamDistCm;
        int32 Face = 0;
        double U = 0.0, V = 0.0;
        PlanetLOD::DirectionToFaceUV(PlanetLOD::Vec3d{Dir.X, Dir.Y, Dir.Z}, Face, U, V);

        const int32 Leaf = PlanetLOD::FindLeafContaining(LastSelection.Chunks, Face, U, V);
        if (Leaf >= 0)
        {
            const PlanetLOD::ChunkKey& NadirKey = LastSelection.Chunks[Leaf];
            NadirLOD = NadirKey.LOD;

            const std::vector<PlanetStreaming::Slot>& Slots = Scheduler.GetSlots();
            for (int32 s = 0; s < (int32)Slots.size(); ++s)
            {
                if (Slots[s].State != PlanetStreaming::SlotState::Active || Slots[s].Key != NadirKey) continue;
                bNadirCollision = Pool.IsValidIndex(s) && Pool[s] &&
                    Pool[s]->GetCollisionEnabled() != ECollisionEnabled::NoCollision;
                break;
            }
        }
    }

    int32 DeepestLevel = 0;
    for (const PlanetLOD::ChunkKey& K : LastSelection.Chunks)
        DeepestLevel = FMath::Max(DeepestLevel, (int32)K.LOD);

    const PlanetStreaming::Stats S = Scheduler.GetStats();
    const FPerfWindow& W = PerfPublished;
    const double NoiseAvg = W.Built     > 0 ? W.NoiseMs / W.Built     : 0.0;
    const double BuildAvg = W.Built     > 0 ? W.BuildMs / W.Built     : 0.0;
    const double RMCAvg   = W.Committed > 0 ? W.RMCMs   / W.Committed : 0.0;

#if STATS
    SET_FLOAT_STAT(STAT_Planet_NoiseAvg, (float)NoiseAvg);
    SET_FLOAT_STAT(STAT_Planet_NoiseMax, (float)W.NoiseMax);
    SET_FLOAT_STAT(STAT_Planet_BuildAvg, (float)BuildAvg);
    SET_FLOAT_STAT(STAT_Planet_BuildMax, (float)W.BuildMax);
    SET_FLOAT_STAT(STAT_Planet_RMCAvg,   (float)RMCAvg);
    SET_FLOAT_STAT(STAT_Planet_RMCMax,   (float)W.RMCMax);
    SET_DWORD_STAT(STAT_Planet_BuiltPerSec,     W.Built);
    SET_DWORD_STAT(STAT_Planet_CommittedPerSec, W.Committed);

    SET_DWORD_STAT(STAT_Planet_Active,    S.Active);
    SET_DWORD_STAT(STAT_Planet_Requested, S.Requested);
    SET_DWORD_STAT(STAT_Planet_Building,  S.Building);
    SET_DWORD_STAT(STAT_Planet_Retiring,  S.Retiring);
    SET_DWORD_STAT(STAT_Planet_Free,      S.Free);

    SET_DWORD_STAT(STAT_Planet_Leaves,    (int32)LastSelection.Chunks.size());
    SET_DWORD_STAT(STAT_Planet_MaxLevel,  DeepestLevel);
    SET_DWORD_STAT(STAT_Planet_BudgetHit, LastSelection.bBudgetHit ? 1 : 0);

    SET_DWORD_STAT(STAT_Planet_NadirLOD,       NadirLOD);
    SET_DWORD_STAT(STAT_Planet_NadirCollision, bNadirCollision ? 1 : 0);
    SET_FLOAT_STAT(STAT_Planet_CameraAGL,      (float)CameraAboveTerrainM);
#endif

    // ── planet.Stats overlay ─────────────────────────────────────────────
    if (CVarPlanetStats.GetValueOnGameThread() == 0 || !GEngine) return;

    const double F = W.Frames > 0 ? 1.0 / (double)W.Frames : 0.0;   // per-frame averages

    const FString Line1 = FString::Printf(
        TEXT("[Planet] per chunk, last 1 s:  noise %.2f ms (max %.2f)  |  mesh build %.2f ms (max %.2f)  |  ")
        TEXT("GT commit %.2f ms (max %.2f)  |  built %d/s, committed %d/s"),
        NoiseAvg, W.NoiseMax, BuildAvg, W.BuildMax, RMCAvg, W.RMCMax, W.Built, W.Committed);

    const FString Line2 = FString::Printf(
        TEXT("[Planet] game thread per frame:  total %.3f ms  =  height %.3f + LOD %.3f + reconcile %.3f + ")
        TEXT("launch %.3f + commit %.3f (RMC remove %.3f, create %.3f, config %.3f) + release %.3f"),
        W.TickMs * F, W.HeightMs * F, W.LODMs * F, W.ReconcileMs * F, W.PumpMs * F,
        W.ApplyMs * F, W.RemoveMs * F, W.CreateMs * F, W.ConfigMs * F, W.ReleaseMs * F);

    const FString Line3 = FString::Printf(
        TEXT("[Planet] under camera: LOD %d, collision %s, %.1f m above terrain  |  leaves %d, deepest L%d, budget %s  |  ")
        TEXT("pool: active %d, waiting %d, building %d, retiring %d, free %d"),
        NadirLOD, bNadirCollision ? TEXT("ON") : TEXT("OFF"), CameraAboveTerrainM,
        (int32)LastSelection.Chunks.size(), DeepestLevel, LastSelection.bBudgetHit ? TEXT("full") : TEXT("not full"),
        S.Active, S.Requested, S.Building, S.Retiring, S.Free);

    // Key -1 + 0 s: drawn for this frame only. Newer messages go on top, so
    // add in reverse to read top-down.
    const FColor Colour = bNadirCollision ? FColor::Cyan : FColor::Orange;
    GEngine->AddOnScreenDebugMessage(-1, 0.0f, Colour, Line3);
    GEngine->AddOnScreenDebugMessage(-1, 0.0f, FColor::Cyan, Line2);
    GEngine->AddOnScreenDebugMessage(-1, 0.0f, FColor::Cyan, Line1);
}

// ─────────────────────────────────────────────────────────────────────────────
// planet.Bench — synchronous per-stage benchmark.
//
// Everything runs on the game thread, one chunk at a time, so each number is
// the plain CPU cost of that stage for one chunk with no contention. The RMC
// numbers are the game-thread cost of the calls; the GPU upload they schedule
// on the render thread and the async collision cook are not included.
// ─────────────────────────────────────────────────────────────────────────────
FString AProceduralPlanet::RunBenchmark(int32 Count, int32 LOD)
{
    Count = FMath::Clamp(Count, 1, 1024);
    LOD   = FMath::Clamp(LOD, 0, 18);

    if (!Generator.IsValid())
    {
        const FString Msg = TEXT("[Planet] planet.Bench: the noise generator is not built (start PIE first)");
        UE_LOG(LogTemp, Warning, TEXT("%s"), *Msg);
        return Msg;
    }

    // Chunks around the camera at the requested LOD, in a square block.
    int32 Face = 0;
    double U = 0.0, V = 0.0;
    const double CamDistCm = LastCameraPosUE.Size();
    if (CamDistCm > 1.0)
    {
        const FVector3d Dir = LastCameraPosUE / CamDistCm;
        PlanetLOD::DirectionToFaceUV(PlanetLOD::Vec3d{Dir.X, Dir.Y, Dir.Z}, Face, U, V);
    }
    const int32  Tiles    = PlanetLOD::TilesAt((uint8)LOD);
    const double TileSize = 2.0 / (double)Tiles;
    const int32  CX = FMath::Clamp((int32)FMath::FloorToDouble((U + 1.0) / TileSize), 0, Tiles - 1);
    const int32  CY = FMath::Clamp((int32)FMath::FloorToDouble((V + 1.0) / TileSize), 0, Tiles - 1);
    const int32  Side = FMath::Max(1, FMath::CeilToInt(FMath::Sqrt((float)Count)));

    TArray<PlanetCore::Surface> Surfaces;
    Surfaces.SetNumUninitialized(PLANET_BODY_VERTS);
    FPlanetChunkMeshBuilder Builder;

    // A temporary component outside the pool, so the live planet is untouched.
    URealtimeMeshComponent* Comp = NewObject<URealtimeMeshComponent>(this);
    Comp->SetupAttachment(RootComponent);
    Comp->RegisterComponent();
    Comp->SetMobility(EComponentMobility::Movable);
    URealtimeMeshSimple* Mesh = Comp->InitializeRealtimeMesh<URealtimeMeshSimple>();
    if (Mesh)
    {
        if (PlanetMaterial) Mesh->SetupMaterialSlot(0, TEXT("Planet"), PlanetMaterial);
        FRealtimeMeshCollisionConfiguration Collision;
        Collision.bUseComplexAsSimpleCollision = true;
        Collision.bUseAsyncCook = true;
        Mesh->SetCollisionConfig(Collision);
    }
    Comp->SetVisibility(false);

    TArray<double> NoiseMs, BuildMs, CreateMs, ConfigMs, TotalMs;
    const FRealtimeMeshSectionGroupKey GK = FRealtimeMeshSectionGroupKey::Create(0, TEXT("Chunk"));
    const FRealtimeMeshSectionKey      SK = FRealtimeMeshSectionKey::CreateForPolyGroup(GK, 0);

    for (int32 i = 0; i < Count; ++i)
    {
        const FChunkKey Key((uint8)Face, (uint8)LOD,
            FMath::Clamp(CX - Side / 2 + i % Side, 0, Tiles - 1),
            FMath::Clamp(CY - Side / 2 + i / Side, 0, Tiles - 1));

        uint64 T = FPlatformTime::Cycles64();
        Generator.SampleChunk(Key, TArrayView<PlanetCore::Surface>(Surfaces.GetData(), Surfaces.Num()));
        NoiseMs.Add(PlanetMsSince(T));

        T = FPlatformTime::Cycles64();
        FVector3d Origin;
        Builder.Build(Key, Surfaces.GetData(), PlanetRadiusMetres, Origin);
        BuildMs.Add(PlanetMsSince(T));

        if (Mesh)
        {
            RealtimeMesh::FRealtimeMeshStreamSet Streams = Builder.TakeStreamSet();
            T = FPlatformTime::Cycles64();
            Mesh->RemoveSectionGroup(GK);
            Mesh->CreateSectionGroup(GK, MoveTemp(Streams));
            CreateMs.Add(PlanetMsSince(T));

            T = FPlatformTime::Cycles64();
            FRealtimeMeshSectionConfig Config;
            Mesh->UpdateSectionConfig(SK, Config, true);
            ConfigMs.Add(PlanetMsSince(T));
        }

        TotalMs.Add(NoiseMs.Last() + BuildMs.Last()
                    + (CreateMs.Num() ? CreateMs.Last() : 0.0)
                    + (ConfigMs.Num() ? ConfigMs.Last() : 0.0));
    }

    Comp->DestroyComponent();

    auto Row = [](const TCHAR* Name, const TArray<double>& Ms) -> FString
    {
        if (Ms.Num() == 0) return FString::Printf(TEXT("%s: n/a"), Name);
        double Sum = 0.0, Min = Ms[0], Max = Ms[0];
        for (double Value : Ms) { Sum += Value; Min = FMath::Min(Min, Value); Max = FMath::Max(Max, Value); }
        return FString::Printf(TEXT("%s: avg %.3f ms | min %.3f | max %.3f"),
                               Name, Sum / Ms.Num(), Min, Max);
    };

    TArray<FString> Lines;
    Lines.Add(FString::Printf(
        TEXT("[Planet] planet.Bench: %d chunks, LOD %d (side ~%.0f m), game thread, one chunk at a time"),
        Count, LOD, TileSize * PlanetRadiusMetres));
    Lines.Add(Row(TEXT("  noise (SampleChunk)         "), NoiseMs));
    Lines.Add(Row(TEXT("  mesh build (stream set)     "), BuildMs));
    Lines.Add(Row(TEXT("  RMC remove + create section "), CreateMs));
    Lines.Add(Row(TEXT("  RMC config + collision call "), ConfigMs));
    Lines.Add(Row(TEXT("  total per chunk             "), TotalMs));
    Lines.Add(TEXT("  not included: render-thread GPU upload, async collision cook"));

    FString Report;
    for (const FString& L : Lines)
    {
        UE_LOG(LogTemp, Log, TEXT("%s"), *L);
        if (GEngine) GEngine->AddOnScreenDebugMessage(-1, 30.f, FColor::Yellow, L, false);
        Report += L + TEXT("\n");
    }
    return Report;
}

static void PlanetBenchCommand(const TArray<FString>& Args, UWorld* World)
{
    if (!World) return;
    const int32 Count = Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 32;
    const int32 LOD   = Args.Num() > 1 ? FCString::Atoi(*Args[1]) : 12;

    bool bFound = false;
    for (TActorIterator<AProceduralPlanet> It(World); It; ++It)
    {
        It->RunBenchmark(Count, LOD);
        bFound = true;
    }
    if (!bFound)
    {
        UE_LOG(LogTemp, Warning, TEXT("[Planet] planet.Bench: no AProceduralPlanet in this world"));
    }
}

static FAutoConsoleCommandWithWorldAndArgs GPlanetBenchCommand(
    TEXT("planet.Bench"),
    TEXT("planet.Bench [Count=32] [LOD=12]: times noise, mesh build and RMC calls per chunk on the game thread."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&PlanetBenchCommand));

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
DECLARE_CYCLE_STAT(TEXT("GT: coverage swap + show/hide/collision"),STAT_Planet_Coverage,     STATGROUP_Planet);

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
DECLARE_DWORD_COUNTER_STAT(TEXT("Pool: kept until replaced (lingering)"),STAT_Planet_Lingering,     STATGROUP_Planet);
DECLARE_DWORD_COUNTER_STAT(TEXT("Pool: built, waiting to swap in"),      STAT_Planet_Hidden,        STATGROUP_Planet);
DECLARE_DWORD_COUNTER_STAT(TEXT("Pool: chunks with collision"),          STAT_Planet_WithCollision, STATGROUP_Planet);
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
        Work->bBuilt     = false;
        Work->bMeshBuilt = false;
        Work->NoiseMs    = 0.0;
        Work->BuildMs    = 0.0;

        if (!Gen || !Gen->IsValid() || !Builder)
        {
            // Still report completion: the scheduler keeps the slot reserved
            // until the game thread sees bDone, so a silent return here would
            // lose the slot for good.
            Work->Generation = Generation;
            Work->bDone.Store(true);
            return;
        }

        FVector3d Origin;

        if (Work->Job == AProceduralPlanet::FSlotWork::EJob::CollisionMesh)
        {
            // The surfaces were sampled by this slot's Full job and have not
            // changed since: the slot is Active and reserved for this job.
            const uint64 BuildStart = FPlatformTime::Cycles64();
            Work->bMeshBuilt = Builder->Build(Work->Key, Work->Surfaces.GetData(), PlanetRadius, Origin);
            Work->bBuilt     = Work->bMeshBuilt;
            Work->BuildMs    = PlanetMsSince(BuildStart);
            Work->Generation = Generation;
            Work->bDone.Store(true);
            return;
        }

        // 1. Sample the surface. This is the expensive step: ~1 ms per chunk
        //    for 4225 vertices on one core.
        TArrayView<PlanetCore::Surface> View(Work->Surfaces.GetData(),
                                             Work->Surfaces.Num());
        const uint64 NoiseStart = FPlatformTime::Cycles64();
        Gen->SampleChunk(Work->Key, View);
        Work->NoiseMs = PlanetMsSince(NoiseStart);

        // 2. GPU renderer: the atlas tile and instance transform. Mesh: only
        //    when the chunk needs collision (GPU) or always (RealtimeMesh).
        const uint64 BuildStart = FPlatformTime::Cycles64();
        if (Work->Tile.IsValid())
        {
            PlanetGpu::BuildTile(PlanetCore::FChunkKey(Work->Key.Face, Work->Key.LOD, Work->Key.X, Work->Key.Y),
                                 Work->Surfaces.GetData(), PlanetRadius, *Work->Tile);
        }
        if (Work->bBuildMesh)
        {
            Work->bMeshBuilt = Builder->Build(Work->Key, Work->Surfaces.GetData(), PlanetRadius, Origin);
        }
        Work->bBuilt  = Work->Tile.IsValid() || Work->bMeshBuilt;
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
    InitGpuTerrain();
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
    SlotVisibleApplied.Reset();
    SlotCollisionApplied.Reset();
    GpuSlots.Reset();
    bGPUTerrain = false;
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
        // Lumen here uses software tracing; hardware ray-tracing structures for
        // 300 chunk meshes cost ~110 MB and a rebuild on every chunk swap.
        Comp->bVisibleInRayTracing = false;
        Comp->bAffectDistanceFieldLighting = false;
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
        SlotVisibleApplied.Add(0);
        SlotCollisionApplied.Add(0);

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

    // ── 5. Swap in chunks whose replacements are ready; show/hide; collision ─
    {
        SCOPE_CYCLE_COUNTER(STAT_Planet_Coverage);
        FPlanetScopedMs CoverageTimer(PerfWindow.CoverageMs);
        Scheduler.UpdateCoverage(FrameCounter, MakeFrameBudget());
        if (bGPUTerrain)
        {
            GpuRebaseIfNeeded(CameraRel);
            SyncSlotPresentationGpu();
        }
        else
        {
            SyncSlotPresentation();
        }
    }
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

    // The leaf budget is below the pool size: the spare slots keep replaced
    // chunks on screen until their replacements are built (PlanetStreaming).
    P.MaxLeaves        = PLANET_LOD_LEAF_BUDGET;
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

    // Distances are measured to chunk centres on the sphere through the
    // terrain under the camera (as the LOD error does), so a camera on a high
    // plateau is near the chunks around it, not kilometres above them.
    const double SurfaceRadiusCm = (PlanetRadiusMetres + CameraTerrainHeightM) * PLANET_METRES_TO_UE_CM;
    const double PlanetRadiusCm  = PlanetRadiusMetres * PLANET_METRES_TO_UE_CM;
    const double CollisionCm     = CollisionDistanceMetres * PLANET_METRES_TO_UE_CM;

    for (const PlanetLOD::ChunkKey& K : LastSelection.Chunks)
    {
        double U0, V0, Size;
        PlanetLOD::ChunkExtent(K, U0, V0, Size);
        const PlanetLOD::Vec3d Dir = PlanetLOD::CubeFaceDirection(
            K.Face, U0 + Size * 0.5, V0 + Size * 0.5);

        const double Dx = CameraPos.X - Dir.X * SurfaceRadiusCm;
        const double Dy = CameraPos.Y - Dir.Y * SurfaceRadiusCm;
        const double Dz = CameraPos.Z - Dir.Z * SurfaceRadiusCm;
        const double Dist = std::sqrt(Dx*Dx + Dy*Dy + Dz*Dz);

        // Nearest point of the chunk, approximated by its bounding circle
        // (half diagonal of a side of Size * radius).
        const double NearestCm = FMath::Max(0.0, Dist - Size * PlanetRadiusCm * 0.7072);

        // Hysteresis: on within CollisionDistanceMetres, off only beyond 1.25x.
        const int32 SlotIndex = Scheduler.FindSlotIndex(K);
        const bool  bHadCollision = SlotIndex >= 0 && Scheduler.GetSlots()[SlotIndex].bNeedsCollision;

        PlanetStreaming::DesiredChunk D;
        D.Key             = K;
        D.Priority        = Dist;                    // lower = closer = built first
        D.bNeedsCollision = NearestCm <= CollisionCm || (bHadCollision && NearestCm <= CollisionCm * 1.25);
        Desired.push_back(D);
    }

    Scheduler.Reconcile(Desired, Frame, MakeFrameBudget());
}

PlanetStreaming::FrameBudget AProceduralPlanet::MakeFrameBudget() const
{
    PlanetStreaming::FrameBudget Budget;
    Budget.MaxNewWorkerRequests = MaxNewRequestsPerFrame;
    Budget.MaxCommits           = MaxCommitsPerFrame;
    Budget.MaxRetires           = MaxRetiresPerFrame;
    Budget.SettleFrames         = ReplacementSettleFrames;
    return Budget;
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

        W.bNeedsCollision = Req.bNeedsCollision;
        W.Job             = FSlotWork::EJob::Full;
        // GPU renderer: the mesh exists only to carry collision.
        W.bBuildMesh      = !bGPUTerrain || Req.bNeedsCollision;

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

        if (W.Job == FSlotWork::EJob::CollisionMesh)
        {
            // Applied only if the slot still holds the same chunk and still
            // wants collision; otherwise the stream set is simply dropped.
            const PlanetStreaming::Slot& Slot = Scheduler.GetSlots()[i];
            if (W.bMeshBuilt && Slot.State == PlanetStreaming::SlotState::Active &&
                Slot.Generation == W.Generation && Slot.bNeedsCollision)
            {
                ApplyCollisionMesh(i);
            }
            else if (W.bMeshBuilt)
            {
                Builders[i]->TakeStreamSet();
            }
            W.bDone.Store(false);
            ++Committed;
            continue;
        }

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

        if (bGPUTerrain)
        {
            // Tile into the atlas; the instance stays hidden until
            // SyncSlotPresentationGpu swaps it in. Collision, if the chunk is
            // close, comes from the RMC component, which is never visible.
            GpuUploadTile(i, *W.Tile);
            if (W.bMeshBuilt && Scheduler.GetSlots()[i].bNeedsCollision)
            {
                ApplyCollisionMesh(i);
            }
            else if (W.bMeshBuilt)
            {
                Builders[i]->TakeStreamSet();
            }

            const double CommitMs = PlanetMsSince(CommitStart);
            ++PerfWindow.Committed;
            PerfWindow.RMCMs += CommitMs;
            PerfWindow.RMCMax = FMath::Max(PerfWindow.RMCMax, CommitMs);

            Scheduler.MarkActive(C);
            W.bDone.Store(false);
            ++Committed;
            continue;
        }

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

                // Collision is cooked only for chunks within
                // CollisionDistanceMetres. Read the scheduler's current flag,
                // not the one captured at launch: the camera may have moved.
                const bool bWantCollision = Scheduler.GetSlots()[i].bNeedsCollision;
                {
                    SCOPE_CYCLE_COUNTER(STAT_Planet_RMCConfig);
                    FPlanetScopedMs ConfigTimer(PerfWindow.ConfigMs);
                    FRealtimeMeshSectionConfig Config;
                    MeshSimple->UpdateSectionConfig(SK, Config, bWantCollision);
                }

                // Built but not shown yet: SyncSlotPresentation shows it once
                // nothing it replaces still covers the same surface. Collision
                // goes on now, so it is cooked by the time the swap happens.
                {
                    SCOPE_CYCLE_COUNTER(STAT_Planet_Component);
                    Pool[i]->SetVisibility(false);
                    Pool[i]->SetCollisionEnabled(bWantCollision
                        ? ECollisionEnabled::QueryAndPhysics
                        : ECollisionEnabled::NoCollision);
                    SlotVisibleApplied[i]   = 0;
                    SlotCollisionApplied[i] = bWantCollision ? 1 : 0;
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
            SlotVisibleApplied[SlotIndex]   = 0;
            SlotCollisionApplied[SlotIndex] = 0;
        }
        if (GpuSlots.IsValidIndex(SlotIndex))
        {
            // The instance was hidden by SyncSlotPresentationGpu when the slot
            // started retiring; the section group is gone now.
            GpuSlots[SlotIndex].bHasCollisionMesh = false;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// SyncSlotPresentation: push the scheduler's per-slot decisions to UE.
//
// Runs right after Scheduler.UpdateCoverage. A retired chunk is hidden here,
// in the same frame the chunks replacing it are shown, even when the retire
// budget delays its release to a later frame. Collision changes on already
// built chunks (camera moved closer or away) are applied closest first,
// MaxCollisionChangesPerFrame at a time.
// ─────────────────────────────────────────────────────────────────────────────
void AProceduralPlanet::SyncSlotPresentation()
{
    const std::vector<PlanetStreaming::Slot>& Slots = Scheduler.GetSlots();
    const int32 Count = FMath::Min((int32)Slots.size(), Pool.Num());

    struct FCollisionChange { int32 SlotIndex; double Priority; bool bOn; };
    TArray<FCollisionChange, TInlineAllocator<32>> CollisionChanges;

    for (int32 i = 0; i < Count; ++i)
    {
        URealtimeMeshComponent* Comp = Pool[i];
        if (!Comp) continue;
        const PlanetStreaming::Slot& Slot = Slots[i];

        if (Slot.State == PlanetStreaming::SlotState::Retiring)
        {
            if (SlotVisibleApplied[i])
            {
                Comp->SetVisibility(false);
                SlotVisibleApplied[i] = 0;
            }
            if (SlotCollisionApplied[i])
            {
                Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
                SlotCollisionApplied[i] = 0;
            }
            continue;
        }
        if (Slot.State != PlanetStreaming::SlotState::Active) continue;

        const uint8 bWantVisible = Slot.bShown ? 1 : 0;
        if (SlotVisibleApplied[i] != bWantVisible)
        {
            Comp->SetVisibility(bWantVisible != 0);
            SlotVisibleApplied[i] = bWantVisible;
        }

        const uint8 bWantCollision = Slot.bNeedsCollision ? 1 : 0;
        if (SlotCollisionApplied[i] != bWantCollision)
        {
            CollisionChanges.Add({i, Slot.Priority, bWantCollision != 0});
        }
    }

    CollisionChanges.Sort([](const FCollisionChange& A, const FCollisionChange& B)
    {
        return A.Priority < B.Priority;
    });

    const FRealtimeMeshSectionGroupKey GK = FRealtimeMeshSectionGroupKey::Create(0, TEXT("Chunk"));
    const FRealtimeMeshSectionKey      SK = FRealtimeMeshSectionKey::CreateForPolyGroup(GK, 0);
    const int32 Budget = FMath::Min(CollisionChanges.Num(), FMath::Max(1, MaxCollisionChangesPerFrame));

    for (int32 c = 0; c < Budget; ++c)
    {
        const FCollisionChange& Change = CollisionChanges[c];
        URealtimeMeshComponent* Comp = Pool[Change.SlotIndex];
        if (URealtimeMeshSimple* MeshSimple = Comp->GetRealtimeMeshAs<URealtimeMeshSimple>())
        {
            FRealtimeMeshSectionConfig Config;
            MeshSimple->UpdateSectionConfig(SK, Config, Change.bOn);
        }
        Comp->SetCollisionEnabled(Change.bOn ? ECollisionEnabled::QueryAndPhysics
                                             : ECollisionEnabled::NoCollision);
        SlotCollisionApplied[Change.SlotIndex] = Change.bOn ? 1 : 0;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Collision meshes (GPU renderer): the RMC component of a slot is never
// visible there and holds a section only while the chunk needs collision.
// ─────────────────────────────────────────────────────────────────────────────
void AProceduralPlanet::LaunchCollisionMeshJob(int32 SlotIndex)
{
    if (!SlotWork.IsValidIndex(SlotIndex) || !Builders.IsValidIndex(SlotIndex)) return;
    if (!Scheduler.BeginSideJob(SlotIndex)) return;

    FSlotWork& W = *SlotWork[SlotIndex];
    W.Job        = FSlotWork::EJob::CollisionMesh;
    W.Generation = Scheduler.GetSlots()[SlotIndex].Generation;
    W.bBuilt     = false;
    W.bMeshBuilt = false;
    W.bDone.Store(false);

    W.Future = FPlanetChunkWorker::Launch(&Generator, Builders[SlotIndex].Get(),
                                          &W, W.Generation, PlanetRadiusMetres);
}

// Takes the slot builder's stream set: the caller has just had it built.
void AProceduralPlanet::ApplyCollisionMesh(int32 SlotIndex)
{
    if (!Pool.IsValidIndex(SlotIndex) || !Pool[SlotIndex]) return;
    URealtimeMeshSimple* MeshSimple = Pool[SlotIndex]->GetRealtimeMeshAs<URealtimeMeshSimple>();
    if (!MeshSimple) return;

    ConfigureComponentForChunk(SlotIndex, SlotWork[SlotIndex]->Key);

    const FRealtimeMeshSectionGroupKey GK = FRealtimeMeshSectionGroupKey::Create(0, TEXT("Chunk"));
    const FRealtimeMeshSectionKey      SK = FRealtimeMeshSectionKey::CreateForPolyGroup(GK, 0);
    RealtimeMesh::FRealtimeMeshStreamSet Streams = Builders[SlotIndex]->TakeStreamSet();
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
    {
        SCOPE_CYCLE_COUNTER(STAT_Planet_RMCConfig);
        FPlanetScopedMs ConfigTimer(PerfWindow.ConfigMs);
        FRealtimeMeshSectionConfig Config;
        MeshSimple->UpdateSectionConfig(SK, Config, true);
    }

    Pool[SlotIndex]->SetVisibility(false);
    Pool[SlotIndex]->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    SlotVisibleApplied[SlotIndex]   = 0;
    SlotCollisionApplied[SlotIndex] = 1;
    if (GpuSlots.IsValidIndex(SlotIndex)) GpuSlots[SlotIndex].bHasCollisionMesh = true;
}

void AProceduralPlanet::RemoveCollisionMesh(int32 SlotIndex)
{
    if (!Pool.IsValidIndex(SlotIndex) || !Pool[SlotIndex]) return;
    if (URealtimeMeshSimple* MeshSimple = Pool[SlotIndex]->GetRealtimeMeshAs<URealtimeMeshSimple>())
    {
        MeshSimple->RemoveSectionGroup(FRealtimeMeshSectionGroupKey::Create(0, TEXT("Chunk")));
    }
    Pool[SlotIndex]->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    SlotCollisionApplied[SlotIndex] = 0;
    if (GpuSlots.IsValidIndex(SlotIndex)) GpuSlots[SlotIndex].bHasCollisionMesh = false;
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
    // What is actually under the camera now: the LOD of the chunk drawn there
    // (during a swap that can be the old, lingering chunk), and whether any
    // built chunk there has collision on. "On" means the cook was requested;
    // with async cooking the body can be a few frames behind.
    NadirLOD = 0;                  // 0 when nothing is drawn under the camera
    bNadirCollision = false;
    const double CamDistCm = LastCameraPosUE.Size();
    CameraAboveTerrainM = CamDistCm / PLANET_METRES_TO_UE_CM - (PlanetRadiusMetres + CameraTerrainHeightM);
    int32 WithCollision = 0;
    for (int32 s = 0; s < SlotCollisionApplied.Num(); ++s)
        WithCollision += SlotCollisionApplied[s];

    if (CamDistCm > 1.0)
    {
        const FVector3d Dir = LastCameraPosUE / CamDistCm;
        int32 Face = 0;
        double U = 0.0, V = 0.0;
        PlanetLOD::DirectionToFaceUV(PlanetLOD::Vec3d{Dir.X, Dir.Y, Dir.Z}, Face, U, V);

        const std::vector<PlanetStreaming::Slot>& Slots = Scheduler.GetSlots();
        for (int32 s = 0; s < (int32)Slots.size() && s < SlotVisibleApplied.Num(); ++s)
        {
            const PlanetStreaming::Slot& Slot = Slots[s];
            if (Slot.State != PlanetStreaming::SlotState::Active || Slot.Key.Face != Face) continue;

            const int32  Tiles    = PlanetLOD::TilesAt(Slot.Key.LOD);
            const double TileSize = 2.0 / (double)Tiles;
            const int32  TX = FMath::Clamp((int32)FMath::FloorToDouble((U + 1.0) / TileSize), 0, Tiles - 1);
            const int32  TY = FMath::Clamp((int32)FMath::FloorToDouble((V + 1.0) / TileSize), 0, Tiles - 1);
            if (TX != Slot.Key.X || TY != Slot.Key.Y) continue;

            const bool bDrawn = bGPUTerrain ? (GpuSlots.IsValidIndex(s) && GpuSlots[s].bShown)
                                            : (SlotVisibleApplied[s] != 0);
            if (bDrawn)                  NadirLOD = Slot.Key.LOD;
            if (SlotCollisionApplied[s]) bNadirCollision = true;
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
    SET_DWORD_STAT(STAT_Planet_Lingering, S.Lingering);
    SET_DWORD_STAT(STAT_Planet_Hidden,    S.Hidden);
    SET_DWORD_STAT(STAT_Planet_WithCollision, WithCollision);

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
        TEXT("[Planet] %s renderer. Per chunk, last 1 s:  noise %.2f ms (max %.2f)  |  %s %.2f ms (max %.2f)  |  ")
        TEXT("GT commit %.2f ms (max %.2f)  |  built %d/s, committed %d/s"),
        bGPUTerrain ? TEXT("GPU") : TEXT("RealtimeMesh"),
        NoiseAvg, W.NoiseMax, bGPUTerrain ? TEXT("tile + collision mesh") : TEXT("mesh build"),
        BuildAvg, W.BuildMax, RMCAvg, W.RMCMax, W.Built, W.Committed);

    const FString Line2 = FString::Printf(
        TEXT("[Planet] game thread per frame:  total %.3f ms  =  height %.3f + LOD %.3f + reconcile %.3f + ")
        TEXT("launch %.3f + commit %.3f (RMC remove %.3f, create %.3f, config %.3f) + swap/show/collision %.3f + release %.3f"),
        W.TickMs * F, W.HeightMs * F, W.LODMs * F, W.ReconcileMs * F, W.PumpMs * F,
        W.ApplyMs * F, W.RemoveMs * F, W.CreateMs * F, W.ConfigMs * F, W.CoverageMs * F, W.ReleaseMs * F);

    const FString Line3 = FString::Printf(
        TEXT("[Planet] under camera: LOD %d, collision %s, %.1f m above terrain  |  leaves %d, deepest L%d, budget %s  |  ")
        TEXT("pool %d: active %d (kept until replaced %d, waiting to swap in %d), waiting for worker %d, building %d, ")
        TEXT("retiring %d, free %d, with collision %d"),
        NadirLOD, bNadirCollision ? TEXT("ON") : TEXT("OFF"), CameraAboveTerrainM,
        (int32)LastSelection.Chunks.size(), DeepestLevel, LastSelection.bBudgetHit ? TEXT("full") : TEXT("not full"),
        S.PoolSize, S.Active, S.Lingering, S.Hidden, S.Requested, S.Building, S.Retiring, S.Free, WithCollision);

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

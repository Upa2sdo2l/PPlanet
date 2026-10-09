// PlanetStreaming.h
// -----------------------------------------------------------------------------
// UE-FREE streaming scheduler for a HARD-FIXED RMC component pool.
//
// POLICY (decided, not configurable at runtime):
//   - The pool has a fixed number of slots (default 64).
//   - It NEVER grows. If demand exceeds capacity the scheduler keeps the
//     CLOSEST chunks and evicts the farthest; the LOD selection then lowers
//     detail. We do not buy detail with unbounded components and VRAM.
//
// Why a separate core: worker completion and RMC application are Unreal's job,
// but the hard part is a state machine, and state machines are testable here
// without the editor:
//
//   * a stale worker result must never overwrite a newer request
//   * a retired slot must never be applied after reassignment
//   * requests must be issued closest-first
//   * capacity pressure must evict, never grow the pool
//   * no key may ever own two slots
// -----------------------------------------------------------------------------
#pragma once

#include "PlanetLOD.h"
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <queue>
#include <algorithm>

namespace PlanetStreaming
{

static constexpr int32_t DEFAULT_POOL_SIZE = 64;

// One slot maps 1:1 to one preallocated RMC component. Its index is stable
// for the lifetime of the actor; only the logical chunk it serves changes.
enum class SlotState : uint8_t
{
    Free,       // no assigned key; RMC hidden
    Requested,  // assigned, queued for a worker
    Building,   // a worker is running
    Ready,      // worker finished; waiting for the game-thread commit budget
    Active,     // visible RMC section belongs to this key
    Retiring,   // hidden / collision off; waiting to be released and reused
};

struct ChunkKeyHash
{
    size_t operator()(const PlanetLOD::ChunkKey& K) const
    {
        uint32_t H = K.Face;
        H = H * 1000003u ^ K.LOD;
        H = H * 1000003u ^ (uint32_t)(K.X + 0x80000000u);
        H = H * 1000003u ^ (uint32_t)(K.Y + 0x80000000u);
        return (size_t)H;
    }
};

struct Slot
{
    PlanetLOD::ChunkKey Key{};
    SlotState State = SlotState::Free;

    // Incremented EVERY time this physical slot changes key. A worker captures
    // it; on completion a mismatching generation means the result is stale and
    // is dropped. This is the central anti-race rule.
    uint32_t Generation = 0;

    // Lower = more important. Camera distance combined with LOD. This alone
    // decides which chunks survive capacity pressure.
    double Priority = 0.0;

    uint64_t LastTouchedFrame = 0;
    bool     bNeedsCollision  = false;
};

struct WorkerRequest
{
    PlanetLOD::ChunkKey Key{};
    int32_t  SlotIndex  = -1;
    uint32_t Generation = 0;
    double   Priority   = 0.0;
    bool     bNeedsCollision = false;
};

struct Completion
{
    PlanetLOD::ChunkKey Key{};
    int32_t  SlotIndex  = -1;
    uint32_t Generation = 0;
};

struct FrameBudget
{
    int32_t MaxNewWorkerRequests = 4;
    int32_t MaxCommits           = 4;
    int32_t MaxRetires           = 8;
};

struct DesiredChunk
{
    PlanetLOD::ChunkKey Key{};
    double Priority = 0.0;
    bool   bNeedsCollision = false;
};

struct Stats
{
    int32_t PoolSize  = 0;
    int32_t Free      = 0;
    int32_t Requested = 0;
    int32_t Building  = 0;
    int32_t Ready     = 0;
    int32_t Active    = 0;
    int32_t Retiring  = 0;
    int32_t Desired   = 0;
    int32_t RejectedByCapacity   = 0;
    int32_t StaleCompletionsDropped = 0;
    int32_t QueuedWorkerRequests = 0;
};

class Scheduler
{
public:
    explicit Scheduler(int32_t InPoolSize = DEFAULT_POOL_SIZE);

    int32_t GetPoolSize() const { return (int32_t)Slots.size(); }
    const std::vector<Slot>& GetSlots() const { return Slots; }

    // Reconcile the active set with a freshly computed desired set. Touches no
    // UObject, starts no worker: it only assigns slots, retires unwanted
    // chunks, and fills the worker-request queue.
    void Reconcile(const std::vector<DesiredChunk>& Desired,
                   uint64_t Frame,
                   const FrameBudget& Budget);

    // Closest-first. Returns false when the queue is empty.
    bool PopWorkerRequest(WorkerRequest& Out);

    // Called immediately before the real worker starts. A queued request can
    // be invalidated before it consumes CPU; this catches that cheaply.
    bool MarkBuilding(const WorkerRequest& Request);

    // Game thread, on async completion. Returns true only if this is still the
    // current owner/generation of the slot.
    bool AcceptCompletion(const Completion& In);

    // Game thread, after RMC CreateSectionGroup succeeded.
    bool MarkActive(const Completion& In);

    // Physical slot indices that the UE side must release, then reuse.
    std::vector<int32_t> DrainRetires(int32_t MaxCount);

    // Test/debug invariant: no key owns two slots, no Free slot carries a key,
    // and the key map agrees with the slot array.
    bool ValidateInvariants(char* OutWhy, int32_t WhyLen) const;

    Stats GetStats() const;

private:
    struct PendingCompare
    {
        bool operator()(const WorkerRequest& A, const WorkerRequest& B) const
        {
            // max-heap: reverse so lower Priority (closer) pops first.
            // Slot index makes the order deterministic for replay.
            if (A.Priority != B.Priority) return A.Priority > B.Priority;
            return A.SlotIndex > B.SlotIndex;
        }
    };

    int32_t FindFreeSlot() const;
    int32_t FindEvictionCandidate(double IncomingPriority) const;
    void Assign(int32_t SlotIndex, const DesiredChunk& D, uint64_t Frame);
    void Retire(int32_t SlotIndex);

    std::vector<Slot> Slots;
    std::unordered_map<PlanetLOD::ChunkKey, int32_t, ChunkKeyHash> KeyToSlot;
    std::priority_queue<WorkerRequest, std::vector<WorkerRequest>, PendingCompare> Pending;
    std::vector<int32_t> RetireQueue;

    int32_t LastDesiredCount = 0;
    int32_t LastRejected     = 0;
    int32_t StaleDropped     = 0;
};

} // namespace PlanetStreaming

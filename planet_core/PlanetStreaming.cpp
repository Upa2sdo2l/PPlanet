// PlanetStreaming.cpp
#include "PlanetStreaming.h"
#include <cstdio>
#include <unordered_set>

namespace PlanetStreaming
{

Scheduler::Scheduler(int32_t InPoolSize)
{
    // A zero-sized pool is never meaningful and turns every caller into a
    // special case. Clamp here, once. The application uses 64.
    const int32_t Size = std::max(1, InPoolSize);
    Slots.resize((size_t)Size);
    KeyToSlot.reserve((size_t)Size * 2);
}

int32_t Scheduler::FindFreeSlot() const
{
    for (int32_t i = 0; i < (int32_t)Slots.size(); ++i)
        if (Slots[i].State == SlotState::Free && !Slots[i].bWorkerInFlight) return i;
    return -1;
}

int32_t Scheduler::FindEvictionCandidate(double IncomingPriority) const
{
    // A slot is evictable only when it is less important than the incoming
    // chunk. Never evict a closer chunk just to churn the visible set.
    // Retiring slots are deliberately skipped: their RMC is already being
    // hidden/released and will become Free via DrainRetires.
    int32_t Candidate = -1;
    double WorstPriority = -1.0;

    for (int32_t i = 0; i < (int32_t)Slots.size(); ++i)
    {
        const Slot& S = Slots[i];
        if (S.State == SlotState::Free || S.State == SlotState::Retiring) continue;
        if (S.Priority <= IncomingPriority) continue;

        if (Candidate < 0 || S.Priority > WorstPriority ||
            (S.Priority == WorstPriority && i > Candidate))
        {
            Candidate = i;
            WorstPriority = S.Priority;
        }
    }
    return Candidate;
}

void Scheduler::Assign(int32_t SlotIndex, const DesiredChunk& D, uint64_t Frame)
{
    Slot& S = Slots[SlotIndex];

    // A Free slot has no map entry. Retiring slots must be drained before
    // assignment; enforcing that here catches an unsafe UE integration early.
    if (S.State != SlotState::Free || S.bWorkerInFlight) return;

    S.Key = D.Key;
    S.State = SlotState::Requested;
    ++S.Generation;
    if (S.Generation == 0) ++S.Generation; // reserve 0 as "never assigned"
    S.Priority = D.Priority;
    S.LastTouchedFrame = Frame;
    S.bNeedsCollision = D.bNeedsCollision;
    KeyToSlot[D.Key] = SlotIndex;
}

void Scheduler::Retire(int32_t SlotIndex)
{
    Slot& S = Slots[SlotIndex];
    if (S.State == SlotState::Free || S.State == SlotState::Retiring) return;

    KeyToSlot.erase(S.Key);

    // Bump immediately, before UE hides the component. Any worker completion
    // that races with the retirement now sees the old generation and dies.
    ++S.Generation;
    if (S.Generation == 0) ++S.Generation;
    S.State = SlotState::Retiring;
    S.bNeedsCollision = false;
    RetireQueue.push_back(SlotIndex);
}

void Scheduler::Reconcile(const std::vector<DesiredChunk>& Desired,
                          uint64_t Frame,
                          const FrameBudget& Budget)
{
    LastDesiredCount = (int32_t)Desired.size();
    LastRejected = 0;

    // Canonicalise duplicate keys. LOD traversal normally produces unique
    // leaves; this defensive pass prevents one malformed desired list from
    // allocating two physical RMC components to the same chunk.
    std::vector<DesiredChunk> Sorted = Desired;
    std::sort(Sorted.begin(), Sorted.end(), [](const DesiredChunk& A, const DesiredChunk& B)
    {
        if (A.Priority != B.Priority) return A.Priority < B.Priority;
        if (A.Key.Face != B.Key.Face) return A.Key.Face < B.Key.Face;
        if (A.Key.LOD  != B.Key.LOD)  return A.Key.LOD  < B.Key.LOD;
        if (A.Key.X    != B.Key.X)    return A.Key.X    < B.Key.X;
        return A.Key.Y < B.Key.Y;
    });

    std::unordered_map<PlanetLOD::ChunkKey, DesiredChunk, ChunkKeyHash> Want;
    Want.reserve(Sorted.size() * 2);
    for (const DesiredChunk& D : Sorted)
        if (Want.find(D.Key) == Want.end()) Want.emplace(D.Key, D);

    // 1. Existing desired chunks stay assigned. Updating priority here affects
    // future capacity comparisons but does not restart already-building work.
    for (const auto& Pair : Want)
    {
        const auto It = KeyToSlot.find(Pair.first);
        if (It == KeyToSlot.end()) continue;
        Slot& S = Slots[It->second];
        S.Priority = Pair.second.Priority;
        S.LastTouchedFrame = Frame;
        S.bNeedsCollision = Pair.second.bNeedsCollision;
    }

    // 2. Retire no-longer-desired slots first. They are not immediately free:
    // UE must hide the RMC/disable collision on the game thread before reuse.
    // This deliberately avoids destroy/create churn and stale geometry flashes.
    for (int32_t i = 0; i < (int32_t)Slots.size(); ++i)
    {
        const Slot& S = Slots[i];
        if (S.State == SlotState::Free || S.State == SlotState::Retiring) continue;
        if (Want.find(S.Key) == Want.end()) Retire(i);
    }

    // 3. Assign missing chunks closest-first. This is where the hard cap is
    // enforced. A slot awaiting retirement is NOT reusable this frame; that is
    // intentional and prevents a result from being applied to the wrong RMC.
    int32_t Issued = 0;
    for (const DesiredChunk& D : Sorted)
    {
        if (KeyToSlot.find(D.Key) != KeyToSlot.end()) continue;
        if (Issued >= std::max(0, Budget.MaxNewWorkerRequests)) break;

        int32_t SlotIndex = FindFreeSlot();
        if (SlotIndex < 0)
        {
            // Capacity pressure: retire only a worse existing chunk. It will
            // be free NEXT reconcile after the UE layer drains it.
            const int32_t Evict = FindEvictionCandidate(D.Priority);
            if (Evict >= 0) Retire(Evict);
            ++LastRejected;
            continue;
        }

        Assign(SlotIndex, D, Frame);
        ++Issued;
    }

    // Anything missing because of the per-frame issue budget is NOT a capacity
    // rejection. It remains desired and will be reconsidered next frame.
}

bool Scheduler::PopWorkerRequest(WorkerRequest& Out)
{
    while (!Pending.empty()) Pending.pop(); // queue rebuilt from current slots

    for (int32_t i = 0; i < (int32_t)Slots.size(); ++i)
    {
        const Slot& S = Slots[i];
        if (S.State != SlotState::Requested) continue;
        // Never offered while a worker owns the slot: MarkBuilding would
        // refuse it, and the caller would get the same request back forever.
        if (S.bWorkerInFlight) continue;
        Pending.push(WorkerRequest{S.Key, i, S.Generation, S.Priority, S.bNeedsCollision});
    }

    if (Pending.empty()) return false;
    Out = Pending.top();
    Pending.pop();
    return true;
}

bool Scheduler::MarkBuilding(const WorkerRequest& R)
{
    if (R.SlotIndex < 0 || R.SlotIndex >= (int32_t)Slots.size()) return false;
    Slot& S = Slots[R.SlotIndex];

    if (S.State != SlotState::Requested || S.Generation != R.Generation || S.Key != R.Key)
        return false;  // stale queued request; worker must not start

    // Defensive: Assign never hands out a slot with a worker in flight, so
    // a Requested slot cannot have one. If it ever does, do not start a
    // second worker on the same scratch.
    if (S.bWorkerInFlight) return false;

    S.State = SlotState::Building;
    S.bWorkerInFlight = true;
    return true;
}

void Scheduler::WorkerFinished(int32_t SlotIndex)
{
    if (SlotIndex < 0 || SlotIndex >= (int32_t)Slots.size()) return;
    Slots[SlotIndex].bWorkerInFlight = false;
}

bool Scheduler::AcceptCompletion(const Completion& C)
{
    if (C.SlotIndex < 0 || C.SlotIndex >= (int32_t)Slots.size()) { ++StaleDropped; return false; }
    Slot& S = Slots[C.SlotIndex];

    if (S.State != SlotState::Building || S.Generation != C.Generation || S.Key != C.Key)
    {
        ++StaleDropped;
        return false;
    }

    S.State = SlotState::Ready;
    return true;
}

bool Scheduler::MarkActive(const Completion& C)
{
    if (C.SlotIndex < 0 || C.SlotIndex >= (int32_t)Slots.size()) return false;
    Slot& S = Slots[C.SlotIndex];

    if (S.State != SlotState::Ready || S.Generation != C.Generation || S.Key != C.Key)
        return false;

    S.State = SlotState::Active;
    return true;
}

std::vector<int32_t> Scheduler::DrainRetires(int32_t MaxCount)
{
    std::vector<int32_t> Out;
    const int32_t Budget = std::max(0, MaxCount);
    Out.reserve((size_t)std::min(Budget, (int32_t)RetireQueue.size()));

    // The UE actor calls this on its game thread, hides each returned RMC and
    // disables collision, then considers the physical slot free. The order is
    // stable; that keeps automated recordings deterministic.
    //
    // A slot whose worker is still running keeps its place in the queue: its
    // scratch is in use, so freeing it now would let Assign give it to a new
    // worker that writes into the same buffers.
    std::vector<int32_t> Kept;
    Kept.reserve(RetireQueue.size());

    for (const int32_t SlotIndex : RetireQueue)
    {
        Slot& S = Slots[SlotIndex];
        if (S.State != SlotState::Retiring) continue;   // already drained

        if (S.bWorkerInFlight || (int32_t)Out.size() >= Budget)
        {
            Kept.push_back(SlotIndex);
            continue;
        }

        S.Key = {};
        S.State = SlotState::Free;
        S.Priority = 0.0;
        S.LastTouchedFrame = 0;
        S.bNeedsCollision = false;
        Out.push_back(SlotIndex);
    }
    RetireQueue.swap(Kept);
    return Out;
}

bool Scheduler::ValidateInvariants(char* OutWhy, int32_t WhyLen) const
{
    auto Fail = [&](const char* Why) -> bool {
        if (OutWhy && WhyLen > 0) std::snprintf(OutWhy, (size_t)WhyLen, "%s", Why);
        return false;
    };

    std::unordered_set<PlanetLOD::ChunkKey, ChunkKeyHash> Seen;
    Seen.reserve(Slots.size() * 2);

    for (int32_t i = 0; i < (int32_t)Slots.size(); ++i)
    {
        const Slot& S = Slots[i];
        if (S.State == SlotState::Free)
        {
            if (S.Generation == 0 && (S.Key.Face || S.Key.LOD || S.Key.X || S.Key.Y))
                return Fail("Free slot carries a key");
            if (S.bWorkerInFlight)
                return Fail("Free slot still has a worker in flight");
            continue;
        }

        if (S.bWorkerInFlight && S.State != SlotState::Building && S.State != SlotState::Retiring)
            return Fail("Worker in flight on a slot that is neither Building nor Retiring");

        if (!Seen.insert(S.Key).second) return Fail("Two slots own one key");

        if (S.State != SlotState::Retiring)
        {
            const auto It = KeyToSlot.find(S.Key);
            if (It == KeyToSlot.end() || It->second != i) return Fail("Key map disagrees with slot array");
        }
        else if (KeyToSlot.find(S.Key) != KeyToSlot.end())
        {
            return Fail("Retiring key is still in key map");
        }
    }

    for (const auto& P : KeyToSlot)
    {
        if (P.second < 0 || P.second >= (int32_t)Slots.size()) return Fail("Key map has bad index");
        const Slot& S = Slots[P.second];
        if (S.State == SlotState::Free || S.State == SlotState::Retiring || S.Key != P.first)
            return Fail("Key map points at non-owning slot");
    }

    if (OutWhy && WhyLen > 0) std::snprintf(OutWhy, (size_t)WhyLen, "ok");
    return true;
}

Stats Scheduler::GetStats() const
{
    Stats S;
    S.PoolSize = (int32_t)Slots.size();
    S.Desired = LastDesiredCount;
    S.RejectedByCapacity = LastRejected;
    S.StaleCompletionsDropped = StaleDropped;

    for (const Slot& Slot : Slots)
    {
        switch (Slot.State)
        {
            case SlotState::Free:      ++S.Free; break;
            case SlotState::Requested: ++S.Requested; break;
            case SlotState::Building:  ++S.Building; break;
            case SlotState::Ready:     ++S.Ready; break;
            case SlotState::Active:    ++S.Active; break;
            case SlotState::Retiring:  ++S.Retiring; break;
        }
    }
    S.QueuedWorkerRequests = S.Requested;
    return S;
}

} // namespace PlanetStreaming

// PlanetStreaming.cpp
#include "PlanetStreaming.h"
#include <cstdio>
#include <unordered_set>

namespace PlanetStreaming
{

Scheduler::Scheduler(int32_t InPoolSize)
{
    // A zero-sized pool is never meaningful and turns every caller into a
    // special case. Clamp here, once.
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

int32_t Scheduler::FindLingeringToEvict() const
{
    // Only lingering slots are ever evicted. Evicting a DESIRED chunk (the old
    // policy) just moved the shortage: the evicted chunk was rebuilt a frame
    // later, doubling the work of every LOD change and opening a hole.
    int32_t Candidate = -1;
    for (int32_t i = 0; i < (int32_t)Slots.size(); ++i)
    {
        const Slot& S = Slots[i];
        if (S.State != SlotState::Active || !S.bLingering) continue;
        if (Candidate < 0) { Candidate = i; continue; }

        const Slot& C = Slots[Candidate];
        if (S.LingerSinceFrame < C.LingerSinceFrame ||
            (S.LingerSinceFrame == C.LingerSinceFrame && S.Priority > C.Priority))
        {
            Candidate = i;
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
    S.bLingering = false;
    S.LingerSinceFrame = 0;
    S.ActiveSinceFrame = 0;
    S.bShown = false;
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
    S.bLingering = false;
    S.bShown = false;
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
    CurrentWant.clear();
    for (const DesiredChunk& D : Sorted)
    {
        if (Want.find(D.Key) != Want.end()) continue;
        Want.emplace(D.Key, D);
        CurrentWant.push_back(D.Key);
    }

    // 1. Existing desired chunks stay assigned. Updating priority here affects
    // future capacity comparisons but does not restart already-building work.
    // A lingering chunk that is desired again simply stops lingering.
    for (const auto& Pair : Want)
    {
        const auto It = KeyToSlot.find(Pair.first);
        if (It == KeyToSlot.end()) continue;
        Slot& S = Slots[It->second];
        S.Priority = Pair.second.Priority;
        S.LastTouchedFrame = Frame;
        S.bNeedsCollision = Pair.second.bNeedsCollision;
        S.bLingering = false;
    }

    // 2. No-longer-desired slots. A chunk on screen lingers: it keeps covering
    // its surface (and keeps its collision) until UpdateCoverage sees its
    // replacements ready. Anything not on screen (still building, or built but
    // hidden) has nothing to cover and is retired now. Retired slots are not
    // immediately free: UE hides them first and DrainRetires releases them.
    for (int32_t i = 0; i < (int32_t)Slots.size(); ++i)
    {
        Slot& S = Slots[i];
        if (S.State == SlotState::Free || S.State == SlotState::Retiring) continue;
        if (Want.find(S.Key) != Want.end()) continue;

        if (S.State == SlotState::Active && S.bShown)
        {
            if (!S.bLingering)
            {
                S.bLingering = true;
                S.LingerSinceFrame = Frame;
            }
        }
        else
        {
            Retire(i);
        }
    }

    // 3. Assign missing chunks closest-first. A slot awaiting retirement is
    // NOT reusable this frame; that prevents a result from being applied to
    // the wrong RMC. With no free slot, give up the longest-lingering chunk
    // (free next frame); a desired chunk is never evicted.
    int32_t Issued = 0;
    for (const DesiredChunk& D : Sorted)
    {
        if (KeyToSlot.find(D.Key) != KeyToSlot.end()) continue;
        if (Issued >= std::max(0, Budget.MaxNewWorkerRequests)) break;

        int32_t SlotIndex = FindFreeSlot();
        if (SlotIndex < 0)
        {
            const int32_t Evict = FindLingeringToEvict();
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

void Scheduler::UpdateCoverage(uint64_t Frame, const FrameBudget& Budget)
{
    const uint64_t Settle   = (uint64_t)std::max(0, Budget.SettleFrames);
    const uint64_t MaxLinger = (uint64_t)std::max(1, Budget.MaxLingerFrames);

    for (Slot& S : Slots)
        if (S.State == SlotState::Active && S.ActiveSinceFrame == 0)
            S.ActiveSinceFrame = Frame;

    // A desired chunk counts as a ready replacement once it has been Active
    // for the settle time.
    auto IsSettled = [&](const PlanetLOD::ChunkKey& K)
    {
        const auto It = KeyToSlot.find(K);
        if (It == KeyToSlot.end()) return false;
        const Slot& R = Slots[It->second];
        return R.State == SlotState::Active && !R.bLingering &&
               Frame >= R.ActiveSinceFrame + Settle;
    };

    // 1. Retire every lingering chunk whose surface is now fully covered by
    //    settled replacements, has no replacement at all (e.g. now beyond
    //    the horizon), or has waited too long.
    std::vector<int32_t> StillLingering;
    for (int32_t i = 0; i < (int32_t)Slots.size(); ++i)
    {
        Slot& L = Slots[i];
        if (L.State != SlotState::Active || !L.bLingering) continue;

        bool bAnyReplacement = false;
        bool bAllSettled = true;
        for (const PlanetLOD::ChunkKey& K : CurrentWant)
        {
            if (!PlanetLOD::TilesOverlap(K, L.Key)) continue;
            bAnyReplacement = true;
            if (!IsSettled(K)) { bAllSettled = false; break; }
        }

        if (!bAnyReplacement || bAllSettled || Frame >= L.LingerSinceFrame + MaxLinger)
            Retire(i);
        else
            StillLingering.push_back(i);
    }

    // 2. Visibility. A lingering chunk stays visible; a desired Active chunk
    //    is shown unless a lingering chunk still covers part of its surface.
    //    When step 1 retires the last lingering chunk over an area, the chunks
    //    replacing it become visible in this same call: the swap is atomic.
    for (Slot& S : Slots)
    {
        if (S.State != SlotState::Active) { S.bShown = false; continue; }
        if (S.bLingering) { S.bShown = true; continue; }

        bool bCovered = false;
        for (const int32_t li : StillLingering)
        {
            if (PlanetLOD::TilesOverlap(S.Key, Slots[li].Key)) { bCovered = true; break; }
        }
        S.bShown = !bCovered;
    }
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
        S.bLingering = false;
        S.LingerSinceFrame = 0;
        S.ActiveSinceFrame = 0;
        S.bShown = false;
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

        if (S.bLingering && S.State != SlotState::Active)
            return Fail("Lingering slot is not Active");
        if (S.bShown && S.State != SlotState::Active)
            return Fail("Shown slot is not Active");

        // A Retiring slot still carries its old key, and that key may already
        // belong to a new slot (a retired slot can wait for its worker while
        // the chunk is wanted again). Only live slots must own keys uniquely.
        if (S.State == SlotState::Retiring)
        {
            const auto It = KeyToSlot.find(S.Key);
            if (It != KeyToSlot.end() && It->second == i)
                return Fail("Key map points at a retiring slot");
            continue;
        }

        if (!Seen.insert(S.Key).second) return Fail("Two slots own one key");

        const auto It = KeyToSlot.find(S.Key);
        if (It == KeyToSlot.end() || It->second != i) return Fail("Key map disagrees with slot array");
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
            case SlotState::Active:
                ++S.Active;
                if (Slot.bLingering) ++S.Lingering;
                else if (!Slot.bShown) ++S.Hidden;
                break;
            case SlotState::Retiring:  ++S.Retiring; break;
        }
    }
    S.QueuedWorkerRequests = S.Requested;
    return S;
}

} // namespace PlanetStreaming

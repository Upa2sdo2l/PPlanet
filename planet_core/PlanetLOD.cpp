// PlanetLOD.cpp — UE-free budgeted best-first LOD selection.
#include "PlanetLOD.h"

namespace PlanetLOD
{

namespace
{
    struct FrontierNode { ChunkKey Key; double Error; };

    // std::priority_queue is max-first; this makes the WORST (largest error)
    // node come out first, which is what best-first refinement wants.
    struct WorseFirst
    {
        bool operator()(const FrontierNode& A, const FrontierNode& B) const
        { return A.Error < B.Error; }
    };
}

void Traverse(const TraversalParams& P, Selection& Out)
{
    Out.Chunks.clear();
    Out.Visited = 0; Out.Culled = 0; Out.Split = 0;
    Out.BudgetStops = 0; Out.bBudgetHit = false;

    std::priority_queue<FrontierNode, std::vector<FrontierNode>, WorseFirst> Frontier;

    for (uint8_t F = 0; F < 6; ++F)
    {
        const ChunkKey Root{F, 0, 0, 0};
        ++Out.Visited;
        // Never cull LOD 0-1: they're essential fallback coverage from orbit
        if (ChunkHorizonCulled(Root, P)) { ++Out.Culled; continue; }
        Frontier.push({Root, ChunkError(Root, P)});
    }

    while (!Frontier.empty())
    {
        // Emitted + Frontier <= MaxLeaves holds at all times, which is what
        // bounds the active set to the fixed pool.
        const int32_t Held = (int32_t)Out.Chunks.size() + (int32_t)Frontier.size();

        // Split одного узла добавляет до 3 будущих листьев.
        // Останавливаемся до превышения бюджета, а frontier потом
        // дренируем полностью. Нельзя просто отбросить его хвост:
        // каждый узел frontier покрывает отдельную область поверхности.
        if (Held + 3 > P.MaxLeaves)
        {
            Out.bBudgetHit = true;
            break;
        }

        const FrontierNode Node = Frontier.top();
        Frontier.pop();

        const bool bCanSplit = Node.Key.LOD < P.MaxLOD;
        const bool bWanted   = Node.Error > P.ErrorThresholdPx;

        if (!bCanSplit || !bWanted) { Out.Chunks.push_back(Node.Key); continue; }

        ++Out.Split;
        const uint8_t ChildLOD = (uint8_t)(Node.Key.LOD + 1);
        const int32_t CX = Node.Key.X * 2, CY = Node.Key.Y * 2;

        const ChunkKey Children[4] = {
            {Node.Key.Face, ChildLOD, CX,     CY    },
            {Node.Key.Face, ChildLOD, CX + 1, CY    },
            {Node.Key.Face, ChildLOD, CX,     CY + 1},
            {Node.Key.Face, ChildLOD, CX + 1, CY + 1},
        };

        for (const ChunkKey& C : Children)
        {
            ++Out.Visited;
            // Never cull LOD 0-1: they're essential fallback coverage from orbit
            if (ChunkHorizonCulled(C, P)) { ++Out.Culled; continue; }
            Frontier.push({C, ChunkError(C, P)});
        }
    }

    while (!Frontier.empty())
    {
        Out.Chunks.push_back(Frontier.top().Key);
        Frontier.pop();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Edge classification.
//
// The first version inferred the neighbour's shared edge from the neighbour's
// TILE POSITION. That is wrong: a corner tile lies on two cube edges at once,
// so the rule picks one arbitrarily. This is the classic quadtree neighbour
// algorithm, which never guesses:
//
//   Climb while the tile does not touch the queried side of its parent.
//   The sibling across that side is then the neighbour node — done.
//   At LOD 0 the parent is a cube face, so the face-adjacency table resolves
//   the neighbour face and the parameter mapping (including the flip).
//   Descend: the neighbour node may be internal, so collect the active leaves
//   beneath it that overlap the queried border segment.
//
// The segment is carried as a parameter interval throughout, so a fine tile
// meeting a coarse neighbour reports exactly its own overlap, not the
// neighbour's whole border.
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
    inline bool TouchesSideOfParent(const ChunkKey& K, int32_t Side)
    {
        const int32_t cx = K.X & 1, cy = K.Y & 1;
        switch (Side)
        {
            case 0: return cx == 0;
            case 1: return cx == 1;
            case 2: return cy == 0;
            default:return cy == 1;
        }
    }

    inline ChunkKey SiblingAcross(const ChunkKey& K, int32_t Side)
    {
        ChunkKey S = K;
        switch (Side)
        {
            case 0: S.X = K.X - 1; break;
            case 1: S.X = K.X + 1; break;
            case 2: S.Y = K.Y - 1; break;
            default:S.Y = K.Y + 1; break;
        }
        return S;
    }

    inline int32_t OppositeSide(int32_t Side) { return Side ^ 1; }

    inline void TileInterval(const ChunkKey& K, int32_t Side, double& T0, double& T1)
    {
        const int32_t M = TilesAt(K.LOD);
        const int32_t k = (Side == 0 || Side == 1) ? K.Y : K.X;
        T0 = 2.0 * (double)k / (double)M - 1.0;
        T1 = 2.0 * (double)(k + 1) / (double)M - 1.0;
    }

    inline bool IntervalsOverlap(double A0, double A1, double B0, double B1)
    { return A1 > B0 && B1 > A0; }

    EdgeMatch Judge(const std::vector<ChunkKey>& Found, uint8_t MyLOD)
    {
        if (Found.empty()) return EdgeMatch::OpenBoundary;

        if (Found.size() == 1)
        {
            const uint8_t NL = Found[0].LOD;
            if (NL == MyLOD) return EdgeMatch::Clean;
            if (NL <  MyLOD) return EdgeMatch::CoarserNeighbour;
            return EdgeMatch::FinerNeighbours;
        }

        // Two or more: a genuine T-junction only if every neighbour is finer.
        for (const ChunkKey& F : Found)
            if (F.LOD <= MyLOD) return EdgeMatch::CoarserNeighbour;
        return EdgeMatch::FinerNeighbours;
    }

    void CollectLeaves(const ChunkKey& Node, int32_t Side, double T0, double T1,
                       const KeySet& Leaves, const KeySet& Internal,
                       std::vector<ChunkKey>& Out)
    {
        if (Leaves.count(Node)) { Out.push_back(Node); return; }
        if (Internal.count(Node) == 0) return;   // neither leaf nor split

        const uint8_t ChildLOD = (uint8_t)(Node.LOD + 1);
        const int32_t CX = Node.X * 2, CY = Node.Y * 2;

        for (int32_t c = 0; c < 4; ++c)
        {
            const ChunkKey Child{Node.Face, ChildLOD, CX + (c & 1), CY + (c >> 1)};

            const int32_t cx = Child.X & 1, cy = Child.Y & 1;
            const bool bOnSide =
                (Side == 0) ? (cx == 0) : (Side == 1) ? (cx == 1) :
                (Side == 2) ? (cy == 0) : (cy == 1);
            if (!bOnSide) continue;

            double C0, C1;
            TileInterval(Child, Side, C0, C1);
            if (!IntervalsOverlap(C0, C1, T0, T1)) continue;

            CollectLeaves(Child, Side, T0, T1, Leaves, Internal, Out);
        }
    }

    KeySet BuildInternal(const KeySet& Leaves)
    {
        KeySet Internal;
        Internal.reserve(Leaves.size() * 2);
        for (const ChunkKey& L : Leaves)
        {
            ChunkKey A = L;
            while (A.LOD > 0)
            {
                A = ChunkKey{A.Face, (uint8_t)(A.LOD - 1), A.X >> 1, A.Y >> 1};
                Internal.insert(A);
            }
        }
        return Internal;
    }
}

EdgeMatch ClassifyBorder(const ChunkKey& K, int32_t Side, const KeySet& Leaves)
{
    const KeySet Internal = BuildInternal(Leaves);

    double T0, T1;
    TileInterval(K, Side, T0, T1);

    ChunkKey N = K;
    while (N.LOD > 0)
    {
        if (!TouchesSideOfParent(N, Side))
        {
            const ChunkKey S = SiblingAcross(N, Side);
            std::vector<ChunkKey> Found;
            CollectLeaves(S, OppositeSide(Side), T0, T1, Leaves, Internal, Found);
            return Judge(Found, K.LOD);
        }
        N = ChunkKey{N.Face, (uint8_t)(N.LOD - 1), N.X >> 1, N.Y >> 1};
    }

    // At LOD 0 the parent is a whole cube face: use the adjacency table.
    const Neighbour Nb = FindNeighbourSameLOD(N, Side);
    if (!Nb.bValid) return EdgeMatch::OpenBoundary;

    const double TN0 = Nb.bFlipped ? -T1 : T0;
    const double TN1 = Nb.bFlipped ? -T0 : T1;

    std::vector<ChunkKey> Found;
    CollectLeaves(Nb.Key, (int32_t)Nb.NbrTableEdge, TN0, TN1, Leaves, Internal, Found);
    return Judge(Found, K.LOD);
}

EdgeStats ClassifyEdges(const std::vector<ChunkKey>& Active)
{
    EdgeStats S;
    KeySet Leaves;
    Leaves.reserve(Active.size() * 2);
    for (const ChunkKey& K : Active) Leaves.insert(K);

    for (const ChunkKey& K : Active)
    {
        for (int32_t Side = 0; Side < 4; ++Side)
        {
            ++S.Total;
            switch (ClassifyBorder(K, Side, Leaves))
            {
                case EdgeMatch::Clean:            ++S.Clean; break;
                case EdgeMatch::FinerNeighbours:  ++S.FinerNeighbours; break;
                case EdgeMatch::CoarserNeighbour: ++S.CoarserNeighbour; break;
                default:                          ++S.OpenBoundary; break;
            }
        }
    }
    return S;
}

} // namespace PlanetLOD

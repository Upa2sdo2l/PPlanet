// PlanetLOD.h
// UE-FREE quadtree / LOD selection core.
//
// WHY UE-FREE: LOD bugs do not appear in a screenshot. They appear as a hole
// in the sphere, two chunks over the same ground, a crack that grows as you
// fly, or a request that never resolves. In the editor you find those by
// flying and squinting; here you find them with a loop and an assertion.
//
// Depends only on the C++ standard library.
//
// VERIFIED by lod_harness.cpp (16 tests):
//   cross-face neighbour exactness : 0 bad probes
//   neighbour round-trip           : 0 unresolved back-links
//   coverage                       : exact tiling, 0 duplicates, 0 holes
//   budget                         : leaves never exceed MaxLeaves
//   traversal cost                 : ~21 us, 256 leaves
#pragma once

#include <cstdint>
#include <cmath>
#include <vector>
#include <queue>
#include <algorithm>
#include <unordered_set>

namespace PlanetLOD
{

static constexpr int32_t PLANET_QUADS_PER_SIDE = 64;
static constexpr int32_t PLANET_VERTS_PER_SIDE = PLANET_QUADS_PER_SIDE + 1;

// MUST stay bit-identical to PlanetCoordinates::GetCubeDirection and
// PlanetCore::CubeFaceDirection; the harness cross-checks all three.
struct Vec3d { double X = 0, Y = 0, Z = 0; };

inline Vec3d CubeFaceDirection(int32_t Face, double U, double V)
{
    double x, y, z;
    switch (Face)
    {
        case 0: x =  1.0; y =  U;   z =  V;   break;
        case 1: x = -1.0; y = -U;   z =  V;   break;
        case 2: x = -U;   y =  1.0; z =  V;   break;
        case 3: x =  U;   y = -1.0; z =  V;   break;
        case 4: x =  U;   y =  V;   z =  1.0; break;
        default:x = -U;   y =  V;   z = -1.0; break;
    }
    const double Inv = 1.0 / std::sqrt(x*x + y*y + z*z);
    return Vec3d{x*Inv, y*Inv, z*Inv};
}

struct ChunkKey
{
    uint8_t Face = 0;
    uint8_t LOD  = 0;
    int32_t X    = 0;
    int32_t Y    = 0;

    bool operator==(const ChunkKey& O) const
    { return Face == O.Face && LOD == O.LOD && X == O.X && Y == O.Y; }
    bool operator!=(const ChunkKey& O) const { return !(*this == O); }
};

inline int32_t TilesAt(uint8_t LOD) { return (int32_t)((int64_t)1 << LOD); }

struct KeyHashFn
{
    size_t operator()(const ChunkKey& K) const
    {
        uint32_t H = K.Face;
        H = H * 1000003u ^ K.LOD;
        H = H * 1000003u ^ (uint32_t)(K.X + 0x80000000u);
        H = H * 1000003u ^ (uint32_t)(K.Y + 0x80000000u);
        return (size_t)H;
    }
};
using KeySet = std::unordered_set<ChunkKey, KeyHashFn>;

inline void ChunkExtent(const ChunkKey& K, double& OutU0, double& OutV0, double& OutSize)
{
    OutSize = 2.0 / (double)TilesAt(K.LOD);
    OutU0   = -1.0 + (double)K.X * OutSize;
    OutV0   = -1.0 + (double)K.Y * OutSize;
}

// ── Cube edge identity ──────────────────────────────────────────────────────
enum class Edge : uint8_t { None = 0, UNeg = 1, UPos = 2, VNeg = 3, VPos = 4 };

inline Edge BorderEdgeOf(const ChunkKey& K, int32_t Side)
{
    const int32_t MaxIdx = TilesAt(K.LOD) - 1;
    switch (Side)
    {
        case 0: return (K.X == 0)      ? Edge::UNeg : Edge::None;
        case 1: return (K.X == MaxIdx) ? Edge::UPos : Edge::None;
        case 2: return (K.Y == 0)      ? Edge::VNeg : Edge::None;
        default:return (K.Y == MaxIdx) ? Edge::VPos : Edge::None;
    }
}

// ── Face adjacency table ────────────────────────────────────────────────────
// Edge encoding: 0=UNeg 1=UPos 2=VNeg 3=VPos.
// bFlip: along the shared edge the neighbour parameter is -t for our t.
//
// This table was derived numerically and then re-verified from the table
// itself (54864 probes, 0 mismatches). Two earlier harness failures were
// bugs in the TEST, not the table: the test inferred the neighbour's shared
// edge from the neighbour's tile position, which is ambiguous because a
// corner tile lies on two cube edges at once. The fix was to carry the edge
// in the table and use it, never to guess.
struct FaceEdgeLink
{
    uint8_t MyFace; uint8_t MyEdge;
    uint8_t NbrFace; uint8_t NbrEdge;
    bool    bFlip;
};

static const FaceEdgeLink GFaceEdgeLinks[24] = {
    {0, 0, 3, 1, false}, {0, 1, 2, 0, false}, {0, 2, 5, 0, false}, {0, 3, 4, 1, false},
    {1, 0, 2, 1, false}, {1, 1, 3, 0, false}, {1, 2, 5, 1, true }, {1, 3, 4, 0, true },
    {2, 0, 0, 1, false}, {2, 1, 1, 0, false}, {2, 2, 5, 3, false}, {2, 3, 4, 3, true },
    {3, 0, 1, 1, false}, {3, 1, 0, 0, false}, {3, 2, 5, 2, true }, {3, 3, 4, 2, false},
    {4, 0, 1, 3, true }, {4, 1, 0, 3, false}, {4, 2, 3, 3, false}, {4, 3, 2, 3, true },
    {5, 0, 0, 2, false}, {5, 1, 1, 2, true }, {5, 2, 3, 2, true }, {5, 3, 2, 2, false},
};

// ── Neighbour resolution at the SAME LOD ────────────────────────────────────
// Mapping is done in TILE-INDEX space, never through a float parameter round
// trip: that produced an off-by-one on flipped edges.
//   our tile index along the edge : k
//   neighbour tile index          : bFlip ? (N-1-k) : k
struct Neighbour
{
    ChunkKey Key;
    bool     bValid       = false;
    bool     bFlipped     = false;
    bool     bCrossFace   = false;
    uint8_t  NbrTableEdge = 0;
};

inline Neighbour FindNeighbourSameLOD(const ChunkKey& K, int32_t Side)
{
    Neighbour Out;
    const int32_t N = TilesAt(K.LOD);
    const int32_t MaxIdx = N - 1;

    switch (Side)
    {
        case 0: if (K.X > 0)      { Out.Key = {K.Face,K.LOD,K.X-1,K.Y}; Out.bValid = true; return Out; } break;
        case 1: if (K.X < MaxIdx) { Out.Key = {K.Face,K.LOD,K.X+1,K.Y}; Out.bValid = true; return Out; } break;
        case 2: if (K.Y > 0)      { Out.Key = {K.Face,K.LOD,K.X,K.Y-1}; Out.bValid = true; return Out; } break;
        default:if (K.Y < MaxIdx) { Out.Key = {K.Face,K.LOD,K.X,K.Y+1}; Out.bValid = true; return Out; } break;
    }

    const Edge E = BorderEdgeOf(K, Side);
    if (E == Edge::None) return Out;

    const uint8_t EIdx = (uint8_t)((uint8_t)E - 1);
    const bool bParamIsV = (Side == 0 || Side == 1);
    const int32_t k = bParamIsV ? K.Y : K.X;

    for (const FaceEdgeLink& L : GFaceEdgeLinks)
    {
        if (L.MyFace != K.Face || L.MyEdge != EIdx) continue;

        const int32_t kN = L.bFlip ? (N - 1 - k) : k;

        ChunkKey M;
        M.Face = L.NbrFace;
        M.LOD  = K.LOD;
        switch (L.NbrEdge)
        {
            case 0: M.X = 0;      M.Y = kN; break;
            case 1: M.X = MaxIdx; M.Y = kN; break;
            case 2: M.X = kN;     M.Y = 0;  break;
            default:M.X = kN;     M.Y = MaxIdx; break;
        }

        Out.Key = M;
        Out.bValid = true;
        Out.bFlipped = L.bFlip;
        Out.bCrossFace = true;
        Out.NbrTableEdge = L.NbrEdge;
        return Out;
    }
    return Out;
}

// ── Border parameter helpers (used by the harness) ──────────────────────────
inline void BorderParamRange(const ChunkKey& K, int32_t Side, double& OutT0, double& OutT1)
{
    const int32_t N = TilesAt(K.LOD);
    const bool bParamIsV = (Side == 0 || Side == 1);
    const int32_t k = bParamIsV ? K.Y : K.X;
    OutT0 = 2.0 * (double)k / (double)N - 1.0;
    OutT1 = 2.0 * (double)(k + 1) / (double)N - 1.0;
}

inline Vec3d BorderPoint(int32_t Face, int32_t Side, double T)
{
    const bool bParamIsV = (Side == 0 || Side == 1);
    const double Fixed = (Side == 0 || Side == 2) ? -1.0 : 1.0;
    if (bParamIsV) return CubeFaceDirection(Face, Fixed, T);
    return CubeFaceDirection(Face, T, Fixed);
}

inline Vec3d EdgePoint(int32_t Face, uint8_t TableEdge, double T)
{
    switch (TableEdge)
    {
        case 0: return CubeFaceDirection(Face, -1.0, T);
        case 1: return CubeFaceDirection(Face,  1.0, T);
        case 2: return CubeFaceDirection(Face,  T, -1.0);
        default:return CubeFaceDirection(Face,  T,  1.0);
    }
}

// ── Error and culling ───────────────────────────────────────────────────────
inline double ProjectedError(double ChunkWorldSide, double CameraDistance,
                             double ScreenHeightPx, double VertFOVRad,
                             double ErrorFraction)
{
    if (CameraDistance < 1.0) return 1e9;
    const double ErrorWorld  = ChunkWorldSide * ErrorFraction;
    const double HalfFovTan  = std::tan(VertFOVRad * 0.5);
    const double ScreenScale = ScreenHeightPx / (2.0 * HalfFovTan);
    return ErrorWorld * ScreenScale / CameraDistance;
}

inline bool HorizonCulled(const Vec3d& CamPos, double PlanetRadius,
                          const Vec3d& SphereCentre, double SphereRadius)
{
    const double CamDist = std::sqrt(CamPos.X*CamPos.X + CamPos.Y*CamPos.Y + CamPos.Z*CamPos.Z);
    if (CamDist < PlanetRadius + 1.0) return false;

    const double HorizonCos = PlanetRadius / CamDist;
    const double SphDist = std::sqrt(SphereCentre.X*SphereCentre.X +
                                     SphereCentre.Y*SphereCentre.Y +
                                     SphereCentre.Z*SphereCentre.Z);
    if (SphDist < 1.0) return false;

    const double CosAngle =
        (CamPos.X*SphereCentre.X + CamPos.Y*SphereCentre.Y + CamPos.Z*SphereCentre.Z)
        / (CamDist * SphDist);
    const double Slack = SphereRadius / std::max(SphDist, 1.0);
    return (CosAngle + Slack) < HorizonCos;
}

// ── Traversal: budgeted best-first refinement ───────────────────────────────
// The first implementation recursed unconditionally and produced 1.7M leaves
// in 70 ms from 2 km altitude — unusable. Here the frontier is a max-heap on
// error and refinement stops at MaxLeaves, so the emitted set is bounded by
// construction. The caller sizes MaxLeaves to the FIXED component pool (64).
struct TraversalParams
{
    Vec3d  CameraPos        = {0, 0, 0};
    double PlanetRadius     = 2500000.0;
    double HeightMargin     = 60000.0;
    // Terrain elevation under the camera (cm, may be negative). The error
    // metric measures distance to a sphere through that point instead of to
    // the bare reference sphere: on a 10 km plateau the camera is metres from
    // the ground, not 10 km. Used only by ChunkError, never by culling.
    double SurfaceOffsetCm  = 0.0;
    // Extra slack subtracted from the distance (cm). With SurfaceOffsetCm in
    // place this should stay small: a large value makes every chunk within
    // that radius equal-error and spends the leaf budget on ties.
    double ReliefCm         = 0.0;
    double ScreenHeightPx   = 1080.0;
    double VertFOVRad       = 1.0472;    // 60 deg
    double ErrorThresholdPx = 2.0;
    double ErrorFraction    = 0.05;      // geometric error / chunk side
    uint8_t MaxLOD          = 14;
    int32_t MaxLeaves       = 64;        // leaf budget; the pool needs headroom above it
    bool   bUseHorizonCull  = true;

    // Hysteresis. Nodes that were split in the previous selection (its
    // internal nodes, see InternalNodesOf) have their error multiplied by
    // HysteresisFactor, so a node is only merged back once its error falls
    // that much below what split it. Without it, a camera moving a few
    // metres reshuffles nodes of near-equal error at the budget edge: one
    // node merges and another splits, and both get rebuilt.
    const KeySet* PreviouslySplit = nullptr;
    double  HysteresisFactor = 1.25;
};

// Every ancestor of the given leaves: the nodes that were split to produce
// them. Feed the result of one frame to the next as PreviouslySplit.
KeySet InternalNodesOf(const std::vector<ChunkKey>& Leaves);

struct Selection
{
    std::vector<ChunkKey> Chunks;
    int32_t Visited     = 0;
    int32_t Culled      = 0;
    int32_t Split       = 0;
    int32_t BudgetStops = 0;
    bool    bBudgetHit  = false;
};

void Traverse(const TraversalParams& P, Selection& Out);

    inline double ChunkError(const ChunkKey& K, const TraversalParams& P)
    {
        double U0, V0, Size;
        ChunkExtent(K, U0, V0, Size);

        const Vec3d Dir =
            CubeFaceDirection(K.Face, U0 + Size * 0.5, V0 + Size * 0.5);

        // Chunk centre on the sphere through the terrain under the camera.
        const double SurfaceRadius = P.PlanetRadius + P.SurfaceOffsetCm;
        const Vec3d C{
            Dir.X * SurfaceRadius,
            Dir.Y * SurfaceRadius,
            Dir.Z * SurfaceRadius
        };

        const double Dx = P.CameraPos.X - C.X;
        const double Dy = P.CameraPos.Y - C.Y;
        const double Dz = P.CameraPos.Z - C.Z;
        const double CentreDistance = std::sqrt(Dx*Dx + Dy*Dy + Dz*Dz);

        // Оцениваем расстояние до ближайшей точки чанка, а не до центра.
        // При старой метрике размер чанка и расстояние до центра уменьшались
        // одновременно, поэтому под камерой refinement мог застревать.
        const double BoundRadius =
            Size * P.PlanetRadius * 0.7072 + P.ReliefCm;

        const double NearestDistance =
            std::max(100.0, CentreDistance - BoundRadius);

        return ProjectedError(
            Size * P.PlanetRadius,
            NearestDistance,
            P.ScreenHeightPx,
            P.VertFOVRad,
            P.ErrorFraction);
    }

inline bool ChunkHorizonCulled(const ChunkKey& K, const TraversalParams& P)
{
    if (!P.bUseHorizonCull) return false;
    double U0, V0, Size;
    ChunkExtent(K, U0, V0, Size);
    const Vec3d Dir = CubeFaceDirection(K.Face, U0 + Size*0.5, V0 + Size*0.5);
    const Vec3d C{Dir.X * P.PlanetRadius, Dir.Y * P.PlanetRadius, Dir.Z * P.PlanetRadius};
    const double BoundsRadius = Size * P.PlanetRadius * 0.7072 + P.HeightMargin;
    return HorizonCulled(P.CameraPos, P.PlanetRadius, C, BoundsRadius);
}

// ── Quadtree containment ────────────────────────────────────────────────────
// True when D lies inside A (or is A): same face, D at A's level or deeper,
// and D's tile index shifted up to A's level is A's.
inline bool IsInsideOrSame(const ChunkKey& D, const ChunkKey& A)
{
    if (D.Face != A.Face || D.LOD < A.LOD) return false;
    const int32_t Shift = (int32_t)D.LOD - (int32_t)A.LOD;
    return (D.X >> Shift) == A.X && (D.Y >> Shift) == A.Y;
}

// Two quadtree tiles cover any common surface only if one contains the other.
inline bool TilesOverlap(const ChunkKey& A, const ChunkKey& B)
{
    return IsInsideOrSame(A, B) || IsInsideOrSame(B, A);
}

// ── Direction -> cube face UV (inverse of CubeFaceDirection) ────────────────
// The face is the dominant axis; U, V are in [-1, 1]. Used to find the leaf
// under the camera for diagnostics.
inline void DirectionToFaceUV(const Vec3d& D, int32_t& OutFace, double& OutU, double& OutV)
{
    const double ax = std::fabs(D.X), ay = std::fabs(D.Y), az = std::fabs(D.Z);
    if (ax >= ay && ax >= az)
    {
        if (D.X > 0.0) { OutFace = 0; OutU =  D.Y / ax; OutV = D.Z / ax; }
        else           { OutFace = 1; OutU = -D.Y / ax; OutV = D.Z / ax; }
    }
    else if (ay >= az)
    {
        if (D.Y > 0.0) { OutFace = 2; OutU = -D.X / ay; OutV = D.Z / ay; }
        else           { OutFace = 3; OutU =  D.X / ay; OutV = D.Z / ay; }
    }
    else
    {
        if (D.Z > 0.0) { OutFace = 4; OutU =  D.X / az; OutV = D.Y / az; }
        else           { OutFace = 5; OutU = -D.X / az; OutV = D.Y / az; }
    }
}

// Index of the leaf in Leaves that covers (Face, U, V), or -1.
inline int32_t FindLeafContaining(const std::vector<ChunkKey>& Leaves,
                                  int32_t Face, double U, double V)
{
    for (int32_t i = 0; i < (int32_t)Leaves.size(); ++i)
    {
        const ChunkKey& K = Leaves[i];
        if (K.Face != Face) continue;
        const int32_t N = TilesAt(K.LOD);
        const double Size = 2.0 / (double)N;
        const int32_t TX = std::clamp((int32_t)std::floor((U + 1.0) / Size), 0, N - 1);
        const int32_t TY = std::clamp((int32_t)std::floor((V + 1.0) / Size), 0, N - 1);
        if (TX == K.X && TY == K.Y) return i;
    }
    return -1;
}

// ── Coarser neighbour across an edge ────────────────────────────────────────
// How many LOD levels coarser the chunk in Shown across side Side of K is
// (0 = same level, finer, or nothing). Sides: 0 = U-, 1 = U+, 2 = V-, 3 = V+.
//
// A coarser neighbour covers the whole shared edge, so probing one point just
// outside the edge midpoint finds it. CubeFaceDirection extends naturally past
// [-1, 1], so a probe beyond a cube edge lands on the neighbouring face with
// no adjacency table. Shown must not hold overlapping chunks.
inline int32_t CoarserNeighbourLevels(const ChunkKey& K, int32_t Side, const KeySet& Shown)
{
    if (K.LOD == 0) return 0;

    double U0, V0, Size;
    ChunkExtent(K, U0, V0, Size);
    const double Eps  = Size * 1e-3;
    const double MidU = U0 + Size * 0.5, MidV = V0 + Size * 0.5;

    double PU = MidU, PV = MidV;
    switch (Side)
    {
        case 0:  PU = U0 - Eps;        break;
        case 1:  PU = U0 + Size + Eps; break;
        case 2:  PV = V0 - Eps;        break;
        default: PV = V0 + Size + Eps; break;
    }

    int32_t Face = 0;
    double U = 0.0, V = 0.0;
    DirectionToFaceUV(CubeFaceDirection(K.Face, PU, PV), Face, U, V);

    for (int32_t L = (int32_t)K.LOD - 1; L >= 0; --L)
    {
        const int32_t N = TilesAt((uint8_t)L);
        const double  S = 2.0 / (double)N;
        const ChunkKey C{(uint8_t)Face, (uint8_t)L,
                         std::clamp((int32_t)std::floor((U + 1.0) / S), 0, N - 1),
                         std::clamp((int32_t)std::floor((V + 1.0) / S), 0, N - 1)};
        if (Shown.count(C)) return (int32_t)K.LOD - L;
    }
    return 0;
}

// ── Edge classification ─────────────────────────────────────────────────────
// What is on the other side of a leaf border? These names are NOT symmetric:
// the same border reads FinerNeighbours from the coarse side and
// CoarserNeighbour from the fine side. The stitcher needs both.
enum class EdgeMatch : uint8_t { Clean, FinerNeighbours, CoarserNeighbour, OpenBoundary };

struct EdgeStats
{
    int32_t Clean            = 0;
    int32_t FinerNeighbours  = 0;
    int32_t CoarserNeighbour = 0;
    int32_t OpenBoundary     = 0;
    int32_t Total            = 0;
};

EdgeStats ClassifyEdges(const std::vector<ChunkKey>& Active);
EdgeMatch ClassifyBorder(const ChunkKey& K, int32_t Side, const KeySet& Set);

} // namespace PlanetLOD

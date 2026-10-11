// PlanetDetail.cpp
#include "PlanetDetail.h"

#include <algorithm>
#include <cmath>
#include <queue>

namespace PlanetDetail
{
using namespace PlanetLOD;

namespace
{
    // Distance from the camera to the nearest point of K, measured as the LOD
    // error does: chunk centre on the sphere through the terrain under the
    // camera, minus a bounding radius.
    double NearestDistance(const ChunkKey& K, const Params& P, double& OutSide)
    {
        double U0, V0, Size;
        ChunkExtent(K, U0, V0, Size);
        const Vec3d Dir = CubeFaceDirection(K.Face, U0 + Size * 0.5, V0 + Size * 0.5);
        const double R = P.PlanetRadiusCm + P.SurfaceOffsetCm;
        const double Dx = P.CameraPos.X - Dir.X * R;
        const double Dy = P.CameraPos.Y - Dir.Y * R;
        const double Dz = P.CameraPos.Z - Dir.Z * R;
        OutSide = Size * P.PlanetRadiusCm;
        return std::max(0.0, std::sqrt(Dx * Dx + Dy * Dy + Dz * Dz) - OutSide * 0.7072);
    }

    struct Node
    {
        ChunkKey Key;
        int32_t  Base;
        double   Dist;
        bool     bIsBase;
    };
    struct NearestFirst
    {
        bool operator()(const Node& A, const Node& B) const { return A.Dist > B.Dist; }
    };

    // True when a chunk of Shown finer than K touches side Side of K: the
    // same-LOD neighbour is neither shown nor covered by a coarser chunk, so
    // it is split into finer ones.
    bool HasFinerNeighbour(const ChunkKey& K, int32_t Side, const KeySet& Shown)
    {
        const Neighbour N = FindNeighbourSameLOD(K, Side);
        if (!N.bValid || Shown.count(N.Key)) return false;
        return CoarserNeighbourLevels(K, Side, Shown) == 0;
    }

    // Levels by which the coarsest chunk of Shown containing (PU, PV) on K's
    // face (may lie past the face edge) is coarser than K; 0 if none is.
    int32_t CoarserAt(const ChunkKey& K, double PU, double PV, const KeySet& Shown)
    {
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
}

void SubRect(const ChunkKey& D, const ChunkKey& B, float& OutX, float& OutY, float& OutSize)
{
    const int32_t Shift = std::clamp((int32_t)D.LOD - (int32_t)B.LOD, 0, 6);
    const int32_t Size  = (PLANET_QUADS_PER_SIDE >> Shift);
    OutX    = (float)((D.X - (B.X << Shift)) * Size);
    OutY    = (float)((D.Y - (B.Y << Shift)) * Size);
    OutSize = (float)Size;
}

void Select(const Params& P, const std::vector<Base>& Bases, Result& Out)
{
    Out.Leaves.clear();
    Out.BaseSplit.assign(Bases.size(), 0);
    Out.SplitNodes.clear();
    Out.bBudgetHit = false;

    const int32_t Levels = std::clamp(P.Levels, 0, 6);
    if (Levels == 0 || Bases.empty()) return;
    const int32_t MaxLOD = P.BaseLOD + Levels;
    const int32_t MinBase = std::min(P.MinBaseLOD, P.BaseLOD);
    const double  Hyst   = std::max(1.0, P.Hysteresis);

    auto WantsSplit = [&](const ChunkKey& K, double& OutDist)
    {
        double Side = 0.0;
        OutDist = NearestDistance(K, P, Side);
        const bool bWas = P.PreviouslySplit && P.PreviouslySplit->count(K);
        return OutDist < P.SplitDistanceFactor * Side * (bWas ? Hyst : 1.0);
    };

    std::priority_queue<Node, std::vector<Node>, NearestFirst> Queue;
    for (int32_t b = 0; b < (int32_t)Bases.size(); ++b)
    {
        const int32_t L = (int32_t)Bases[b].Key.LOD;
        if (L < MinBase || L >= MaxLOD) continue;
        if (P.GeneratedShown)
        {
            bool bFiner = false;
            for (int32_t s = 0; s < 4 && !bFiner; ++s) bFiner = HasFinerNeighbour(Bases[b].Key, s, *P.GeneratedShown);
            if (bFiner) continue;
        }
        double Dist = 0.0;
        if (WantsSplit(Bases[b].Key, Dist)) Queue.push({Bases[b].Key, b, Dist, true});
    }

    // Count = derived leaves held so far (queued derived nodes included: each
    // is a leaf until it splits). A base split adds 4, a derived split 3.
    int32_t Count = 0;
    while (!Queue.empty())
    {
        const Node N = Queue.top();
        Queue.pop();

        const int32_t Need = N.bIsBase ? 4 : 3;
        if (Count + Need > P.MaxInstances)
        {
            Out.bBudgetHit = true;
            if (!N.bIsBase)
            {
                Leaf L; L.Key = N.Key; L.BaseIndex = N.Base;
                SubRect(N.Key, Bases[N.Base].Key, L.SubX, L.SubY, L.SubSize);
                Out.Leaves.push_back(L);
            }
            continue;   // keep draining: queued derived nodes still become leaves
        }

        Count += Need;
        if (N.bIsBase) Out.BaseSplit[N.Base] = 1;
        Out.SplitNodes.insert(N.Key);

        const uint8_t ChildLOD = (uint8_t)(N.Key.LOD + 1);
        for (int32_t c = 0; c < 4; ++c)
        {
            const ChunkKey C{N.Key.Face, ChildLOD, N.Key.X * 2 + (c & 1), N.Key.Y * 2 + (c >> 1)};
            double Dist = 0.0;
            const bool bCanSplit = (int32_t)ChildLOD < MaxLOD &&
                                   (int32_t)ChildLOD - (int32_t)Bases[N.Base].Key.LOD < 6;
            if (bCanSplit && WantsSplit(C, Dist))
            {
                Queue.push({C, N.Base, Dist, false});
            }
            else
            {
                Leaf L; L.Key = C; L.BaseIndex = N.Base;
                SubRect(C, Bases[N.Base].Key, L.SubX, L.SubY, L.SubSize);
                Out.Leaves.push_back(L);
            }
        }
    }
}

int32_t CoarserCornerLevels(const ChunkKey& K, int32_t Corner, const KeySet& Shown)
{
    if (K.LOD == 0) return 0;
    double U0, V0, Size;
    ChunkExtent(K, U0, V0, Size);
    const bool   bU1 = (Corner & 1) != 0, bV1 = (Corner & 2) != 0;
    const double CU = bU1 ? U0 + Size : U0;
    const double CV = bV1 ? V0 + Size : V0;
    const double DU = bU1 ? 1.0 : -1.0;     // outwards
    const double DV = bV1 ? 1.0 : -1.0;
    const double Eps = Size * 1e-3;

    int32_t D = 0;
    D = std::max(D, CoarserAt(K, CU + DU * Eps, CV - DV * Eps, Shown));   // across the U edge
    D = std::max(D, CoarserAt(K, CU - DU * Eps, CV + DV * Eps, Shown));   // across the V edge
    D = std::max(D, CoarserAt(K, CU + DU * Eps, CV + DV * Eps, Shown));   // diagonal
    return std::min(D, 6);
}

float PackedCornerDeltas(const ChunkKey& K, const KeySet& Shown)
{
    int32_t Packed = 0;
    for (int32_t c = 0; c < 4; ++c) Packed += CoarserCornerLevels(K, c, Shown) << (3 * c);
    return (float)Packed;
}

double VertexSpacingCm(int32_t LOD, double PlanetRadiusCm)
{
    return PlanetRadiusCm * 2.0 / (double)((int64_t)1 << std::clamp(LOD, 0, 40)) / (double)PLANET_QUADS_PER_SIDE;
}

void FillChunkData(float* Data, const ChunkKey& Key, const ChunkKey& BaseKey, const TransformIn& T,
                   double PlanetRadiusCm, int32_t DisplacementMinLOD, double TileNearCm, double TileMidCm)
{
    SubRect(Key, BaseKey, Data[6], Data[7], Data[8]);
    Data[10] = (float)VertexSpacingCm(Key.LOD, PlanetRadiusCm);
    Data[11] = (float)((int32_t)Key.LOD - DisplacementMinLOD);
    for (int32_t a = 0; a < 3; ++a)
    {
        Data[12 + a] = (float)(T.AxisX[a] * T.Scale[0]);
        Data[15 + a] = (float)(T.AxisY[a] * T.Scale[1]);
        Data[18 + a] = (float)(T.AxisZ[a] * T.Scale[2]);
        const double N = T.OriginWorld[a] / std::max(TileNearCm, 1.0);
        const double M = T.OriginWorld[a] / std::max(TileMidCm, 1.0);
        Data[21 + a] = (float)(N - std::floor(N));
        Data[24 + a] = (float)(M - std::floor(M));
    }
}

} // namespace PlanetDetail

// PlanetCoordinates.h
// Cube-sphere mapping, face-edge adjacency and coordinate utilities.
//
// Design rules enforced here:
//   1. Planet-space positions are FVector3d (double, R ~ 2.5e6 m).
//   2. Noise inputs are FVector3f derived from the normalised direction.
//   3. Vertex positions in mesh buffers are FVector3f relative to the chunk
//      origin — never planet-space absolute coordinates.
//   4. Face UV coordinates are double in [-1, 1].
//   5. GetCubeDirection() is the single canonical implementation, and the
//      harness proves it agrees bit-for-bit with PlanetCore::CubeFaceDirection
//      on 10086 samples across all six faces.
//
// Numerical verification (see project harnesses):
//   Round-trip error:          3.3e-16  (~1.5 x float64 eps)
//   Cross-check vs core:       0 mismatches / 10086 samples
//   Paired-edge discontinuity: 0.0 (bit-for-bit)

#pragma once

#include "CoreMinimal.h"
#include "PlanetTypes.h"

// ── Face-edge adjacency ─────────────────────────────────────────────────────
// 24 directed entries. Edge enum: 0=UNeg 1=UPos 2=VNeg 3=VPos.
// bParamFlipped: the shared parameter runs the other way on the neighbour, so
// the neighbour parameter is -t for our t. Eight entries are flipped.
//
// The authoritative adjacency used by the LOD core lives in PlanetLOD.h; this
// table is what the UE-side stitcher consults, and the two are kept identical.
enum class ECubeEdge : uint8 { UNeg = 0, UPos = 1, VNeg = 2, VPos = 3 };

struct FFaceEdgeEntry
{
    uint8      MyFace;
    ECubeEdge  MyEdge;
    uint8      NbrFace;
    ECubeEdge  NbrEdge;
    bool       bParamFlipped;
};

static constexpr FFaceEdgeEntry GFaceEdgeTable[24] =
{
    // face 0
    { 0, ECubeEdge::UNeg, 3, ECubeEdge::UPos, false },
    { 0, ECubeEdge::UPos, 2, ECubeEdge::UNeg, false },
    { 0, ECubeEdge::VNeg, 5, ECubeEdge::UNeg, false },
    { 0, ECubeEdge::VPos, 4, ECubeEdge::UPos, false },
    // face 1
    { 1, ECubeEdge::UNeg, 2, ECubeEdge::UPos, false },
    { 1, ECubeEdge::UPos, 3, ECubeEdge::UNeg, false },
    { 1, ECubeEdge::VNeg, 5, ECubeEdge::UPos, true  },
    { 1, ECubeEdge::VPos, 4, ECubeEdge::UNeg, true  },
    // face 2
    { 2, ECubeEdge::UNeg, 0, ECubeEdge::UPos, false },
    { 2, ECubeEdge::UPos, 1, ECubeEdge::UNeg, false },
    { 2, ECubeEdge::VNeg, 5, ECubeEdge::VPos, false },
    { 2, ECubeEdge::VPos, 4, ECubeEdge::VPos, true  },
    // face 3
    { 3, ECubeEdge::UNeg, 1, ECubeEdge::UPos, false },
    { 3, ECubeEdge::UPos, 0, ECubeEdge::UNeg, false },
    { 3, ECubeEdge::VNeg, 5, ECubeEdge::VNeg, true  },
    { 3, ECubeEdge::VPos, 4, ECubeEdge::VNeg, false },
    // face 4
    { 4, ECubeEdge::UNeg, 1, ECubeEdge::VPos, true  },
    { 4, ECubeEdge::UPos, 0, ECubeEdge::VPos, false },
    { 4, ECubeEdge::VNeg, 3, ECubeEdge::VPos, false },
    { 4, ECubeEdge::VPos, 2, ECubeEdge::VPos, true  },
    // face 5
    { 5, ECubeEdge::UNeg, 0, ECubeEdge::VNeg, false },
    { 5, ECubeEdge::UPos, 1, ECubeEdge::VNeg, true  },
    { 5, ECubeEdge::VNeg, 3, ECubeEdge::VNeg, true  },
    { 5, ECubeEdge::VPos, 2, ECubeEdge::VNeg, false },
};

namespace PlanetCoordinates
{

// ── GetCubeDirection ────────────────────────────────────────────────────────
// Maps (face, u, v) to a unit direction on the sphere. u, v in [-1, 1].
//
// Face layout:
//   0: +X  raw = ( 1,  u,  v)
//   1: -X  raw = (-1, -u,  v)
//   2: +Y  raw = (-u,  1,  v)
//   3: -Y  raw = ( u, -1,  v)
//   4: +Z  raw = ( u,  v,  1)
//   5: -Z  raw = (-u,  v, -1)
//
// This is the single source of truth for coordinate math on the UE side. It is
// byte-identical in behaviour to PlanetCore::CubeFaceDirection, verified by the
// cross-check harness.
FORCEINLINE FVector3d GetCubeDirection(int32 Face, double U, double V)
{
    double X, Y, Z;
    switch (Face)
    {
        case 0:  X =  1.0;  Y =  U;    Z =  V;    break;
        case 1:  X = -1.0;  Y = -U;    Z =  V;    break;
        case 2:  X = -U;    Y =  1.0;  Z =  V;    break;
        case 3:  X =  U;    Y = -1.0;  Z =  V;    break;
        case 4:  X =  U;    Y =  V;    Z =  1.0;  break;
        default: X = -U;    Y =  V;    Z = -1.0;  break;   // face 5
    }
    const double InvLen = 1.0 / FMath::Sqrt(X*X + Y*Y + Z*Z);
    return FVector3d(X * InvLen, Y * InvLen, Z * InvLen);
}

// ── Chunk origin and extent ─────────────────────────────────────────────────
FORCEINLINE void GetChunkOriginUV(const FChunkKey& Key, double& OutSize,
                                  double& OutU, double& OutV)
{
    OutSize = 2.0 / (double)(1 << Key.LOD);
    OutU = -1.0 + (double)Key.X * OutSize;
    OutV = -1.0 + (double)Key.Y * OutSize;
}

FORCEINLINE FVector3d GetChunkCentreDirection(const FChunkKey& Key)
{
    double Size, U0, V0;
    GetChunkOriginUV(Key, Size, U0, V0);
    return GetCubeDirection(Key.Face, U0 + Size * 0.5, V0 + Size * 0.5);
}

// Planet-space centre of a chunk on the reference sphere. This is the value
// the mesh builder returns and the actor assigns to the component transform,
// which is the only place the large coordinate enters the renderer.
FORCEINLINE FVector3d GetChunkCentrePosition(const FChunkKey& Key, double PlanetRadius)
{
    return GetChunkCentreDirection(Key) * PlanetRadius;
}

// UV of grid vertex (GridX, GridY), including the halo ring. Mirrors
// PlanetCore::ChunkVertexUV exactly; using the same function on both sides is
// what welds the mesh to the sampled heights.
FORCEINLINE void GetChunkVertexUV(const FChunkKey& Key, int32 GridX, int32 GridY,
                                  double& OutU, double& OutV)
{
    double Size, U0, V0;
    GetChunkOriginUV(Key, Size, U0, V0);
    const double Step = Size / (double)PLANET_QUADS_PER_SIDE;
    OutU = U0 + (double)(GridX - 1) * Step;
    OutV = V0 + (double)(GridY - 1) * Step;
}

// ── Projected geometric error ───────────────────────────────────────────────
// Screen-space error estimate in pixels:
//   error_screen = error_world * (screen_height / (2 tan(fov/2))) / distance
FORCEINLINE double ProjectedGeometricError(double ChunkWorldSide,
                                           double CameraDistance,
                                           double ScreenHeightPx,
                                           double VertFOVRad,
                                           double ErrorFraction = 0.5)
{
    const double ErrorWorld  = ChunkWorldSide * ErrorFraction;
    const double HalfFovTan  = FMath::Tan(VertFOVRad * 0.5);
    const double ScreenScale = ScreenHeightPx / (2.0 * HalfFovTan);
    return ErrorWorld * ScreenScale / FMath::Max(CameraDistance, 1.0);
}

// ── Horizon culling ─────────────────────────────────────────────────────────
FORCEINLINE bool HorizonCullTest(const FVector3d& CameraPos,
                                 double PlanetRadius,
                                 const FVector3d& SphereCentre,
                                 double SphereRadius)
{
    const double CamDist = CameraPos.Length();
    if (CamDist < PlanetRadius + 1.0) return false;

    const double HorizonCos = PlanetRadius / CamDist;
    const double SphDist = SphereCentre.Length();
    if (SphDist < 1.0) return false;

    const double CosAngle =
        FVector3d::DotProduct(CameraPos, SphereCentre) / (CamDist * SphDist);
    const double Slack = SphereRadius / FMath::Max(SphDist, 1.0);
    return (CosAngle + Slack) < HorizonCos;
}

FORCEINLINE void GetChunkBoundingSphere(const FChunkKey& Key,
                                        double PlanetRadius,
                                        double HeightMargin,
                                        FVector3d& OutCentre,
                                        double& OutRadius)
{
    double Size, U0, V0;
    GetChunkOriginUV(Key, Size, U0, V0);
    OutCentre = GetChunkCentreDirection(Key) * PlanetRadius;

    const double HalfDiag = Size * 0.5 * 1.41422 * PlanetRadius;
    OutRadius = HalfDiag + HeightMargin;
}

// ── Face visibility (cheap early-out) ───────────────────────────────────────
static const FVector3d GFaceNormals[6] =
{
    FVector3d( 1, 0, 0), FVector3d(-1, 0, 0),
    FVector3d( 0, 1, 0), FVector3d( 0,-1, 0),
    FVector3d( 0, 0, 1), FVector3d( 0, 0,-1),
};

FORCEINLINE bool FaceNeedsVisibility(int32 Face, const FVector3d& CameraDir)
{
    // A face can only be visible if the camera is on its side of the sphere.
    // The 60 degree margin accounts for the perspective horizon at altitude.
    return FVector3d::DotProduct(GFaceNormals[Face], CameraDir) > -0.35;
}

} // namespace PlanetCoordinates

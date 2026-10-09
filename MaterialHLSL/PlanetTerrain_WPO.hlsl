// PlanetTerrain_WPO.hlsl
// Body of a Material "Custom" node. Output Type: CMOT Float 3.
// Paste everything below the dashed line into the node's Code field.
//
// Inputs, in this order and with exactly these names:
//   UV            TexCoord[0]
//   LocalPos      Pre-Skinned Local Position
//   PosAtlas      TextureObjectParameter "PlanetPosAtlas"
//   CD0           PerInstanceCustomData3Vector, DataIndex 0  (tile, morph start, edge U-)
//   CD1           PerInstanceCustomData3Vector, DataIndex 3  (edge U+, edge V-, edge V+)
//   TimeSec       Time
//   TilesPerRow   ScalarParameter "AtlasTilesPerRow"
//   MorphSeconds  ScalarParameter "MorphSeconds"
//
// Returns a LOCAL-space offset. Feed it through TransformVector
// (Local Space -> World Space) into World Position Offset.
//
// What it does, per grid vertex (gx, gy) of the 65x65 chunk grid:
//   * reads the vertex's target position from the chunk's atlas tile;
//   * on an edge whose neighbour is d LOD levels coarser, slides the vertex
//     onto the neighbour's edge (linear between every 2^d-th vertex), so the
//     two chunks share the same border line: no cracks;
//   * for a chunk that just replaced its parent, blends interior vertices from
//     the parent's shape (every 2nd vertex, parent's triangle split) to its
//     own over MorphSeconds, so a split does not pop. Border vertices never
//     morph: that keeps them identical to the neighbours at all times.
//
// This file also compiles as C++ (planet harness) and as plain HLSL (glslang),
// so keep to the shared subset: no swizzles beyond .x/.y/.z/.w, no helper
// functions, no HLSL-only intrinsics other than those the harness mocks.
// -----------------------------------------------------------------------------
int TileIndex = (int)(CD0.x + 0.5);
int TPR       = max((int)(TilesPerRow + 0.5), 1);
int OriginX   = (TileIndex % TPR) * 65;
int OriginY   = (TileIndex / TPR) * 65;

int gx = clamp((int)round(UV.x * 64.0), 0, 64);
int gy = clamp((int)round(UV.y * 64.0), 0, 64);

int dU0 = clamp((int)(CD0.z + 0.5), 0, 6);
int dU1 = clamp((int)(CD1.x + 0.5), 0, 6);
int dV0 = clamp((int)(CD1.y + 0.5), 0, 6);
int dV1 = clamp((int)(CD1.z + 0.5), 0, 6);

bool bBorder = (gx == 0) || (gx == 64) || (gy == 0) || (gy == 64);

float Morph = 0.0;
if (!bBorder && MorphSeconds > 0.0)
{
    Morph = saturate(1.0 - (TimeSec - CD0.y) / MorphSeconds);
}

float3 Result = float3(0.0, 0.0, 0.0);
int Passes = (Morph > 0.0) ? 2 : 1;

for (int Pass = 0; Pass < Passes; ++Pass)
{
    // Pass 0: own shape. Pass 1: parent's shape (step 2).
    int MinStep = (Pass == 0) ? 1 : 2;
    int sU = MinStep;
    int sV = MinStep;
    if (gx == 0)  { sV = max(sV, 1 << dU0); }
    if (gx == 64) { sV = max(sV, 1 << dU1); }
    if (gy == 0)  { sU = max(sU, 1 << dV0); }
    if (gy == 64) { sU = max(sU, 1 << dV1); }

    int x0 = gx - (gx % sU);
    int y0 = gy - (gy % sV);
    int x1 = min(x0 + sU, 64);
    int y1 = min(y0 + sV, 64);
    float fx = (float)(gx - x0) / (float)sU;
    float fy = (float)(gy - y0) / (float)sV;

    float4 T00 = PosAtlas.Load(int3(OriginX + x0, OriginY + y0, 0));
    float4 T10 = PosAtlas.Load(int3(OriginX + x1, OriginY + y0, 0));
    float4 T01 = PosAtlas.Load(int3(OriginX + x0, OriginY + y1, 0));
    float4 T11 = PosAtlas.Load(int3(OriginX + x1, OriginY + y1, 0));
    float3 P00 = float3(T00.x, T00.y, T00.z);
    float3 P10 = float3(T10.x, T10.y, T10.z);
    float3 P01 = float3(T01.x, T01.y, T01.z);
    float3 P11 = float3(T11.x, T11.y, T11.z);

    // The mesh splits each quad along v10-v01, so the point lies in
    // (v00, v10, v01) when fx + fy <= 1, else in (v10, v11, v01).
    float3 P;
    if (fx + fy <= 1.0)
    {
        P = P00 + (P10 - P00) * fx + (P01 - P00) * fy;
    }
    else
    {
        P = P11 + (P01 - P11) * (1.0 - fx) + (P10 - P11) * (1.0 - fy);
    }

    if (Pass == 0) { Result = P; }
    else           { Result = Result + (P - Result) * Morph; }
}

return Result - LocalPos;

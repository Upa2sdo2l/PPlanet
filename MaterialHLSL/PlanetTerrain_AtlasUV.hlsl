// PlanetTerrain_AtlasUV.hlsl
// Body of a Material "Custom" node. Output Type: CMOT Float 2.
// Paste everything below the dashed line into the node's Code field.
//
// Inputs, in this order and with exactly these names:
//   UV           TexCoord[0]
//   CD0          PerInstanceCustomData3Vector, DataIndex 0
//   CD2          PerInstanceCustomData3Vector, DataIndex 6
//   TilesPerRow  ScalarParameter "AtlasTilesPerRow"
//   AtlasTexels  ScalarParameter "AtlasTexels"
//
// Returns the atlas UV of this vertex. Pass it through a VertexInterpolator
// node and sample PlanetNormalAtlas / PlanetBiomeAtlas(2) with it in the pixel
// shader: bilinear filtering then blends the nearest vertices, exactly like
// interpolated vertex attributes would. A detail-level chunk covers the
// sub-rectangle CD2 = (x, y, size) of its base chunk's tile, in base quads.
// -----------------------------------------------------------------------------
int TileIndex = (int)(CD0.x + 0.5);
int TPR       = max((int)(TilesPerRow + 0.5), 1);
float OX      = (float)((TileIndex % TPR) * 65);
float OY      = (float)((TileIndex / TPR) * 65);
float S       = (CD2.z > 0.5) ? CD2.z : 64.0;
float SX      = (CD2.z > 0.5) ? CD2.x : 0.0;
float SY      = (CD2.z > 0.5) ? CD2.y : 0.0;

return float2((OX + SX + UV.x * S + 0.5) / AtlasTexels,
              (OY + SY + UV.y * S + 0.5) / AtlasTexels);

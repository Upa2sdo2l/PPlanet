// PlanetTerrain_AtlasUV.hlsl
// Body of a Material "Custom" node. Output Type: CMOT Float 2.
// Paste everything below the dashed line into the node's Code field.
//
// Inputs, in this order and with exactly these names:
//   UV           TexCoord[0]
//   CD0          PerInstanceCustomData3Vector, DataIndex 0
//   TilesPerRow  ScalarParameter "AtlasTilesPerRow"
//   AtlasTexels  ScalarParameter "AtlasTexels"
//
// Returns the atlas UV of this vertex's texel centre. Pass it through a
// VertexInterpolator node and sample PlanetNormalAtlas / PlanetBiomeAtlas with
// it in the pixel shader: bilinear filtering then blends the four nearest
// vertices, exactly like interpolated vertex attributes would.
// -----------------------------------------------------------------------------
int TileIndex = (int)(CD0.x + 0.5);
int TPR       = max((int)(TilesPerRow + 0.5), 1);
float OX      = (float)((TileIndex % TPR) * 65);
float OY      = (float)((TileIndex / TPR) * 65);

return float2((OX + UV.x * 64.0 + 0.5) / AtlasTexels,
              (OY + UV.y * 64.0 + 0.5) / AtlasTexels);

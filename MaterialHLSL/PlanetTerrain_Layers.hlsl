// PlanetTerrain_Layers.hlsl
// Body of a Material "Custom" node. Output Type: CMOT Float 3 (base colour).
// Paste everything below the dashed line into the node's Code field.
//
// Inputs, in this order and with exactly these names:
//   W0   PlanetBiomeAtlas  RGBA  (Sand, DryGrass, Grass, Forest)
//   W1   PlanetBiomeAtlas2 RGBA  (Jungle, Tundra, Snow, Rock)
//   C0 .. C7   Vector Parameters, the colour of each layer in that order
//
// The compute shader writes eight surface-layer weights (sum 1) per vertex;
// bilinear filtering keeps the sum close to 1, the division fixes the rest.
// Later each colour becomes a texture (Texture2DArray slice, triplanar on
// slopes) without changing the atlases.
// -----------------------------------------------------------------------------
float W[8] = { W0.x, W0.y, W0.z, W0.w, W1.x, W1.y, W1.z, W1.w };
float3 C[8] = { C0, C1, C2, C3, C4, C5, C6, C7 };

float3 Colour = float3(0.0, 0.0, 0.0);
float  Sum = 0.0;
for (int i = 0; i < 8; ++i)
{
    Colour += C[i] * W[i];
    Sum += W[i];
}
return Colour / max(Sum, 1e-4);

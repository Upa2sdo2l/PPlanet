// PlanetTerrain_Surface.hlsl
// Body of the Material "Custom" node of the textured planet surface.
// The code lives in GpuErosion/Shaders/PlanetTerrainSurface.ush (updates with
// git); this node only calls it.
//
// Custom node settings:
//   Output Type        CMOT Float 3  (-> Base Color)
//   Include File Paths /PlanetErosion/PlanetTerrainSurface.ush
//   Inputs, in this order and with exactly these names:
//     W0       PlanetBiomeAtlas  RGBA
//     W1       PlanetBiomeAtlas2 RGBA
//     N        PlanetNormalAtlas RGB * 2 - 1
//     RelPos   World Position node, mode "Camera Relative World Position"
//     UVNear   Frac(Absolute World Position / TileNear)    (see README_Textures.md)
//     UVMid    Frac(Absolute World Position / TileMid)
//     UVMacro  Frac(Absolute World Position / TileMacro)
//     AH       Texture Object Parameter "PlanetLayersAH"  (Texture 2D Array)
//     NRA      Texture Object Parameter "PlanetLayersNRA" (Texture 2D Array)
//     C0 .. C7 the eight layer colour parameters
//     P0       Append(TileNear, TileMid, TileMacro, NearEnd)
//     P1       Append(FarStart, FarEnd, ParallaxDepth, ParallaxSteps)
//   Additional Outputs:
//     OutNormal     CMOT Float 3  -> Normal
//     OutRoughness  CMOT Float 1  -> Roughness
//     OutAO         CMOT Float 1  -> Ambient Occlusion
// Paste everything below the dashed line into the node's Code field.
// -----------------------------------------------------------------------------
FPlanetSurfaceOut S = PlanetSurface(W0, W1, N, RelPos, UVNear, UVMid, UVMacro,
                                    AH, AHSampler, NRA, NRASampler,
                                    C0, C1, C2, C3, C4, C5, C6, C7, P0, P1);
OutNormal    = S.Normal;
OutRoughness = S.Roughness;
OutAO        = S.AO;
return S.Color;

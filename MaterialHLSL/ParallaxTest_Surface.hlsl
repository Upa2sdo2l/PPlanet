// ParallaxTest_Surface.hlsl
// Body of the surface Custom node of the parallax test material (M_ParallaxTest),
// and later of the planet material once parallax is accepted. Same node as
// PlanetTerrain_Surface.hlsl plus two inputs, P3 and P4.
//
// Custom node settings:
//   Output Type        CMOT Float 3  (-> Base Color)
//   Include File Paths /PlanetErosion/PlanetTerrainSurface.ush
//   Inputs, in this order (the first 22 exactly as in PlanetTerrain_Surface.hlsl):
//     W0, W1, N, RelPos, UVNear, UVMid, UVMacro, AH, NRA, C0 .. C7, P0, P1, P2, SunDir,
//     P3       Append(PxMode, PxDepth, PxEnd, PxBudget)
//     P4       Append(PxDebug, PxMipBias, PxGrazeFade, 0)
//   Additional Outputs:
//     OutNormal     CMOT Float 3  -> Normal
//     OutRoughness  CMOT Float 1  -> Roughness
//     OutAO         CMOT Float 1  -> Ambient Occlusion
// After updating the .ush files, change the version comment at the end to
// force a recompile.
// Paste everything below the dashed line into the node's Code field.
// -----------------------------------------------------------------------------
FPlanetSurfaceOut S = PlanetSurfaceEx(W0, W1, N, RelPos, UVNear, UVMid, UVMacro,
                                      AH, AHSampler, NRA, NRASampler,
                                      C0, C1, C2, C3, C4, C5, C6, C7, P0, P1, P2, SunDir, P3, P4);
OutNormal    = S.Normal;
OutRoughness = S.Roughness;
OutAO        = S.AO;
return S.Color;
// px1

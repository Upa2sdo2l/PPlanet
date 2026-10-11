// PlanetTerrain_WPO.hlsl
// Body of the material's World Position Offset Custom node. The code lives in
// GpuErosion/Shaders/PlanetTerrainVertex.ush (updates come with git); this
// node only calls it. It returns a WORLD offset: connect it straight to
// World Position Offset (no Transform Vector node any more).
//
// Custom node settings:
//   Output Type        CMOT Float 3
//   Include File Paths /PlanetErosion/PlanetTerrainVertex.ush
//   Inputs, in this order and with exactly these names:
//     UV            TexCoord[0]
//     LocalPos      Pre-Skinned Local Position
//     PosAtlas      Texture Object Parameter "PlanetPosAtlas"
//     NrmAtlas      Texture Object Parameter "PlanetNormalAtlasVS"
//     BioAtlas      Texture Object Parameter "PlanetBiomeAtlasVS"
//     Bio2Atlas     Texture Object Parameter "PlanetBiomeAtlas2VS"
//     AH            Texture Object Parameter "PlanetLayersAH" (the same node as the pixel shader's)
//     CD0 .. CD8    PerInstanceCustomData3Vector, Data Index 0, 3, 6, 9, 12, 15, 18, 21, 24
//     TimeSec       Time
//     TilesPerRow   Scalar Parameter "AtlasTilesPerRow"
//     MorphSeconds  Scalar Parameter "MorphSeconds"
//     D0            Append(TileNear, TileMid, DisplaceNear, DisplaceMid)
// After updating PlanetTerrainVertex.ush, change the version comment at the
// end to force a recompile.
// Paste everything below the dashed line into the node's Code field.
// -----------------------------------------------------------------------------
return PlanetTerrainWPO(UV, LocalPos, PosAtlas, NrmAtlas, BioAtlas, Bio2Atlas, AH, AHSampler,
                        CD0, CD1, CD2, CD3, CD4, CD5, CD6, CD7, CD8,
                        TimeSec, TilesPerRow, MorphSeconds, D0);
// v1

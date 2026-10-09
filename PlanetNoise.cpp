// PlanetNoise.cpp
#include "PlanetNoise.h"

void FPlanetNoiseGenerator::Build(const FPlanetNoiseParams& InParams, double PlanetRadiusMetres)
{
    Graph.Build(PlanetBridge::ToCoreParams(InParams, PlanetRadiusMetres));

    // Auto sea level: a quantile over a coarse sampling of all six faces
    // (32x32 per face = 6144 samples, sub-millisecond). This is what stops a
    // random seed from producing a world that is 99% ocean.
    if (Graph.IsValid())
    {
        Graph.ComputeSeaLevelForLandFraction(InParams.TargetLandFraction, 32);
    }
}

void FPlanetNoiseGenerator::SampleChunk(const FChunkKey& Key,
                                        TArrayView<PlanetCore::Surface> Out) const
{
    if (!Graph.IsValid()) return;
    check(Out.Num() >= PLANET_BODY_VERTS);
    Graph.SampleChunkBatch(PlanetBridge::ToCoreKey(Key), Out.GetData());
}

double FPlanetNoiseGenerator::GetHeightAt(const FVector3d& UnitDir) const
{
    if (!Graph.IsValid()) return 0.0;
    return Graph.EvaluateHeight(PlanetBridge::ToCore(UnitDir.GetSafeNormal()));
}

PlanetCore::Surface FPlanetNoiseGenerator::GetSurfaceAt(const FVector3d& UnitDir) const
{
    if (!Graph.IsValid()) return PlanetCore::Surface();
    return Graph.SampleSurface(PlanetBridge::ToCore(UnitDir.GetSafeNormal()));
}

FVector3d FPlanetNoiseGenerator::GetSurfacePositionAt(const FVector3d& UnitDir,
                                                      double PlanetRadius) const
{
    const FVector3d Dir = UnitDir.GetSafeNormal();
    return Dir * (PlanetRadius + GetHeightAt(Dir));
}

FPlanetNoiseGenerator::FHeightStats
FPlanetNoiseGenerator::MeasureHeightStats(int32 Samples, int32 SampleSeed) const
{
    FHeightStats Out;
    if (!Graph.IsValid()) return Out;

    const PlanetCore::NoiseGraph::Range R =
        Graph.MeasureHeightRange(FMath::Max(16, Samples), SampleSeed);

    Out.Min  = (double)R.Min;
    Out.Max  = (double)R.Max;
    Out.Mean = R.Mean;
    return Out;
}

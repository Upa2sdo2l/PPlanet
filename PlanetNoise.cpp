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
                                        TArrayView<PlanetCore::Surface> Out,
                                        PlanetCore::HaloGrid* OutHalo) const
{
    if (!Graph.IsValid()) return;
    check(Out.Num() >= PLANET_BODY_VERTS);
    Graph.SampleChunkBatch(PlanetBridge::ToCoreKey(Key), Out.GetData(), OutHalo);
}

double FPlanetNoiseGenerator::GetHeightAt(const FVector3d& UnitDir) const
{
    if (!Graph.IsValid()) return 0.0;
    const PlanetCore::Vec3d Dir = PlanetBridge::ToCore(UnitDir.GetSafeNormal());
    double H = Graph.EvaluateHeight(Dir);
    if (HasErosion())
    {
        H += PlanetErosion::DeltaAt(Graph, Dir, Erosion);
    }
    return H;
}

PlanetCore::Surface FPlanetNoiseGenerator::GetSurfaceAt(const FVector3d& UnitDir) const
{
    if (!Graph.IsValid()) return PlanetCore::Surface();
    const PlanetCore::Vec3d Dir = PlanetBridge::ToCore(UnitDir.GetSafeNormal());
    PlanetCore::Surface S = Graph.SampleSurface(Dir);
    // Height only: normal and biomes stay those of the uneroded surface.
    if (HasErosion()) S.Height += PlanetErosion::DeltaAt(Graph, Dir, Erosion);
    return S;
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

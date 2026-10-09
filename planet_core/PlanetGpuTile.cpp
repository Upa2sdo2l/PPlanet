// PlanetGpuTile.cpp
#include "PlanetGpuTile.h"
#include <algorithm>
#include <cmath>

namespace PlanetGpu
{

namespace
{
    constexpr double METRES_TO_CM = 100.0;
}

void BuildTile(const PlanetCore::FChunkKey& Key, const PlanetCore::HaloGrid& Halo,
               const PlanetErosion::ChunkInputs* Inputs, const PlanetErosion::Params& Erosion,
               double PlanetRadiusMetres, float SeaLevel, TileData& Out)
{
    using PlanetCore::Vec3d;

    // ── Chunk frame: origin on the reference sphere at the chunk centre,
    //    X along +U, Y along +V (orthogonalised), Z = X x Y.
    const double D    = 2.0 / (double)((int64_t)1 << Key.LOD);
    const double MidU = -1.0 + (double)Key.X * D + D * 0.5;
    const double MidV = -1.0 + (double)Key.Y * D + D * 0.5;
    const double H    = D * 1e-3;

    const Vec3d C  = PlanetCore::CubeFaceDirection(Key.Face, MidU, MidV);
    const Vec3d CU = PlanetCore::CubeFaceDirection(Key.Face, MidU + H, MidV);
    const Vec3d CV = PlanetCore::CubeFaceDirection(Key.Face, MidU, MidV + H);

    const Vec3d X = (CU - C).Normalised();
    Vec3d Y = CV - C;
    Y = (Y - X * Vec3d::Dot(X, Y)).Normalised();
    const Vec3d Z = Vec3d::Cross(X, Y);

    const double RadiusCm = PlanetRadiusMetres * METRES_TO_CM;
    const Vec3d  O = C * RadiusCm;

    double ErodeLo = 0.0, ErodeHi = 0.0;
    if (Inputs) PlanetErosion::DeltaRange(Erosion, ErodeLo, ErodeHi);

    auto Frame = [&](const Vec3d& P, double q[3])
    {
        const Vec3d R = P - O;
        q[0] = Vec3d::Dot(R, X); q[1] = Vec3d::Dot(R, Y); q[2] = Vec3d::Dot(R, Z);
    };

    // ── Bounding box of the chunk's own vertices (not the halo), widened by
    //    the erosion range wherever erosion may act.
    double Min[3] = { 1e300,  1e300,  1e300};
    double Max[3] = {-1e300, -1e300, -1e300};
    auto Grow = [&](const double q[3])
    {
        for (int a = 0; a < 3; ++a) { Min[a] = std::min(Min[a], q[a]); Max[a] = std::max(Max[a], q[a]); }
    };

    constexpr int32_t G = HALO_SIDE;
    for (int32_t y = 0; y < TILE_SIDE; ++y)
        for (int32_t x = 0; x < TILE_SIDE; ++x)
        {
            const int32_t g = (x + 1) + (y + 1) * G;
            const Vec3d& Dir = Halo.Dir[g];
            double q[3];
            Frame(Dir * ((PlanetRadiusMetres + Halo.Height[g]) * METRES_TO_CM), q);
            Grow(q);
            const double M = Inputs ? (double)Inputs->Mask[g] : 0.0;
            if (M > 0.0)
            {
                Frame(Dir * ((PlanetRadiusMetres + Halo.Height[g] + ErodeLo * M) * METRES_TO_CM), q); Grow(q);
                Frame(Dir * ((PlanetRadiusMetres + Halo.Height[g] + ErodeHi * M) * METRES_TO_CM), q); Grow(q);
            }
        }

    // ── Box -> instance transform. Thin boxes get a minimum thickness so the
    //    scale never degenerates (a flat ocean chunk would otherwise be 0 cm).
    double Ext[3], Mid[3];
    for (int a = 0; a < 3; ++a)
    {
        Ext[a] = Max[a] - Min[a];
        Mid[a] = (Max[a] + Min[a]) * 0.5;
    }
    Ext[0] = std::max(Ext[0], 1.0);
    Ext[1] = std::max(Ext[1], 1.0);
    Ext[2] = std::max(Ext[2], std::max(1.0, 0.01 * std::max(Ext[0], Ext[1])));

    const Vec3d T = O + X * Mid[0] + Y * Mid[1] + Z * Mid[2];
    Out.AxisX[0] = X.X; Out.AxisX[1] = X.Y; Out.AxisX[2] = X.Z;
    Out.AxisY[0] = Y.X; Out.AxisY[1] = Y.Y; Out.AxisY[2] = Y.Z;
    Out.AxisZ[0] = Z.X; Out.AxisZ[1] = Z.Y; Out.AxisZ[2] = Z.Z;
    for (int a = 0; a < 3; ++a) Out.Scale[a] = Ext[a];
    Out.Translation[0] = T.X;
    Out.Translation[1] = T.Y;
    Out.Translation[2] = T.Z;

    GpuTileInfo& I = Out.Info;
    I = GpuTileInfo();
    for (int a = 0; a < 3; ++a)
    {
        I.AxisX[a] = (float)Out.AxisX[a];
        I.AxisY[a] = (float)Out.AxisY[a];
        I.AxisZ[a] = (float)Out.AxisZ[a];
        I.ExtMetres[a] = (float)(Ext[a] / METRES_TO_CM);
    }
    I.AtlasTile = -1;

    // ── Every halo-grid point.
    for (int32_t g = 0; g < HALO_POINTS; ++g)
    {
        const Vec3d& Dir = Halo.Dir[g];
        double q[3];
        Frame(Dir * ((PlanetRadiusMetres + Halo.Height[g]) * METRES_TO_CM), q);

        GpuVertex& V = Out.Vertices[g];
        for (int a = 0; a < 3; ++a) V.LocalBase[a] = (float)((q[a] - Mid[a]) / Ext[a]);
        V.Height0 = (float)Halo.Height[g];
        V.PRef[0] = (float)(Dir.X * PlanetRadiusMetres);
        V.PRef[1] = (float)(Dir.Y * PlanetRadiusMetres);
        V.PRef[2] = (float)(Dir.Z * PlanetRadiusMetres);
        V.Humidity  = Halo.Humidity[g];
        V.TempNoise = Halo.TempNoise[g];
        V.Inland    = Halo.Continent[g] - SeaLevel;
        V.Pad       = 0.f;
        if (Inputs)
        {
            V.Fade = Inputs->Fade[g];
            V.Mask = Inputs->Mask[g];
            V.Gradient[0] = (float)Inputs->Gradient[g].X;
            V.Gradient[1] = (float)Inputs->Gradient[g].Y;
            V.Gradient[2] = (float)Inputs->Gradient[g].Z;
        }
        else
        {
            V.Fade = 0.f; V.Mask = 0.f;
            V.Gradient[0] = V.Gradient[1] = V.Gradient[2] = 0.f;
        }
    }
}

void LocalToPlanet(const TileData& T, const double Local[3], double OutPlanet[3])
{
    const double S0 = Local[0] * T.Scale[0];
    const double S1 = Local[1] * T.Scale[1];
    const double S2 = Local[2] * T.Scale[2];
    for (int a = 0; a < 3; ++a)
        OutPlanet[a] = T.Translation[a] + T.AxisX[a] * S0 + T.AxisY[a] * S1 + T.AxisZ[a] * S2;
}

} // namespace PlanetGpu

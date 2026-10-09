// PlanetGpuTile.cpp
#include "PlanetGpuTile.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace PlanetGpu
{

namespace
{
    constexpr double METRES_TO_CM = 100.0;

    inline uint8_t ToUnorm8(double V)
    {
        return (uint8_t)std::clamp((int)std::lround(std::clamp(V, 0.0, 1.0) * 255.0), 0, 255);
    }
}

void BuildTile(const PlanetCore::FChunkKey& Key, const PlanetCore::Surface* Surfaces,
               double PlanetRadiusMetres, TileData& Out)
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

    // ── Positions in the chunk frame, and their bounding box.
    static thread_local std::vector<double> Q;
    Q.resize((size_t)TILE_TEXELS * 3);

    double Min[3] = { 1e300,  1e300,  1e300};
    double Max[3] = {-1e300, -1e300, -1e300};

    for (int32_t gy = 0; gy < TILE_SIDE; ++gy)
    {
        for (int32_t gx = 0; gx < TILE_SIDE; ++gx)
        {
            const int32_t i = gx + gy * TILE_SIDE;
            double U, V;
            PlanetCore::ChunkVertexUV(Key, gx + 1, gy + 1, U, V);
            const Vec3d Dir = PlanetCore::CubeFaceDirection(Key.Face, U, V);
            const Vec3d P   = Dir * ((PlanetRadiusMetres + Surfaces[i].Height) * METRES_TO_CM);
            const Vec3d R   = P - O;

            const double q[3] = {Vec3d::Dot(R, X), Vec3d::Dot(R, Y), Vec3d::Dot(R, Z)};
            for (int a = 0; a < 3; ++a)
            {
                Q[(size_t)i * 3 + a] = q[a];
                Min[a] = std::min(Min[a], q[a]);
                Max[a] = std::max(Max[a], q[a]);
            }
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

    // ── Texels.
    for (int32_t i = 0; i < TILE_TEXELS; ++i)
    {
        for (int a = 0; a < 3; ++a)
            Out.Position[i][a] = (float)((Q[(size_t)i * 3 + a] - Mid[a]) / Ext[a]);
        Out.Position[i][3] = 0.f;

        const Vec3d& N = Surfaces[i].Normal;
        Out.Normal[i][0] = ToUnorm8(N.X * 0.5 + 0.5);
        Out.Normal[i][1] = ToUnorm8(N.Y * 0.5 + 0.5);
        Out.Normal[i][2] = ToUnorm8(N.Z * 0.5 + 0.5);
        Out.Normal[i][3] = 255;

        for (int b = 0; b < 4; ++b)
            Out.Biome[i][b] = ToUnorm8((double)Surfaces[i].Biome[b]);
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

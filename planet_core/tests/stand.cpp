// Test stand for planet_core. Stand-alone: no Unreal, only FastNoise2 and a C++17 compiler.
// How to build and run: README.md next to this file.
//
//   stand check     the pass/fail tests (basic, threads, seams, stream, basis); exit code 1 on a failure
//   stand <test>    one test; the rest are measurements that only print numbers
//
// The whole file is inside PLANET_CORE_STAND: the Unreal build compiles every .cpp
// under the module folder, and there this file must come out empty.
#if defined(PLANET_CORE_STAND)
#include "PlanetNoiseCore.h"
#include "PlanetErosion.h"
#include "PlanetGpuTile.h"
#include "PlanetDetail.h"
#include "PlanetLOD.h"
#include "PlanetStreaming.h"
#include "PlanetBiomes.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <tuple>
#include <vector>
#include <cstdlib>

using namespace PlanetCore;
static double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

struct Variant { const char* Name; ETerrainShape Shape; EMountainBlock Block; bool ByH; };
static const Variant VARS[] = {
    {"Musgrave", ETerrainShape::V6, EMountainBlock::Musgrave, false},
    {"Hybrid", ETerrainShape::V6, EMountainBlock::Hybrid, false},
    {"Uber", ETerrainShape::V6, EMountainBlock::Uber, false},
    {"Uber+ridges", ETerrainShape::V6, EMountainBlock::Uber, true},
    {"V4", ETerrainShape::V4, EMountainBlock::Musgrave, false},
    {"Original", ETerrainShape::Original, EMountainBlock::Musgrave, false},
};
static void BuildGraph(NoiseGraph& G, const Variant& V, int Seed = 1337)
{
    NoiseParams P; P.TerrainShape = V.Shape; P.ShapeV6.Block = V.Block; P.ShapeV6.UberRidgesByHeight = V.ByH; P.MasterSeed = Seed;
    G.Build(P); G.ComputeSeaLevelForLandFraction(0.30f, 32);
}
static PlanetErosion::Params Ero() { PlanetErosion::Params E; E.Strength = 0.12; E.HeightOffset = -0.1; return E; }

// find chunks of a LOD by kind: 0 sea, 1 plains, 2 mountains
static std::vector<FChunkKey> FindChunks(const NoiseGraph& G, int LOD, int Kind, int Want, uint32_t Seed)
{
    std::vector<FChunkKey> Out; std::mt19937 r(Seed);
    auto H = std::make_unique<HaloGrid>();
    const int N = 1 << LOD;
    for (int tries = 0; tries < 20000 && (int)Out.size() < Want; ++tries)
    {
        FChunkKey K((uint8_t)(r() % 6), (uint8_t)LOD, (int32_t)(r() % N), (int32_t)(r() % N));
        G.SampleChunkHalo(K, *H);
        bool land = false, mtn = false;
        for (int g = 0; g < HaloGrid::Count; ++g) { land |= H->Height[g] > 0; mtn |= H->ErosionMask[g] > 0.f; }
        if ((mtn ? 2 : (land ? 1 : 0)) == Kind) Out.push_back(K);
    }
    return Out;
}

// ── basic: every public path once per variant (run under ASan/UBSan) ─────────
static int TestBasic()
{
    int Fail = 0;
    for (const Variant& V : VARS)
    {
        NoiseGraph G; BuildGraph(G, V);
        const PlanetErosion::Params E = Ero();
        auto H = std::make_unique<HaloGrid>(); auto In = std::make_unique<PlanetErosion::ChunkInputs>();
        auto T = std::make_unique<PlanetGpu::TileData>();
        std::vector<Surface> S(PLANET_VERTS_PER_SIDE * PLANET_VERTS_PER_SIDE);
        std::vector<double> D(S.size());
        int nan = 0, cnt = 0; double hmin = 1e9, hmax = -1e9;
        for (int kind = 0; kind < 3; ++kind)
            for (int lod : {3, 9, 13, 14})
                for (const FChunkKey& K : FindChunks(G, lod, kind, 2, 7 + lod))
                {
                    G.SampleChunkBatch(K, S.data(), H.get());
                    G.SampleChunkHalo(K, *H);
                    PlanetErosion::BuildChunkInputs(G, K, *H, E, *In);
                    PlanetGpu::BuildTile(K, *H, In.get(), E, G.GetParams().PlanetRadiusMetres, G.GetEffectiveSeaLevel(), *T, 50.0);
                    if (lod >= 13) PlanetErosion::ErodeChunk(*H, *In, E, G.GetParams().PlanetRadiusMetres, D.data());
                    for (int g = 0; g < HaloGrid::Count; ++g)
                    {
                        ++cnt; if (!std::isfinite(H->Height[g]) || !std::isfinite(In->Gradient[g].X)) ++nan;
                        hmin = std::min(hmin, H->Height[g]); hmax = std::max(hmax, H->Height[g]);
                        for (int a = 0; a < 3; ++a) if (!std::isfinite(T->Vertices[g].LocalBase[a])) ++nan;
                    }
                }
        // corners and poles
        for (int f = 0; f < 6; ++f) for (double u : {-1.0, 1.0}) for (double v : {-1.0, 1.0})
        { const Vec3d d = CubeFaceDirection(f, u, v); const Surface s = G.SampleSurface(d); if (!std::isfinite(s.Height)) ++nan;
          const double e = PlanetErosion::DeltaAt(G, d, E); if (!std::isfinite(e)) ++nan; }
        for (Vec3d d : {Vec3d(0, 0, 1), Vec3d(0, 0, -1), Vec3d(1, 0, 0), Vec3d(0, 1, 0)}) { if (!std::isfinite(G.EvaluateHeight(d))) ++nan; }
        const bool bPass = nan == 0 && hmax <= G.MaxHeightEstimate();
        if (!bPass) ++Fail;
        std::printf("%-12s points %d, non-finite %d, height %.0f .. %.0f m, bound %.0f m, sea %.4f, land scale %.3f  %s\n",
                    V.Name, cnt, nan, hmin, hmax, G.MaxHeightEstimate(), G.GetEffectiveSeaLevel(), G.GetLandScale(), bPass ? "ok" : "FAIL");
    }
    return Fail ? 1 : 0;
}

// ── threads: concurrent chunk sampling on one graph (run under TSan) ────────
static int TestThreads()
{
    int Fail = 0;
    for (const Variant& V : {VARS[0], VARS[2]})
    {
        NoiseGraph G; BuildGraph(G, V);
        const PlanetErosion::Params E = Ero();
        std::vector<FChunkKey> Keys = FindChunks(G, 13, 2, 6, 3);
        std::vector<std::vector<double>> Ref;
        auto H = std::make_unique<HaloGrid>(); auto In = std::make_unique<PlanetErosion::ChunkInputs>();
        for (const FChunkKey& K : Keys) { G.SampleChunkHalo(K, *H); PlanetErosion::BuildChunkInputs(G, K, *H, E, *In);
            std::vector<double> R(H->Height, H->Height + HaloGrid::Count); for (int g = 0; g < HaloGrid::Count; ++g) R.push_back(In->Gradient[g].X); Ref.push_back(R); }
        std::atomic<int> Bad{0};
        auto Work = [&](int t)
        {
            auto h = std::make_unique<HaloGrid>(); auto in = std::make_unique<PlanetErosion::ChunkInputs>();
            for (int it = 0; it < 12; ++it)
            {
                const size_t k = (size_t)(t + it) % Keys.size();
                G.SampleChunkHalo(Keys[k], *h); PlanetErosion::BuildChunkInputs(G, Keys[k], *h, E, *in);
                for (int g = 0; g < HaloGrid::Count; ++g) if (h->Height[g] != Ref[k][g] || in->Gradient[g].X != Ref[k][HaloGrid::Count + g]) { ++Bad; break; }
                (void)G.EvaluateHeight(h->Dir[100]); (void)PlanetErosion::DeltaAt(G, h->Dir[200], E);
            }
        };
        std::vector<std::thread> Th; for (int t = 0; t < 4; ++t) Th.emplace_back(Work, t); for (auto& t : Th) t.join();
        if (Bad.load()) ++Fail;
        std::printf("%-12s 4 threads x 12 chunks: mismatches against the single-thread result %d  %s\n", V.Name, Bad.load(), Bad.load() ? "FAIL" : "ok");
    }
    return Fail ? 1 : 0;
}

// ── seams: shared vertices of neighbour chunks (same LOD, LOD+1, cube edges) ─
static int TestSeams()
{
    using PlanetLOD::ChunkKey;
    int Fail = 0;
    for (const Variant& V : VARS)
    {
        NoiseGraph G; BuildGraph(G, V);
        const PlanetErosion::Params E = Ero();
        auto HA = std::make_unique<HaloGrid>(); auto HB = std::make_unique<HaloGrid>();
        auto IA = std::make_unique<PlanetErosion::ChunkInputs>(); auto IB = std::make_unique<PlanetErosion::ChunkInputs>();
        std::vector<double> DA(65 * 65), DB(65 * 65);
        const double R = G.GetParams().PlanetRadiusMetres;
        long shared = 0, badH = 0, badM = 0, badE = 0, badMask = 0; double maxdH = 0, maxdE = 0, maxdM = 0;
        auto Compare = [&](const FChunkKey& A, const FChunkKey& B)
        {
            G.SampleChunkHalo(A, *HA); G.SampleChunkHalo(B, *HB);
            PlanetErosion::BuildChunkInputs(G, A, *HA, E, *IA); PlanetErosion::BuildChunkInputs(G, B, *HB, E, *IB);
            PlanetErosion::ErodeChunk(*HA, *IA, E, R, DA.data()); PlanetErosion::ErodeChunk(*HB, *IB, E, R, DB.data());
            // match border vertices of A and B by direction
            std::map<std::tuple<long long, long long, long long>, int> Map;
            auto Q = [](const Vec3d& d) { return std::make_tuple((long long)std::llround(d.X * 4e9), (long long)std::llround(d.Y * 4e9), (long long)std::llround(d.Z * 4e9)); };
            for (int y = 0; y < 65; ++y) for (int x = 0; x < 65; ++x) if (x == 0 || y == 0 || x == 64 || y == 64) Map[Q(HA->Dir[(x + 1) + (y + 1) * 67])] = x + y * 65;
            for (int y = 0; y < 65; ++y) for (int x = 0; x < 65; ++x)
            {
                if (!(x == 0 || y == 0 || x == 64 || y == 64)) continue;
                const int gb = (x + 1) + (y + 1) * 67;
                auto it = Map.find(Q(HB->Dir[gb])); if (it == Map.end()) continue;
                const int ia = it->second, ga = (ia % 65 + 1) + (ia / 65 + 1) * 67;
                ++shared;
                const double dH = std::fabs(HA->Height[ga] - HB->Height[gb]), dE = std::fabs(DA[ia] - DB[x + y * 65]);
                const double dM = std::fabs((double)HA->Mountain[ga] - (double)HB->Mountain[gb]);
                if (dH > 0) ++badH;
                if (dE > 1e-9) ++badE;
                if (dM > 0) ++badM;
                if (IA->Mask[ga] != IB->Mask[gb] || IA->Fade[ga] != IB->Fade[gb]) ++badMask;
                maxdH = std::max(maxdH, dH); maxdE = std::max(maxdE, dE); maxdM = std::max(maxdM, dM);
            }
        };
        std::mt19937 r(11);
        for (int kind = 1; kind < 3; ++kind)
            for (const FChunkKey& K : FindChunks(G, 12, kind, 6, 5))
                for (int side = 0; side < 4; ++side)
                {
                    const PlanetLOD::Neighbour N = PlanetLOD::FindNeighbourSameLOD(ChunkKey{K.Face, K.LOD, K.X, K.Y}, side);
                    if (!N.bValid) continue;
                    const FChunkKey B(N.Key.Face, N.Key.LOD, N.Key.X, N.Key.Y);
                    Compare(K, B);                                              // same LOD
                    for (int c = 0; c < 4; ++c) Compare(K, FChunkKey(B.Face, B.LOD + 1, B.X * 2 + (c & 1), B.Y * 2 + (c >> 1)));   // LOD + 1
                }
        // across cube edges and at cube corners
        for (int f = 0; f < 6; ++f) for (int side = 0; side < 4; ++side) for (int t : {0, 100, 255})
        {
            const int n = 256; ChunkKey K{(uint8_t)f, 8, 0, 0};
            if (side == 0) { K.X = 0; K.Y = t; } else if (side == 1) { K.X = n - 1; K.Y = t; } else if (side == 2) { K.Y = 0; K.X = t; } else { K.Y = n - 1; K.X = t; }
            const PlanetLOD::Neighbour N = PlanetLOD::FindNeighbourSameLOD(K, side);
            if (N.bValid) Compare(FChunkKey(K.Face, K.LOD, K.X, K.Y), FChunkKey(N.Key.Face, N.Key.LOD, N.Key.X, N.Key.Y));
        }
        // The Mountain field is reported only: it is known to differ (batches without mountains skip
        // the ridged octaves) and is not sent to the GPU.
        const bool bPass = shared > 0 && badH == 0 && badE == 0 && badMask == 0;
        if (!bPass) ++Fail;
        std::printf("%-12s shared vertices %ld: height differs %ld (max %.3g m), erosion differs %ld (max %.3g m), mask/fade differs %ld, Mountain field differs %ld (max %.3g)  %s\n",
                    V.Name, shared, badH, maxdH, badE, maxdE, badMask, badM, maxdM, bPass ? "ok" : "FAIL");
    }
    return Fail ? 1 : 0;
}

// ── point: GetHeightAt path against chunk vertices ───────────────────────────
static int TestPoint()
{
    for (const Variant& V : VARS)
    {
        NoiseGraph G; BuildGraph(G, V);
        const PlanetErosion::Params E = Ero();
        auto H = std::make_unique<HaloGrid>(); auto In = std::make_unique<PlanetErosion::ChunkInputs>();
        std::vector<double> D(65 * 65);
        double maxB = 0, maxE = 0, sumE = 0; long n = 0;
        for (const FChunkKey& K : FindChunks(G, 14, 2, 6, 21))
        {
            G.SampleChunkHalo(K, *H); PlanetErosion::BuildChunkInputs(G, K, *H, E, *In);
            PlanetErosion::ErodeChunk(*H, *In, E, G.GetParams().PlanetRadiusMetres, D.data());
            for (int y = 0; y < 65; y += 3) for (int x = 0; x < 65; x += 3)
            {
                const int g = (x + 1) + (y + 1) * 67;
                const double hb = G.EvaluateHeight(H->Dir[g]);
                const double he = PlanetErosion::DeltaAt(G, H->Dir[g], E);
                maxB = std::max(maxB, std::fabs(hb - H->Height[g]));
                const double d = std::fabs(he - D[x + y * 65]); maxE = std::max(maxE, d); sumE += d; ++n;
            }
        }
        std::printf("%-12s point vs chunk vertex: base height max %.3g m; erosion delta mean %.3f m, max %.2f m (%ld points)\n", V.Name, maxB, n ? sumE / n : 0, maxE, n);
    }
    return 0;
}

// ── jump: height discontinuities along lines (function of the point alone) ───
// Walks great-circle lines with a 1 m step and reports the largest step in height.
static int TestJump()
{
    for (const Variant& V : {VARS[0], VARS[1], VARS[2], VARS[3]})
    {
        NoiseGraph G; BuildGraph(G, V);
        const double R = G.GetParams().PlanetRadiusMetres;
        // lines crossing |z| = 0.9 (the Uber tangent basis switches there) and control lines at |z| = 0.6
        for (double zc : {0.9, -0.9, 0.6})
        {
            double worst = 0, worstLon = 0, worstAmp = 0; int lines = 0, worstI = 0;
            std::vector<float> X, Y, Z; std::vector<double> H;
            for (int li = 0; li < 720; ++li)
            {
                const double lon = li * (2 * M_PI / 720);
                const int NP = 801; X.resize(NP); Y.resize(NP); Z.resize(NP); H.resize(NP);
                for (int i = 0; i < NP; ++i)
                {
                    const double lat = std::asin(zc) + (i - NP / 2) * (1.0 / R);     // 1 m steps across the circle
                    X[i] = (float)(std::cos(lat) * std::cos(lon)); Y[i] = (float)(std::cos(lat) * std::sin(lon)); Z[i] = (float)std::sin(lat);
                }
                G.EvaluateBaseHeightBatch(X.data(), Y.data(), Z.data(), NP, H.data());
                double hi = -1e9; for (double h : H) hi = std::max(hi, h);
                if (hi < 800) continue;     // mountains only
                ++lines;
                for (int i = 1; i < NP; ++i) { if (std::min(H[i], H[i - 1]) < 300.0) continue; const double d = std::fabs(H[i] - H[i - 1]); if (d > worst) { worst = d; worstLon = lon; worstAmp = H[i]; worstI = i - NP / 2; } }
            }
            std::printf("%-12s z = %+.1f: %d mountain lines, largest height step over 1 m (above 300 m): %.2f m (lon %.3f, height %.0f m, %+d m from the circle)\n", V.Name, zc, lines, worst, worstLon, worstAmp, worstI);
        }
    }
    return 0;
}

// ── coast: height on both sides of the shoreline ─────────────────────────────
static int TestCoast()
{
    for (const Variant& V : {VARS[0], VARS[4]})
    {
        NoiseGraph G; BuildGraph(G, V);
        const double R = G.GetParams().PlanetRadiusMetres;
        std::mt19937 r(3); std::uniform_real_distribution<double> U(-1, 1);
        std::vector<double> Steps, LandH; int below = 0, n = 0;
        while ((int)Steps.size() < 2000)
        {
            Vec3d a(U(r), U(r), U(r)), b(U(r), U(r), U(r)); a = a.Normalised(); b = (a + b.Normalised() * 0.02).Normalised();
            Surface sa = G.SampleSurface(a), sb = G.SampleSurface(b);
            const float sea = G.GetEffectiveSeaLevel();
            if ((sa.Continent - sea < 0) == (sb.Continent - sea < 0)) continue;
            for (int it = 0; it < 40; ++it)       // bisect to the shoreline (continent value = sea level)
            {
                const Vec3d m = ((a + b) * 0.5).Normalised(); const Surface sm = G.SampleSurface(m);
                if ((sm.Continent - sea < 0) == (sa.Continent - sea < 0)) { a = m; sa = sm; } else { b = m; sb = sm; }
            }
            const double dist = (a - b).Length() * R;
            const double hl = sa.Continent - sea >= 0 ? sa.Height : sb.Height, hs = sa.Continent - sea >= 0 ? sb.Height : sa.Height;
            Steps.push_back(hl - hs); LandH.push_back(hl); ++n; if (hl < 0) ++below; (void)dist;
        }
        std::sort(Steps.begin(), Steps.end()); std::sort(LandH.begin(), LandH.end());
        auto q = [&](std::vector<double>& A, double p) { return A[(size_t)(p * (A.size() - 1))]; };
        std::printf("%-9s shoreline (2000 crossings, points < 1 m apart): height step land - sea: p5 %.1f, median %.1f, p95 %.1f m; land side below sea level in %.1f %%\n",
                    V.Name, q(Steps, 0.05), q(Steps, 0.5), q(Steps, 0.95), 100.0 * below / n);
    }
    return 0;
}

// ── faults: height steps between points 1 m apart inside the mountains ───────
static int TestFaults()
{
    for (const Variant& V : {VARS[0], VARS[1], VARS[2], VARS[3], VARS[4]})
    {
        NoiseGraph G; BuildGraph(G, V);
        const double R = G.GetParams().PlanetRadiusMetres;
        std::mt19937 r(5); std::uniform_real_distribution<double> U(-1, 1);
        const int B = 65536; std::vector<float> X(B), Y(B), Z(B), X2(B), Y2(B), Z2(B); std::vector<Surface> S(B); std::vector<double> H1(B), H2(B);
        long n = 0, c1 = 0, c3 = 0, c10 = 0; double worst = 0;
        std::vector<double> Slopes;
        for (int round = 0; round < 60; ++round)
        {
            int m = 0;
            std::vector<Vec3d> D(B);
            for (int i = 0; i < B; ++i) { Vec3d d(U(r), U(r), U(r)); D[i] = d.Normalised(); X[i] = (float)D[i].X; Y[i] = (float)D[i].Y; Z[i] = (float)D[i].Z; }
            std::vector<double> Hc(B); std::vector<float> Mk(B);
            G.EvaluateBaseHeightBatch(X.data(), Y.data(), Z.data(), B, Hc.data());
            for (int i = 0; i < B; ++i)
            {
                if (Hc[i] < 1200.0) continue;                 // inside the mountains
                Vec3d t(U(r), U(r), U(r)); t = (t - D[i] * Vec3d::Dot(t, D[i])).Normalised();
                const Vec3d e = (D[i] + t * (1.0 / R)).Normalised();
                X[m] = (float)D[i].X; Y[m] = (float)D[i].Y; Z[m] = (float)D[i].Z; X2[m] = (float)e.X; Y2[m] = (float)e.Y; Z2[m] = (float)e.Z; ++m;
            }
            G.EvaluateBaseHeightBatch(X.data(), Y.data(), Z.data(), m, H1.data());
            G.EvaluateBaseHeightBatch(X2.data(), Y2.data(), Z2.data(), m, H2.data());
            for (int i = 0; i < m; ++i)
            {
                const double d = std::fabs(H1[i] - H2[i]); ++n; Slopes.push_back(d);
                if (d > 1.0) ++c1;
                if (d > 3.0) ++c3;
                if (d > 10.0) ++c10;
                worst = std::max(worst, d);
                if (d > 10.0 && std::getenv("FAULT_DUMP")) std::printf("    step %.1f m at xyz %.5f %.5f %.5f h %.0f\n", d, X[i], Y[i], Z[i], H1[i]);
            }
        }
        std::sort(Slopes.begin(), Slopes.end());
        // a segment of length s crosses lines of density L (m of line per m^2) with probability (2 / pi) L s
        const double dens3 = (double)c3 / n * M_PI / 2 * 1e6 / 1000;   // km of line per km^2
        std::printf("%-12s %ld pairs 1 m apart above 1200 m: median |dh| %.2f m, p99 %.2f m; steps > 3 m: %ld (%.4f %%), > 10 m: %ld, largest %.1f m; ~%.2f km of step lines per km^2\n",
                    V.Name, n, Slopes[n / 2], Slopes[(size_t)(n * 0.99)], c3, 100.0 * c3 / n, c10, worst, dens3);
    }
    return 0;
}

// ── horizon: MaxHeightEstimate against sampled maxima ────────────────────────
static int TestBound()
{
    for (const Variant& V : VARS)
    {
        double worst = 0, bound = 0, emax = 0;
        for (int seed : {1337, 7, 99})
        {
            NoiseGraph G; BuildGraph(G, V, seed);
            const PlanetErosion::Params E = Ero();
            const int NP = 3000000; std::vector<float> X(NP), Y(NP), Z(NP);
            for (int i = 0; i < NP; ++i) { const double z = 1.0 - 2.0 * (i + 0.5) / NP, r = std::sqrt(1.0 - z * z), ph = i * 2.399963229728653; X[i] = (float)(r * std::cos(ph)); Y[i] = (float)(r * std::sin(ph)); Z[i] = (float)z; }
            double hi = -1e9;
            std::vector<Surface> S(65536);
            for (int o = 0; o < NP; o += 65536)
            {
                const int n = std::min(65536, NP - o);
                std::vector<double> H(n); G.EvaluateBaseHeightBatch(&X[o], &Y[o], &Z[o], n, H.data());
                for (double h : H) hi = std::max(hi, h);
            }
            double Lo, Hi; PlanetErosion::DeltaRange(E, Lo, Hi);
            worst = std::max(worst, hi); bound = G.MaxHeightEstimate(); emax = Hi;
        }
        std::printf("%-12s sampled max (3 seeds, base height) %.0f m; estimate %.0f m (+ erosion %.0f m)%s\n", V.Name, worst, bound, emax, worst > bound ? "  <-- BELOW THE REAL MAX" : "");
    }
    return 0;
}

// ── land: real land fraction against the target ──────────────────────────────
static int TestLand()
{
    for (int seed : {1337, 7, 99, 2024, 5})
        for (float target : {0.3f, 0.5f})
        {
            NoiseGraph G; NoiseParams P; P.MasterSeed = seed; G.Build(P); G.ComputeSeaLevelForLandFraction(target, 32);
            const int NP = 400000; std::vector<float> X(NP), Y(NP), Z(NP); std::vector<double> H(NP);
            for (int i = 0; i < NP; ++i) { const double z = 1.0 - 2.0 * (i + 0.5) / NP, r = std::sqrt(1.0 - z * z), ph = i * 2.399963229728653; X[i] = (float)(r * std::cos(ph)); Y[i] = (float)(r * std::sin(ph)); Z[i] = (float)z; }
            G.EvaluateBaseHeightBatch(X.data(), Y.data(), Z.data(), NP, H.data());
            long land = 0; for (double h : H) land += h > 0;
            std::printf("seed %5d target %.2f -> real land fraction %.3f\n", seed, target, (double)land / NP);
        }
    return 0;
}

// ── lod: leaves by level for a camera at a face centre, an edge and a corner ─
static int TestLod()
{
    using PlanetLOD::TraversalParams; using PlanetLOD::Selection; using PlanetLOD::ChunkKey; using PlanetLOD::ChunkExtent; using PlanetLOD::Traverse;
    typedef PlanetLOD::Vec3d LV;
    const double R = 2.5e8;   // cm
    struct Cam { const char* Name; int Face; double U, V; } Cams[] = {{"face centre", 0, 0.01, 0.013}, {"edge middle", 0, 0.999, 0.013}, {"cube corner", 0, 0.995, 0.993}};
    for (double alt : {200.0, 300000.0, 1200000.0})
        for (const Cam& C : Cams)
        {
            TraversalParams P; P.PlanetRadius = R; P.MaxLeaves = 256; P.MaxLOD = 14; P.ErrorThresholdPx = 1.0; P.HeightMargin = 1.1e6;
            const LV d = PlanetLOD::CubeFaceDirection(C.Face, C.U, C.V);
            P.CameraPos = {d.X * (R + alt), d.Y * (R + alt), d.Z * (R + alt)};
            Selection S; Traverse(P, S);
            int byLod[20] = {}; int deepest = 0; for (const ChunkKey& K : S.Chunks) { byLod[K.LOD]++; deepest = std::max(deepest, (int)K.LOD); }
            // real size of the deepest chunk under the camera vs the size the metric assumes
            double side = 0, assumed = 0, radius = 1e30;
            for (const ChunkKey& K : S.Chunks)
            {
                double U0, V0, Sz; ChunkExtent(K, U0, V0, Sz);
                if (K.Face != C.Face || C.U < U0 || C.U > U0 + Sz || C.V < V0 || C.V > V0 + Sz) continue;
                const LV a = PlanetLOD::CubeFaceDirection(K.Face, U0, V0), b = PlanetLOD::CubeFaceDirection(K.Face, U0 + Sz, V0), c = PlanetLOD::CubeFaceDirection(K.Face, U0, V0 + Sz);
                const double su = std::sqrt((a.X - b.X) * (a.X - b.X) + (a.Y - b.Y) * (a.Y - b.Y) + (a.Z - b.Z) * (a.Z - b.Z)) * R / 100;
                const double sv = std::sqrt((a.X - c.X) * (a.X - c.X) + (a.Y - c.Y) * (a.Y - c.Y) + (a.Z - c.Z) * (a.Z - c.Z)) * R / 100;
                side = std::sqrt(su * sv); assumed = Sz * R / 100;
            }
            // radius (m) within which every leaf is at MaxLOD or the deepest level reached
            for (const ChunkKey& K : S.Chunks)
            {
                if ((int)K.LOD >= deepest) continue;
                double U0, V0, Sz; ChunkExtent(K, U0, V0, Sz);
                const LV c = PlanetLOD::CubeFaceDirection(K.Face, U0 + Sz / 2, V0 + Sz / 2);
                const double dist = std::sqrt((c.X - d.X) * (c.X - d.X) + (c.Y - d.Y) * (c.Y - d.Y) + (c.Z - d.Z) * (c.Z - d.Z)) * R / 100 - assumed * 0.71 * (double)(1 << (deepest - K.LOD));
                radius = std::min(radius, dist);
            }
            std::printf("alt %8.0f m, %-11s leaves %3zu budget %s deepest L%d x%d; chunk under camera: real side %.0f m, metric assumes %.0f m\n",
                        alt / 100, C.Name, S.Chunks.size(), S.bBudgetHit ? "HIT" : "ok ", deepest, byLod[deepest], side, assumed);
        }
    return 0;
}

// ── cost: per chunk, per kind (single thread) ────────────────────────────────
static int TestCost()
{
    for (const Variant& V : {VARS[0], VARS[1], VARS[2], VARS[3]})
    {
        const double t0 = Now(); NoiseGraph G; NoiseParams P; P.ShapeV6.Block = V.Block; P.ShapeV6.UberRidgesByHeight = V.ByH; G.Build(P); const double t1 = Now();
        G.ComputeSeaLevelForLandFraction(0.3f, 32); const double t2 = Now();
        const PlanetErosion::Params E = Ero();
        auto H = std::make_unique<HaloGrid>(); auto In = std::make_unique<PlanetErosion::ChunkInputs>();
        double t[3] = {0, 0, 0}; int c[3] = {0, 0, 0}; std::mt19937 r(9);
        for (int k = 0; k < 300; ++k)
        {
            FChunkKey K((uint8_t)(r() % 6), 13, (int32_t)(r() % 8192), (int32_t)(r() % 8192));
            const double a = Now(); G.SampleChunkHalo(K, *H); PlanetErosion::BuildChunkInputs(G, K, *H, E, *In); const double dt = Now() - a;
            bool land = false, mtn = false; for (int g = 0; g < HaloGrid::Count; ++g) { land |= H->Height[g] > 0; mtn |= In->Mask[g] > 0.f; }
            const int kind = mtn ? 2 : (land ? 1 : 0); t[kind] += dt; ++c[kind];
        }
        std::printf("%-12s Build %.0f ms + sea level %.0f ms; chunk: sea %.2f, plains %.2f, mountains %.2f ms\n", V.Name, 1000 * (t1 - t0), 1000 * (t2 - t1),
                    c[0] ? 1000 * t[0] / c[0] : 0., c[1] ? 1000 * t[1] / c[1] : 0., c[2] ? 1000 * t[2] / c[2] : 0.);
    }
    return 0;
}

// Streaming scheduler: a chunk that carries collision must not go before the chunks
// replacing it have their own (GPU renderer: collision arrives frames after "Active").
namespace StreamTest
{
    using namespace PlanetStreaming;
    struct Sim
    {
        Scheduler S{64};
        FrameBudget B;
        uint64_t Frame = 0;
        std::vector<DesiredChunk> Want;
        std::map<int32_t, uint64_t> ActiveAt;   // slot -> frame it became Active
        bool bOk = true;
        void Step()
        {
            ++Frame;
            S.Reconcile(Want, Frame, B);
            WorkerRequest R;
            while (S.PopWorkerRequest(R))
            {
                if (!S.MarkBuilding(R)) continue;
                Completion C; C.Key = R.Key; C.SlotIndex = R.SlotIndex; C.Generation = R.Generation;
                S.WorkerFinished(R.SlotIndex);
                if (S.AcceptCompletion(C) && S.MarkActive(C)) ActiveAt[R.SlotIndex] = Frame;
            }
            S.UpdateCoverage(Frame, B);
            S.DrainRetires(B.MaxRetires);
            char Why[128];
            if (!S.ValidateInvariants(Why, sizeof(Why))) { std::printf("  invariant broken at frame %llu: %s\n", (unsigned long long)Frame, Why); bOk = false; }
        }
        bool Live(const PlanetLOD::ChunkKey& K) const { return S.FindSlotIndex(K) >= 0; }
        bool HasCollision(const PlanetLOD::ChunkKey& K) const
        {
            const int32_t i = S.FindSlotIndex(K);
            return i >= 0 && S.GetSlots()[i].State == SlotState::Active && S.GetSlots()[i].bCollisionApplied;
        }
    };
    static PlanetLOD::ChunkKey Key(int LOD, int X, int Y) { PlanetLOD::ChunkKey K; K.Face = 0; K.LOD = (uint8_t)LOD; K.X = X; K.Y = Y; return K; }
    static DesiredChunk Want(const PlanetLOD::ChunkKey& K, bool bCol, double Pri) { DesiredChunk D; D.Key = K; D.bNeedsCollision = bCol; D.Priority = Pri; return D; }

    // Parent (collision as given) is replaced by four children. Children get collision
    // CollisionDelay[i] frames after they become Active (-1: never). Returns the frame the
    // parent went, counted from the frame the children became Active; Gap = frames in which
    // the surface had no collision although a chunk wanting it was live.
    static int Split(bool bParentCollision, bool bChildrenNeed, const int CollisionDelay[4], int& Gap, bool& bOk)
    {
        Sim M;
        const PlanetLOD::ChunkKey P = Key(10, 5, 7);
        const PlanetLOD::ChunkKey C[4] = {Key(11, 10, 14), Key(11, 11, 14), Key(11, 10, 15), Key(11, 11, 15)};
        M.Want = {Want(P, bParentCollision, 1.0)};
        for (int f = 0; f < 10; ++f) M.Step();
        if (bParentCollision) M.S.SetCollisionApplied(M.S.FindSlotIndex(P), true, M.Frame);
        for (int f = 0; f < 10; ++f) M.Step();

        M.Want.clear();
        for (int i = 0; i < 4; ++i) M.Want.push_back(Want(C[i], bChildrenNeed, 1.0 + 0.1 * i));
        uint64_t ChildrenActive = 0, ParentGone = 0;
        Gap = 0;
        for (int f = 0; f < 400 && !ParentGone; ++f)
        {
            M.Step();
            bool bAllActive = true;
            for (int i = 0; i < 4; ++i)
            {
                const int32_t si = M.S.FindSlotIndex(C[i]);
                if (si < 0 || M.S.GetSlots()[si].State != SlotState::Active) { bAllActive = false; continue; }
                // the UE side: the collision mesh arrives some frames after the chunk became Active
                if (bChildrenNeed && CollisionDelay[i] >= 0 && !M.S.GetSlots()[si].bCollisionApplied &&
                    M.Frame >= M.ActiveAt[si] + (uint64_t)CollisionDelay[i])
                    M.S.SetCollisionApplied(si, true, M.Frame);
            }
            if (bAllActive && !ChildrenActive) ChildrenActive = M.Frame;
            if (!M.Live(P)) ParentGone = M.Frame;
            // collision coverage of the parent's surface
            bool bChildren = true;
            for (int i = 0; i < 4; ++i) bChildren = bChildren && M.HasCollision(C[i]);
            if (bParentCollision && bChildrenNeed && !M.HasCollision(P) && !bChildren) ++Gap;
        }
        bOk = bOk && M.bOk && ParentGone != 0 && ChildrenActive != 0;
        return (int)(ParentGone - ChildrenActive);
    }
}
static int TestStream()
{
    using namespace StreamTest;
    bool bOk = true; int Gap = 0, Fail = 0;
    auto Check = [&](const char* Name, int Got, int Expect, int GapNow, int GapMax)
    {
        const bool bPass = Got == Expect && GapNow <= GapMax;
        std::printf("%-58s parent left %3d frames after the children became Active (expected %d), frames without collision %d  %s\n",
                    Name, Got, Expect, GapNow, bPass ? "ok" : "FAIL");
        if (!bPass) ++Fail;
    };
    const FrameBudget B; const int Settle = B.SettleFrames, MaxLinger = B.MaxLingerFrames;
    { const int D[4] = {4, 4, 4, 4};  const int R = Split(true, true, D, Gap, bOk);  Check("collision on both sides, mesh 4 frames late", R, 4 + Settle, Gap, 0); }
    { const int D[4] = {3, 9, 5, 20}; const int R = Split(true, true, D, Gap, bOk);  Check("collision on both sides, meshes 3..20 frames late", R, 20 + Settle, Gap, 0); }
    { const int D[4] = {0, 0, 0, 0};  const int R = Split(true, true, D, Gap, bOk);  Check("collision on both sides, mesh in the same frame", R, Settle, Gap, 0); }
    { const int D[4] = {4, 4, 4, 4};  const int R = Split(false, true, D, Gap, bOk); Check("parent without collision: nothing to wait for", R, Settle, Gap, 0); }
    { const int D[4] = {4, 4, 4, 4};  const int R = Split(true, false, D, Gap, bOk); Check("children do not need collision", R, Settle, Gap, 0); }
    { const int D[4] = {4, -1, 4, 4}; const int R = Split(true, true, D, Gap, bOk);
      // the safety valve (in this run the children become Active in the frame the parent starts lingering)
      Check("one mesh never arrives: the linger limit lets go", R, MaxLinger, Gap, 1 << 30); }
    if (!bOk) { std::printf("scheduler run failed (invariants or the swap never happened)\n"); ++Fail; }
    std::printf("stream: %s\n", Fail ? "FAILED" : "all passed");
    return Fail ? 1 : 0;
}

// ── basis: the Uber block must have no height step where a tangent basis would switch ──
// Until 2026-11 the slope for the slope erosion was taken in a tangent basis that changed its
// reference axis at |z| = 0.9; the height stepped there by 15-19 m (median), up to 387 m.
// UberWarp = 0 puts the old switch exactly on the circle. 1 m steps across the circle must
// look like 1 m steps anywhere else.
static int TestBasis()
{
    NoiseParams P; P.ShapeV6.Block = EMountainBlock::Uber; P.ShapeV6.UberWarp = 0.f;
    NoiseGraph G; G.Build(P); G.ComputeSeaLevelForLandFraction(0.3f, 32);
    const double R = P.PlanetRadiusMetres, Pi = 3.14159265358979323846;
    int Fail = 0;
    for (double zc : {0.9, -0.9})
    {
        std::vector<double> AtSwitch, Else; double worst = 0;
        for (int li = 0; li < 20000; ++li)
        {
            const double lon = li * (2 * Pi / 20000);
            const int NP = 41; float X[NP], Y[NP], Z[NP]; double H[NP];
            for (int i = 0; i < NP; ++i)
            {
                const double lat = std::asin(zc) + (i - NP / 2 + 0.5) * (1.0 / R);
                X[i] = (float)(std::cos(lat) * std::cos(lon)); Y[i] = (float)(std::cos(lat) * std::sin(lon)); Z[i] = (float)std::sin(lat);
            }
            G.EvaluateBaseHeightBatch(X, Y, Z, NP, H);
            if (H[NP / 2] < 1200) continue;
            for (int i = 1; i < NP; ++i)
            {
                const bool sw = (std::fabs(Z[i - 1]) < 0.9) != (std::fabs(Z[i]) < 0.9);
                const double d = std::fabs(H[i] - H[i - 1]);
                (sw ? AtSwitch : Else).push_back(d);
                if (sw) worst = std::max(worst, d);
            }
        }
        std::sort(AtSwitch.begin(), AtSwitch.end()); std::sort(Else.begin(), Else.end());
        auto q = [](std::vector<double>& A, double p) { return A.empty() ? 0.0 : A[(size_t)(p * (double)(A.size() - 1))]; };
        const bool bPass = !AtSwitch.empty() && q(AtSwitch, 0.5) <= 3.0 * q(Else, 0.5) + 0.05 && q(AtSwitch, 0.9) <= 3.0 * q(Else, 0.9) + 0.1;
        if (!bPass) ++Fail;
        std::printf("z = %+.1f: 1 m steps across the circle (%zu): median %.2f m, p90 %.2f m, max %.1f m | elsewhere (%zu): median %.2f m, p90 %.2f m  %s\n",
                    zc, AtSwitch.size(), q(AtSwitch, 0.5), q(AtSwitch, 0.9), worst, Else.size(), q(Else, 0.5), q(Else, 0.9), bPass ? "ok" : "FAIL");
    }
    return Fail ? 1 : 0;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
    const std::string T = argc > 1 ? argv[1] : "";
    if (T == "basic") return TestBasic();
    if (T == "threads") return TestThreads();
    if (T == "seams") return TestSeams();
    if (T == "point") return TestPoint();
    if (T == "jump") return TestJump();
    if (T == "bound") return TestBound();
    if (T == "coast") return TestCoast();
    if (T == "faults") return TestFaults();
    if (T == "land") return TestLand();
    if (T == "lod") return TestLod();
    if (T == "cost") return TestCost();
    if (T == "stream") return TestStream();
    if (T == "basis") return TestBasis();
    if (T == "check")
    {
        int Fail = 0;
        struct { const char* Name; int (*Run)(); } const Checks[] = {
            {"basic", TestBasic}, {"threads", TestThreads}, {"seams", TestSeams}, {"stream", TestStream}, {"basis", TestBasis}};
        for (const auto& C : Checks) { std::printf("== %s\n", C.Name); if (C.Run()) { ++Fail; std::printf("== %s FAILED\n", C.Name); } }
        std::printf(Fail ? "check: %d test(s) FAILED\n" : "check: all passed\n", Fail);
        return Fail ? 1 : 0;
    }
    std::fprintf(stderr, "tests: check | basic threads seams stream basis | point jump coast faults bound land lod cost\n");
    return 1;
}

#endif // PLANET_CORE_STAND

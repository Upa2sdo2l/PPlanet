// PlanetTectonics.cpp
#include "PlanetTectonics.h"
#include <algorithm>
#include <cmath>
#include <random>

namespace PlanetTectonics
{

using PlanetCore::TectonicParams;

namespace
{
    inline float SStep(float A, float B, float X)
    {
        const float T = std::clamp((X - A) / (B - A), 0.f, 1.f);
        return T * T * (3.f - 2.f * T);
    }
    inline double Angle(const Vec3d& A, const Vec3d& B)
    {
        return std::acos(std::clamp(Vec3d::Dot(A, B), -1.0, 1.0));
    }

    // Distributions written out: libstdc++'s algorithms (generate_canonical
    // with two 32-bit draws; Marsaglia's polar method), so the plates match
    // the lab bit for bit and do not change with the standard library.
    struct Rng
    {
        std::mt19937 G;
        bool   bSaved = false;
        double Saved  = 0.0;
        explicit Rng(uint32_t Seed) : G(Seed) {}

        double Canonical()
        {
            const double A = (double)G(), B = (double)G();
            const double C = (A + B * 4294967296.0) / 18446744073709551616.0;
            return (C >= 1.0) ? std::nextafter(1.0, 0.0) : C;
        }
        double Uniform(double Lo, double Hi) { return Canonical() * (Hi - Lo) + Lo; }
        double Normal()
        {
            if (bSaved) { bSaved = false; return Saved; }
            double X, Y, R2;
            do
            {
                X = 2.0 * Canonical() - 1.0;
                Y = 2.0 * Canonical() - 1.0;
                R2 = X * X + Y * Y;
            } while (R2 > 1.0 || R2 == 0.0);
            const double Mult = std::sqrt(-2.0 * std::log(R2) / R2);
            Saved = X * Mult; bSaved = true;
            return Y * Mult;
        }
    };
}

void Plates::Build(uint32_t Seed, const std::vector<Vec3d>& Pts, const std::vector<float>& Above,
                   const TectonicParams& P)
{
    Centre.clear(); Velocity.clear(); Continental.clear();
    const int32_t NCont = std::max(0, P.ContinentalPlates);
    const int32_t NTotal = std::max(NCont + 2, P.TotalPlates);
    const size_t N = Pts.size();

    std::vector<int32_t> Order(N);
    for (size_t i = 0; i < N; ++i) Order[i] = (int32_t)i;
    auto FarEnough = [&](const Vec3d& V, double Sep)
    {
        for (const Vec3d& O : Centre) if (Angle(V, O) < Sep) return false;
        return true;
    };

    // Continental plates: the most inland samples first.
    std::sort(Order.begin(), Order.end(), [&](int32_t A, int32_t B)
              { return Above[A] > Above[B] || (Above[A] == Above[B] && A < B); });
    for (int32_t i : Order)
    {
        if ((int32_t)Centre.size() >= NCont || Above[i] < 0.12f) break;
        if (FarEnough(Pts[i], P.PlateSeparation)) { Centre.push_back(Pts[i]); Continental.push_back(1); }
    }
    // Oceanic plates: the deepest samples first, relaxing the separation
    // until every plate has a place.
    std::reverse(Order.begin(), Order.end());
    for (double Sep = P.PlateSeparation; (int32_t)Centre.size() < NTotal && Sep > 0.2; Sep *= 0.85)
        for (int32_t i : Order)
        {
            if ((int32_t)Centre.size() >= NTotal) break;
            if (Above[i] > -0.05f) continue;
            if (FarEnough(Pts[i], Sep)) { Centre.push_back(Pts[i]); Continental.push_back(0); }
        }

    // Motions: most ocean plates dive towards the nearest continent (active
    // margins, like the Pacific rim); continents drift slowly; the rest is
    // random (passive margins, like the Atlantic).
    Rng R(Seed);
    for (size_t i = 0; i < Centre.size(); ++i)
    {
        // The lab built this vector as Vec3d(G(), G(), G()); GCC evaluates
        // those arguments right to left.
        const double Zc = R.Normal(), Yc = R.Normal(), Xc = R.Normal();
        Vec3d Axis = Vec3d(Xc, Yc, Zc).Normalised();
        double Speed = R.Uniform(0.3, 1.0);
        if (Continental[i]) Speed *= 0.45;
        else if (R.Uniform(0.0, 1.0) < P.SubductionShare)
        {
            int32_t Best = -1; double BestD = 1e9;
            for (size_t j = 0; j < Centre.size(); ++j)
                if (Continental[j])
                {
                    const double D = Angle(Centre[i], Centre[j]);
                    if (D < BestD) { BestD = D; Best = (int32_t)j; }
                }
            if (Best >= 0)
            {
                Axis  = Vec3d::Cross(Centre[i], Centre[Best]).Normalised();
                Speed = 0.6 + 0.4 * R.Uniform(0.0, 1.0);
            }
        }
        Velocity.push_back(Axis * Speed);
    }
}

void Plates::Query(const Vec3d& p, double RadiusMetres, double OutDist[2], double OutConv[2], int32_t Id[3]) const
{
    // Nearest three by dot product (no acos per plate), then three acos.
    double Dot[3] = {-2.0, -2.0, -2.0};
    Id[0] = Id[1] = Id[2] = 0;
    const int32_t N = (int32_t)Centre.size();
    for (int32_t i = 0; i < N; ++i)
    {
        const double D = Vec3d::Dot(p, Centre[i]);
        if (D > Dot[0])      { Dot[2] = Dot[1]; Id[2] = Id[1]; Dot[1] = Dot[0]; Id[1] = Id[0]; Dot[0] = D; Id[0] = i; }
        else if (D > Dot[1]) { Dot[2] = Dot[1]; Id[2] = Id[1]; Dot[1] = D; Id[1] = i; }
        else if (D > Dot[2]) { Dot[2] = D; Id[2] = i; }
    }
    const double A0 = std::acos(std::clamp(Dot[0], -1.0, 1.0));
    const int32_t a = Id[0];
    const Vec3d Va = Vec3d::Cross(Velocity[a], p);
    for (int k = 0; k < 2; ++k)
    {
        const int32_t b = Id[k + 1];
        OutDist[k] = (std::acos(std::clamp(Dot[k + 1], -1.0, 1.0)) - A0) * 0.5 * RadiusMetres;   // to the bisector
        Vec3d n = Centre[b] - Centre[a];
        n = (n - p * Vec3d::Dot(n, p)).Normalised();
        const Vec3d Vb = Vec3d::Cross(Velocity[b], p);
        OutConv[k] = std::clamp(-Vec3d::Dot(Vb - Va, n) / 0.9, -1.0, 1.0);   // b moving towards a => +
    }
}

void Plates::Candidates(const Vec3d* Points, int32_t Count, std::vector<int32_t>& Out) const
{
    Out.clear();
    const int32_t N = Num();
    if (Count <= 0 || N == 0) return;
    Vec3d Sum;
    for (int32_t i = 0; i < Count; ++i) Sum = Sum + Points[i];
    double Cap = 3.14159265358979;   // angular radius of the points around their centre
    Vec3d C = Points[0];
    if (Sum.Length() > 1e-9)
    {
        C = Sum.Normalised();
        double MinDot = 1.0;
        for (int32_t i = 0; i < Count; ++i) MinDot = std::min(MinDot, Vec3d::Dot(Points[i], C));
        Cap = std::acos(std::clamp(MinDot, -1.0, 1.0)) + 1e-6;
    }
    // A point of the cap is at least Ang - Cap and at most Ang + Cap from a
    // plate centre. Plate i can be among a point's three nearest only if
    // its lower bound does not exceed the third smallest upper bound.
    std::vector<double> Ang(N), Upper(N);
    for (int32_t i = 0; i < N; ++i) { Ang[i] = Angle(C, Centre[i]); Upper[i] = Ang[i] + Cap; }
    std::vector<double> Sorted = Upper;
    std::nth_element(Sorted.begin(), Sorted.begin() + std::min(2, N - 1), Sorted.end());
    const double Third = Sorted[std::min(2, N - 1)] + 1e-9;
    for (int32_t i = 0; i < N; ++i)
        if (Ang[i] - Cap <= Third) Out.push_back(i);
}

Boundary BoundaryAt(const Plates& Pl, const Vec3d& q, double RadiusMetres, const TectonicParams& P,
                    const int32_t* Cand, int32_t NCand)
{
    Boundary B;
    const int32_t N = Pl.Num();
    if (N < 3) return B;
    const int32_t Loop = Cand ? NCand : N;

    // Nearest three plates by dot product (ascending index: ties resolve
    // the same with or without the candidate list).
    double Dot[3] = {-2.0, -2.0, -2.0};
    int32_t Id[3] = {0, 0, 0};
    for (int32_t k = 0; k < Loop; ++k)
    {
        const int32_t i = Cand ? Cand[k] : k;
        const double D = Vec3d::Dot(q, Pl.Centre[i]);
        if (D > Dot[0])      { Dot[2] = Dot[1]; Id[2] = Id[1]; Dot[1] = Dot[0]; Id[1] = Id[0]; Dot[0] = D; Id[0] = i; }
        else if (D > Dot[1]) { Dot[2] = Dot[1]; Id[2] = Id[1]; Dot[1] = D; Id[1] = i; }
        else if (D > Dot[2]) { Dot[2] = D; Id[2] = i; }
    }

    const float W = P.RangeWidthMetres, Off = P.CoastalRangeOffsetMetres;
    const int32_t a = Id[0];
    const bool aC = Pl.Continental[a] != 0;
    double A0 = -1.0;          // acos(Dot[0]), when first needed
    bool   bVa = false;
    Vec3d  Va;
    for (int bi = 0; bi < 2; ++bi)
    {
        const int32_t b = Id[1 + bi];
        const bool bC = Pl.Continental[b] != 0;

        // Every feature of this pair of plates is exactly 0 beyond Reach
        // (the smoothsteps saturate). acos is steeper than 1 everywhere, so
        // (Dot[0] - Dot[b]) R / 2 is a lower bound of the distance: most
        // points, deep inside a plate, stop here without an acos.
        const float Reach = (aC && bC) ? 3.5f * W
                          : (aC && !bC) ? Off + 2.6f * W
                          : (!aC && bC) ? 0.35f * W
                          : std::max(7.0f * W, 0.6f * Off + 0.4f * W);
        if ((Dot[0] - Dot[1 + bi]) * 0.5 * RadiusMetres > (double)Reach * 1.001 + 1.0) continue;

        if (!bVa) { Va = Vec3d::Cross(Pl.Velocity[a], q); bVa = true; }
        Vec3d n = Pl.Centre[b] - Pl.Centre[a];
        n = (n - q * Vec3d::Dot(n, q)).Normalised();
        const Vec3d Vb = Vec3d::Cross(Pl.Velocity[b], q);
        const float Cn = (float)std::clamp(-Vec3d::Dot(Vb - Va, n) / 0.9, -1.0, 1.0);   // b moving towards a => +
        const float Cv = SStep(-0.05f, 0.35f, Cn), Dv = SStep(0.15f, 0.5f, -Cn);
        if (Cv <= 0.f && Dv <= 0.f) continue;
        // The features this motion can make, and how far they reach.
        const float Reach2 = (aC && bC) ? ((Cv > 0.f) ? 3.5f * W : 0.7f * W)
                           : (aC && !bC) ? ((Cv > 0.f) ? Off + 2.6f * W : 0.f)
                           : (!aC && bC) ? ((Cv > 0.f) ? 0.35f * W : 0.f)
                           : std::max((Cv > 0.f) ? 0.6f * Off + 0.4f * W : 0.f, (Dv > 0.f) ? 7.0f * W : 0.f);
        if ((Dot[0] - Dot[1 + bi]) * 0.5 * RadiusMetres > (double)Reach2 * 1.001 + 1.0) continue;

        if (A0 < 0.0) A0 = std::acos(std::clamp(Dot[0], -1.0, 1.0));
        const float D = (float)((std::acos(std::clamp(Dot[1 + bi], -1.0, 1.0)) - A0) * 0.5 * RadiusMetres);   // to the bisector

        if (aC && bC)
        {
            const float Spine = 1.f - SStep(0.f, W, D);
            B.Range   = std::max(B.Range,   std::pow(Spine, 1.3f) * Cv);
            B.Foot    = std::max(B.Foot,    (1.f - SStep(0.f, 2.2f * W, D)) * Cv);
            B.Plateau = std::max(B.Plateau, (1.f - SStep(0.f, 3.5f * W, D)) * Cv);
            B.Rift    = std::max(B.Rift,    (1.f - SStep(0.f, 0.7f * W, D)) * Dv);
        }
        else if (aC && !bC)
        {
            const float Spine = 1.f - SStep(0.f, 0.8f * W, std::fabs(D - Off));
            B.Range   = std::max(B.Range, std::pow(Spine, 1.3f) * Cv);
            B.Foot    = std::max(B.Foot,  (1.f - SStep(0.f, 1.8f * W, std::fabs(D - Off))) * Cv);
            B.Plateau = std::max(B.Plateau, 0.6f * (1.f - SStep(0.f, 1.6f * W, std::fabs(D - Off - W))) * Cv);
        }
        else if (!aC && bC)
        {
            B.Trench = std::max(B.Trench, (1.f - SStep(0.f, 0.35f * W, D)) * Cv);
        }
        else
        {
            if (a > b)   // the overriding plate carries the arc
                B.Arc = std::max(B.Arc, (1.f - SStep(0.f, 0.4f * W, std::fabs(D - 0.6f * Off))) * Cv);
            else
                B.Trench = std::max(B.Trench, (1.f - SStep(0.f, 0.35f * W, D)) * Cv);
            B.Ridge = std::max(B.Ridge, (1.f - SStep(0.f, 0.9f * W, D)) * Dv);
            B.Flank = std::max(B.Flank, (1.f - SStep(0.f, 7.0f * W, D)) * Dv);
        }
    }
    return B;
}

Result Compose(const Layers& L, const Boundary& B, double LandScale, const TectonicParams& P)
{
    Result Out;
    const float A  = L.Above;
    const float Mn = std::clamp((L.Mountain + 1.2f) / 3.1f, 0.f, 1.f);
    const float Land = SStep(0.0f, 0.03f, A);
    const float R2 = std::max(B.Range, B.Arc);
    Out.ErosionFade = std::clamp((Mn - 0.5f) / 0.3f, -1.f, 1.f);

    // Relief of the ranges: the broad ridged layer and alpine peaks on top.
    auto Rugged = [&]()
    {
        const float Al = std::clamp((L.Alpine + 1.2f) / 3.1f, 0.f, 1.f);
        return 0.35f * Mn * std::sqrt(Mn) + 0.65f * std::pow(Al, 2.2f);
    };

    if (A >= 0.f)
    {
        const float Old = SStep(0.22f, 0.5f, L.OldRanges);
        const float a01 = std::clamp(A / (float)LandScale, 0.f, 1.6f);
        float Base = P.LowlandHeight + P.InteriorHeight * std::pow(a01, 1.7f)
                   + P.UpliftHeight * L.Uplift * SStep(0.02f, 0.25f, a01);
        Base += P.PlateauHeight * B.Plateau * Land - P.RiftDepth * B.Rift * Land;
        Base = std::max(Base, 3.f + 20.f * a01 - 0.7f * P.RiftDepth * B.Rift);
        const float HillAmt = SStep(-0.1f, 0.35f, L.HillRegime);
        const float Hl = (L.Hills * 0.5f + 0.5f) * P.HillsHeight * (0.15f + 0.85f * std::max(HillAmt, B.Foot));
        const float Peaks     = (R2 > 0.f) ? R2 * (0.20f + 0.80f * Rugged()) * 1.15f : 0.f;
        const float FootPeaks = 0.07f * (Mn * Mn * Mn) * B.Foot * (1.f - R2);
        const float OldPeaks  = P.OldRangeShare * (Mn * Mn) * Old * (1.f - R2);
        const float DetailM   = P.DetailPlains + P.DetailHills * HillAmt + P.DetailMountains * std::max(R2, Old);
        // Ranges reach the coast too (Andes): no shore fade for them.
        Out.Height = (double)Base + (double)Hl + (double)((Land * (FootPeaks + OldPeaks) + Peaks) * P.RangeHeight)
                   + (double)(L.Detail * DetailM);
        Out.Inland = Base / 12000.f;
        Out.ErosionMask = std::max(R2, 0.5f * Old * Land);
        return Out;
    }

    // Ocean: shelf, continental slope, abyssal plain; ridges, trenches, arcs.
    const float d = -A;
    double Depth = P.ShelfDepth * SStep(0.f, P.ShelfWidth, d)
                 + (P.AbyssDepth - P.ShelfDepth) * SStep(P.ShelfWidth, P.ShelfWidth + P.SlopeWidth, d);
    const float Deep = SStep(P.ShelfWidth, 0.10f, d);
    double Floor = -Depth + ((L.Hills * 0.5f + 0.5f) * 300.f + L.Uplift * 1100.f) * Deep + B.Flank * 1300.0 * Deep;
    Floor += B.Ridge * Deep * (P.RidgeHeight - 900.0 * B.Rift) + Mn * 400.0 * B.Ridge * Deep;
    Floor -= B.Trench * Deep * P.TrenchDepth;
    // A range or an arc standing in the sea rises from the floor: islands.
    if (R2 > 0.f) Floor += R2 * (0.25f + 0.75f * Rugged()) * (Depth + 2600.0);
    Out.Height = Floor;
    Out.Inland = A;
    Out.ErosionMask = R2;
    return Out;
}

} // namespace PlanetTectonics

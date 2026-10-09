// PlanetBiomes.cpp
#include "PlanetBiomes.h"
#include <algorithm>
#include <cmath>

namespace PlanetBiomes
{

namespace
{
    inline double Sat(double V) { return std::clamp(V, 0.0, 1.0); }
    inline double SmoothStep(double A, double B, double X)
    {
        const double T = Sat((X - A) / (B - A));
        return T * T * (3.0 - 2.0 * T);
    }
    // Soft membership of X in [Lo, Hi]; edges W wide. Lo/Hi beyond +-1e3 = open.
    inline double Band(double X, double Lo, double Hi, double W)
    {
        const double A = (Lo < -1e3) ? 1.0 : SmoothStep(Lo - W, Lo + W, X);
        const double B = (Hi >  1e3) ? 1.0 : 1.0 - SmoothStep(Hi - W, Hi + W, X);
        return A * B;
    }

    // Layer mix of every biome (rows sum to 1).
    //                                   Sand  DryGr Grass Forest Jungle Tundra Snow  Rock
    const double BiomeLayers[Biome_COUNT][Layer_COUNT] = {
        /* Ice             */ {          0.00, 0.00, 0.00, 0.00,  0.00,  0.00,  1.00, 0.00},
        /* Tundra          */ {          0.00, 0.05, 0.00, 0.00,  0.00,  0.85,  0.05, 0.05},
        /* Taiga           */ {          0.00, 0.00, 0.10, 0.70,  0.00,  0.20,  0.00, 0.00},
        /* TemperateForest */ {          0.00, 0.00, 0.30, 0.70,  0.00,  0.00,  0.00, 0.00},
        /* Grassland       */ {          0.00, 0.30, 0.70, 0.00,  0.00,  0.00,  0.00, 0.00},
        /* Desert          */ {          0.85, 0.05, 0.00, 0.00,  0.00,  0.00,  0.00, 0.10},
        /* Savanna         */ {          0.10, 0.75, 0.15, 0.00,  0.00,  0.00,  0.00, 0.00},
        /* Rainforest      */ {          0.00, 0.00, 0.00, 0.10,  0.90,  0.00,  0.00, 0.00},
        /* Swamp           */ {          0.00, 0.00, 0.20, 0.40,  0.40,  0.00,  0.00, 0.00},
        /* Beach           */ {          1.00, 0.00, 0.00, 0.00,  0.00,  0.00,  0.00, 0.00},
    };
}

Climate ClimateAt(double SinLat, double HeightM, double HumidityRaw, double TempNoise,
                  double Inland, const Params& P)
{
    Climate C;
    const double CosLat = std::sqrt(std::max(0.0, 1.0 - SinLat * SinLat));
    const double Plateau  = std::max(0.0, Inland) * 12000.0;   // continent layer height, m
    const double Altitude = std::max(0.0, HeightM - P.PlateauDiscount * Plateau);
    C.TemperatureC = P.PoleTempC + (P.EquatorTempC - P.PoleTempC) * CosLat
                   - P.LapseCPerKm * Altitude * 0.001
                   + P.TempNoiseC * std::clamp(TempNoise, -1.0, 1.0);

    // Hadley/Ferrel/polar cells: wet at 0 and 60 deg, dry at 30 and 90 deg.
    const double Lat     = std::asin(std::clamp(SinLat, -1.0, 1.0));
    const double Bands   = 0.5 + 0.5 * std::cos(6.0 * Lat);
    const double Noise   = Sat(0.5 + 0.6 * HumidityRaw);
    C.Moisture = Sat(Bands * (1.0 - P.MoistureNoise) + Noise * P.MoistureNoise
                     - P.InlandDryness * Sat(Inland * 12000.0 / 6000.0));
    return C;
}

void BiomeWeights(const Climate& C, double HeightM, double Slope, const Params& P,
                  double Out[Biome_COUNT])
{
    const double T = C.TemperatureC, M = C.Moisture;
    const double WT = 2.5, WM = 0.06;   // edge widths: deg C, moisture

    Out[Biome_Ice]             = Band(T, -1e9, -14.0, WT);
    Out[Biome_Tundra]          = Band(T, -14.0, -3.0, WT);
    Out[Biome_Taiga]           = Band(T, -3.0, 6.0, WT)  * Band(M, 0.30, 1e9, WM);
    Out[Biome_TemperateForest] = Band(T, 6.0, 20.0, WT)  * Band(M, 0.45, 1e9, WM);
    Out[Biome_Grassland]       = Band(T, -3.0, 21.0, WT) * Band(M, 0.15, 0.45, WM) * 0.9
                               + Band(T, -3.0, 6.0, WT)  * Band(M, 0.15, 0.30, WM) * 0.1;
    Out[Biome_Desert]          = Band(T, -3.0, 1e9, WT)  * Band(M, -1e9, 0.17, WM);
    Out[Biome_Savanna]         = Band(T, 20.0, 1e9, WT)  * Band(M, 0.17, 0.55, WM);
    Out[Biome_Rainforest]      = Band(T, 20.0, 1e9, WT)  * Band(M, 0.55, 1e9, WM);

    // Swamp: warm, very wet, low and flat; takes over from the forests there.
    const double Swamp = Band(T, 6.0, 1e9, WT) * SmoothStep(0.70, 0.82, M)
                       * (1.0 - SmoothStep(P.SwampMaxHeightM * 0.5, P.SwampMaxHeightM, HeightM))
                       * (1.0 - SmoothStep(0.01, 0.04, Slope));
    double Sum = 0.0;
    for (int b = 0; b < Biome_Swamp; ++b) { Out[b] *= (1.0 - Swamp); Sum += Out[b]; }
    Out[Biome_Swamp] = Swamp;
    Sum += Swamp;

    // Beach: a thin band just above sea level on gentle slopes (and the sea
    // floor, which is mostly hidden by water).
    const double Beach = (1.0 - SmoothStep(P.BeachHeightM * 0.6, P.BeachHeightM, HeightM))
                       * (1.0 - SmoothStep(0.05, 0.15, Slope) * (HeightM > 0.0 ? 1.0 : 0.0));
    if (Sum < 1e-9) { for (int b = 0; b < Biome_COUNT; ++b) Out[b] = 0.0; Out[Biome_Grassland] = 1.0; Sum = 1.0; }
    for (int b = 0; b < Biome_Beach; ++b) Out[b] = Out[b] / Sum * (1.0 - Beach);
    Out[Biome_Beach] = Beach;
}

void LayerWeights(const Climate& C, double HeightM, double Slope, const Params& P,
                  double Out[Layer_COUNT])
{
    double B[Biome_COUNT];
    BiomeWeights(C, HeightM, Slope, P, B);
    for (int l = 0; l < Layer_COUNT; ++l) Out[l] = 0.0;
    for (int b = 0; b < Biome_COUNT; ++b)
        for (int l = 0; l < Layer_COUNT; ++l)
            Out[l] += B[b] * BiomeLayers[b][l];

    // Cold flats are snow, wherever the biome says otherwise (alpine snow).
    const double Snow = SmoothStep(P.SnowTempC + 2.0, P.SnowTempC - 2.0, C.TemperatureC)
                      * (1.0 - SmoothStep(0.25, 0.45, Slope));
    // Steep slopes are bare rock (snow does not stick either).
    const double Rock = SmoothStep(P.RockSlopeStart, P.RockSlopeFull, Slope);

    for (int l = 0; l < Layer_COUNT; ++l) Out[l] *= (1.0 - Snow);
    Out[Layer_Snow] += Snow;
    for (int l = 0; l < Layer_COUNT; ++l) Out[l] *= (1.0 - Rock);
    Out[Layer_Rock] += Rock;
}

EBiome BiomeAt(const Climate& C, double HeightM, double Slope, const Params& P)
{
    double B[Biome_COUNT];
    BiomeWeights(C, HeightM, Slope, P, B);
    int Best = 0;
    for (int b = 1; b < Biome_COUNT; ++b) if (B[b] > B[Best]) Best = b;
    return (EBiome)Best;
}

} // namespace PlanetBiomes

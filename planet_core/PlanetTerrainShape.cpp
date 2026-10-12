// PlanetTerrainShape.cpp
#include "PlanetTerrainShape.h"
#include <algorithm>
#include <cmath>

namespace PlanetTerrainShape
{

using PlanetCore::ShapeParams;

namespace
{
    inline float SStep(float A, float B, float X)
    {
        const float T = std::clamp((X - A) / (B - A), 0.f, 1.f);
        return T * T * (3.f - 2.f * T);
    }
    inline float Lerp(float A, float B, float T) { return A + (B - A) * T; }
    // |x| without the crease at 0 (hard folds left straight lines in the terrain)
    inline float SoftAbs(float X) { return std::sqrt(X * X + 0.0025f); }
}

Regimes RegimesAt(const Layers& L, double LandScale, const ShapeParams& P)
{
    Regimes R;
    if (L.Above < 0.f) return R;
    R.Land = 1.f;
    const float a = std::clamp(L.Above / (float)LandScale, 0.f, 1.5f);
    R.Coast = SStep(0.f, 0.06f, a);
    // Erosion field: low = rugged, high = flat (as in Minecraft). The amounts
    // shift the thresholds; 0.5 = the tuned values.
    const float M = (P.MountainAmount - 0.5f), H = (P.HillsAmount - 0.5f), T = (P.TableAmount - 0.5f);
    float Mtn  = SStep(-0.55f + M, -1.15f + M, L.Erosion) * (0.35f + 0.65f * R.Coast);
    float Hill = SStep(0.75f + H, 0.05f + H, L.Erosion) * (1.f - Mtn);
    // Table lands among hills and plains, also among mountains (tepuis), not
    // on the flattest plains.
    const float PlateauOn = SStep(0.55f - T, 1.0f - T, L.Plateau);
    const float Tab = PlateauOn * SStep(-1.6f, -0.6f, L.Erosion) * SStep(1.3f, 0.6f, L.Erosion) * R.Coast;
    Mtn *= (1.f - Tab);   // no mountain peaks inside table lands (they poked out of the edges)
    R.Mountain = Mtn; R.Hill = Hill; R.Table = Tab;
    return R;
}

Result Compose(const Layers& L, const Regimes& Rg, double LandScale, const ShapeParams& P)
{
    Result Out;
    const float Hn = L.Hills;
    if (L.Above < 0.f)
    {
        // Ocean: shelf, continental slope, abyssal plain with gentle swells.
        const float d = -L.Above;
        const float Depth = P.ShelfDepth * SStep(0.f, 0.03f, d) + (P.AbyssDepth - P.ShelfDepth) * SStep(0.03f, 0.16f, d);
        Out.Height = -Depth + (Hn * 0.5f) * 120.f * SStep(0.02f, 0.1f, d);
        Out.Inland = L.Above;
        return Out;
    }

    // Peaks & valleys: |weird| folded, -1 valley floor .. +1 peak (Minecraft's PV).
    const float W1 = SoftAbs(L.Weird) / 2.f;
    const float PV = 1.f - SoftAbs(3.f * W1 - 2.f);
    const float Pk = std::clamp((L.Peaks + 1.f) / 2.f, 0.f, 1.f);
    const float a = std::clamp(L.Above / (float)LandScale, 0.f, 1.5f);
    const float Base = P.LowlandHeight + P.InteriorRise * a;
    const float Mtn = Rg.Mountain, Hill = Rg.Hill, Tab = Rg.Table;

    // Plains: almost flat, gentle swells.
    float H = Base + 18.f * Hn;
    // Hills, bigger where PV is high.
    H += Hill * Lerp(P.HillsMinHeight, P.HillsMaxHeight, SStep(-0.5f, 1.f, PV)) * (0.5f + 0.5f * std::tanh(Hn * 0.6f));
    // Mountains: massifs on PV peaks, valleys between them, soft ridged crests.
    if (Mtn > 0.f)
    {
        const float Massif = Lerp(P.MountainMinHeight, P.MountainMaxHeight, SStep(-0.7f, 0.9f, PV));
        H += Mtn * Massif * (0.35f + 0.65f * std::pow(Pk, 1.6f));
    }

    float Flat = 0.f;    // table tops: no detail, no erosion
    float Cliff = 0.f;   // table escarpments: eroded like mountains
    if (Tab > 0.f)
    {
        // 4 levels as a SMOOTH staircase: a hard floor() made height steps
        // along the level contours wherever a table faded in.
        const float Lx = std::clamp(L.Level * 0.9f + 1.5f, 0.f, 3.99f);
        const float Lq = std::floor(Lx) + SStep(0.8f, 1.f, Lx - std::floor(Lx));
        const float Top = P.TableMinHeight + P.TableLevelStep * Lq;
        const float Mn = L.Mass;
        // Escarpment: a talus apron (35 %) under a steep, not vertical cliff
        // (65 %); taller tables get wider escarpments (same steepness).
        const float Wd = 1.f + 0.25f * Lq;
        const float Edge = 0.35f * SStep(-0.45f * Wd, 0.15f, Mn) + 0.65f * SStep(0.05f, 0.05f + 0.5f * Wd, Mn);
        const float TierOn = SStep(1.5f, 2.f, Lq);
        const float Tier = SStep(1.1f * Wd, 1.6f * Wd, Mn) * TierOn;   // wide, rare second tier
        const float Table = Base + Top * Edge + 0.45f * Top * Tier + 25.f * Hn * Edge;
        Flat = Tab * SStep(0.5f * Wd, 0.5f * Wd + 0.12f, Mn) * (1.f - SStep(1.05f * Wd, 1.15f * Wd, Mn) * TierOn);
        // Erosion on the escarpment, fading out before the rim: gully ridges
        // there would rise above the flat top as spikes.
        Cliff = Tab * SStep(-0.35f * Wd, -0.05f, Mn) * (1.f - SStep(0.25f * Wd, 0.45f * Wd, Mn));
        // Where a table stands it replaces the terrain; around it the terrain goes on.
        const float Under = std::min(H, Base + Top * 0.6f);
        H = Lerp(H, std::max(Under, Table), Tab);
    }

    const float DetailM = (P.DetailPlains + P.DetailHills * Hill + P.DetailMountains * Mtn + P.DetailCliffs * Cliff)
                        * (1.f - 0.7f * Flat);
    H += L.Detail * DetailM;

    Out.Height = H;
    Out.Inland = Base / 12000.f;
    // Erosion: mountains and escarpments; not on flat tops, not on valley
    // floors, ramped in above the waterline.
    Out.ErosionMask = std::clamp(Mtn * 1.4f + Cliff, 0.f, 1.f) * (1.f - Flat)
                    * std::clamp((H - Base) / 400.f, 0.f, 1.f) * std::clamp(H / 300.f, 0.f, 1.f);
    // Small fade: with +-1 the filter digs ~1 km pits into every valley.
    Out.ErosionFade = 0.3f * std::clamp((Pk - 0.5f) / 0.3f, -1.f, 1.f) * Mtn;
    return Out;
}

} // namespace PlanetTerrainShape

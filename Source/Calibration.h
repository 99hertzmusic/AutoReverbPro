#pragma once
// Engine RT60 needed so the measured tail (-60 dB point of the T30 fit, 500 Hz-2 kHz) ends exactly
// `tail` seconds after the (pre-delayed) hit. Measured per space by `AutoReverbProTest calib-table`
// at SIZE 50 %, then pasted here. Before a table exists, a simple model is used.
#include <cmath>
#include <algorithm>

namespace ar
{
inline constexpr int numCalib = 21;
inline constexpr float calibTail[numCalib] = { 0.06f, 0.08f, 0.10f, 0.125f, 0.16f, 0.20f, 0.25f, 0.32f, 0.40f, 0.50f, 0.63f,
                                               0.80f, 1.0f, 1.25f, 1.6f, 2.0f, 2.5f, 3.2f, 4.0f, 5.0f, 6.4f };
// rows: ROOM, PLATE, HALL, CHAMBER, SPRING, SHIMMER (0 = no table yet)
inline constexpr float calibRt[6][numCalib] = {   // generated 24 Nov 2026 (shelf damping, v1.0 voicing)
    { 0.0471f, 0.0687f, 0.0806f, 0.1046f, 0.1429f, 0.1869f, 0.2374f, 0.3123f, 0.3948f, 0.5022f, 0.6409f, 0.8209f, 1.0228f, 1.2828f, 1.6533f, 2.0871f, 2.6061f, 3.3342f, 4.1868f, 5.2497f, 6.7169f },   // ROOM
    { 0.0450f, 0.0621f, 0.0808f, 0.1035f, 0.1425f, 0.1845f, 0.2430f, 0.3170f, 0.3979f, 0.5045f, 0.6434f, 0.8181f, 1.0282f, 1.2951f, 1.6555f, 2.0778f, 2.5978f, 3.3388f, 4.1722f, 5.2177f, 6.6849f },   // PLATE
    { 0.0319f, 0.0540f, 0.0753f, 0.1014f, 0.1322f, 0.1712f, 0.2202f, 0.2842f, 0.3637f, 0.4799f, 0.6328f, 0.8256f, 1.0529f, 1.3439f, 1.7486f, 2.2089f, 2.7535f, 3.5231f, 4.4078f, 5.5152f, 7.0984f },   // HALL
    { 0.0399f, 0.0610f, 0.0807f, 0.0997f, 0.1350f, 0.1692f, 0.2216f, 0.2995f, 0.3870f, 0.5027f, 0.6424f, 0.8209f, 1.0461f, 1.3260f, 1.7072f, 2.1204f, 2.6450f, 3.4099f, 4.2833f, 5.3482f, 6.8225f },   // CHAMBER
    { 0.0271f, 0.0507f, 0.0645f, 0.0903f, 0.1156f, 0.1515f, 0.1953f, 0.2516f, 0.3308f, 0.4309f, 0.5536f, 0.7456f, 0.9546f, 1.2173f, 1.5902f, 2.0242f, 2.5577f, 3.3149f, 4.1542f, 5.2092f, 6.6940f },   // SPRING
    { 0.0259f, 0.0518f, 0.0714f, 0.0917f, 0.1270f, 0.1643f, 0.2091f, 0.2687f, 0.3435f, 0.4530f, 0.5914f, 0.7692f, 0.9799f, 1.2484f, 1.6193f, 2.0434f, 2.5519f, 3.2797f, 4.0953f, 5.1161f, 6.5785f },   // SHIMMER
};

inline float rtForTail (int space, float tail, float diffMs)
{
    tail = std::max (0.02f, tail);
    if (space >= 0 && space < 6 && calibRt[space][0] > 0.0f)
    {
        const float* rt = calibRt[space];
        if (tail <= calibTail[0]) return rt[0] * tail / calibTail[0];
        if (tail >= calibTail[numCalib - 1]) return rt[numCalib - 1] * tail / calibTail[numCalib - 1];
        int i = 0; while (tail > calibTail[i + 1]) ++i;
        const float u = std::log (tail / calibTail[i]) / std::log (calibTail[i + 1] / calibTail[i]);
        return std::exp (std::log (rt[i]) + u * (std::log (rt[i + 1]) - std::log (rt[i])));
    }
    return std::max (0.05f, tail - 0.0005f * diffMs - 0.03f * std::exp (-tail / 0.8f));
}
} // namespace ar

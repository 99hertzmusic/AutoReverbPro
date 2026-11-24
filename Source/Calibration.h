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
inline constexpr float calibRt[6][numCalib] = {   // generated 24 Nov 2026
    { 0.0487f, 0.0728f, 0.0882f, 0.1100f, 0.1441f, 0.1856f, 0.2343f, 0.3071f, 0.3868f, 0.4903f, 0.6236f, 0.7974f, 0.9927f, 1.2441f, 1.6024f, 2.0217f, 2.5238f, 3.2262f, 4.0484f, 5.0731f, 6.4924f },   // ROOM
    { 0.0426f, 0.0614f, 0.0814f, 0.1031f, 0.1383f, 0.1803f, 0.2321f, 0.3061f, 0.3847f, 0.4872f, 0.6178f, 0.7918f, 0.9931f, 1.2459f, 1.6029f, 2.0034f, 2.5192f, 3.2278f, 4.0202f, 5.0269f, 6.4530f },   // PLATE
    { 0.0338f, 0.0576f, 0.0789f, 0.1015f, 0.1390f, 0.1810f, 0.2308f, 0.2974f, 0.3808f, 0.4752f, 0.6115f, 0.7874f, 0.9871f, 1.2485f, 1.6089f, 2.0116f, 2.5128f, 3.2255f, 4.0371f, 5.0376f, 6.4742f },   // HALL
    { 0.0423f, 0.0634f, 0.0801f, 0.0996f, 0.1388f, 0.1742f, 0.2201f, 0.2910f, 0.3699f, 0.4751f, 0.6073f, 0.7881f, 0.9905f, 1.2469f, 1.6024f, 1.9957f, 2.4910f, 3.2089f, 4.0258f, 5.0234f, 6.4487f },   // CHAMBER
    { 0.0275f, 0.0510f, 0.0646f, 0.0903f, 0.1157f, 0.1515f, 0.1897f, 0.2483f, 0.3256f, 0.4235f, 0.5425f, 0.7222f, 0.9217f, 1.1728f, 1.5297f, 1.9452f, 2.4539f, 3.1768f, 3.9830f, 4.9908f, 6.4050f },   // SPRING
    { 0.0262f, 0.0519f, 0.0721f, 0.0928f, 0.1279f, 0.1635f, 0.2126f, 0.2716f, 0.3468f, 0.4504f, 0.5834f, 0.7552f, 0.9584f, 1.2173f, 1.5741f, 1.9822f, 2.4744f, 3.1756f, 3.9641f, 4.9513f, 6.3641f },   // SHIMMER
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

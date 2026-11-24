#pragma once
// AutoReverb Pro: the reverb engine (99Hertz's own DSP, no IRs, no samples).
//
//   input -> low cut -> [spring drip loop] -> 16-channel diffuser (4 stages: delays, shuffle, Hadamard)
//         -> 16-line feedback delay network (Householder matrix, modulated cubic reads, per-line decay filters)
//         -> orthogonal L/R taps (+ the diffuser as early sound) -> tone high cut
//   SHIMMER adds an octave-up pitch shifter inside the loop.
//
// Decay is exact by construction: every line loses 60 dB per RT60 seconds below dampHz, and above it
// faster (RT60 * hfRatio), set by a passive one-pole shelf whose two gains are solved directly.
#include <cmath>
#include <vector>
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <bitset>
#include "Features.h"

namespace ar
{
inline constexpr int N = 16;
inline constexpr int numDiffStages = 4;
inline constexpr float pi = 3.14159265358979f;
inline constexpr float wetMakeupDb = 4.0f;   // all spaces: MIX 100 % sits near the dry's loudness, like the big-name reverbs

// How each space is voiced. Times in ms at SIZE 50 %.
struct Voice
{
    float lineMinMs, lineMaxMs;   // FDN line lengths (spread exponentially between these)
    float diffMs;                 // diffuser length (all 4 stages)
    float erGain;                 // diffuser sent straight out (early sound)
    float modMs, modHz;           // delay modulation depth / rate (chorus in the tail, kills metallic modes)
    float hfRatio;                // RT60 above dampHz / RT60 below it, at TONE 50 %
    float dampHz;                 // where the tail's high-frequency damping starts (air / plate absorption)
    float outLpHz;                // output high cut at TONE 50 %
    float lowCutHz;               // built-in low cut on the reverb (24 dB/oct)
    float outGainDb;              // level match between spaces
    float autoSec;                // AUTO length (tail -60 dB) at SIZE 50 %, snapped to the nearest beat step; 0 = autoDecay
    int   autoDecay;              // GATED / REVERSE AUTO length: index into decayStepsBeats
    int   autoPre;                // AUTO pre-delay: index into preStepsBeats
    float rtMul;                  // GATED / REVERSE: engine RT60 = rtMul * gate length
    float shimmer;                // octave-up feedback amount
    float spring;                 // spring drip amount
    int   shape;                  // 0 natural decay, 1 gated, 2 reverse swell
};

inline const Voice& voice (int s)
{
    // AUTO lengths follow what engineers reach for: rooms ~1 s, plates ~2 s, halls ~3 s, shimmer washes ~5 s.
    static const Voice v[numSpaces] = {
        //  min   max  diff   er    mod   hz    hf    damp   lp     lowcut gain  aSec aDec aPre rtMul shim  spring shape
        {   7,   34,  12, 0.55f, 0.12f, 0.45f, 0.55f, 4500,  9000, 140, 2.5f, 1.1f, 2, 1, 1, 0.0f, 0.0f, 0 },   // ROOM
        {   9,   40,  16, 0.45f, 0.18f, 0.80f, 0.50f, 5000,  9500, 160, 0.0f, 1.9f, 3, 1, 1, 0.0f, 0.0f, 0 },   // PLATE
        {  20,  105,  32, 0.55f, 0.30f, 0.33f, 0.40f, 3500,  8000, 160, 0.0f, 3.2f, 4, 1, 1, 0.0f, 0.0f, 0 },   // HALL
        {  14,   60,  22, 0.45f, 0.18f, 0.50f, 0.45f, 4000,  8000, 150, 1.5f, 1.6f, 3, 1, 1, 0.0f, 0.0f, 0 },   // CHAMBER
        {  14,   48,  14, 0.15f, 0.18f, 0.60f, 0.55f, 4000,  4800, 200, 0.5f, 2.0f, 3, 1, 1, 0.0f, 0.6f, 0 },   // SPRING
        {  22,  115,  34, 0.25f, 0.55f, 0.30f, 0.50f, 5000, 10000, 200, 0.0f, 3.8f, 5, 2, 1, 0.5f, 0.0f, 0 },   // SHIMMER
        {   8,   36,  12, 0.50f, 0.15f, 0.50f, 0.60f, 5000, 10000, 160, 3.0f, 0.0f, 1, 0, 3, 0.0f, 0.0f, 1 },   // GATED    1/2 beat
        {  14,   66,  24, 0.00f, 0.30f, 0.50f, 0.55f, 5000, 10000, 160, 9.0f, 0.0f, 2, 0, 4, 0.0f, 0.0f, 2 },   // REVERSE  1 beat
    };
    return v[s >= 0 && s < numSpaces ? s : 0];
}

// Shimmer needs room to bloom: none below ~0.12 s of tail, full from ~0.6 s.
inline float shimmerAmount (float rtSec) { return std::min (1.0f, std::max (0.0f, (rtSec - 0.12f) / 0.5f)); }

// Deterministic: two instances with the same settings always sound identical.
struct Rng
{
    uint32_t s;
    explicit Rng (uint32_t seed) : s (seed) {}
    float next() { s = s * 1664525u + 1013904223u; return (float) (s >> 8) * (1.0f / 16777216.0f); }
};

inline void hadamard (float* x)
{
    for (int h = 1; h < N; h <<= 1)
        for (int i = 0; i < N; i += h * 2)
            for (int j = i; j < i + h; ++j) { const float a = x[j], b = x[j + h]; x[j] = a + b; x[j + h] = a - b; }
    for (int i = 0; i < N; ++i) x[i] *= 0.25f;   // 1/sqrt(16): orthonormal
}

inline void householder (float* x)
{
    float s = 0.0f;
    for (int i = 0; i < N; ++i) s += x[i];
    s *= -2.0f / (float) N;
    for (int i = 0; i < N; ++i) x[i] += s;
}

struct Line
{
    std::vector<float> buf; int mask = 0, w = 0;
    void prepare (int maxDelay)
    {
        int sz = 16; while (sz < maxDelay + 8) sz <<= 1;
        buf.assign ((size_t) sz, 0.0f); mask = sz - 1; w = 0;
    }
    void clear() { std::fill (buf.begin(), buf.end(), 0.0f); w = 0; }
    inline void push (float x) { buf[(size_t) w] = x; w = (w + 1) & mask; }
    inline float tap (int d) const { return buf[(size_t) ((w - 1 - d) & mask)]; }   // d = 0: the newest sample
    // 4-point Lagrange (passive inside the centre interval: never adds energy, so FREEZE can't run away)
    inline float tapFrac (float d) const
    {
        const int i = (int) d; const float f = d - (float) i;
        const float xm1 = tap (i - 1), x0 = tap (i), x1 = tap (i + 1), x2 = tap (i + 2);
        const float fm1 = f + 1.0f, f1 = f - 1.0f, f2 = f - 2.0f;
        return -f * f1 * f2 * (1.0f / 6.0f) * xm1 + fm1 * f1 * f2 * 0.5f * x0
               - fm1 * f * f2 * 0.5f * x1 + fm1 * f * f1 * (1.0f / 6.0f) * x2;
    }
    inline float tapLin (float d) const
    {
        const int i = (int) d; const float f = d - (float) i;
        return tap (i) + (tap (i + 1) - tap (i)) * f;
    }
};

// Topology-preserving state variable filter (safe to sweep every few samples).
struct Svf
{
    float a1 = 1, a2 = 0, a3 = 0, k = 1.414f, ic1 = 0, ic2 = 0;
    void set (float fc, float q, float fs)
    {
        const float g = std::tan (pi * std::min (fc, 0.45f * fs) / fs);
        k = 1.0f / q; a1 = 1.0f / (1.0f + g * (g + k)); a2 = g * a1; a3 = g * a2;
    }
    void reset() { ic1 = ic2 = 0.0f; }
    inline float lp (float v0) { float l, h; tick (v0, l, h); return l; }
    inline float hp (float v0) { float l, h; tick (v0, l, h); return h; }
    inline void tick (float v0, float& low, float& high)
    {
        const float v3 = v0 - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1; ic2 = 2.0f * v2 - ic2;
        low = v2; high = v0 - k * v1 - v2;
    }
};

class Engine
{
public:
    struct Targets
    {
        float rtSec = 1.0f, hfRatio = 0.5f, outLpHz = 10000.0f, sizeScale = 1.0f;
        float freeze = 0.0f;     // 0..1
        float input = 1.0f;      // 0 = stop feeding (space change / release)
    };
    Targets t;
    int space = -1;

    static constexpr float maxLineMs = 125.0f, maxScale = 1.35f, maxDiffStageMs = 20.0f;

    void prepare (double sampleRate)
    {
        fs = (float) sampleRate;
        const int lineMax = (int) std::ceil (maxLineMs * maxScale * 0.001f * fs + 2.0f * fs * 0.001f) + 8;
        for (auto& l : fdn) l.prepare (lineMax);
        for (auto& st : diff) for (auto& l : st.line) l.prepare ((int) (maxDiffStageMs * 0.001f * fs) + 4);
        shBuf.prepare ((int) (0.2f * fs));
        spLine.prepare ((int) (0.06f * fs));
        spK = std::max (1, (int) std::lround (8.0f * fs / 48000.0f));
        for (auto& a : spAp) a.prepare (spK);
        space = -1;
    }

    // Load a space and start from silence. Clears only what's in use (bounded, ~0.1 ms at 48 kHz).
    void configure (int sp, float sizeScale, bool rampIn = true)
    {
        space = sp;
        const auto& v = voice (sp);
        Rng rng (0x99a7u + (uint32_t) sp * 7919u);
        const float msToS = 0.001f * fs;

        // FDN line lengths: exponential spread with jitter, order shuffled so neighbours differ.
        std::array<int, N> order; for (int i = 0; i < N; ++i) order[(size_t) i] = i;
        for (int i = N - 1; i > 0; --i) std::swap (order[(size_t) i], order[(size_t) (rng.next() * (float) (i + 1)) % (size_t) (i + 1)]);
        for (int i = 0; i < N; ++i)
        {
            const float u = ((float) order[(size_t) i] + 0.15f + 0.7f * rng.next()) / (float) N;
            base[i] = v.lineMinMs * std::pow (v.lineMaxMs / v.lineMinMs, u) * msToS;
            lenCur[i] = base[i] * sizeScale;
            const float ph = 2.0f * pi * rng.next();
            mc[i] = std::cos (ph); ms[i] = std::sin (ph);
            const float w = 2.0f * pi * v.modHz * (0.6f + 0.8f * rng.next()) / fs;
            mcw[i] = std::cos (w); msw[i] = std::sin (w);
            lp[i] = 0.0f;
        }
        modDepth = v.modMs * msToS;

        // Diffuser: each stage's delays are spread evenly over its range, shuffled, random polarity.
        // 8:4:2:1 ranges: the sum of the four delays spreads evenly over the whole span (no bunching).
        const float stageFrac[numDiffStages] = { 8.0f / 15.0f, 4.0f / 15.0f, 2.0f / 15.0f, 1.0f / 15.0f };
        for (int s = 0; s < numDiffStages; ++s)
        {
            auto& st = diff[s];
            const float range = std::min (maxDiffStageMs, v.diffMs * stageFrac[s]) * msToS;
            for (int i = 0; i < N; ++i)
            {
                st.d[i] = std::max (0, (int) (range * ((float) i + rng.next()) / (float) N));
                st.perm[i] = i;
                st.sign[i] = rng.next() < 0.5f ? -1.0f : 1.0f;
            }
            for (int i = N - 1; i > 0; --i) std::swap (st.d[i], st.d[(int) (rng.next() * (float) (i + 1)) % (i + 1)]);
            for (int i = N - 1; i > 0; --i) std::swap (st.perm[i], st.perm[(int) (rng.next() * (float) (i + 1)) % (i + 1)]);
        }
        for (int i = 0; i < N; ++i)
        {
            inSign[i] = rng.next() < 0.5f ? -1.0f : 1.0f;
            shSign[i] = rng.next() < 0.5f ? -1.0f : 1.0f;
            // Output taps: two orthogonal Hadamard rows -> L and R share no common component.
            oL[i] = (std::bitset<32> ((unsigned) (i & 5)).count() & 1) ? -0.25f : 0.25f;
            oR[i] = (std::bitset<32> ((unsigned) (i & 10)).count() & 1) ? -0.25f : 0.25f;
        }

        for (int c = 0; c < 2; ++c)
        {
            lowCut[c][0].set (v.lowCutHz, 0.5412f, fs); lowCut[c][1].set (v.lowCutHz, 1.3066f, fs);
            lowCut[c][0].reset(); lowCut[c][1].reset();
            tone[c].reset();
        }
        lpCur = -1.0f;
        shPhase = 0.0f; shW = 0.09f * fs;
        shHp.set (380.0f, 0.707f, fs); shLp.set (6500.0f, 0.707f, fs); shHp.reset(); shLp.reset();
        shGain = v.shimmer * 0.42f;
        spAmt = v.spring; spLast = 0.0f; spD = 0.029f * fs;
        spLp.set (4200.0f, 0.707f, fs); spLp.reset();

        for (auto& l : fdn) l.clear();
        for (auto& st : diff) for (auto& l : st.line) l.clear();
        shBuf.clear(); spLine.clear();
        for (auto& a : spAp) a.clear();

        rtLog = std::log (std::max (0.01f, t.rtSec));
        frz = t.freeze; inGain = 0.0f; counter = 0;
        hfCur = t.hfRatio;
        update (true);
        if (! rampIn) inGain = inTarget;   // after prepareToPlay the lines are empty: no need to fade the input in
    }

    // One stereo sample in (already pre-delayed), wet stereo out.
    inline void process (float inL, float inR, float& outL, float& outR)
    {
        if (--counter <= 0) update (false);

        const float ig = inGain;
        inGain += (inTarget - inGain) * inK;

        // Built-in low cut (24 dB/oct) on what goes into the reverb.
        float l = lowCut[0][1].hp (lowCut[0][0].hp (inL));
        float r = lowCut[1][1].hp (lowCut[1][0].hp (inR));

        if (spAmt > 0.0f)
        {
            // Spring drip: a dispersive (stretched all-pass) loop -> repeating chirps.
            const float m = 0.5f * (l + r);
            spLine.push (m + spFb * spLast);
            float x = spLp.lp (spLine.tapLin (spDCur));
            spDCur += (spDFit - spDCur) * 0.001f;
            for (auto& a : spAp) x = a.process (x, -0.62f, spK);
            spLast = x;
            l = l * (1.0f - spAmt) + x * spAmt * 1.6f;
            r = r * (1.0f - spAmt) - x * spAmt * 1.6f;
        }

        float x[N];
        for (int i = 0; i < N; ++i) x[i] = ((i & 1) ? r : l) * inSign[i] * 0.3536f * ig;

        for (auto& st : diff)
        {
            float tmp[N];
            for (int i = 0; i < N; ++i) { st.line[i].push (x[i]); tmp[i] = st.line[i].tap (st.d[i]); }
            for (int i = 0; i < N; ++i) x[i] = tmp[st.perm[i]] * st.sign[i];
            hadamard (x);
        }

        float y[N], z[N];
        for (int i = 0; i < N; ++i)
        {
            lenCur[i] += (lenTarget[i] - lenCur[i]) * sizeK;
            const float c = mc[i], s = ms[i];
            mc[i] = c * mcw[i] - s * msw[i]; ms[i] = s * mcw[i] + c * msw[i];
            y[i] = fdn[i].tapFrac (std::max (2.0f, lenCur[i] + modDepth * s - 1.0f));
            lp[i] += dampC * (y[i] - lp[i]);                  // split at dampHz: lows decay at RT60, highs faster
            z[i] = gLo[i] * lp[i] + gHi[i] * (y[i] - lp[i]);
        }
        householder (z);

        float sh = 0.0f;
        if (shGain > 0.0f)
        {
            // Octave-up pitch shifter: two taps sweeping through a 90 ms window, sin^2 crossfade.
            float m = 0.0f; for (int i = 0; i < N; ++i) m += y[i] * shSign[i];
            shBuf.push (m * 0.25f);
            shPhase -= 1.0f / shW; if (shPhase < 0.0f) shPhase += 1.0f;
            float p2 = shPhase + 0.5f; if (p2 >= 1.0f) p2 -= 1.0f;
            const float w1 = std::sin (pi * shPhase);
            const float a = shBuf.tapLin (2.0f + shPhase * shW), b = shBuf.tapLin (2.0f + p2 * shW);
            sh = shLp.lp (shHp.hp (a * w1 * w1 + b * (1.0f - w1 * w1)));
            sh *= shGain * shFit * (1.0f - frz) * 0.25f;
        }

        for (int i = 0; i < N; ++i) fdn[i].push (x[i] + z[i] + sh * shSign[i]);

        float oLv = 0.0f, oRv = 0.0f;
        for (int i = 0; i < N; ++i)
        {
            const float v = y[i] + erGain * x[i];
            oLv += oL[i] * v; oRv += oR[i] * v;
        }
        outL = tone[0].lp (oLv) * outGain;
        outR = tone[1].lp (oRv) * outGain;
    }

    float inTarget = 1.0f;
    void snap() { update (true); }   // jump straight to the targets (first block after prepare)

private:
    struct Stage { std::array<Line, N> line; int d[N] {}; int perm[N] {}; float sign[N] {}; };
    struct StretchedAllpass
    {
        std::vector<float> b; int mask = 0, w = 0;
        void prepare (int k) { int sz = 4; while (sz < k + 2) sz <<= 1; b.assign ((size_t) sz, 0.0f); mask = sz - 1; w = 0; }
        void clear() { std::fill (b.begin(), b.end(), 0.0f); w = 0; }
        inline float process (float x, float a, int k)
        {
            const float vk = b[(size_t) ((w - k) & mask)];
            const float v = x - a * vk;
            b[(size_t) w] = v; w = (w + 1) & mask;
            return a * v + vk;
        }
    };

    void update (bool snap)
    {
        counter = 16;
        const float k16 = 1.0f - std::exp (-16.0f / (0.06f * fs));   // ~60 ms glide
        const float rtT = std::log (std::max (0.01f, t.rtSec));
        rtLog = snap ? rtT : rtLog + (rtT - rtLog) * k16;
        frz = snap ? t.freeze : frz + (t.freeze - frz) * k16;
        if (t.freeze >= 1.0f && frz > 0.999f) frz = 1.0f;
        if (t.freeze <= 0.0f && frz < 0.001f) frz = 0.0f;
        hfCur = snap ? t.hfRatio : hfCur + (t.hfRatio - hfCur) * k16;
        const float rt = std::exp (rtLog);
        for (int i = 0; i < N; ++i)
        {
            lenTarget[i] = base[i] * t.sizeScale;
            if (snap) lenCur[i] = lenTarget[i];
            float g  = std::exp (-6.9078f * lenCur[i] / (rt * fs));
            float gn = std::exp (-6.9078f * lenCur[i] / (rt * hfCur * fs));
            g += (1.0f - g) * frz; gn += (1.0f - gn) * frz;
            gLo[i] = g; gHi[i] = gn;
        }
        spFb = std::min (0.55f, std::exp (-6.9078f * 0.05f / rt));   // the drip never outlasts the tail
        spDFit = std::max (8.0f, std::min (spD, 0.12f * rt * fs));     // short tails: a shorter spring
        if (snap) spDCur = spDFit;
        shFit = shimmerAmount (rt);
        const float lpT = std::min (t.outLpHz, 0.45f * fs);
        lpCur = (snap || lpCur < 0.0f) ? lpT : lpCur * std::pow (lpT / lpCur, k16);
        tone[0].set (lpCur, 0.6f, fs); tone[1].set (lpCur, 0.6f, fs);
        inTarget = t.input * (1.0f - frz);
        const auto& v = voice (space);
        erGain = v.erGain;
        dampC = 1.0f - std::exp (-2.0f * pi * std::min (v.dampHz, 0.4f * fs) / fs);
        outGain = std::pow (10.0f, (v.outGainDb + wetMakeupDb) / 20.0f);
        inK = 1.0f - std::exp (-1.0f / (0.012f * fs));
        sizeK = 1.0f - std::exp (-1.0f / (0.4f * fs));   // slow glide: no zipper / Doppler grit when SIZE jumps
    }

    float fs = 48000.0f;
    std::array<Line, N> fdn;
    std::array<Stage, numDiffStages> diff;
    float base[N] {}, lenCur[N] {}, lenTarget[N] {}, mc[N] {}, ms[N] {}, mcw[N] {}, msw[N] {};
    float lp[N] {}, gLo[N] {}, gHi[N] {}, inSign[N] {}, shSign[N] {}, oL[N] {}, oR[N] {};
    float dampC = 0.3f, modDepth = 0.0f, rtLog = 0.0f, frz = 0.0f, hfCur = 0.5f, lpCur = -1.0f, erGain = 0.0f, outGain = 1.0f;
    float inGain = 0.0f, inK = 0.001f, sizeK = 0.0001f;
    int counter = 0;
    Svf lowCut[2][2], tone[2];
    // shimmer
    Line shBuf; float shPhase = 0.0f, shW = 4000.0f, shGain = 0.0f; Svf shHp, shLp;
    // spring
    Line spLine; std::array<StretchedAllpass, 40> spAp; int spK = 8; float spAmt = 0.0f, spLast = 0.0f, spFb = 0.5f, spDFit = 1000.0f, spDCur = 1000.0f, shFit = 1.0f, spD = 1000.0f; Svf spLp;
};
} // namespace ar

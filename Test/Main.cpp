// AutoReverb Pro offline test app.
// Usage: AutoReverbProTest <outdir> [name=key ...]
//        AutoReverbProTest demos <outdir> <bpm> file.wav ...   (every space on each file)
//        AutoReverbProTest levels file.wav <bpm>               (wet loudness of each space)
//        AutoReverbProTest calib-table                         (re-measure Source/Calibration.h)
//        AutoReverbProTest duck                                (only the ducking tests)
//  - the tail is 60 dB down on the chosen beat (EDC / T30 fit, 500 Hz-2 kHz) at several tempos, Lite AUTO too
//  - AUTO picks a musical length per space (room ~1 s, plate ~2 s, hall ~3 s ...) snapped to the nearest beat step
//  - pre-delay exact, echo density builds fast (normalised echo density), no ringing modes,
//    stereo decorrelation, low cut, ducking depth during input vs gaps, GATED / REVERSE end on the beat
//  - freeze stable, shimmer never runs away, no clicks on any change, CPU per block
//  - save/restore, Lite bit-identical with Pro params forced, licence keys, UI snapshots
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"
#include "../Source/Calibration.h"
#include "LicenceTests.h"
#include <cmath>
#include <chrono>
#include <complex>

using APP = AutoReverbProAudioProcessor;
static constexpr double SR = 48000.0;
static constexpr int BS = 256;
static int failures = 0;
static void check (bool ok, const juce::String& what)
{
    std::printf ("  [%s] %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8());
    if (! ok) ++failures;
}
using Buf = juce::AudioBuffer<float>;
static const double twoPi = juce::MathConstants<double>::twoPi;

// ── Host stand-in ────────────────────────────────────────────────────────────
struct TestHead : juce::AudioPlayHead
{
    double bpm = 120.0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p; p.setBpm (bpm); p.setIsPlaying (true); return p;
    }
};

static void setParam (APP& p, const char* id, float plain)
{
    auto* prm = p.apvts.getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (plain));
}

static void setLic (APP& p, int kind)
{
    hz::LicenceInfo li; li.kind = (hz::LicenceInfo::Kind) kind; li.email = "buyer@example.com";
    li.expires = kind == hz::LicenceInfo::trial ? juce::Time::currentTimeMillis() / 1000 + 5 * 86400 - 60 : 0;
    p.setLicenceInfo (li);
}

struct Rig
{
    std::unique_ptr<APP> p = std::make_unique<APP>();
    TestHead head;
    Rig (int lic, double bpm, double fs = SR)
    {
        setLic (*p, lic);
        head.bpm = bpm;
        p->setPlayHead (&head);
        p->setRateAndBufferSizeDetails (fs, BS);
        p->prepareToPlay (fs, BS);
    }
    void reprepare (double fs = SR) { p->prepareToPlay (fs, BS); }
};

static Buf run (APP& p, const Buf& in, int len = -1, int offset = 0, bool loop = true)
{
    if (len < 0) len = in.getNumSamples();
    Buf out (2, len), blk (2, BS);
    juce::MidiBuffer m;
    for (int pos = 0; pos < len; pos += BS)
    {
        const int n = juce::jmin (BS, len - pos);
        blk.setSize (2, n, false, false, true);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i)
            {
                const int k = offset + pos + i;
                blk.setSample (c, i, loop ? in.getSample (c, k % in.getNumSamples()) : (k < in.getNumSamples() ? in.getSample (c, k) : 0.0f));
            }
        p.processBlock (blk, m);
        for (int c = 0; c < 2; ++c) out.copyFrom (c, pos, blk, c, 0, n);
    }
    return out;
}

static Buf impulse (int len) { Buf b (2, len); b.clear(); b.setSample (0, 0, 1.0f); b.setSample (1, 0, 1.0f); return b; }

// Wet-only impulse response (MIX 100 %, ducking off, Pro).
static Buf ir (int space, double bpm, int decayParam, int preParam, double secs, int lic = hz::LicenceInfo::pro, float size = 50.0f)
{
    Rig r (lic, bpm);
    setParam (*r.p, "space", (float) space); setParam (*r.p, "mix", 100.0f); setParam (*r.p, "duck", 0.0f);
    setParam (*r.p, "decay", (float) decayParam); setParam (*r.p, "predelay", (float) preParam); setParam (*r.p, "size", size);
    r.reprepare();
    const int len = (int) (secs * SR);
    return run (*r.p, impulse (len), len, 0, false);
}

static void writeWav (const juce::File& f, const Buf& b, double rate = SR)
{
    f.deleteFile();
    juce::WavAudioFormat w;
    if (auto os = std::unique_ptr<juce::OutputStream> (f.createOutputStream().release()))
        if (auto wr = std::unique_ptr<juce::AudioFormatWriter> (w.createWriterFor (os.get(), rate, (unsigned int) b.getNumChannels(), 24, {}, 0)))
        { os.release(); wr->writeFromAudioSampleBuffer (b, 0, b.getNumSamples()); }
}

// Band-pass (500 Hz - 2 kHz) energy of L+R per sample.
static std::vector<double> bandEnergy (const Buf& b, double fs = SR)
{
    std::vector<double> e ((size_t) b.getNumSamples(), 0.0);
    for (int c = 0; c < 2; ++c)
    {
        juce::dsp::IIR::Filter<float> h1, h2, l1, l2;
        h1.coefficients = h2.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighPass (fs, 500.0f);
        l1.coefficients = l2.coefficients = juce::dsp::IIR::Coefficients<float>::makeLowPass (fs, 2000.0f);
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const float y = l2.processSample (l1.processSample (h2.processSample (h1.processSample (b.getSample (c, i)))));
            e[(size_t) i] += (double) y * y;
        }
    }
    return e;
}

// End of the tail: the T30 line (EDC -5 .. -35 dB) extrapolated to -60 dB, in seconds after the hit.
struct DecayFit { double endSec, t30Sec; bool ok; };
static DecayFit fitEnd (const std::vector<double>& e, double fs = SR)
{
    std::vector<double> edc (e.size());
    double acc = 0; for (size_t i = e.size(); i-- > 0;) { acc += e[i]; edc[i] = acc; }
    const double tot = edc[0];
    if (tot <= 0) return { 0, 0, false };
    double sx = 0, sy = 0, sxx = 0, sxy = 0; int n = 0;
    for (size_t i = 0; i < edc.size(); i += 8)
    {
        const double db = 10.0 * std::log10 (edc[i] / tot + 1e-30);
        if (db > -5.0) continue;
        if (db < -35.0) break;
        const double t = (double) i / fs;
        sx += t; sy += db; sxx += t * t; sxy += t * db; ++n;
    }
    if (n < 4) return { 0, 0, false };
    const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx), icpt = (sy - slope * sx) / n;
    return { (-60.0 - icpt) / slope, -60.0 / slope, slope < 0 };
}

// ── Decay on the beat ────────────────────────────────────────────────────────
static void decayTests()
{
    std::printf ("\n== The tail ends on the beat (EDC T30 fit, 500 Hz-2 kHz, -60 dB point after the hit, pre-delay AUTO) ==\n");
    std::printf ("%-8s %6s %8s %10s %10s %8s\n", "space", "BPM", "beats", "target ms", "end ms", "error");
    double worst = 0;
    for (int s : { ar::room, ar::plate, ar::hall, ar::chamber, ar::spring, ar::shimmer })
        for (double bpm : { 90.0, 120.0, 140.0, 174.0 })
            for (int step = 0; step < ar::numDecaySteps; ++step)   // 1/4 .. 8 beats (param index = step + 1)
            {
                const double target = ar::decayStepsBeats[step] * 60.0 / bpm;
                const auto b = ir (s, bpm, step + 1, 0, target * 1.4 + 0.6);
                const auto f = fitEnd (bandEnergy (b));
                const double err = (f.endSec - target) / target;
                worst = std::max (worst, std::abs (err));
                std::printf ("%-8s %6.0f %8g %10.0f %10.0f %7.1f%%\n", ar::spaceName (s), bpm, (double) ar::decayStepsBeats[step], target * 1000, f.endSec * 1000, err * 100);
                check (f.ok && std::abs (err) < 0.10, juce::String (ar::spaceName (s)) + " " + juce::String (bpm, 0) + " BPM " + juce::String (ar::decayStepsBeats[step]) + " beats ends within 10 %");
            }
    std::printf ("  worst error %.1f %%\n", worst * 100);

    std::printf ("\n== Lite AUTO: SIZE picks the length, the tempo sets the time (duck off for the fit; Lite's fixed duck shown as info) ==\n");
    for (int s : { ar::room, ar::plate })
        for (float size : { 0.0f, 25.0f, 50.0f, 75.0f, 100.0f })
            for (double bpm : { 100.0, 128.0 })
            {
                const int stepIdx = APP::autoDecayStep (s, size / 100.0f, bpm);
                const double target = ar::decayStepsBeats[stepIdx] * 60.0 / bpm;
                const auto f = fitEnd (bandEnergy (ir (s, bpm, 0, 0, target * 1.4 + 0.6, hz::LicenceInfo::pro, size)));
                Rig r (hz::LicenceInfo::none, bpm);
                setParam (*r.p, "space", (float) s); setParam (*r.p, "mix", 100.0f); setParam (*r.p, "size", size);
                r.reprepare();
                check (r.p->resolve (false, bpm).decayStep == stepIdx, "Lite resolves the same AUTO length");
                const int len = (int) ((target * 1.4 + 0.6) * SR);
                const auto fl = fitEnd (bandEnergy (run (*r.p, impulse (len), len, 0, false)));
                const double err = (f.endSec - target) / target;
                std::printf ("  AUTO %-6s size %3.0f%% %4.0f BPM -> %-4g beats = %4.0f ms, measured %4.0f ms (%+.1f%%)   [Lite with its duck: %4.0f ms]\n",
                             ar::spaceName (s), size, bpm, (double) ar::decayStepsBeats[stepIdx], target * 1000, f.endSec * 1000, err * 100, fl.endSec * 1000);
                check (std::abs (err) < 0.10, juce::String ("AUTO ") + ar::spaceName (s) + " size " + juce::String (size, 0) + " @" + juce::String (bpm, 0) + " within 10 %");
            }

    std::printf ("\n== AUTO picks a musical length for the space, snapped to a beat step (SIZE 50 %%) ==\n");
    for (int s : { ar::room, ar::plate, ar::hall, ar::chamber, ar::spring, ar::shimmer })
    {
        std::printf ("  %-8s want ~%.1f s:", ar::spaceName (s), (double) ar::voice (s).autoSec);
        for (double bpm : { 90.0, 120.0, 128.0, 140.0, 174.0 })
        {
            Rig r (hz::LicenceInfo::pro, bpm);
            setParam (*r.p, "space", (float) s);
            const auto e = r.p->resolve (true, bpm);
            std::printf ("  %3.0f BPM %g beats = %.2f s", bpm, (double) e.decayBeats, (double) e.endSec);
            const double ratio = e.endSec / ar::voice (s).autoSec;
            check (ratio > 0.70 && ratio < 1.42, juce::String (ar::spaceName (s)) + " @" + juce::String (bpm, 0) + " AUTO within half a step of its musical length");
        }
        std::printf ("\n");
    }

    std::printf ("\n== GATED / REVERSE stop on the beat (level 30 ms after the end vs peak) ==\n");
    for (int s : { ar::gated, ar::reverse })
        for (double bpm : { 120.0, 140.0 })
            for (int step : { 1, 2 })
            {
                const double target = ar::decayStepsBeats[step] * 60.0 / bpm;
                const auto b = ir (s, bpm, step + 1, 1, target + 0.5);
                float pk = 0, after = 0; int pkAt = 0;
                for (int i = 0; i < b.getNumSamples(); ++i)
                {
                    const float a = juce::jmax (std::abs (b.getSample (0, i)), std::abs (b.getSample (1, i)));
                    if (a > pk) { pk = a; pkAt = i; }
                    if (i > (int) ((target + 0.03) * SR)) after = juce::jmax (after, a);
                }
                const double dB = juce::Decibels::gainToDecibels (after / pk, -150.0f);
                std::printf ("  %-8s %4.0f BPM %g beat: peak at %4.0f ms, after the end %6.1f dB\n", ar::spaceName (s), bpm, (double) ar::decayStepsBeats[step], pkAt / SR * 1000, dB);
                check (dB < -40.0, juce::String (ar::spaceName (s)) + " silent 30 ms after the beat");
                if (s == ar::reverse) check (pkAt > 0.6 * target * SR, "REVERSE swells: peak in the last 40 % before the beat");
            }
}

static Buf testLoop (double secs, double bpm);
static void rateAndLevelTests()
{
    std::printf ("\n== Other sample rates: 2 beats @ 120 BPM still ends on the beat ==\n");
    for (double fs : { 44100.0, 96000.0 })
        for (int s : { ar::room, ar::plate, ar::hall, ar::shimmer })
        {
            Rig r (hz::LicenceInfo::pro, 120.0, fs);
            setParam (*r.p, "space", (float) s); setParam (*r.p, "mix", 100.0f); setParam (*r.p, "duck", 0.0f); setParam (*r.p, "decay", 4.0f);
            r.reprepare (fs);
            const int len = (int) (1.8 * fs);
            Buf imp (2, len); imp.clear(); imp.setSample (0, 0, 1.0f); imp.setSample (1, 0, 1.0f);
            const auto f = fitEnd (bandEnergy (run (*r.p, imp, len, 0, false), fs), fs);
            std::printf ("  %6.0f Hz %-8s end %4.0f ms (%+.1f%%)\n", fs, ar::spaceName (s), f.endSec * 1000, (f.endSec - 1.0) * 100);
            check (std::abs (f.endSec - 1.0) < 0.10, juce::String (ar::spaceName (s)) + " @ " + juce::String (fs, 0) + " Hz ends within 10 %");
        }

    std::printf ("\n== Level match: every space's reverb within 4.5 dB of PLATE (AUTO, claps + tone loop) ==\n");
    const auto loop = testLoop (8.0, 124.0);
    double ref = 0;
    for (int s : { ar::plate, ar::room, ar::hall, ar::chamber, ar::spring, ar::shimmer, ar::gated, ar::reverse })
    {
        Rig r (hz::LicenceInfo::pro, 124.0);
        setParam (*r.p, "space", (float) s); setParam (*r.p, "mix", 100.0f); r.reprepare();
        const auto o = run (*r.p, loop);
        double e = 0; for (int c = 0; c < 2; ++c) for (int i = 0; i < o.getNumSamples(); ++i) e += o.getSample (c, i) * o.getSample (c, i);
        const double db = 10 * std::log10 (e + 1e-30);
        if (s == ar::plate) ref = db;
        std::printf ("  %-8s %+5.1f dB\n", ar::spaceName (s), db - ref);
        check (std::abs (db - ref) < 4.5, juce::String (ar::spaceName (s)) + " within 4.5 dB of PLATE (synthetic loop; real stems are within ~2 dB)");
    }
}

// ── Pre-delay ────────────────────────────────────────────────────────────────
static int onsetOf (const Buf& b)
{
    float pk = 0; for (int c = 0; c < 2; ++c) pk = juce::jmax (pk, b.getMagnitude (c, 0, b.getNumSamples()));
    for (int i = 0; i < b.getNumSamples(); ++i)
        if (std::abs (b.getSample (0, i)) > pk * 1.0e-3f || std::abs (b.getSample (1, i)) > pk * 1.0e-3f) return i;
    return -1;
}

static void preDelayTests()
{
    std::printf ("\n== Pre-delay exact (onset vs pre-delay OFF) ==\n");
    for (double bpm : { 120.0, 137.0 })
    {
        const int base = onsetOf (ir (ar::plate, bpm, 5, 1, 0.5));
        std::printf ("  %3.0f BPM: onset with pre-delay OFF = %d samples (%.2f ms)\n", bpm, base, base / SR * 1000);
        for (int k = 1; k < ar::numPreSteps; ++k)
        {
            const int want = (int) std::lround (ar::preStepsBeats[k] * 60.0 / bpm * SR);
            const int got = onsetOf (ir (ar::plate, bpm, 5, k + 1, 0.9)) - base;
            std::printf ("    %-5s want %5d got %5d samples (%.2f ms)\n", ar::preStepName (k), want, got, got / SR * 1000);
            check (std::abs (got - want) <= 1, juce::String ("pre-delay ") + ar::preStepName (k) + " @" + juce::String (bpm, 0) + " exact");
        }
    }
}

// ── Density, ringing, stereo ─────────────────────────────────────────────────
static double nedAt (const Buf& b, int centre, int half)
{
    // Normalised echo density (Abel & Huang): share of samples beyond 1 std dev, / erfc(1/sqrt2)
    const int a = juce::jmax (0, centre - half), z = juce::jmin (b.getNumSamples(), centre + half);
    double sq = 0; int n = 0;
    for (int i = a; i < z; ++i) { const double x = b.getSample (0, i) + b.getSample (1, i); sq += x * x; ++n; }
    const double sd = std::sqrt (sq / juce::jmax (1, n));
    int over = 0;
    for (int i = a; i < z; ++i) if (std::abs (b.getSample (0, i) + b.getSample (1, i)) > sd) ++over;
    return (double) over / juce::jmax (1, n) / 0.3173;
}

static void densityRingingStereo()
{
    std::printf ("\n== Echo density, ringing, stereo (4 beats @ 120 BPM, pre-delay OFF) ==\n");
    std::printf ("%-8s %12s %12s %10s %12s %8s\n", "space", "dense after", "NED 50-400", "ringing", "worst at", "L/R corr");
    for (int s = 0; s < ar::numSpaces; ++s)
    {
        const bool shaped = ar::voice (s).shape != 0;
        const auto b = ir (s, 120.0, shaped ? 4 : 5, 1, 2.6);
        const int on = onsetOf (b);
        // 1) time from onset until NED >= 0.9 (20 ms window)
        const int half = (int) (0.010 * SR);
        double denseMs = -1;
        for (int t = on + half; t < on + (int) (0.3 * SR); t += (int) (0.001 * SR))
            if (nedAt (b, t, half) >= 0.9) { denseMs = (t - on) / SR * 1000.0; break; }
        double nedMin = 9; const int nedEnd = shaped ? (int) (0.4 * SR) : (int) (0.4 * SR);
        for (int t = on + (int) (0.05 * SR); t < on + nedEnd; t += (int) (0.01 * SR)) nedMin = std::min (nedMin, nedAt (b, t, half));

        // 2) ringing: Welch PSD of the tail (each frame normalised), vs its 1/3-octave smoothed self
        const int fftOrder = 12, M = 1 << fftOrder;
        juce::dsp::FFT fft (fftOrder);
        std::vector<double> psd ((size_t) M / 2, 0.0);
        const int t0 = on + (int) (0.08 * SR), t1 = on + (int) ((shaped ? 0.4 : 1.1) * SR);
        int frames = 0;
        for (int st = t0; st + M <= t1; st += M / 2)
            for (int c = 0; c < 2; ++c)
            {
                std::vector<float> buf ((size_t) M * 2, 0.0f);
                double en = 0;
                for (int i = 0; i < M; ++i)
                {
                    const float w = 0.5f - 0.5f * std::cos ((float) twoPi * i / M);
                    buf[(size_t) i] = b.getSample (c, st + i) * w; en += buf[(size_t) i] * buf[(size_t) i];
                }
                fft.performFrequencyOnlyForwardTransform (buf.data());
                for (int k = 0; k < M / 2; ++k) psd[(size_t) k] += (double) buf[(size_t) k] * buf[(size_t) k] / (en + 1e-30);
                ++frames;
            }
        double worst = -99, worstHz = 0;
        for (int k = 1; k < M / 2; ++k)
        {
            const double hz = k * SR / M;
            if (hz < 250.0 || hz > 8000.0) continue;
            const double lo = hz / std::pow (2.0, 1.0 / 6.0), hi = hz * std::pow (2.0, 1.0 / 6.0);
            double sum = 0; int cnt = 0;
            for (int j = (int) (lo * M / SR); j <= (int) (hi * M / SR); ++j) { sum += psd[(size_t) j]; ++cnt; }
            const double ex = 10.0 * std::log10 (psd[(size_t) k] / (sum / cnt));
            if (ex > worst) { worst = ex; worstHz = hz; }
        }
        // 3) stereo correlation of the tail
        double lr = 0, ll = 0, rr = 0;
        for (int i = on + (int) (0.03 * SR); i < b.getNumSamples(); ++i)
        { const double l = b.getSample (0, i), r = b.getSample (1, i); lr += l * r; ll += l * l; rr += r * r; }
        const double corr = lr / std::sqrt (ll * rr + 1e-30);
        std::printf ("%-8s %9.1f ms %12.2f %7.1f dB %9.0f Hz %8.2f   (%d frames)\n", ar::spaceName (s), denseMs, nedMin, worst, worstHz, corr, frames);
        check (denseMs >= 0 && denseMs < (s == ar::spring ? 80.0 : 40.0), juce::String (ar::spaceName (s)) + ": dense (NED >= 0.9) within " + (s == ar::spring ? "80" : "40") + " ms of the onset");
        check (nedMin > 0.8, juce::String (ar::spaceName (s)) + ": stays dense 50-400 ms (no flutter, NED min > 0.8)");
        check (worst < 6.0, juce::String (ar::spaceName (s)) + ": no ringing mode (peak within 6 dB of the smoothed tail spectrum)");
        check (std::abs (corr) < 0.3, juce::String (ar::spaceName (s)) + ": L/R decorrelated (|corr| < 0.3)");
    }
}

// ── Low cut ──────────────────────────────────────────────────────────────────
static double wetRmsSine (int space, double hz)
{
    // Average of three steady tones (hz x 0.9 / 1 / 1.1): one sine alone can land on a room mode or between two.
    double tot = 0;
    for (double f : { 0.9 * hz, hz, 1.1 * hz })
    {
        Rig r (hz::LicenceInfo::pro, 120.0);
        setParam (*r.p, "space", (float) space); setParam (*r.p, "mix", 100.0f); setParam (*r.p, "duck", 0.0f);
        r.reprepare();
        const int len = (int) (3.0 * SR);
        Buf s (2, len);
        for (int i = 0; i < len; ++i) { const float v = 0.3f * (float) std::sin (twoPi * f * i / SR); s.setSample (0, i, v); s.setSample (1, i, v); }
        const auto o = run (*r.p, s, len, 0, false);
        double sq = 0; for (int c = 0; c < 2; ++c) for (int i = (int) (1.5 * SR); i < len; ++i) sq += o.getSample (c, i) * o.getSample (c, i);
        tot += sq / (2.0 * (len - 1.5 * SR));
    }
    return 10.0 * std::log10 (tot / 3.0 + 1e-30);
}

static void lowCutTests()
{
    std::printf ("\n== Built-in low cut (steady tones +-10 %%, wet only, vs 1 kHz) ==\n");
    for (int s : { ar::room, ar::plate, ar::hall })
    {
        const double k1 = wetRmsSine (s, 1000.0), h50 = wetRmsSine (s, 50.0), h100 = wetRmsSine (s, 100.0);
        std::printf ("  %-6s 50 Hz %6.1f dB, 100 Hz %6.1f dB (re 1 kHz)\n", ar::spaceName (s), h50 - k1, h100 - k1);
        check (h50 - k1 < -24.0, juce::String (ar::spaceName (s)) + ": 50 Hz at least 24 dB down in the reverb");
        check (h100 - k1 < -10.0, juce::String (ar::spaceName (s)) + ": 100 Hz at least 10 dB down in the reverb");
    }
}

// ── Ducking ──────────────────────────────────────────────────────────────────
static void duckTests()
{
    std::printf ("\n== Ducking: wet level while the input plays vs in the gaps (1 beat on / 1 beat off, 120 BPM) ==\n");
    const int len = (int) (8.0 * SR), beat = (int) (0.5 * SR);
    Buf in (2, len);
    juce::Random rnd (7);
    for (int i = 0; i < len; ++i)
    {
        const bool on = (i / beat) % 2 == 0;
        const float v = on ? 0.25f * (float) std::sin (twoPi * 440.0 * i / SR) + 0.05f * (rnd.nextFloat() * 2 - 1) : 0.0f;
        in.setSample (0, i, v); in.setSample (1, i, v);
    }
    auto measure = [&] (int lic, float duck, double& during, double& gap)
    {
        Rig r (lic, 120.0);
        setParam (*r.p, "space", ar::plate); setParam (*r.p, "mix", 100.0f); setParam (*r.p, "duck", duck);
        r.reprepare();
        const auto o = run (*r.p, in, len, 0, false);
        double sd = 0, sg = 0; int nd = 0, ng = 0;
        for (int i = 2 * 2 * beat; i < len; ++i)
        {
            const int ph = i % (2 * beat);
            const double e = o.getSample (0, i) * o.getSample (0, i) + o.getSample (1, i) * o.getSample (1, i);
            if (ph > beat / 2 && ph < beat - beat / 10) { sd += e; ++nd; }                 // middle of the note
            else if (ph > beat + beat / 4 && ph < 2 * beat - beat / 10) { sg += e; ++ng; } // the gap, from 1/16 after
        }
        during = 10 * std::log10 (sd / nd + 1e-30); gap = 10 * std::log10 (sg / ng + 1e-30);
    };
    double d0, g0; measure (hz::LicenceInfo::pro, 0.0f, d0, g0);
    struct Case { const char* name; int lic; float duck; double expect; };
    for (auto c : { Case { "Lite (fixed)", hz::LicenceInfo::none, 0.0f, ar::liteDuck / 100.0 * ar::maxDuckDb },
                    Case { "Pro 50 %", hz::LicenceInfo::pro, 50.0f, 0.5 * ar::maxDuckDb },
                    Case { "Pro 100 %", hz::LicenceInfo::pro, 100.0f, ar::maxDuckDb } })
    {
        double d, gp; measure (c.lic, c.duck, d, gp);
        std::printf ("  %-18s during the note %+6.1f dB, in the gap %+6.1f dB (vs duck off)\n", c.name, d - d0, gp - g0);
        check (std::abs ((d0 - d) - c.expect) < 2.5, juce::String (c.name) + ": ducks ~" + juce::String (c.expect, 0) + " dB under the note");
        const double bloom = c.expect > 10.0 ? 4.5 : 3.0;   // full 18 dB releases a little slower (no pumping), still back within the gap
        check (g0 - gp < bloom, juce::String (c.name) + ": blooms back in the gap (< " + juce::String (bloom, 1) + " dB down)");
    }

    std::printf ("\n== Drums don't pump the tail (clap every beat @ 124 BPM, wet 20 ms RMS vs duck off) ==\n");
    {
        const int n = (int) (6.0 * SR); Buf d (2, n); d.clear();
        juce::Random r2 (5);
        const double bt = 60.0 / 124.0;
        for (int k = 0; k * bt < 6.0; ++k)
            for (int i = 0, st = (int) (k * bt * SR); i < (int) (0.15 * SR) && st + i < n; ++i)
            {
                const float v = 0.6f * (r2.nextFloat() * 2 - 1) * (float) std::exp (-(i / SR) / 0.035);
                d.setSample (0, st + i, v); d.setSample (1, st + i, v);
            }
        auto env = [&] (int lic, float duck)
        {
            Rig r (lic, 124.0);
            setParam (*r.p, "space", ar::plate); setParam (*r.p, "mix", 100.0f); setParam (*r.p, "duck", duck);
            r.reprepare();
            const auto o = run (*r.p, d, n, 0, false);
            std::vector<double> e; const int h = (int) (0.02 * SR);
            for (int st = (int) (1.0 * SR); st + h <= n; st += h)
            {
                double q = 0; for (int c = 0; c < 2; ++c) for (int i = 0; i < h; ++i) q += o.getSample (c, st + i) * o.getSample (c, st + i);
                e.push_back (10 * std::log10 (q / h + 1e-30));
            }
            return e;
        };
        const auto off = env (hz::LicenceInfo::pro, 0.0f);
        for (auto c : { std::pair<const char*, int> { "Lite", hz::LicenceInfo::none }, std::pair<const char*, int> { "Pro 50 %", hz::LicenceInfo::pro } })
        {
            const auto on = env (c.second, c.second == hz::LicenceInfo::pro ? 50.0f : 0.0f);
            double sum = 0, sq = 0; std::vector<double> dev;
            for (size_t i = 0; i < off.size(); ++i) { const double dv = off[i] - on[i]; dev.push_back (dv); sum += dv; sq += dv * dv; }
            const double mean = sum / (double) dev.size(), ripple = std::sqrt (juce::jmax (0.0, sq / (double) dev.size() - mean * mean));
            std::printf ("  %-9s duck depth average %4.1f dB, moves by %4.1f dB (std dev over the groove)\n", c.first, mean, ripple);
            check (ripple < (c.second == hz::LicenceInfo::pro ? 1.5 : 0.75), juce::String (c.first) + ": the duck moves smoothly with the groove, not on every hit");
        }
    }
}

// ── Freeze + shimmer stability ───────────────────────────────────────────────
static void stabilityTests()
{
    std::printf ("\n== Freeze stable / shimmer never runs away ==\n");
    juce::Random rnd (3);
    for (int s : { ar::plate, ar::hall, ar::shimmer, ar::spring })
    {
        Rig r (hz::LicenceInfo::pro, 120.0);
        setParam (*r.p, "space", (float) s); setParam (*r.p, "mix", 100.0f); setParam (*r.p, "duck", 0.0f); setParam (*r.p, "decay", 6.0f);
        r.reprepare();
        const int sec = (int) SR;
        Buf noise (2, 2 * sec);
        for (int i = 0; i < 2 * sec; ++i) { const float v = 0.2f * (rnd.nextFloat() * 2 - 1); noise.setSample (0, i, v); noise.setSample (1, i, rnd.nextFloat() * 0.4f - 0.2f); }
        run (*r.p, noise, 2 * sec);
        setParam (*r.p, "freeze", 1.0f);
        run (*r.p, noise, sec);            // input keeps playing: frozen, so it must not build up
        std::vector<double> lv;
        bool finite = true;
        for (int k = 0; k < 20; ++k)
        {
            const auto o = run (*r.p, noise, sec, (k * sec) % (2 * sec));
            double sq = 0; for (int c = 0; c < 2; ++c) for (int i = 0; i < sec; ++i) { const float x = o.getSample (c, i); if (! std::isfinite (x)) finite = false; sq += x * x; }
            lv.push_back (10 * std::log10 (sq / (2.0 * sec) + 1e-30));
        }
        const double first = lv.front(), mx = *std::max_element (lv.begin(), lv.end()), last = lv.back();
        std::printf ("  %-8s frozen 20 s: first %6.1f dB, max %6.1f, last %6.1f\n", ar::spaceName (s), first, mx, last);
        check (finite && mx < first + 1.0, juce::String (ar::spaceName (s)) + ": freeze never grows (max <= first + 1 dB)");
        check (last > first - 6.0, juce::String (ar::spaceName (s)) + ": freeze holds (loses < 6 dB in 20 s)");
        setParam (*r.p, "freeze", 0.0f);
        Buf sil (2, sec); sil.clear();
        for (int k = 0; k < 6; ++k) run (*r.p, sil, sec);
        const auto o = run (*r.p, sil, sec);
        const double after = juce::Decibels::gainToDecibels (juce::jmax (o.getMagnitude (0, 0, sec), o.getMagnitude (1, 0, sec)), -150.0f);
        std::printf ("           unfrozen, 7 s later: peak %6.1f dB\n", after);
        check (after < -60.0, juce::String (ar::spaceName (s)) + ": un-freeze lets it decay away");
    }
    // Shimmer at its longest: impulse energy must keep falling.
    {
        const auto b = ir (ar::shimmer, 70.0, 6, 0, 14.0);
        double e1 = 0, e2 = 0;
        for (int c = 0; c < 2; ++c) for (int i = 0; i < b.getNumSamples(); ++i)
        { const double x = b.getSample (c, i); (i < (int) (2 * SR) ? e1 : e2) += (i > (int) (12 * SR)) ? x * x : (i < (int) (2 * SR) ? x * x : 0.0); }
        const double ratio = 10 * std::log10 (e2 / (e1 + 1e-30) + 1e-30);
        std::printf ("  SHIMMER 8 beats @ 70 BPM (6.9 s): energy 12-14 s vs first 2 s = %.1f dB\n", ratio);
        check (ratio < -60.0, "SHIMMER never runs away (tail 12-14 s more than 60 dB down)");
    }
}

// ── Clicks ───────────────────────────────────────────────────────────────────
struct HF
{
    juce::dsp::IIR::Filter<float> a, b, c;
    HF() { a.coefficients = b.coefficients = c.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighPass (SR, 3000.0f); }
    float operator() (float x) { return c.processSample (b.processSample (a.processSample (x))); }
};

static void clickTests()
{
    std::printf ("\n== Clicks when switching (220 Hz tone, 100 %% wet; HF > 3 kHz right after each switch vs steady) ==\n");
    struct Cat { const char* name; std::function<void (APP&, int)> change; };
    std::vector<Cat> cats = {
        { "space",         [] (APP& p, int k) { setParam (p, "space", (float) (k % 6)); } },   // natural spaces (gated/reverse need hits)
        { "room<->plate",  [] (APP& p, int k) { setParam (p, "space", (float) (k % 2)); } },
        { "size 0/100",    [] (APP& p, int k) { setParam (p, "size", k % 2 ? 100.0f : 0.0f); } },
        { "decay steps",   [] (APP& p, int k) { setParam (p, "decay", (float) (k % 7)); } },
        { "pre-delay",     [] (APP& p, int k) { setParam (p, "predelay", (float) (k % 6)); } },
        { "tone 0/100",    [] (APP& p, int k) { setParam (p, "tone", k % 2 ? 100.0f : 0.0f); } },
        { "width 0/150",   [] (APP& p, int k) { setParam (p, "width", k % 2 ? 150.0f : 0.0f); } },
        { "duck 0/100",    [] (APP& p, int k) { setParam (p, "duck", k % 2 ? 100.0f : 0.0f); } },
        { "freeze",        [] (APP& p, int k) { setParam (p, "freeze", (float) (k % 2)); } },
        { "mix 0/100",     [] (APP& p, int k) { setParam (p, "mix", k % 2 ? 0.0f : 100.0f); } },
        { "Pro on/off",    [] (APP& p, int k) { setLic (p, k % 2 ? hz::LicenceInfo::none : hz::LicenceInfo::pro); } },
    };
    const double secs = 7.0; const int total = (int) (secs * SR);
    Buf tone (2, total);
    for (int i = 0; i < total; ++i) { const float v = 0.4f * (float) std::sin (twoPi * 220.0 * i / SR); tone.setSample (0, i, v); tone.setSample (1, i, v); }
    std::printf ("%-14s %12s %12s %8s\n", "switch", "steady dB", "switch dB", "click");
    for (auto& cat : cats)
    {
        Rig r (hz::LicenceInfo::pro, 120.0);
        setParam (*r.p, "mix", 100.0f);
        if (juce::String (cat.name) == "Pro on/off") { setParam (*r.p, "space", ar::hall); setParam (*r.p, "decay", 2); setParam (*r.p, "tone", 90); setParam (*r.p, "width", 30); }
        r.reprepare();
        HF hf; float steady = 0, sw = 0;
        const int every = (int) (0.5 * SR) / BS * BS, after = (int) (0.06 * SR);
        int k = 0, lastChange = -1000000;
        Buf blk (2, BS); juce::MidiBuffer m;
        for (int pos = 0; pos + BS <= total; pos += BS)
        {
            if (pos >= (int) (1.5 * SR) && pos % every == 0) { cat.change (*r.p, ++k); lastChange = pos; }
            for (int c = 0; c < 2; ++c) blk.copyFrom (c, 0, tone, c, pos, BS);
            r.p->processBlock (blk, m);
            for (int i = 0; i < BS; ++i)
            {
                const float y = std::abs (hf (blk.getSample (0, i)));
                if (pos + i < (int) (1.2 * SR)) continue;
                if (pos + i - lastChange < after) sw = juce::jmax (sw, y);
                else steady = juce::jmax (steady, y);
            }
        }
        const float sDb = juce::Decibels::gainToDecibels (steady, -150.0f), wDb = juce::Decibels::gainToDecibels (sw, -150.0f);
        const bool ok = wDb < sDb + 6.0f || wDb < -70.0f;
        std::printf ("%-14s %12.1f %12.1f %8s\n", cat.name, sDb, wDb, ok ? "none" : "CLICK");
        check (ok, juce::String ("no click: ") + cat.name);
    }
}

// ── CPU ──────────────────────────────────────────────────────────────────────
static void cpuTests()
{
    std::printf ("\n== CPU (256-sample blocks @ 48 kHz, stereo; block = %.2f ms) ==\n", BS / SR * 1000);
    juce::Random rnd (1);
    Buf noise (2, (int) SR);
    for (int c = 0; c < 2; ++c) for (int i = 0; i < noise.getNumSamples(); ++i) noise.setSample (c, i, 0.2f * (rnd.nextFloat() * 2 - 1));
    double worstAvg = 0;
    for (int s = 0; s < ar::numSpaces; ++s)
    {
        Rig r (hz::LicenceInfo::pro, 124.0);
        setParam (*r.p, "space", (float) s); r.reprepare();
        Buf blk (2, BS); juce::MidiBuffer m;
        double tot = 0, mx = 0; const int blocks = 1500;
        for (int b = 0; b < blocks; ++b)
        {
            for (int c = 0; c < 2; ++c) blk.copyFrom (c, 0, noise, c, (b * BS) % (noise.getNumSamples() - BS), BS);
            const auto t0 = std::chrono::high_resolution_clock::now();
            r.p->processBlock (blk, m);
            const double us = std::chrono::duration<double, std::micro> (std::chrono::high_resolution_clock::now() - t0).count();
            if (b > 20) { tot += us; mx = std::max (mx, us); }
        }
        const double avg = tot / (blocks - 21);
        worstAvg = std::max (worstAvg, avg);
        std::printf ("  %-8s avg %6.1f us  max %6.1f us  (%.1f %% of real time)\n", ar::spaceName (s), avg, mx, avg / (BS / SR * 1.0e6) * 100);
    }
    // during a space change three engines can run at once
    {
        Rig r (hz::LicenceInfo::pro, 124.0);
        Buf blk (2, BS); juce::MidiBuffer m; double mx = 0;
        for (int b = 0; b < 400; ++b)
        {
            if (b % 40 == 0) setParam (*r.p, "space", (float) ((b / 40) % 6));
            for (int c = 0; c < 2; ++c) blk.copyFrom (c, 0, noise, c, 0, BS);
            const auto t0 = std::chrono::high_resolution_clock::now();
            r.p->processBlock (blk, m);
            mx = std::max (mx, std::chrono::duration<double, std::micro> (std::chrono::high_resolution_clock::now() - t0).count());
        }
        std::printf ("  switching spaces: max %.1f us per block (%.1f %%)\n", mx, mx / (BS / SR * 1.0e6) * 100);
        check (mx < 0.5 * BS / SR * 1.0e6, "space switching stays under 50 % of a block");
    }
    check (worstAvg < 0.15 * BS / SR * 1.0e6, "every space under 15 % of real time on one core");
}

// ── State + Lite gating ──────────────────────────────────────────────────────
static Buf testLoop (double secs, double bpm)
{
    // Claps / snares on 2 and 4 + a short vocal-ish tone, so tails, ducking and hits all show.
    const int n = (int) (secs * SR); Buf b (2, n); b.clear();
    juce::Random rnd (11);
    const double beat = 60.0 / bpm;
    for (int k = 0; k * beat < secs; ++k)
    {
        const int st = (int) (k * beat * SR);
        if (k % 2 == 1)
            for (int i = 0; i < (int) (0.2 * SR) && st + i < n; ++i)
            {
                const double t = i / SR;
                const float v = (rnd.nextFloat() * 2 - 1) * (float) std::exp (-t / 0.05) * 0.5f;
                b.addSample (0, st + i, v); b.addSample (1, st + i, v * 0.9f);
            }
        if (k % 4 == 0)
            for (int i = 0; i < (int) (beat * 1.5 * SR) && st + i < n; ++i)
            {
                const double t = i / SR;
                const float v = 0.2f * (float) (std::sin (twoPi * 330.0 * t) + 0.4 * std::sin (twoPi * 660.0 * t)) * (float) std::min (1.0, t / 0.01) * (float) std::min (1.0, (beat * 1.5 - t) / 0.02);
                b.addSample (0, st + i, v); b.addSample (1, st + i, v);
            }
    }
    return b;
}

static float maxDiff (const Buf& a, const Buf& b)
{
    float d = 0; for (int c = 0; c < 2; ++c) for (int i = 0; i < a.getNumSamples(); ++i) d = juce::jmax (d, std::abs (a.getSample (c, i) - b.getSample (c, i)));
    return d;
}

static void stateTests()
{
    std::printf ("\n== Save -> new instance -> restore ==\n");
    const auto loop = testLoop (4.0, 124.0);
    Rig a (hz::LicenceInfo::pro, 124.0);
    setParam (*a.p, "space", ar::chamber); setParam (*a.p, "size", 71.0f); setParam (*a.p, "mix", 37.0f); setParam (*a.p, "decay", 5.0f);
    setParam (*a.p, "predelay", 3.0f); setParam (*a.p, "duck", 64.0f); setParam (*a.p, "tone", 22.0f); setParam (*a.p, "width", 120.0f);
    juce::MemoryBlock mb; a.p->getStateInformation (mb);
    Rig b (hz::LicenceInfo::pro, 124.0), c (hz::LicenceInfo::pro, 124.0);
    b.p->setStateInformation (mb.getData(), (int) mb.getSize()); c.p->setStateInformation (mb.getData(), (int) mb.getSize());
    bool same = true;
    for (auto* prm : a.p->getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (prm))
            same = same && std::abs (b.p->apvts.getParameter (rp->getParameterID())->getValue() - rp->getValue()) < 1.0e-6f;
    check (same, "every parameter survives save/restore");
    b.reprepare(); c.reprepare(); a.reprepare();
    const auto oa = run (*a.p, loop), ob = run (*b.p, loop), oc = run (*c.p, loop);
    check (maxDiff (ob, oc) == 0.0f, "two restored instances are bit-identical");
    check (maxDiff (oa, ob) == 0.0f, "restored instance sounds exactly like the original");
    for (int i = 0; i < 2; ++i) { std::unique_ptr<juce::AudioProcessorEditor> ed (b.p->createEditor()); }
    check (true, "editor opens / closes twice without trouble");
}

static void liteTests()
{
    std::printf ("\n== Lite can't use Pro features ==\n");
    const auto loop = testLoop (5.0, 124.0);
    auto mk = [&] (int lic) { auto r = std::make_unique<Rig> (lic, 124.0); return r; };
    auto plain = mk (hz::LicenceInfo::none);
    const auto ref = run (*plain->p, loop);
    auto force = [] (APP& p, int space)
    {
        setParam (p, "space", (float) space); setParam (p, "decay", 6.0f); setParam (p, "predelay", 5.0f);
        setParam (p, "duck", 0.0f); setParam (p, "tone", 100.0f); setParam (p, "width", 0.0f); setParam (p, "freeze", 1.0f);
    };
    for (int s : { ar::hall, ar::chamber, ar::spring, ar::shimmer, ar::gated, ar::reverse })
    {
        auto lite = mk (hz::LicenceInfo::none); force (*lite->p, s); lite->reprepare();
        const float d = maxDiff (run (*lite->p, loop), ref);
        check (d == 0.0f, juce::String ("Lite + automation to ") + ar::spaceName (s) + " / 8 beats / 1/8 / duck 0 / tone 100 / width 0 / FREEZE = plain Lite (max diff " + juce::String (d) + ")");
    }
    {
        auto pro = mk (hz::LicenceInfo::pro); force (*pro->p, ar::shimmer);
        juce::MemoryBlock mb; pro->p->getStateInformation (mb);
        auto lite = mk (hz::LicenceInfo::none); lite->p->setStateInformation (mb.getData(), (int) mb.getSize()); lite->reprepare();
        check (maxDiff (run (*lite->p, loop), ref) == 0.0f, "Pro session opened in Lite falls back to plain Lite");
        auto exp = mk (hz::LicenceInfo::trialExpired); exp->p->setStateInformation (mb.getData(), (int) mb.getSize()); exp->reprepare();
        check (maxDiff (run (*exp->p, loop), ref) == 0.0f, "expired trial = Lite");
        auto pro2 = mk (hz::LicenceInfo::pro); force (*pro2->p, ar::hall); setParam (*pro2->p, "freeze", 0.0f); pro2->reprepare();
        check (maxDiff (run (*pro2->p, loop), ref) > 0.01f, "sanity: the same settings DO change the sound in Pro");
        auto room = mk (hz::LicenceInfo::none); setParam (*room->p, "space", ar::room); room->reprepare();
        check (maxDiff (run (*room->p, loop), ref) > 0.001f, "ROOM works in Lite");
        auto sz = mk (hz::LicenceInfo::none); setParam (*sz->p, "size", 90.0f); sz->reprepare();
        check (maxDiff (run (*sz->p, loop), ref) > 0.001f, "SIZE works in Lite");
    }
}

// Load a file (mono -> stereo), skipping leading silence, at most `secs` long, peak-normalised to -6 dBFS.
static Buf loadTrimmed (const juce::File& f, double secs, double& fs)
{
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
    if (! r) return {};
    fs = r->sampleRate;
    const int total = (int) r->lengthInSamples;
    Buf all (2, total); r->read (&all, 0, total, 0, true, true);
    if (r->numChannels == 1) all.copyFrom (1, 0, all, 0, 0, total);
    int st = 0;
    while (st < total && std::abs (all.getSample (0, st)) < 1.0e-4f && std::abs (all.getSample (1, st)) < 1.0e-4f) ++st;
    st = juce::jmax (0, st - (int) (0.02 * fs));
    const int len = juce::jmin (total - st, (int) (secs * fs));
    Buf x (2, juce::jmax (1, len));
    for (int c = 0; c < 2; ++c) x.copyFrom (c, 0, all, c, st, len);
    const float mag = x.getMagnitude (0, len);
    if (mag > 0.0f) x.applyGain (0.5f / mag);
    return x;
}

// ── Sound examples ───────────────────────────────────────────────────────────
static void renderDemos (const juce::File& od, double bpm, const juce::StringArray& files)
{
    od.createDirectory();
    for (auto& path : files)
    {
        const juce::File f (path);
        double fs = SR;
        const Buf x = loadTrimmed (f, 10.0, fs);
        if (x.getNumSamples() < 2) { std::printf ("  can't read %s\n", path.toRawUTF8()); continue; }
        const int len = x.getNumSamples();
        const auto stem = f.getParentDirectory().getFileName().upToFirstOccurrenceOf ("-", false, false) + "-" + f.getFileNameWithoutExtension();
        const int tail = (int) (fs * 3.0);
        Buf padded (2, len + tail); padded.clear();
        for (int c = 0; c < 2; ++c) padded.copyFrom (c, 0, x, c, 0, len);
        writeWav (od.getChildFile (stem + "-0-DRY.wav"), padded, fs);
        for (int s = 0; s < ar::numSpaces; ++s)
        {
            APP p; setLic (p, hz::LicenceInfo::pro);
            TestHead h; h.bpm = bpm; p.setPlayHead (&h);
            setParam (p, "space", (float) s); setParam (p, "mix", s == ar::gated || s == ar::reverse ? 40.0f : 32.0f);
            p.setRateAndBufferSizeDetails (fs, BS); p.prepareToPlay (fs, BS);
            Buf out (2, len + tail), blk (2, BS); juce::MidiBuffer m;
            for (int pos = 0; pos < len + tail; pos += BS)
            {
                const int n = juce::jmin (BS, len + tail - pos);
                blk.setSize (2, n, false, false, true);
                for (int c = 0; c < 2; ++c) blk.copyFrom (c, 0, padded, c, pos, n);
                p.processBlock (blk, m);
                for (int c = 0; c < 2; ++c) out.copyFrom (c, pos, blk, c, 0, n);
            }
            const auto e = p.resolve (true, bpm);
            writeWav (od.getChildFile (stem + "-" + juce::String (s + 1) + "-" + ar::spaceName (s) + ".wav"), out, fs);
            std::printf ("  %-22s %-8s AUTO %g beats @ %.0f BPM = %.0f ms, pre %s, peak %.1f dBFS\n", stem.toRawUTF8(), ar::spaceName (s),
                         (double) e.decayBeats, bpm, e.endSec * 1000, ar::preStepName (e.preStep), juce::Decibels::gainToDecibels (out.getMagnitude (0, out.getNumSamples())));
        }
    }
}

// ── UI ───────────────────────────────────────────────────────────────────────
static void snapshots (const juce::File& outDir)
{
    std::printf ("\n== UI snapshots ==\n");
    const auto loop = testLoop (3.0, 124.0);
    auto snap = [&] (const char* name, int licKind, const char* panel, std::function<void (APP&)> setup, std::function<void (APP&)> after = nullptr)
    {
        Rig r (licKind, 124.0);
        if (setup) setup (*r.p);
        r.reprepare();
        run (*r.p, loop, (int) (2.3 * SR));
        if (after) { after (*r.p); run (*r.p, loop, (int) (0.6 * SR), (int) (2.3 * SR)); }
        std::unique_ptr<juce::AudioProcessorEditor> ed (r.p->createEditor());
        if (auto* t = dynamic_cast<juce::Timer*> (ed.get())) for (int i = 0; i < 20; ++i) t->timerCallback();
        if (panel != nullptr)
            for (auto* c : ed->getChildren())
                if (auto* pp = dynamic_cast<ProPanel*> (c)) pp->show (panel);
        auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f);
        juce::FileOutputStream fo (outDir.getChildFile (name));
        fo.setPosition (0); fo.truncate();
        juce::PNGImageFormat().writeImageToStream (img, fo);
        std::printf ("  %s\n", name);
    };
    snap ("ui-1-lite.png", hz::LicenceInfo::none, nullptr, nullptr);
    snap ("ui-2-lite-room-size.png", hz::LicenceInfo::none, nullptr, [] (APP& p) { setParam (p, "space", ar::room); setParam (p, "size", 80.0f); setParam (p, "mix", 40.0f); });
    snap ("ui-3-lite-pro-panel.png", hz::LicenceInfo::none, "SHIMMER IS AN UPGRADE SPACE", nullptr);
    snap ("ui-4-trial.png", hz::LicenceInfo::trial, nullptr, [] (APP& p) { setParam (p, "space", ar::hall); setParam (p, "decay", 5.0f); setParam (p, "predelay", 4.0f); });
    snap ("ui-5-trial-ended.png", hz::LicenceInfo::trialExpired, nullptr, [] (APP& p) { setParam (p, "space", ar::hall); });
    snap ("ui-6-trial-ended-panel.png", hz::LicenceInfo::trialExpired, "", nullptr);
    snap ("ui-7-pro.png", hz::LicenceInfo::pro, nullptr, [] (APP& p)
          { setParam (p, "space", ar::shimmer); setParam (p, "decay", 4.0f); setParam (p, "predelay", 3.0f); setParam (p, "duck", 75.0f);
            setParam (p, "tone", 65.0f); setParam (p, "width", 130.0f); setParam (p, "mix", 35.0f); });
    snap ("ui-8-pro-gated.png", hz::LicenceInfo::pro, nullptr, [] (APP& p) { setParam (p, "space", ar::gated); setParam (p, "decay", 3.0f); });
    snap ("ui-9-pro-reverse.png", hz::LicenceInfo::pro, nullptr, [] (APP& p) { setParam (p, "space", ar::reverse); });
    snap ("ui-10-pro-freeze.png", hz::LicenceInfo::pro, nullptr, [] (APP& p) { setParam (p, "space", ar::hall); }, [] (APP& p) { setParam (p, "freeze", 1.0f); });
    snap ("ui-11-pro-panel.png", hz::LicenceInfo::pro, "", nullptr);
    snap ("ui-12-quarter-beat.png", hz::LicenceInfo::pro, nullptr, [] (APP& p) { setParam (p, "space", ar::room); setParam (p, "decay", 1.0f); });

    // Licence states (Launch prep renders)
    auto hallSet = [] (APP& p) { setParam (p, "space", ar::hall); setParam (p, "decay", 5.0f); setParam (p, "predelay", 4.0f); };
    snap ("licence-1-free.png", hz::LicenceInfo::none, nullptr, nullptr);
    snap ("licence-2-free-panel.png", hz::LicenceInfo::none, "", nullptr);
    snap ("licence-3-trial.png", hz::LicenceInfo::trial, nullptr, hallSet);
    snap ("licence-4-trial-panel.png", hz::LicenceInfo::trial, "", hallSet);
    snap ("licence-5-trial-ended.png", hz::LicenceInfo::trialExpired, nullptr, nullptr);
    snap ("licence-6-trial-ended-panel.png", hz::LicenceInfo::trialExpired, "", nullptr);
    snap ("licence-7-licensed.png", hz::LicenceInfo::pro, nullptr, hallSet);
    snap ("licence-8-licensed-panel.png", hz::LicenceInfo::pro, "", hallSet);
    snap ("licence-9-saved-upgrade-settings.png", hz::LicenceInfo::trialExpired, nullptr, [] (APP& p)
          { setParam (p, "space", ar::shimmer); setParam (p, "decay", 4.0f); setParam (p, "predelay", 3.0f); setParam (p, "duck", 75.0f);
            setParam (p, "tone", 65.0f); setParam (p, "width", 130.0f); });
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc > 4 && juce::String (argv[1]) == "demos")
    {
        juce::StringArray files; for (int a = 4; a < argc; ++a) files.add (argv[a]);
        renderDemos (juce::File (juce::String (argv[2])), juce::String (argv[3]).getDoubleValue(), files);
        return 0;
    }
    if (argc > 3 && juce::String (argv[1]) == "levels")
    {
        // Wet-only loudness of each space (AUTO, duck as Lite) on a file, relative to PLATE.
        double fsIn = SR;
        const Buf x = loadTrimmed (juce::File (juce::String (argv[2])), 10.0, fsIn);
        const double bpm = juce::String (argv[3]).getDoubleValue();
        const int len = x.getNumSamples();
        double ref = 0, dry = 0;
        for (int c = 0; c < 2; ++c) for (int i = 0; i < len; ++i) dry += x.getSample (c, i) * x.getSample (c, i);
        for (int s = 0; s < ar::numSpaces; ++s)
        {
            Rig r (hz::LicenceInfo::pro, bpm, fsIn);
            setParam (*r.p, "space", (float) s); setParam (*r.p, "mix", 100.0f); r.reprepare (fsIn);
            const auto o = run (*r.p, x, len, 0, false);
            double e = 0; int firstBad = -1; for (int c = 0; c < 2; ++c) for (int i = 0; i < len; ++i) { const float v = o.getSample (c, i); if (! std::isfinite (v)) { if (firstBad < 0) firstBad = i; continue; } e += v * v; }
            if (firstBad >= 0) std::printf ("  non-finite output from sample %d (input there %f)\n", firstBad, x.getSample (0, firstBad));
            if (s == ar::plate) ref = e;
            std::printf ("  %-8s wet vs dry %+5.1f dB\n", ar::spaceName (s), 10 * std::log10 (e / dry));
        }
        return 0;
    }
    if (argc > 1 && juce::String (argv[1]) == "duck")   // just the ducking tests (quick tuning)
    {
        duckTests();
        std::printf ("\n%d failure(s)\n", failures);
        return failures == 0 ? 0 : 1;
    }
    if (argc > 1 && juce::String (argv[1]) == "calib-table")
    {
        // Solve, per space, for the engine RT60 that makes the measured tail end exactly on each length.
        std::printf ("inline constexpr float calibRt[6][numCalib] = {\n");
        for (int s = 0; s < 6; ++s)
        {
            std::printf ("    { ");
            for (int k = 0; k < ar::numCalib; ++k)
            {
                const double T = ar::calibTail[k];
                double rt = std::max (0.02, T - 0.02), err = 1;
                for (int it = 0; it < 10 && std::abs (err) > 0.003; ++it)
                {
                    APP::rtOverride = (float) rt;
                    const auto f = fitEnd (bandEnergy (ir (s, 120.0, 4, 1, T * 1.6 + 0.6)));
                    err = (f.endSec - T) / T;
                    rt *= juce::jlimit (0.5, 2.0, T / std::max (1e-3, f.endSec));
                    rt = std::max (0.01, rt);
                }
                std::printf ("%.4ff%s", rt, k + 1 < ar::numCalib ? ", " : "");
                std::fflush (stdout);
                if (std::abs (err) > 0.01) std::fprintf (stderr, "  %s %.3f s: residual %.1f %%\n", ar::spaceName (s), T, err * 100);
            }
            std::printf (" },   // %s\n", ar::spaceName (s));
        }
        std::printf ("};\n");
        return 0;
    }
    const juce::File outDir (argc > 1 ? juce::String (argv[1]) : juce::File::getCurrentWorkingDirectory().getFullPathName());
    outDir.createDirectory();
    const bool quick = argc > 2 && juce::String (argv[2]) == "quick";

    decayTests();
    rateAndLevelTests();
    preDelayTests();
    densityRingingStereo();
    lowCutTests();
    duckTests();
    if (! quick)
    {
        stabilityTests();
        clickTests();
        cpuTests();
    }
    stateTests();
    liteTests();
    if (! quick) snapshots (outDir);

    std::printf ("\n== Licence keys ==\n");
    for (int a = 2; a < argc; ++a)
    {
        const juce::String arg (argv[a]);
        if (! arg.containsChar ('=')) continue;
        const auto name = arg.upToFirstOccurrenceOf ("=", false, false);
        const auto info = hz::Licence::verify (arg.fromFirstOccurrenceOf ("=", false, false), "autoreverbpro");
        const char* kinds[] = { "none", "trial", "pro", "trialExpired", "invalid", "saveFailed" };
        std::printf ("  key %-14s -> %-12s daysLeft=%d\n", name.toRawUTF8(), kinds[(int) info.kind], info.daysLeft());
        hz::LicenceInfo::Kind want = hz::LicenceInfo::invalid;
        if (name == "pro") want = hz::LicenceInfo::pro;
        else if (name == "trial") want = hz::LicenceInfo::trial;
        else if (name == "expired") want = hz::LicenceInfo::trialExpired;
        else if (name == "all") want = hz::LicenceInfo::pro;
        check (info.kind == want, "key " + name + " -> " + kinds[(int) info.kind]);
    }

    hztest::runLicenceTests<APP> ("autoreverbpro", [] (APP& p) { return p.proUnlocked.load(); });
    hztest::renderLicenceStates<APP> ("autoreverbpro", outDir);
    hztest::runSavedSettingsTests<APP> ([] (APP& p)
    {
        setParam (p, "space", ar::shimmer); setParam (p, "decay", 4.0f); setParam (p, "predelay", 3.0f);
        setParam (p, "duck", 75.0f); setParam (p, "tone", 65.0f); setParam (p, "width", 130.0f); setParam (p, "freeze", 1.0f);
    });
    {
        // no licence + saved upgrade settings: flagged, and what runs is still Lite
        APP p; p.setLicenceInfo ({});
        setParam (p, "space", ar::shimmer); setParam (p, "decay", 4.0f);
        const auto e = p.resolve (p.proUnlocked.load(), 124.0);
        hztest::expect (p.usesUpgradeSettings() && e.space == ar::gate::liteFallbackSpace && e.decayAuto,
                        "no licence + saved SHIMMER / decay: flagged, plays as Lite (PLATE, AUTO)");
    }
    const int licenceResult = hztest::finish();

    std::printf ("\n%s: %d failure(s)\n", failures == 0 ? "ALL PASS" : "FAILURES", failures);
    return failures == 0 && licenceResult == 0 ? 0 : 1;
}

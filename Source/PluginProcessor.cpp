#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Calibration.h"
#include <cmath>

AutoReverbProAudioProcessor::AutoReverbProAudioProcessor()
    : AudioProcessor (BusesProperties()
          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "AutoReverbProState", createParameterLayout())
{
    pSpace  = apvts.getRawParameterValue ("space");
    pSize   = apvts.getRawParameterValue ("size");
    pMix    = apvts.getRawParameterValue ("mix");
    pDecay  = apvts.getRawParameterValue ("decay");
    pPre    = apvts.getRawParameterValue ("predelay");
    pDuck   = apvts.getRawParameterValue ("duck");
    pTone   = apvts.getRawParameterValue ("tone");
    pWidth  = apvts.getRawParameterValue ("width");
    pFreeze = apvts.getRawParameterValue ("freeze");
    setLicenceInfo (licence.check());
    licenceWatcher = std::make_unique<hz::LicenceWatcher> (licence, licenceInfo, [this] (const hz::LicenceInfo& i) { setLicenceInfo (i); });
}

bool AutoReverbProAudioProcessor::usesUpgradeSettings() const
{
    using namespace ar;
    auto differs = [] (float v, float lite) { return std::abs (v - lite) > 0.05f; };
    return ! gate::spaceAllowed ((int) pSpace->load(), false)
        || (gate::proDecay    && (int) pDecay->load() != 0)
        || (gate::proPreDelay && (int) pPre->load() != 0)
        || (gate::proDuck     && differs (pDuck->load(),  liteDuck))
        || (gate::proTone     && differs (pTone->load(),  liteTone))
        || (gate::proWidth    && differs (pWidth->load(), liteWidth))
        || (gate::proFreeze   && pFreeze->load() > 0.5f);
}

juce::AudioProcessorValueTreeState::ParameterLayout AutoReverbProAudioProcessor::createParameterLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout l;
    auto id = [] (const char* s) { return ParameterID { s, 1 }; };
    auto pct = [] { return AudioParameterFloatAttributes().withLabel ("%")
                        .withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v)); }); };

    l.add (std::make_unique<AudioParameterChoice> (id ("space"), "Space",
               StringArray { "Room", "Plate", "Hall", "Chamber", "Spring", "Shimmer", "Gated", "Reverse" }, ar::plate));
    l.add (std::make_unique<AudioParameterFloat> (id ("size"), "Size", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f, pct()));
    l.add (std::make_unique<AudioParameterFloat> (id ("mix"), "Mix", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 25.0f, pct()));
    l.add (std::make_unique<AudioParameterChoice> (id ("decay"), "Decay",
               StringArray { "Auto", "1/4 beat", "1/2 beat", "1 beat", "2 beats", "4 beats", "8 beats" }, 0));
    l.add (std::make_unique<AudioParameterChoice> (id ("predelay"), "Pre-delay",
               StringArray { "Auto", "Off", "1/64", "1/32", "1/16", "1/8" }, 0));
    l.add (std::make_unique<AudioParameterFloat> (id ("duck"), "Duck", NormalisableRange<float> (0.0f, 100.0f, 0.1f), ar::liteDuck, pct()));
    l.add (std::make_unique<AudioParameterFloat> (id ("tone"), "Tone", NormalisableRange<float> (0.0f, 100.0f, 0.1f), ar::liteTone, pct()));
    l.add (std::make_unique<AudioParameterFloat> (id ("width"), "Width", NormalisableRange<float> (0.0f, 150.0f, 0.1f), ar::liteWidth, pct()));
    l.add (std::make_unique<AudioParameterBool> (id ("freeze"), "Freeze", false));
    return l;
}

// ── Resolve: Lite/Pro gate + AUTO -> the numbers that actually run ────────────
int AutoReverbProAudioProcessor::autoDecayStep (int space, float size01, double bpm)
{
    const auto& v = ar::voice (space);
    if (v.autoSec <= 0.0f)
    {
        // GATED / REVERSE: a beat length is the effect. SIZE 50 % = the space's own; every 25 % either side = one step.
        const int shift = (int) std::floor ((size01 * 100.0f - 50.0f) / 25.0f + 0.5f);
        return juce::jlimit (0, ar::numDecaySteps - 1, v.autoDecay + shift);
    }
    // Natural spaces: a musical length for the space (x0.5 at SIZE 0 %, x2 at 100 %), snapped to the nearest beat step.
    const double want = v.autoSec * std::pow (2.0, (size01 - 0.5) * 2.0) / (ar::beatMs (bpm) / 1000.0);
    int best = 0; double bestD = 1.0e9;
    for (int k = 0; k < ar::numDecaySteps; ++k)
    {
        const double d = std::abs (std::log2 (ar::decayStepsBeats[k] / want));
        if (d < bestD - 1.0e-9) { bestD = d; best = k; }
    }
    return best;
}

AutoReverbProAudioProcessor::Effective AutoReverbProAudioProcessor::resolve (bool pro, double bpm) const
{
    using namespace ar;
    Effective e;
    e.bpm   = juce::jlimit (20.0, 400.0, bpm);
    e.space = gate::effectiveSpace (juce::jlimit (0, numSpaces - 1, (int) pSpace->load()), pro);
    e.size  = juce::jlimit (0.0f, 1.0f, pSize->load() / 100.0f);
    e.mix   = juce::jlimit (0.0f, 1.0f, pMix->load() / 100.0f);
    const auto& v = voice (e.space);

    const int d = (pro || ! gate::proDecay) ? (int) pDecay->load() : 0;
    e.decayAuto  = d <= 0 || d > numDecaySteps;
    e.decayStep  = e.decayAuto ? autoDecayStep (e.space, e.size, e.bpm) : d - 1;
    e.decayBeats = decayStepsBeats[e.decayStep];

    const int p = (pro || ! gate::proPreDelay) ? (int) pPre->load() : 0;
    e.preAuto  = p <= 0 || p > numPreSteps;
    e.preStep  = p - 1;
    if (e.preAuto)
    {
        e.preStep = v.autoPre;   // AUTO: the space's own, but never more than an eighth of the tail
        while (e.preStep > 0 && preStepsBeats[e.preStep] > 0.125f * e.decayBeats) --e.preStep;
    }
    e.preBeats = preStepsBeats[e.preStep];

    e.duck   = ((pro || ! gate::proDuck)  ? pDuck->load()  : liteDuck)  / 100.0f;
    e.tone   = ((pro || ! gate::proTone)  ? pTone->load()  : liteTone)  / 100.0f;
    e.width  = ((pro || ! gate::proWidth) ? pWidth->load() : liteWidth) / 100.0f;
    e.freeze = (pro || ! gate::proFreeze) && pFreeze->load() > 0.5f;
    e.duck = juce::jlimit (0.0f, 1.0f, e.duck); e.tone = juce::jlimit (0.0f, 1.0f, e.tone); e.width = juce::jlimit (0.0f, 1.5f, e.width);

    // The tail ENDS (-60 dB) this long after the hit: decay beats x one beat. Pre-delay is inside that time.
    const double beatSec = beatMs (e.bpm) / 1000.0;
    e.endSec = (float) (e.decayBeats * beatSec);
    e.preSec = (float) std::min ((double) e.preBeats * beatSec, 0.5 * e.endSec);
    if (v.shape == 0)
        e.rtSec = rtOverride > 0.0f ? rtOverride : ar::rtForTail (e.space, e.endSec - e.preSec, v.diffMs);   // Calibration.h
    else
        e.rtSec = v.rtMul * e.endSec;   // GATED / REVERSE: a long dense tail cut by the shape at endSec

    e.sizeScale = 0.72f + 0.6f * e.size;
    if (v.shape == 0)
    {
        // A tail can't end sooner than the space can ring out: very short decays shrink the room to fit.
        const float longest = v.lineMaxMs * 0.001f * e.sizeScale;
        e.sizeScale *= juce::jlimit (0.1f, 1.0f, rtFitRatio * e.rtSec / longest);
    }
    e.hfRatio   = juce::jlimit (0.1f, 1.0f, v.hfRatio * std::pow (2.0f, (e.tone - 0.5f) * 2.0f));
    e.outLpHz   = juce::jlimit (1500.0f, 18000.0f, v.outLpHz * std::pow (2.0f, (e.tone - 0.5f) * 3.0f));
    return e;
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────
void AutoReverbProAudioProcessor::prepareToPlay (double sampleRate, int)
{
    sr = sampleRate;
    for (auto& s : slots) { s.eng.prepare (sampleRate); s.state = idle; s.outGain = 0.0f; }

    int sz = 16; while (sz < (int) (sampleRate * 2.2) + 8) sz <<= 1;   // 1/8 note at 20 BPM = 1.5 s
    for (auto& b : preBuf) b.assign ((size_t) sz, 0.0f);
    preMask = sz - 1; preW = 0;
    preFadeStep = (float) (1.0 / (0.03 * sampleRate));

    const auto e = resolve (proUnlocked.load(), lastBpm.load());
    cur = 0;
    auto& s = slots[0];
    s.eng.t.rtSec = e.rtSec; s.eng.t.hfRatio = e.hfRatio; s.eng.t.outLpHz = e.outLpHz;
    s.eng.t.sizeScale = e.sizeScale; s.eng.t.freeze = e.freeze ? 1.0f : 0.0f; s.eng.t.input = 1.0f;
    s.eng.configure (e.space, e.sizeScale, false);
    s.state = active; s.outGain = 1.0f;
    preCur = prePrev = (int) std::lround (e.preSec * sampleRate); preFade = 1.0f;

    duckMs = 0.0f; duckRefDb = -100.0f; duckGr = 0.0f; duckSus = 0.0f; onFast = onSlow = 0.0f;
    since = revSince = 1.0e9; gateEnv = revEnv = 0.0f; shapeBypass = e.freeze ? 1.0f : 0.0f;
    sMix.reset (sampleRate, 0.05);   sMix.setCurrentAndTargetValue (e.mix);
    sWidth.reset (sampleRate, 0.05); sWidth.setCurrentAndTargetValue (e.width);
    sDuck.reset (sampleRate, 0.1);   sDuck.setCurrentAndTargetValue (e.duck * ar::maxDuckDb);
    freePpq = 0.0;
    firstBlock = true;
    binIdx = numBins; binWet = binDuck = 0.0f;
}

bool AutoReverbProAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo()) return false;
    return layouts.getMainInputChannelSet() == out;
}

// ── Audio ─────────────────────────────────────────────────────────────────────
void AutoReverbProAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    const int numCh = juce::jmin (2, getTotalNumInputChannels(), getTotalNumOutputChannels(), buffer.getNumChannels());
    for (int c = numCh; c < buffer.getNumChannels(); ++c) buffer.clear (c, 0, n);
    if (numCh <= 0 || n <= 0) return;

    // Tempo: the host's, or a free-running 120 BPM clock in the standalone app / hosts without tempo.
    double bpm = lastBpm.load();
    bool fromHost = false;
    if (wrapperType == wrapperType_Standalone)
    {
        bpm = 120.0;
        freePpq += n * bpm / (60.0 * sr);
    }
    else if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
            if (auto b = pos->getBpm()) { bpm = *b; fromHost = true; }
    bpm = juce::jlimit (20.0, 400.0, bpm);
    lastBpm.store (bpm);
    hostTempo.store (fromHost);

    // Locked features fall back to their Lite values, so automation or an old session can't bypass the lock.
    const bool pro = proUnlocked.load();
    const Effective e = resolve (pro, bpm);
    const auto& v = ar::voice (e.space);

    // Space change: the old space stops being fed and spills over for 0.4 s while the new one fills up.
    if (slots[(size_t) cur].eng.space != e.space)
    {
        for (int k = 1; k < 3; ++k)
        {
            const int j = (cur + k) % 3;
            if (slots[(size_t) j].state != idle) continue;
            auto& old = slots[(size_t) cur];
            old.state = releasing; old.eng.t.input = 0.0f; old.eng.t.freeze = 0.0f;
            old.outStep = (float) (1.0 / (0.4 * sr));
            auto& s = slots[(size_t) j];
            s.eng.t.rtSec = e.rtSec; s.eng.t.hfRatio = e.hfRatio; s.eng.t.outLpHz = e.outLpHz;
            s.eng.t.sizeScale = e.sizeScale; s.eng.t.freeze = 0.0f; s.eng.t.input = 1.0f;
            s.eng.configure (e.space, e.sizeScale);
            s.state = active; s.outGain = 1.0f;
            cur = j;
            break;
        }
    }
    {
        auto& t = slots[(size_t) cur].eng.t;
        t.rtSec = e.rtSec; t.hfRatio = e.hfRatio; t.outLpHz = e.outLpHz; t.sizeScale = e.sizeScale;
        t.freeze = e.freeze ? 1.0f : 0.0f; t.input = 1.0f;
    }

    const int preTarget = (int) std::lround (e.preSec * sr);
    if (firstBlock)
    {
        // prepareToPlay can't see the host tempo: start exactly on it instead of gliding there.
        slots[(size_t) cur].eng.snap();
        preCur = prePrev = preTarget; preFade = 1.0f;
        sMix.setCurrentAndTargetValue (e.mix); sWidth.setCurrentAndTargetValue (e.width); sDuck.setCurrentAndTargetValue (e.duck * ar::maxDuckDb);
        firstBlock = false;
    }
    if (preTarget != preCur && preFade >= 1.0f) { prePrev = preCur; preCur = preTarget; preFade = 0.0f; }

    sMix.setTargetValue (e.mix);
    sWidth.setTargetValue (e.width);

    // Ducking: a gentle dip while the source is sustained and loud, back up smoothly when it stops.
    // RMS detection, and it only goes full depth once the source has stayed loud for ~250 ms: a vocal line
    // or a held chord ducks the tail, single drum hits barely move it (no pumping on every transient).
    // Release is program dependent: slow while the source keeps playing (no pumping between hits),
    // quick (tempo-tied) once the source has really stopped, so the tail blooms in the gaps.
    sDuck.setTargetValue (e.duck * ar::maxDuckDb);
    const double beatSec = 60.0 / e.bpm;
    const float fsf = (float) sr;
    const float kMsA = 1.0f - std::exp (-1.0f / (0.010f * fsf)), kMsR = 1.0f - std::exp (-1.0f / (0.040f * fsf));
    const float refFall = 4.0f / fsf;                                         // dB per sample (4 dB/s)
    const float kGrA = 1.0f - std::exp (-1.0f / (0.035f * fsf));
    const float kGrSlow = 1.0f - std::exp (-1.0f / ((float) juce::jlimit (0.15, 0.40, 0.5 * beatSec) * fsf));
    const float kGrFast = 1.0f - std::exp (-1.0f / ((float) juce::jlimit (0.04, 0.12, beatSec / 8.0) * fsf));
    const float susFull = 0.25f * fsf;                                        // loud this long = sustained
    const float kFastA = 1.0f - std::exp (-1.0f / (0.0005f * fsf)), kFastR = 1.0f - std::exp (-1.0f / (0.02f * fsf));
    const float kSlow  = 1.0f - std::exp (-1.0f / (0.04f * fsf));
    const double holdoff = 0.25 * beatSec * sr;
    const float shapeK = 1.0f - std::exp (-1.0f / (0.003f * (float) sr));
    const float bypK = 1.0f - std::exp (-1.0f / (0.05f * (float) sr));
    const double T = (double) e.endSec * sr, gRel = std::min (0.3 * T, 0.06 * sr);

    float* data[2] = { buffer.getWritePointer (0), buffer.getWritePointer (numCh > 1 ? 1 : 0) };
    float blkIn = 0.0f, blkWet = 0.0f, blkDuck = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        const float inL = data[0][i], inR = numCh > 1 ? data[1][i] : inL;
        const float x = juce::jmax (std::abs (inL), std::abs (inR));

        // Hits (for GATED / REVERSE and the display).
        onFast += (x - onFast) * (x > onFast ? kFastA : kFastR);
        onSlow += (x - onSlow) * kSlow;
        since += 1.0; revSince += 1.0;
        if (onFast > 0.004f && onFast > 2.0f * onSlow && since > holdoff)
        {
            since = 0.0;
            if (revSince > T + 0.01 * sr) revSince = 0.0;   // a reverse swell always finishes before the next one starts
            for (auto& b : bins) { b.wet.store (0.0f, std::memory_order_relaxed); b.duck.store (0.0f, std::memory_order_relaxed); }
            const float span = (float) juce::jmax (beatSec, 1.25 * e.endSec);
            binSpanSec.store (span);
            binSamples = span * sr / numBins;
            binIdx = 0; binWet = binDuck = 0.0f;
            binGen.fetch_add (1);
        }

        // Ducking
        duckMs += (x * x - duckMs) * (x * x > duckMs ? kMsA : kMsR);
        const float envDb = 10.0f * std::log10 (duckMs + 1.0e-12f);
        duckRefDb = juce::jmax (envDb, juce::jmax (-60.0f, duckRefDb - refFall));
        float amt = juce::jlimit (0.0f, 1.0f, (envDb - duckRefDb + 18.0f) / 12.0f);   // 0 at 18 dB under the recent level, 1 within 6 dB
        duckSus = envDb > duckRefDb - 12.0f ? juce::jmin (susFull, duckSus + 1.0f) : juce::jmax (0.0f, duckSus - 2.0f);
        const float sus = duckSus / susFull;
        amt *= sus * sus * (3.0f - 2.0f * sus);
        amt = envDb < -55.0f ? 0.0f : amt * amt * (3.0f - 2.0f * amt);
        const float grT = amt * sDuck.getNextValue();
        const bool stopped = 20.0f * std::log10 (onFast + 1.0e-9f) < duckRefDb - 30.0f;   // the source has really gone quiet
        duckGr += (grT - duckGr) * (grT > duckGr ? kGrA : (stopped ? kGrFast : kGrSlow));
        const float duckG = std::pow (10.0f, -duckGr / 20.0f);

        // GATED: open until the end, closing over the last bit. REVERSE: swells up to the end, then cuts.
        double gT = 0.0, rT = 0.0;
        if (since < T - gRel) gT = 1.0;
        else if (since < T) gT = 0.5 * (1.0 + std::cos (juce::MathConstants<double>::pi * (since - (T - gRel)) / gRel));
        if (revSince < T) { const double u = revSince / T; rT = u * u; }
        else if (revSince < T + 0.01 * sr) rT = 0.5 * (1.0 + std::cos (juce::MathConstants<double>::pi * (revSince - T) / (0.01 * sr)));
        if ((float) gT >= gateEnv) gateEnv += ((float) gT - gateEnv) * shapeK;   // opens over ~3 ms
        else                       gateEnv = (float) gT;                          // closes along the cosine
        revEnv += ((float) rT - revEnv) * shapeK;
        shapeBypass += ((e.freeze ? 1.0f : 0.0f) - shapeBypass) * bypK;

        // Pre-delay
        preBuf[0][(size_t) preW] = inL; preBuf[1][(size_t) preW] = inR;
        auto readPre = [&] (int c, int d) { return preBuf[c][(size_t) ((preW - d) & preMask)]; };
        float pl = readPre (0, preCur), pr = readPre (1, preCur);
        if (preFade < 1.0f)
        {
            const float f = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * preFade);
            pl = readPre (0, prePrev) + (pl - readPre (0, prePrev)) * f;
            pr = readPre (1, prePrev) + (pr - readPre (1, prePrev)) * f;
            preFade = juce::jmin (1.0f, preFade + preFadeStep);
        }
        preW = (preW + 1) & preMask;

        // Engines
        float wl = 0.0f, wr = 0.0f;
        for (auto& s : slots)
        {
            if (s.state == idle) continue;
            float yl, yr;
            s.eng.process (pl, pr, yl, yr);
            float g = 1.0f;
            const int shape = ar::voice (s.eng.space).shape;
            if (shape == 1) g = gateEnv; else if (shape == 2) g = revEnv;
            g += (1.0f - g) * shapeBypass;
            if (s.state == releasing)
            {
                const float u = s.outGain;
                g *= u * u * (3.0f - 2.0f * u);
                s.outGain -= s.outStep;
                if (s.outGain <= 0.0f) { s.state = idle; s.outGain = 0.0f; }
            }
            wl += yl * g; wr += yr * g;
        }
        if (! std::isfinite (wl) || ! std::isfinite (wr))
        {
            wl = wr = 0.0f;
            for (auto& s : slots) if (s.state != idle) s.eng.configure (s.eng.space, e.sizeScale);
        }

        wl *= duckG; wr *= duckG;
        const float wd = sWidth.getNextValue();
        const float mid = 0.5f * (wl + wr), side = 0.5f * (wl - wr) * wd;
        wl = mid + side; wr = mid - side;

        // MIX: dry stays at full level up to 50 %, the reverb reaches full level at 50 %.
        const float m = sMix.getNextValue();
        const float dryG = juce::jmin (1.0f, 2.0f * (1.0f - m)), wetG = juce::jmin (1.0f, 2.0f * m);
        if (numCh > 1)
        {
            data[0][i] = inL * dryG + wl * wetG;
            data[1][i] = inR * dryG + wr * wetG;
        }
        else data[0][i] = inL * dryG + 0.5f * (wl + wr) * wetG;

        const float wAbs = juce::jmax (std::abs (wl), std::abs (wr));
        blkIn = juce::jmax (blkIn, x); blkWet = juce::jmax (blkWet, wAbs); blkDuck = juce::jmax (blkDuck, duckGr);
        if (binIdx < numBins)
        {
            binWet = juce::jmax (binWet, wAbs); binDuck = juce::jmax (binDuck, duckGr);
            const int idx = (int) (since / binSamples);
            if (idx != binIdx)
            {
                bins[(size_t) binIdx].wet.store (binWet, std::memory_order_relaxed);
                bins[(size_t) binIdx].duck.store (binDuck, std::memory_order_relaxed);
                binIdx = idx; binWet = binDuck = 0.0f;
            }
        }
    }
    (void) v;
    inPeak.store (blkIn); wetPeak.store (blkWet); duckDb.store (blkDuck);
    secsSinceHit.store (since / sr);
}

// ── State ─────────────────────────────────────────────────────────────────────
void AutoReverbProAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    if (auto xml = state.createXml()) copyXmlToBinary (*xml, destData);
}

void AutoReverbProAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml && xml->hasTagName (apvts.state.getType()))
        apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* AutoReverbProAudioProcessor::createEditor() { return new AutoReverbProAudioProcessorEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new AutoReverbProAudioProcessor(); }

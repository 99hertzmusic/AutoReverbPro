#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include <vector>
#include "Features.h"
#include "ReverbEngine.h"
#include "Licence/Licence.h"

// AutoReverb Pro: the reverb that times itself to your track.
//  Lite (free): reads the tempo and sets decay + pre-delay so the tail ends on the beat, SIZE, MIX,
//  ROOM + PLATE, built-in low cut and auto ducking (fixed amount).
//  Pro (key or 7-day trial): HALL / CHAMBER / SPRING / SHIMMER / GATED / REVERSE, DECAY and PRE-DELAY
//  in beats, DUCK amount, TONE, WIDTH, FREEZE. The split itself lives in Features.h (ar::gate).
class AutoReverbProAudioProcessor : public juce::AudioProcessor
{
public:
    AutoReverbProAudioProcessor();
    ~AutoReverbProAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "AutoReverb Pro"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 12.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    // ── Licence ─────────────────────────────────────────────────────────────
    hz::Licence licence { "autoreverbpro" };
    hz::LicenceInfo licenceInfo;                 // message thread
    std::atomic<bool> proUnlocked { false };     // audio thread reads this
    std::atomic<int> licenceVersion { 0 };       // bumps on every change, so open editors can follow
    void setLicenceInfo (const hz::LicenceInfo& i) { licenceInfo = i; proUnlocked.store (i.unlocked()); ++licenceVersion; }
    std::unique_ptr<hz::LicenceWatcher> licenceWatcher;   // picks up keys saved by other instances / trial expiry
    // True if the current settings include upgrade-only values (kept in the session, played as free).
    bool usesUpgradeSettings() const;

    // ── What actually runs, after the Lite/Pro gate and AUTO are resolved ───
    struct Effective
    {
        int   space = ar::plate;
        float size = 0.5f, mix = 0.25f;
        bool  decayAuto = true; int decayStep = 3; float decayBeats = 2.0f;
        bool  preAuto = true;   int preStep = 2;   float preBeats = 0.125f;
        float duck = 0.5f, tone = 0.5f, width = 1.0f;
        bool  freeze = false;
        double bpm = 120.0;
        float endSec = 1.0f, preSec = 0.0625f, rtSec = 0.9f;   // tail end (from the hit), pre-delay, engine RT60
        float sizeScale = 1.0f, hfRatio = 0.5f, outLpHz = 10000.0f;
    };
    Effective resolve (bool pro, double bpm) const;
    static int autoDecayStep (int space, float size01, double bpm);
    static inline float rtOverride = 0.0f;        // test hook: force the engine RT60 (calibration)
    static inline float rtFitRatio = 0.35f;       // longest line <= this x RT60

    // ── Tempo ──────────────────────────────────────────────────────────────
    std::atomic<double> lastBpm { 120.0 };
    std::atomic<bool>   hostTempo { false };     // false = standalone / no tempo from the host (120 BPM clock)

    // ── Meters + the hero display ──────────────────────────────────────────
    std::atomic<float> inPeak { 0.0f }, wetPeak { 0.0f }, duckDb { 0.0f };
    static constexpr int numBins = 200;          // wet level after the last hit, across the display span
    struct Bin { std::atomic<float> wet { 0 }, duck { 0 }; };
    std::array<Bin, numBins> bins;
    std::atomic<float> binSpanSec { 1.25f };     // what the bins cover
    std::atomic<int>   binGen { 0 };             // bumps on every hit
    std::atomic<double> secsSinceHit { 1.0e9 };

private:
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    std::atomic<float>* pSpace = nullptr; std::atomic<float>* pSize = nullptr; std::atomic<float>* pMix = nullptr;
    std::atomic<float>* pDecay = nullptr; std::atomic<float>* pPre = nullptr; std::atomic<float>* pDuck = nullptr;
    std::atomic<float>* pTone = nullptr;  std::atomic<float>* pWidth = nullptr; std::atomic<float>* pFreeze = nullptr;

    double sr = 44100.0;

    // Three engine slots: the active one, and up to two tails spilling over after a space change.
    enum SlotState { idle = 0, active, releasing };
    struct Slot { ar::Engine eng; int state = idle; float outGain = 0.0f, outStep = 0.0f; };
    std::array<Slot, 3> slots;
    int cur = 0;

    // Pre-delay (stereo, before the engines). Changes crossfade between two taps.
    std::vector<float> preBuf[2]; int preMask = 0, preW = 0;
    int preCur = 0, prePrev = 0; float preFade = 1.0f, preFadeStep = 0.001f;

    // Ducking + hit detection on the dry input.
    float duckMs = 0.0f, duckRefDb = -100.0f, duckGr = 0.0f, duckSus = 0.0f;
    float onFast = 0.0f, onSlow = 0.0f;
    double since = 1.0e9;              // samples since the last hit
    double revSince = 1.0e9;           // samples since the current REVERSE swell started
    float gateEnv = 0.0f, revEnv = 0.0f, shapeBypass = 0.0f;
    juce::SmoothedValue<float> sMix, sWidth, sDuck;
    double freePpq = 0.0;
    bool firstBlock = true;
    int binIdx = 0; float binWet = 0.0f, binDuck = 0.0f; double binSamples = 1.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AutoReverbProAudioProcessor)
};

#pragma once
// AutoReverb Pro: spaces, stepped dial values and THE Lite/Pro split.
// Everything about what is free and what needs Pro lives in this file. To move a feature,
// flip its flag below: the processor (audio), the editor (padlocks) and the tests all read it.

namespace ar
{
enum Space { room = 0, plate, hall, chamber, spring, shimmer, gated, reverse, numSpaces };

inline const char* spaceName (int s)
{
    static const char* n[] = { "ROOM", "PLATE", "HALL", "CHAMBER", "SPRING", "SHIMMER", "GATED", "REVERSE" };
    return n[s >= 0 && s < numSpaces ? s : 0];
}

// Tempo maths: one beat (a quarter note) in ms = 60000 / BPM.
inline double beatMs (double bpm) { return 60000.0 / (bpm > 1.0 ? bpm : 120.0); }

// Stepped dials. Position 0 of DECAY and PRE-DELAY is AUTO (set from the tempo and SIZE).
inline constexpr float decayStepsBeats[] = { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f };
inline constexpr int   numDecaySteps = 6;                 // parameter: 0 = AUTO, 1..6 = the steps above
inline constexpr float preStepsBeats[] = { 0.0f, 1.0f / 16.0f, 1.0f / 8.0f, 1.0f / 4.0f, 1.0f / 2.0f };   // OFF, 1/64, 1/32, 1/16, 1/8 note
inline constexpr int   numPreSteps = 5;                   // parameter: 0 = AUTO, 1..5 = the steps above
inline const char* preStepName (int i)
{
    static const char* n[] = { "OFF", "1/64", "1/32", "1/16", "1/8" };
    return n[i >= 0 && i < numPreSteps ? i : 0];
}

// Fixed values Lite runs with.
inline constexpr float liteDuck  = 25.0f;    // %  (100 % = 18 dB of ducking at full input level; Lite = a gentle 4.5 dB)
inline constexpr float liteTone  = 50.0f;    // %  (the space's own voicing)
inline constexpr float liteWidth = 100.0f;   // %
inline constexpr float maxDuckDb = 18.0f;

// ── THE LITE / PRO SPLIT ──────────────────────────────────────────────────────
// true = needs Pro (licence key or 7-day trial). false = free in Lite.
// Always free: AUTO tempo timing (decay + pre-delay), SIZE, MIX, the built-in low cut, auto ducking (fixed).
namespace gate
{
    inline constexpr bool proSpace[numSpaces] = {
        false,  // ROOM
        false,  // PLATE
        true,   // HALL
        true,   // CHAMBER
        true,   // SPRING
        true,   // SHIMMER
        true,   // GATED
        true    // REVERSE
    };
    inline constexpr bool proDecay    = true;   // Lite: AUTO (from tempo + SIZE)
    inline constexpr bool proPreDelay = true;   // Lite: AUTO (from tempo)
    inline constexpr bool proDuck     = true;   // Lite: fixed liteDuck
    inline constexpr bool proTone     = true;   // Lite: liteTone
    inline constexpr bool proWidth    = true;   // Lite: liteWidth
    inline constexpr bool proFreeze   = true;   // Lite: off

    inline constexpr int liteFallbackSpace = plate;
    inline bool spaceAllowed (int s, bool pro) { return pro || ! proSpace[s >= 0 && s < numSpaces ? s : 0]; }
    // What a locked space falls back to in Lite.
    inline int  effectiveSpace (int s, bool pro) { return spaceAllowed (s, pro) ? s : liteFallbackSpace; }
}
} // namespace ar

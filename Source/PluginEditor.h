#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"

class HardwareLookAndFeel;

// Row of hardware lamp keys bound to a choice parameter.
class SegmentControl : public juce::Component
{
public:
    SegmentControl (juce::RangedAudioParameter& param, juce::StringArray labels);
    void resized() override;
    void setLocked (int value, bool locked);          // padlock on that key
    void showValue (int value);                       // which key is lit (the value that actually runs)
    std::function<bool (int value)> allowValue;       // return false to refuse a click (locked Pro value)
    int gap = 8;
private:
    juce::OwnedArray<juce::TextButton> buttons;
    juce::ParameterAttachment attachment;
    int shown = -1;
};

// Dial that can be padlocked: a click on a locked dial opens the Pro panel instead of turning it.
class Dial : public juce::Slider
{
public:
    bool locked = false;
    std::function<void()> onLockedClick;
    void mouseDown (const juce::MouseEvent& e) override        { if (locked) { if (onLockedClick) onLockedClick(); return; } Slider::mouseDown (e); }
    void mouseDrag (const juce::MouseEvent& e) override        { if (! locked) Slider::mouseDrag (e); }
    void mouseUp (const juce::MouseEvent& e) override          { if (! locked) Slider::mouseUp (e); }
    void mouseDoubleClick (const juce::MouseEvent& e) override { if (! locked) Slider::mouseDoubleClick (e); }
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override { if (! locked) Slider::mouseWheelMove (e, w); }
};

// Upgrade panel: what Pro adds, start the 7-day trial, paste a key, or buy.
class ProPanel : public juce::Component
{
public:
    explicit ProPanel (AutoReverbProAudioProcessor& p);
    void show (const juce::String& reason);
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;   // click outside the card closes it
    std::function<void()> onLicenceChanged;
private:
    void refresh();
    juce::Rectangle<int> card() const;
    AutoReverbProAudioProcessor& proc;
    juce::String reason, message;
    bool messageIsError = false, busy = false;
    juce::TextEditor emailBox, keyBox;
    juce::ToggleButton subscribe { "Send me tips and offers" };
    juce::TextButton trialButton { "START 7-DAY FREE TRIAL" }, unlockButton { "UNLOCK" },
                     buyButton { "BUY PRO" }, closeButton { "CLOSE" };
};

// The hero: the decay envelope against a beat grid (it ends on the beat), the real tail after the
// last hit drawn over it, and the ducking.
class DecayView : public juce::Component
{
public:
    explicit DecayView (AutoReverbProAudioProcessor& p) : proc (p) {}
    void paint (juce::Graphics&) override;
    AutoReverbProAudioProcessor::Effective e;
private:
    AutoReverbProAudioProcessor& proc;
};

class AutoReverbProAudioProcessorEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit AutoReverbProAudioProcessorEditor (AutoReverbProAudioProcessor&);
    ~AutoReverbProAudioProcessorEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void timerCallback() override;
    void openPro (const juce::String& reason) { proPanel.show (reason); }

private:
    struct Knob
    {
        Dial slider;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> att;
        juce::String label, paramId;
        juce::StringArray stepLabels;   // empty = continuous dial
        juce::String value;             // text under the dial
    };
    Knob& addKnob (const juce::String& id, const juce::String& label, juce::StringArray steps = {});
    Knob* knob (const juce::String& id);
    void renderBackground();
    void updateLocks();

    AutoReverbProAudioProcessor& proc;
    std::unique_ptr<HardwareLookAndFeel> laf;

    SegmentControl spaceKeys;
    juce::TextButton freezeButton { "FREEZE" };
    DecayView decayView;
    juce::TextButton licenceButton;
    ProPanel proPanel;
    juce::OwnedArray<Knob> knobs;

    juce::Image background;
    juce::Rectangle<int> ledArea, meterArea, secSpace, secAuto, secPro, secOut;
    juce::String ledLabel, ledText, ledRightLabel, ledRightText;
    float mIn = 0.0f, mWet = 0.0f, mDuck = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AutoReverbProAudioProcessorEditor)
};

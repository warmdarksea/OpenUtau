// Minimal editor: connection status, project info, the synchronized track list
// and the playback mode (all tracks through OpenUtau's mixer model, or one
// track raw pre-fader for per-track DAW routing).
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "BridgeProcessor.h"

namespace oubridge {

class BridgeEditor final : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit BridgeEditor(BridgeProcessor& processor);
    ~BridgeEditor() override;

    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    void timerCallback() override;
    void refresh();
    void rebuildTrackChoices();
    // HiDPI: resolve the effective scale and apply it as the editor transform.
    // VST3 and CLAP hosts hand a content scale to the wrapper; VST2 has no such
    // channel on Linux, so "Auto" falls back to the display's own scale there.
    float automaticScale() const;
    void applyScale();

    BridgeProcessor& bridge;
    int lastRevision = -1;

    juce::Label title;
    juce::Label status;
    juce::Label project;
    juce::Label trackHeader;
    juce::TextEditor trackList;
    juce::Label modeLabel;
    juce::ComboBox trackChoice;
    juce::Label scaleLabel;
    juce::ComboBox scaleChoice;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BridgeEditor)
};

}  // namespace oubridge

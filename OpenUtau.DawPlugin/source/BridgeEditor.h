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

    BridgeProcessor& bridge;
    int lastRevision = -1;

    juce::Label title;
    juce::Label status;
    juce::Label project;
    juce::Label trackHeader;
    juce::TextEditor trackList;
    juce::Label modeLabel;
    juce::ComboBox trackChoice;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BridgeEditor)
};

}  // namespace oubridge

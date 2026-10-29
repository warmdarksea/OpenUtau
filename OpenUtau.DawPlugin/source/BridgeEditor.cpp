#include "BridgeEditor.h"

namespace oubridge {

BridgeEditor::BridgeEditor(BridgeProcessor& processorIn)
    : juce::AudioProcessorEditor(processorIn), bridge(processorIn) {
    title.setText("OpenUtau Bridge", juce::dontSendNotification);
    title.setFont(juce::Font(20.0f, juce::Font::bold));
    addAndMakeVisible(title);

    status.setText("Starting", juce::dontSendNotification);
    addAndMakeVisible(status);

    project.setText("No project", juce::dontSendNotification);
    addAndMakeVisible(project);

    trackHeader.setText("Tracks", juce::dontSendNotification);
    trackHeader.setFont(juce::Font(15.0f, juce::Font::bold));
    addAndMakeVisible(trackHeader);

    trackList.setMultiLine(true);
    trackList.setReadOnly(true);
    trackList.setCaretVisible(false);
    addAndMakeVisible(trackList);

    modeLabel.setText("Play", juce::dontSendNotification);
    addAndMakeVisible(modeLabel);

    trackChoice.onChange = [this] {
        // Item id 1 = all tracks (mixer model); 2 + n = track n, raw pre-fader.
        const int id = trackChoice.getSelectedId();
        bridge.bridgeState().setTrackFilter(id <= 1 ? -1 : id - 2);
    };
    addAndMakeVisible(trackChoice);

    setResizable(true, true);
    setSize(420, 320);
    rebuildTrackChoices();
    refresh();
    startTimerHz(4);
}

BridgeEditor::~BridgeEditor() {
    stopTimer();
}

void BridgeEditor::timerCallback() {
    const int revision = bridge.bridgeState().revision();
    if (revision != lastRevision) {
        lastRevision = revision;
        rebuildTrackChoices();
    }
    refresh();
}

void BridgeEditor::rebuildTrackChoices() {
    const int selected = bridge.bridgeState().trackFilter();
    trackChoice.clear(juce::dontSendNotification);
    trackChoice.addItem("All tracks (OpenUtau mix)", 1);
    const auto tracks = bridge.bridgeState().tracks();
    for (int i = 0; i < static_cast<int>(tracks.size()); ++i) {
        const auto& track = tracks[static_cast<size_t>(i)];
        trackChoice.addItem("Track " + juce::String(i + 1) + ": " +
                            (track.name.isNotEmpty() ? track.name : juce::String("(unnamed)")) +
                            " (raw)", 2 + i);
    }
    trackChoice.setSelectedId(selected < 0 ? 1 : 2 + selected, juce::dontSendNotification);
}

void BridgeEditor::refresh() {
    auto& state = bridge.bridgeState();
    status.setText(bridge.bridgeServer().statusText(), juce::dontSendNotification);
    juce::String projectText = state.projectName().isNotEmpty()
        ? state.projectName() : juce::String("(unsaved project)");
    projectText << "  -  " << state.numCachedParts() << "/" << state.numParts() << " parts cached";
    project.setText(projectText, juce::dontSendNotification);

    juce::String tracksText;
    const auto tracks = state.tracks();
    for (int i = 0; i < static_cast<int>(tracks.size()); ++i) {
        const auto& track = tracks[static_cast<size_t>(i)];
        tracksText << (i + 1) << ". " << track.name;
        if (track.singer.isNotEmpty()) {
            tracksText << "  [" << track.singer << "]";
        }
        if (track.engine.isNotEmpty()) {
            tracksText << "  " << track.engine;
        }
        if (track.muted) {
            tracksText << "  (muted)";
        }
        tracksText << "\n";
    }
    if (tracksText.isEmpty()) {
        tracksText = "Connect from OpenUtau:\nTools > DAW Integration";
    }
    if (trackList.getText() != tracksText) {
        trackList.setText(tracksText, juce::dontSendNotification);
    }
}

void BridgeEditor::paint(juce::Graphics& graphics) {
    graphics.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
}

void BridgeEditor::resized() {
    auto area = getLocalBounds().reduced(12);
    title.setBounds(area.removeFromTop(28));
    status.setBounds(area.removeFromTop(22));
    project.setBounds(area.removeFromTop(22));
    area.removeFromTop(6);
    auto modeRow = area.removeFromBottom(26);
    modeLabel.setBounds(modeRow.removeFromLeft(40));
    trackChoice.setBounds(modeRow);
    area.removeFromBottom(6);
    trackHeader.setBounds(area.removeFromTop(22));
    trackList.setBounds(area);
}

}  // namespace oubridge

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

    scaleLabel.setText("UI scale", juce::dontSendNotification);
    addAndMakeVisible(scaleLabel);
    scaleChoice.addItem("Auto", 1);
    for (const int percent : {75, 100, 125, 150, 175, 200, 250, 300}) {
        scaleChoice.addItem(juce::String(percent) + "%", percent);
    }
    const int storedScale = bridge.uiScalePercent();
    scaleChoice.setSelectedId(storedScale == 0 ? 1 : storedScale, juce::dontSendNotification);
    scaleChoice.onChange = [this] {
        const int id = scaleChoice.getSelectedId();
        bridge.setUiScalePercent(id == 1 ? 0 : id);
        applyScale();
    };
    addAndMakeVisible(scaleChoice);

    setResizable(true, true);
    setSize(420, 346);
    applyScale();
    rebuildTrackChoices();
    refresh();
    startTimerHz(4);
}

float BridgeEditor::automaticScale() const {
    // Hosts speaking VST3 or CLAP deliver a content scale through the wrapper,
    // which JUCE already applies on top of this transform - so automatic means
    // factor 1 there, and anything else would scale twice. VST2 carries no DPI
    // information on Linux: a plain X11 child window always comes up at 1:1 on
    // a HiDPI screen, so automatic reads the display's own scale instead.
    if (bridge.wrapperType == juce::AudioProcessor::wrapperType_VST) {
        if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()) {
            return static_cast<float>(juce::jlimit(0.5, 3.0, display->scale));
        }
    }
    return 1.0f;
}

void BridgeEditor::applyScale() {
    const int percent = bridge.uiScalePercent();
    const float factor = percent == 0 ? automaticScale() : percent / 100.0f;
    setTransform(juce::AffineTransform::scale(factor));
    // Bounds stay in unscaled coordinates; the transform maps them to pixels,
    // and the wrapper reports the transformed size to the host.
    setSize(getWidth(), getHeight());
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
    auto scaleRow = area.removeFromBottom(26);
    scaleLabel.setBounds(scaleRow.removeFromLeft(70));
    scaleChoice.setBounds(scaleRow.removeFromLeft(120));
    area.removeFromBottom(6);
    auto modeRow = area.removeFromBottom(26);
    modeLabel.setBounds(modeRow.removeFromLeft(70));
    trackChoice.setBounds(modeRow);
    area.removeFromBottom(6);
    trackHeader.setBounds(area.removeFromTop(22));
    trackList.setBounds(area);
}

}  // namespace oubridge

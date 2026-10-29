// The AudioProcessor: renders the synchronized OpenUtau timeline against the
// host transport and publishes transport facts back through the outbox.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "BridgeServer.h"
#include "BridgeState.h"

namespace oubridge {

class BridgeProcessor final : public juce::AudioProcessor {
public:
    BridgeProcessor();
    ~BridgeProcessor() override;

    void prepareToPlay(double sampleRate, int maximumBlockSize) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "OpenUtau Bridge"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destination) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    BridgeState& bridgeState() { return state; }
    BridgeServer& bridgeServer() { return server; }

private:
    void publishTransport(const juce::AudioPlayHead::PositionInfo& position, double sampleRate);

    BridgeState state;
    TransportOutbox outbox;
    BridgeServer server;

    bool wasPlaying = false;
    double lastSentPlayheadMs = -1.0e9;
    juce::int64 lastPlayheadSendMs = 0;
    double lastSentBpm = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BridgeProcessor)
};

}  // namespace oubridge

#include "BridgeProcessor.h"

#include "BridgeEditor.h"

namespace oubridge {

namespace {
// Playhead pacing (§6.2): immediately on state changes, every 100 ms while
// playing, and only when a parked playhead moves more than 50 ms.
constexpr juce::int64 kPlayingResendMs = 100;
constexpr double kParkedMoveThresholdMs = 50.0;
constexpr double kBpmChangeThreshold = 0.01;
}  // namespace

BridgeProcessor::BridgeProcessor()
    : juce::AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      server(state, outbox) {
    server.start("OpenUtau Bridge");
}

BridgeProcessor::~BridgeProcessor() {
    server.stop();
}

void BridgeProcessor::prepareToPlay(double sampleRate, int) {
    state.setTargetSampleRate(sampleRate);
}

bool BridgeProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    return layouts.getMainInputChannelSet().isDisabled() &&
           layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void BridgeProcessor::publishTransport(const juce::AudioPlayHead::PositionInfo& position, double sampleRate) {
    const bool playing = position.getIsPlaying();
    double positionMs = 0.0;
    if (const auto seconds = position.getTimeInSeconds()) {
        positionMs = *seconds * 1000.0;
    } else if (const auto samples = position.getTimeInSamples()) {
        positionMs = static_cast<double>(*samples) / sampleRate * 1000.0;
    }

    const auto nowMs = juce::Time::getMillisecondCounter();
    bool send = false;
    if (playing != wasPlaying) {
        send = true;
        if (playing) {
            // §6.1: the play rising edge flushes OpenUtau's pending debounced updates.
            outbox.playbackStartedCount.fetch_add(1, std::memory_order_release);
        }
    } else if (playing) {
        send = nowMs - lastPlayheadSendMs >= kPlayingResendMs;
    } else {
        send = std::abs(positionMs - lastSentPlayheadMs) > kParkedMoveThresholdMs;
    }
    if (send) {
        lastPlayheadSendMs = nowMs;
        lastSentPlayheadMs = positionMs;
        outbox.playheadMs.store(positionMs, std::memory_order_relaxed);
        outbox.playheadPlaying.store(playing, std::memory_order_relaxed);
        outbox.playheadSeq.fetch_add(1, std::memory_order_release);
    }
    if (const auto bpm = position.getBpm()) {
        if (*bpm > 0 && std::abs(*bpm - lastSentBpm) > kBpmChangeThreshold) {
            lastSentBpm = *bpm;
            outbox.bpm.store(*bpm, std::memory_order_relaxed);
            outbox.bpmSeq.fetch_add(1, std::memory_order_release);
        }
    }
    wasPlaying = playing;
}

void BridgeProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    juce::ScopedNoDenormals noDenormals;
    midi.clear();
    buffer.clear();

    juce::AudioPlayHead::PositionInfo position;
    if (auto* hostPlayHead = getPlayHead()) {
        if (const auto info = hostPlayHead->getPosition()) {
            position = *info;
        }
    }
    const double sampleRate = getSampleRate() > 0 ? getSampleRate() : kSampleRate;
    publishTransport(position, sampleRate);

    if (!position.getIsPlaying()) {
        return;
    }
    juce::int64 blockStart = 0;
    if (const auto samples = position.getTimeInSamples()) {
        blockStart = *samples;
    } else if (const auto seconds = position.getTimeInSeconds()) {
        blockStart = static_cast<juce::int64>(std::llround(*seconds * sampleRate));
    }
    const auto playlist = state.playlist();
    if (playlist == nullptr || playlist->entries.empty()) {
        return;
    }
    const int numSamples = buffer.getNumSamples();
    const juce::int64 blockEnd = blockStart + numSamples;
    for (const auto& entry : playlist->entries) {
        const juce::int64 entryEnd = entry.startSample + entry.audio->getNumSamples();
        if (entry.startSample >= blockEnd || entryEnd <= blockStart) {
            continue;
        }
        const juce::int64 from = juce::jmax(blockStart, entry.startSample);
        const juce::int64 to = juce::jmin(blockEnd, entryEnd);
        const int destOffset = static_cast<int>(from - blockStart);
        const int sourceOffset = static_cast<int>(from - entry.startSample);
        const int count = static_cast<int>(to - from);
        buffer.addFrom(0, destOffset, *entry.audio, 0, sourceOffset, count, entry.gainLeft);
        buffer.addFrom(1, destOffset, *entry.audio, 1, sourceOffset, count, entry.gainRight);
    }
}

void BridgeProcessor::getStateInformation(juce::MemoryBlock& destination) {
    juce::MemoryBlock stateBlock;
    state.save(stateBlock);
    juce::MemoryOutputStream out(destination, false);
    out.writeInt(uiScale.load());
    out.write(stateBlock.getData(), stateBlock.getSize());
}

void BridgeProcessor::setStateInformation(const void* data, int sizeInBytes) {
    if (data == nullptr || sizeInBytes < 4) {
        return;
    }
    juce::MemoryInputStream in(data, static_cast<size_t>(sizeInBytes), false);
    const int first = in.readInt();
    if (first == 0x4f554252) {  // "OUBR": a chunk from before the scale prefix existed
        state.restore(data, static_cast<size_t>(sizeInBytes));
    } else {
        setUiScalePercent(first);
        state.restore(juce::addBytesToPointer(data, in.getPosition()),
                      static_cast<size_t>(in.getNumBytesRemaining()));
    }
    if (getSampleRate() > 0) {
        state.setTargetSampleRate(getSampleRate());
    }
}

juce::AudioProcessorEditor* BridgeProcessor::createEditor() {
    return new BridgeEditor(*this);
}

}  // namespace oubridge

#if !defined(OUBRIDGE_HEADLESS_HOST)
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new oubridge::BridgeProcessor();
}
#endif

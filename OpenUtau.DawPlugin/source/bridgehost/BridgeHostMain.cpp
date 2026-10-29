// bridge-host: the standalone conformance harness for the DAW bridge
// (API.md §14, OpenUtau.Test/Core/DawIntegration/DawRealPluginTest.cs).
//
// It runs the exact server and timeline the plugin formats ship, paced by a
// fake DAW transport, and prints a peak meter so a human (or a test log
// reader) can see whether the pulled audio actually lands where the layout
// put it:
//
//   bridge-host --dir /tmp/bridge-live --rate 48000 --loop
//   OPENUTAU_BRIDGE_DISCOVERY=/tmp/bridge-live dotnet test --filter DawRealPluginTest

#include <juce_events/juce_events.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>

#include "../BridgeProcessor.h"

namespace {

std::atomic<bool> interrupted{false};

void onSignal(int) {
    interrupted.store(true);
}

// A looping (or one-way) fake DAW transport.
class FakePlayHead final : public juce::AudioPlayHead {
public:
    juce::Optional<juce::AudioPlayHead::PositionInfo> getPosition() const override {
        juce::AudioPlayHead::PositionInfo info;
        info.setIsPlaying(playing.load());
        info.setTimeInSamples(positionSamples.load());
        info.setTimeInSeconds(static_cast<double>(positionSamples.load()) / sampleRate);
        info.setBpm(120.0);
        return info;
    }

    std::atomic<bool> playing{true};
    std::atomic<juce::int64> positionSamples{0};
    double sampleRate = 44100.0;
};

juce::String argValue(const juce::StringArray& args, const juce::String& name, const juce::String& fallback) {
    const int index = args.indexOf(name);
    if (index >= 0 && index + 1 < args.size()) {
        return args[index + 1];
    }
    return fallback;
}

}  // namespace

int main(int argc, char* argv[]) {
    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::StringArray args;
    for (int i = 1; i < argc; ++i) {
        args.add(juce::String::fromUTF8(argv[i]));
    }
    if (args.contains("--help") || args.contains("-h")) {
        printf("bridge-host [--dir <discovery dir>] [--rate <hz>] [--block <frames>] [--loop] [--name <instance>]\n");
        return 0;
    }
    const juce::String dir = argValue(args, "--dir", {});
    if (dir.isNotEmpty()) {
#if defined(_WIN32)
        _putenv_s("OPENUTAU_BRIDGE_DISCOVERY", dir.toRawUTF8());
#else
        setenv("OPENUTAU_BRIDGE_DISCOVERY", dir.toRawUTF8(), 1);
#endif
    }
    const double rate = argValue(args, "--rate", "44100").getDoubleValue();
    const int blockSize = juce::jlimit(32, 8192, argValue(args, "--block", "512").getIntValue());
    const bool loop = args.contains("--loop");
    const juce::String name = argValue(args, "--name", "bridge-host");

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    oubridge::BridgeProcessor processor;
    // The processor started its server under the plugin's default instance
    // name; restart it under ours so concurrent hosts stay distinguishable.
    processor.bridgeServer().stop();
    processor.bridgeServer().start(name);

    FakePlayHead playHead;
    playHead.sampleRate = rate;
    processor.setPlayHead(&playHead);
    processor.setPlayConfigDetails(0, 2, rate, blockSize);
    processor.prepareToPlay(rate, blockSize);

    for (int waited = 0; processor.bridgeServer().port() == 0 && waited < 100; ++waited) {
        juce::Thread::sleep(20);
    }
    printf("bridge-host: listening on 127.0.0.1:%d\n", processor.bridgeServer().port());
    printf("bridge-host: discovery in %s\n",
           oubridge::BridgeServer::discoveryDirectory().getFullPathName().toRawUTF8());
    printf("bridge-host: rate %.0f Hz, block %d, %s\n", rate, blockSize, loop ? "looping" : "one-way");
    fflush(stdout);

    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer midi;
    const double blockMs = blockSize / rate * 1000.0;
    double accumulatedMs = 0;
    float peakLeft = 0;
    float peakRight = 0;
    double lastPrintedS = -1;
    bool pausedForWrap = false;

    while (!interrupted.load()) {
        processor.processBlock(buffer, midi);
        peakLeft = juce::jmax(peakLeft, buffer.getMagnitude(0, 0, blockSize));
        peakRight = juce::jmax(peakRight, buffer.getMagnitude(1, 0, blockSize));

        const double nowS = static_cast<double>(playHead.positionSamples.load()) / rate;
        if (nowS - lastPrintedS >= 0.5 || nowS < lastPrintedS) {
            printf("peak L=%.4f R=%.4f t=%.2fs parts=%d/%d%s\n", peakLeft, peakRight, nowS,
                   processor.bridgeState().numCachedParts(), processor.bridgeState().numParts(),
                   processor.bridgeServer().isConnected() ? " [connected]" : "");
            fflush(stdout);
            lastPrintedS = nowS;
            peakLeft = 0;
            peakRight = 0;
        }

        if (pausedForWrap) {
            // One stopped block at zero makes the next block a play rising
            // edge, which is what reports a fresh start on every loop (§6.2).
            playHead.playing.store(true);
            pausedForWrap = false;
        } else if (playHead.playing.load()) {
            playHead.positionSamples.fetch_add(blockSize);
        }

        if (loop) {
            // Wrap once past the end of the furthest part, with a little tail.
            const auto playlist = processor.bridgeState().playlist();
            const double lengthS = playlist != nullptr && playlist->lengthSamples > 0
                ? static_cast<double>(playlist->lengthSamples) / playlist->sampleRate + 0.5
                : 4.0;
            if (static_cast<double>(playHead.positionSamples.load()) / rate >= juce::jmax(4.0, lengthS)) {
                playHead.positionSamples.store(0);
                playHead.playing.store(false);
                pausedForWrap = true;
                lastPrintedS = -1;
            }
        }

        accumulatedMs += blockMs;
        if (accumulatedMs >= 1.0) {
            const int sleepMs = static_cast<int>(accumulatedMs);
            accumulatedMs -= sleepMs;
            juce::Thread::sleep(sleepMs);
        }
    }

    printf("bridge-host: shutting down\n");
    processor.setPlayHead(nullptr);
    processor.bridgeServer().stop();
    return 0;
}

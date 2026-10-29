// The plugin half of the DAW integration transport (API.md §2): a loopback TCP
// server that publishes a discovery file, accepts one OpenUtau connection at a
// time, answers the control plane and pulls part audio over the data plane.
//
// Single I/O thread per instance: the accept loop, the read loop, the ping
// timer and every outbound line share it, so writes never interleave. The
// audio thread communicates through atomics only (TransportOutbox).
#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>

#include "BridgeProtocol.h"
#include "BridgeState.h"

namespace oubridge {

// Transport facts the audio thread publishes and the I/O thread transmits.
// The audio thread owns the send policy (edges, pacing); a bumped sequence
// number means "this value is due on the wire".
struct TransportOutbox {
    std::atomic<int> playbackStartedCount{0};

    std::atomic<double> playheadMs{0.0};
    std::atomic<bool> playheadPlaying{false};
    std::atomic<int> playheadSeq{0};

    std::atomic<double> bpm{0.0};
    std::atomic<int> bpmSeq{0};
};

class BridgeServer : private juce::Thread {
public:
    BridgeServer(BridgeState& state, TransportOutbox& outbox);
    ~BridgeServer() override;

    // Binds an ephemeral loopback port, publishes the discovery file and
    // starts serving. Idempotent.
    void start(const juce::String& instanceName);
    void stop();

    bool isListening() const { return boundPort.load() > 0; }
    bool isConnected() const { return connected.load(); }
    int port() const { return boundPort.load(); }
    juce::String statusText() const;

    // The §4.1 discovery directory: $OPENUTAU_BRIDGE_DISCOVERY when set, else
    // <tmp>/OpenUtau/PluginServers.
    static juce::File discoveryDirectory();

private:
    void run() override;
    void serveConnection(juce::StreamingSocket& socket);

    // Inbound plumbing (I/O thread only).
    bool processBuffered(juce::StreamingSocket& socket);
    bool handleLine(const juce::String& line);
    bool handleFramePayload();
    void handleRequest(const ControlLine& line);
    void handleNotification(const ControlLine& line);
    void handleUpdatePartLayout(const ControlLine& line);

    // Outbound plumbing (I/O thread only).
    bool sendLine(const juce::String& line);
    void drainOutbox();
    void startNextPull();

    bool publishDiscoveryFile();
    void removeDiscoveryFile();
    void setStatus(const juce::String& text);

    BridgeState& state;
    TransportOutbox& outbox;

    std::unique_ptr<juce::StreamingSocket> listener;
    juce::StreamingSocket* activeSocket = nullptr;  // valid on the I/O thread only
    juce::String instanceName;
    juce::File discoveryFile;
    std::atomic<int> boundPort{0};
    std::atomic<bool> connected{false};

    // Receive framing.
    juce::MemoryBlock receiveBuffer;
    size_t receiveUsed = 0;
    int expectedFrameBytes = -1;  // -1: reading lines; >= 0: inside a data frame
    std::uint64_t expectedFrameHash = 0;
    juce::MemoryBlock frameBuffer;
    size_t frameUsed = 0;

    // Audio pulls: one outstanding request at a time (§6.2).
    std::deque<juce::String> pullQueue;
    juce::String pullUuid;
    juce::String pullHash;
    juce::int64 pullStartedMs = 0;

    // Outbox bookkeeping (I/O thread only).
    int sentPlaybackStarted = 0;
    int sentPlayheadSeq = 0;
    int sentBpmSeq = 0;
    juce::int64 lastPingMs = 0;

    mutable juce::CriticalSection statusLock;
    juce::String status{"Starting"};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BridgeServer)
};

}  // namespace oubridge

// Wire protocol helpers for the OpenUtau DAW integration, plugin side.
// The contract is OpenUtau.Core/DawIntegration/API.md (v1.2); the reference
// peer implementation is DawTransport.cs / DawAudio.cs in the same directory.
// Everything here is pure string/bytes work: no sockets, no threads.
#pragma once

#include <juce_core/juce_core.h>

#include <cstdint>

namespace oubridge {

constexpr int kSampleRate = 44100;   // fixed wire format, never negotiated (§1)
constexpr int kChannels = 2;
constexpr int kMaxFrameBytes = 1 << 28;  // §5.2: refuse larger lengths, never allocate
constexpr const char* kApiVersion = "1.2";

// XXH64 of a payload, the data-plane content hash (§5.2).
std::uint64_t hashBytes(const void* data, size_t size);

// Hashes travel as decimal strings: 64-bit values overflow double JSON parsers.
juce::String formatHash(std::uint64_t hash);
bool tryParseHash(const juce::String& text, std::uint64_t& hash);

// ---- control plane ---------------------------------------------------------

enum class LineKind {
    request,        // request:<uuid>:<kind> <json>
    response,       // response:<uuid> <json>
    notification,   // notification:<kind> <json>
    close,          // bare "close"
    audioHeader,    // audio <hash> <length>
    malformed,
};

struct ControlLine {
    LineKind kind = LineKind::malformed;
    juce::String uuid;          // request / response
    juce::String messageKind;   // request / notification
    juce::var payload;          // parsed JSON; void() when absent or malformed
    bool hasPayload = false;

    // audioHeader only:
    juce::String audioHash;
    int audioLength = 0;
};

// Classifies and parses one line (without its trailing newline).
ControlLine parseLine(const juce::String& line);

// ---- outbound builders (each returns a full line WITHOUT the newline) ------

// { "success": true, "data": <data>, "error": null }
juce::String okEnvelope(const juce::var& data);
juce::String failEnvelope(const juce::String& error);

juce::String responseLine(const juce::String& uuid, const juce::String& envelope);
juce::String notificationLine(const juce::String& kind, const juce::var& payload);
juce::String requestLine(const juce::String& uuid, const juce::String& kind, const juce::var& payload);

// ---- message kinds (§6) ----------------------------------------------------

namespace msg {
constexpr const char* init = "init";
constexpr const char* updateUstx = "updateUstx";
constexpr const char* updatePartLayout = "updatePartLayout";
constexpr const char* updateTracks = "updateTracks";
constexpr const char* updateProjectInfo = "updateProjectInfo";
constexpr const char* getAudio = "getAudio";
constexpr const char* ping = "ping";
constexpr const char* playbackStarted = "playbackStarted";
constexpr const char* playhead = "playhead";
constexpr const char* bpm = "bpm";
}  // namespace msg

}  // namespace oubridge

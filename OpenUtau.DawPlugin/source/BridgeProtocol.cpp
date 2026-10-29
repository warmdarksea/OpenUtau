#include "BridgeProtocol.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace oubridge {

std::uint64_t hashBytes(const void* data, size_t size) {
    return XXH64(data, size, 0);
}

juce::String formatHash(std::uint64_t hash) {
    // juce::String has no unsigned 64-bit constructor; go through chars.
    char buffer[24];
    snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(hash));
    return juce::String(buffer);
}

bool tryParseHash(const juce::String& text, std::uint64_t& hash) {
    if (text.isEmpty() || !text.containsOnly("0123456789")) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const auto raw = text.toRawUTF8();
    unsigned long long value = strtoull(raw, &end, 10);
    if (errno != 0 || end == raw || *end != '\0') {
        return false;
    }
    hash = static_cast<std::uint64_t>(value);
    return true;
}

static bool parseAudioHeader(const juce::String& line, ControlLine& out) {
    // "audio <hash> <length>" — §5.2. Reject anything oversized or non-numeric
    // before any allocation happens; the length is peer-controlled.
    juce::StringArray parts;
    parts.addTokens(line.substring(6), " ", "");
    parts.removeEmptyStrings();
    std::uint64_t hash = 0;
    if (parts.size() != 2 || !tryParseHash(parts[0], hash)) {
        return false;
    }
    if (!parts[1].containsOnly("0123456789") || parts[1].isEmpty() || parts[1].length() > 10) {
        return false;
    }
    const auto length = parts[1].getLargeIntValue();
    if (length < 0 || length > kMaxFrameBytes) {
        return false;
    }
    out.kind = LineKind::audioHeader;
    out.audioHash = parts[0];
    out.audioLength = static_cast<int>(length);
    return true;
}

ControlLine parseLine(const juce::String& line) {
    ControlLine out;
    if (line == "close") {
        out.kind = LineKind::close;
        return out;
    }
    if (line.startsWith("audio ")) {
        if (!parseAudioHeader(line, out)) {
            out.kind = LineKind::malformed;
        }
        return out;
    }
    const int space = line.indexOfChar(' ');
    const juce::String header = space < 0 ? line : line.substring(0, space);
    const juce::String payload = space < 0 ? juce::String() : line.substring(space + 1);

    if (header.startsWith("request:")) {
        // request:<uuid>:<kind> — the kind may itself contain ':' (DawTransport.cs).
        const juce::String rest = header.substring(8);
        const int colon = rest.indexOfChar(':');
        if (colon <= 0 || colon == rest.length() - 1) {
            return out;
        }
        out.kind = LineKind::request;
        out.uuid = rest.substring(0, colon);
        out.messageKind = rest.substring(colon + 1);
    } else if (header.startsWith("response:")) {
        const juce::String uuid = header.substring(9);
        if (uuid.isEmpty()) {
            return out;
        }
        out.kind = LineKind::response;
        out.uuid = uuid;
    } else if (header.startsWith("notification:")) {
        const juce::String kind = header.substring(13);
        if (kind.isEmpty()) {
            return out;
        }
        out.kind = LineKind::notification;
        out.messageKind = kind;
    } else {
        return out;
    }

    if (payload.trim().isNotEmpty()) {
        juce::var parsed;
        // Malformed JSON payloads are treated as empty, mirroring DawTransport (§8).
        if (juce::JSON::parse(payload, parsed).wasOk()) {
            out.payload = parsed;
            out.hasPayload = true;
        }
    }
    return out;
}

static juce::String toJson(const juce::var& value) {
    return juce::JSON::toString(value, true);
}

juce::String okEnvelope(const juce::var& data) {
    auto* object = new juce::DynamicObject();
    object->setProperty("success", true);
    object->setProperty("data", data);
    object->setProperty("error", juce::var());
    return toJson(juce::var(object));
}

juce::String failEnvelope(const juce::String& error) {
    auto* object = new juce::DynamicObject();
    object->setProperty("success", false);
    object->setProperty("data", juce::var());
    object->setProperty("error", error);
    return toJson(juce::var(object));
}

juce::String responseLine(const juce::String& uuid, const juce::String& envelope) {
    return "response:" + uuid + " " + envelope;
}

juce::String notificationLine(const juce::String& kind, const juce::var& payload) {
    return "notification:" + kind + " " + toJson(payload);
}

juce::String requestLine(const juce::String& uuid, const juce::String& kind, const juce::var& payload) {
    return "request:" + uuid + ":" + kind + " " + toJson(payload);
}

}  // namespace oubridge

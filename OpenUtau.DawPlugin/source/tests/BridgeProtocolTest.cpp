// Framing and envelope unit tests, mirroring the shapes DawTransportTest.cs
// asserts on the other side of the wire.
#include <cstdio>
#include <cstring>

#include "../BridgeProtocol.h"

using namespace oubridge;

static int failures = 0;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            ++failures;                                                      \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);     \
        }                                                                    \
    } while (false)

int main() {
    // XXH64 of empty input with seed 0 is the algorithm's published constant;
    // the C# side (K4os.Hash.xxHash) produces the same value.
    CHECK(hashBytes("", 0) == 0xEF46DB3751D8E999ull);
    CHECK(formatHash(13507256038857166760ull) == "13507256038857166760");
    std::uint64_t hash = 0;
    CHECK(tryParseHash("13507256038857166760", hash) && hash == 13507256038857166760ull);
    CHECK(!tryParseHash("-3", hash));
    CHECK(!tryParseHash("12f", hash));
    CHECK(!tryParseHash("", hash));

    // Requests, with a kind that itself contains a colon.
    auto line = parseLine("request:abc-123:init {\"ustx\":\"doc\"}");
    CHECK(line.kind == LineKind::request);
    CHECK(line.uuid == "abc-123");
    CHECK(line.messageKind == "init");
    CHECK(line.payload.getProperty("ustx", juce::var()).toString() == "doc");
    line = parseLine("request:u:name:spaced {}");
    CHECK(line.kind == LineKind::request && line.messageKind == "name:spaced");

    // Notifications and close.
    line = parseLine("notification:playbackStarted {}");
    CHECK(line.kind == LineKind::notification && line.messageKind == "playbackStarted");
    line = parseLine("close");
    CHECK(line.kind == LineKind::close);

    // Malformed JSON is tolerated as an empty payload, like DawTransport.
    line = parseLine("notification:bpm {not json");
    CHECK(line.kind == LineKind::notification && !line.hasPayload);

    // Data-plane headers (§5.2).
    line = parseLine("audio 13507256038857166760 3528000");
    CHECK(line.kind == LineKind::audioHeader);
    CHECK(line.audioHash == "13507256038857166760");
    CHECK(line.audioLength == 3528000);
    CHECK(parseLine("audio 13507256038857166760 268435457").kind == LineKind::malformed);
    CHECK(parseLine("audio xyz 100").kind == LineKind::malformed);
    CHECK(parseLine("audio 1 2 3").kind == LineKind::malformed);
    CHECK(parseLine("audio 1 -5").kind == LineKind::malformed);

    // Envelopes: the C# DawResult deserializer reads these back.
    juce::var parsed;
    CHECK(juce::JSON::parse(okEnvelope(juce::var("x")), parsed).wasOk());
    CHECK(static_cast<bool>(parsed.getProperty("success", false)));
    CHECK(parsed.getProperty("data", juce::var()).toString() == "x");
    CHECK(parsed.getProperty("error", juce::var("y")).isVoid());
    CHECK(juce::JSON::parse(failEnvelope("boom"), parsed).wasOk());
    CHECK(!static_cast<bool>(parsed.getProperty("success", true)));
    CHECK(parsed.getProperty("error", juce::var()).toString() == "boom");

    // Round trip of a built request through the parser.
    auto* payload = new juce::DynamicObject();
    payload->setProperty("hash", "42");
    line = parseLine(requestLine("uuid-1", msg::getAudio, juce::var(payload)));
    CHECK(line.kind == LineKind::request && line.messageKind == "getAudio");
    CHECK(line.payload.getProperty("hash", juce::var()).toString() == "42");

    // Lines must stay single-line even when payloads carry newlines.
    auto* multi = new juce::DynamicObject();
    multi->setProperty("ustx", "line1\nline2");
    const auto notification = notificationLine(msg::updateUstx, juce::var(multi));
    CHECK(!notification.containsChar('\n'));
    line = parseLine(notification);
    CHECK(line.payload.getProperty("ustx", juce::var()).toString() == "line1\nline2");

    if (failures == 0) {
        printf("bridge-protocol-test: all checks passed\n");
        return 0;
    }
    printf("bridge-protocol-test: %d check(s) FAILED\n", failures);
    return 1;
}

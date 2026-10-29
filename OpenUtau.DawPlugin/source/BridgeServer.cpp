#include "BridgeServer.h"

#if !JUCE_WINDOWS
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace oubridge {

namespace {
constexpr int kPingIntervalMs = 5000;        // §3.1
constexpr int kPullTimeoutMs = 10000;        // §3.1 request timeout
constexpr int kSocketPollMs = 200;
}  // namespace

BridgeServer::BridgeServer(BridgeState& stateIn, TransportOutbox& outboxIn)
    : juce::Thread("OpenUtau bridge server"), state(stateIn), outbox(outboxIn) {
    receiveBuffer.setSize(64 * 1024);
}

BridgeServer::~BridgeServer() {
    stop();
}

juce::File BridgeServer::discoveryDirectory() {
    if (const char* override = getenv("OPENUTAU_BRIDGE_DISCOVERY")) {
        if (override[0] != '\0') {
            return juce::File(juce::String::fromUTF8(override));
        }
    }
    // Match .NET Path.GetTempPath(): $TMPDIR with a /tmp fallback on Unix.
    juce::File temp = juce::File::getSpecialLocation(juce::File::tempDirectory);
    return temp.getChildFile("OpenUtau").getChildFile("PluginServers");
}

void BridgeServer::start(const juce::String& nameIn) {
    if (isThreadRunning()) {
        return;
    }
    instanceName = nameIn;
    startThread();
}

void BridgeServer::stop() {
    signalThreadShouldExit();
    // Closing the listener releases a blocked accept; the poll loop sees the flag.
    if (listener != nullptr) {
        listener->close();
    }
    stopThread(5000);
    removeDiscoveryFile();
    boundPort.store(0);
    connected.store(false);
}

juce::String BridgeServer::statusText() const {
    const juce::ScopedLock guard(statusLock);
    return status;
}

void BridgeServer::setStatus(const juce::String& text) {
    const juce::ScopedLock guard(statusLock);
    status = text;
}

// ---- discovery (§4.1, §11) ---------------------------------------------------

bool BridgeServer::publishDiscoveryFile() {
    juce::File directory = discoveryDirectory();
    if (!directory.createDirectory()) {
        setStatus("Cannot create discovery directory " + directory.getFullPathName());
        return false;
    }
#if !JUCE_WINDOWS
    // §11: owner-only discovery directory; a directory this user does not own
    // cannot be trusted or tightened, so publishing there is refused.
    struct stat info {};
    if (stat(directory.getFullPathName().toRawUTF8(), &info) != 0 || info.st_uid != geteuid()) {
        setStatus("Discovery directory is not owned by this user; refusing to publish.");
        return false;
    }
    if ((info.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        chmod(directory.getFullPathName().toRawUTF8(), S_IRWXU);
    }
#endif
    auto* object = new juce::DynamicObject();
    object->setProperty("port", boundPort.load());
    object->setProperty("name", instanceName);
    object->setProperty("apiVersion", juce::String(kApiVersion));
    const juce::String json = juce::JSON::toString(juce::var(object), true);
    discoveryFile = directory.getChildFile(instanceName + " " + juce::String(boundPort.load()) + ".json");
    if (!discoveryFile.replaceWithText(json)) {
        setStatus("Cannot write discovery file " + discoveryFile.getFullPathName());
        return false;
    }
#if !JUCE_WINDOWS
    chmod(discoveryFile.getFullPathName().toRawUTF8(), S_IRUSR | S_IWUSR);
#endif
    return true;
}

void BridgeServer::removeDiscoveryFile() {
    if (discoveryFile != juce::File()) {
        discoveryFile.deleteFile();
        discoveryFile = juce::File();
    }
}

// ---- server loop --------------------------------------------------------------

void BridgeServer::run() {
    listener = std::make_unique<juce::StreamingSocket>();
    // Ephemeral port on loopback only (§3).
    if (!listener->createListener(0, "127.0.0.1")) {
        setStatus("Cannot listen on 127.0.0.1.");
        return;
    }
    boundPort.store(listener->getBoundPort());
    if (!publishDiscoveryFile()) {
        return;
    }
    setStatus("Listening on port " + juce::String(boundPort.load()));

    while (!threadShouldExit()) {
        std::unique_ptr<juce::StreamingSocket> socket(listener->waitForNextConnection());
        if (socket == nullptr) {
            if (!threadShouldExit()) {
                juce::Thread::sleep(50);
            }
            continue;
        }
        connected.store(true);
        setStatus("OpenUtau connected");
        serveConnection(*socket);
        connected.store(false);
        if (!threadShouldExit()) {
            setStatus("Listening on port " + juce::String(boundPort.load()));
        }
    }
}

void BridgeServer::serveConnection(juce::StreamingSocket& socket) {
    activeSocket = &socket;
    receiveUsed = 0;
    expectedFrameBytes = -1;
    frameUsed = 0;
    pullQueue.clear();
    pullUuid.clear();
    pullHash.clear();
    lastPingMs = juce::Time::getMillisecondCounter();

    while (!threadShouldExit()) {
        drainOutbox();
        if (pullUuid.isNotEmpty() &&
            juce::Time::getMillisecondCounter() - pullStartedMs > kPullTimeoutMs) {
            // §8: a peer that never answers is wedged; drop and let it reconnect.
            break;
        }
        const int ready = socket.waitUntilReady(true, kSocketPollMs);
        if (ready < 0) {
            break;
        }
        if (ready == 0) {
            continue;
        }
        if (receiveUsed == receiveBuffer.getSize()) {
            receiveBuffer.setSize(receiveBuffer.getSize() * 2, true);
        }
        const int bytesRead = socket.read(
            juce::addBytesToPointer(receiveBuffer.getData(), receiveUsed),
            static_cast<int>(receiveBuffer.getSize() - receiveUsed), false);
        if (bytesRead <= 0) {
            break;
        }
        receiveUsed += static_cast<size_t>(bytesRead);
        if (!processBuffered(socket)) {
            break;
        }
    }
    activeSocket = nullptr;
    socket.close();
}

bool BridgeServer::processBuffered(juce::StreamingSocket&) {
    const auto* bytes = static_cast<const juce::uint8*>(receiveBuffer.getData());
    size_t consumed = 0;
    while (true) {
        if (expectedFrameBytes >= 0) {
            // Inside a data frame: move exactly the missing payload bytes over.
            const size_t missing = static_cast<size_t>(expectedFrameBytes) - frameUsed;
            const size_t available = receiveUsed - consumed;
            const size_t take = juce::jmin(missing, available);
            if (take > 0) {
                frameBuffer.copyFrom(bytes + consumed, static_cast<int>(frameUsed), take);
                frameUsed += take;
                consumed += take;
            }
            if (frameUsed < static_cast<size_t>(expectedFrameBytes)) {
                break;  // wait for more bytes
            }
            expectedFrameBytes = -1;
            if (!handleFramePayload()) {
                return false;
            }
            continue;
        }
        // Reading lines: find the next newline.
        size_t newline = consumed;
        while (newline < receiveUsed && bytes[newline] != '\n') {
            ++newline;
        }
        if (newline == receiveUsed) {
            if (receiveUsed - consumed > static_cast<size_t>(kMaxFrameBytes)) {
                return false;  // never-ending line; peer-controlled growth bound (§5.1)
            }
            break;
        }
        size_t length = newline - consumed;
        if (length > 0 && bytes[consumed + length - 1] == '\r') {
            --length;  // tolerate CRLF peers, like DawFrameReader does
        }
        const juce::String line = juce::String::fromUTF8(
            reinterpret_cast<const char*>(bytes + consumed), static_cast<int>(length));
        consumed = newline + 1;
        if (!handleLine(line)) {
            return false;
        }
    }
    if (consumed > 0) {
        std::memmove(receiveBuffer.getData(), bytes + consumed, receiveUsed - consumed);
        receiveUsed -= consumed;
    }
    return true;
}

bool BridgeServer::handleLine(const juce::String& line) {
    const ControlLine parsed = parseLine(line);
    switch (parsed.kind) {
        case LineKind::close:
            return false;  // clean teardown (§9)
        case LineKind::audioHeader: {
            std::uint64_t hash = 0;
            tryParseHash(parsed.audioHash, hash);
            expectedFrameHash = hash;
            expectedFrameBytes = parsed.audioLength;
            frameBuffer.ensureSize(static_cast<size_t>(juce::jmax(1, parsed.audioLength)));
            frameUsed = 0;
            return true;
        }
        case LineKind::request:
            handleRequest(parsed);
            return true;
        case LineKind::notification:
            handleNotification(parsed);
            return true;
        case LineKind::response: {
            // The only requests this side sends are getAudio pulls, and their
            // success path is a data frame. An envelope on the pull uuid is a
            // refusal: skip that hash and move on (§6.2).
            if (parsed.uuid == pullUuid) {
                pullUuid.clear();
                pullHash.clear();
                startNextPull();
            }
            return true;
        }
        case LineKind::malformed:
        default:
            // §8: malformed control lines are logged and survived.
            return true;
    }
}

bool BridgeServer::handleFramePayload() {
    const std::uint64_t actual = hashBytes(frameBuffer.getData(), frameUsed);
    if (actual != expectedFrameHash) {
        // Mislabeled audio would poison the cache under the requested hash (§5.2).
        return false;
    }
    state.storeAudio(expectedFrameHash, frameBuffer.getData(), static_cast<int>(frameUsed));
    if (formatHash(expectedFrameHash) == pullHash) {
        pullUuid.clear();
        pullHash.clear();
    }
    startNextPull();
    return true;
}

void BridgeServer::handleRequest(const ControlLine& line) {
    if (line.messageKind == msg::init) {
        state.setUstx(line.payload.getProperty("ustx", juce::String()).toString());
        auto* data = new juce::DynamicObject();
        data->setProperty("apiVersion", juce::String(kApiVersion));
        sendLine(responseLine(line.uuid, okEnvelope(juce::var(data))));
        return;
    }
    if (line.messageKind == msg::updatePartLayout) {
        handleUpdatePartLayout(line);
        return;
    }
    // §5.1: unknown request kinds get a failed envelope, never a dropped connection.
    sendLine(responseLine(line.uuid, failEnvelope("Unsupported request kind: " + line.messageKind)));
}

void BridgeServer::handleUpdatePartLayout(const ControlLine& line) {
    std::vector<PartLayout> layout;
    if (const auto* parts = line.payload.getProperty("parts", juce::var()).getArray()) {
        layout.reserve(static_cast<size_t>(parts->size()));
        for (const auto& item : *parts) {
            PartLayout part;
            part.trackNo = static_cast<int>(item.getProperty("trackNo", 0));
            part.startMs = static_cast<double>(item.getProperty("startMs", 0.0));
            part.endMs = static_cast<double>(item.getProperty("endMs", 0.0));
            if (!tryParseHash(item.getProperty("audioHash", juce::String()).toString(), part.audioHash)) {
                continue;  // tolerate unknown shapes; the rest of the layout still applies
            }
            layout.push_back(part);
        }
    }
    const juce::StringArray missing = state.setLayout(std::move(layout));
    auto* data = new juce::DynamicObject();
    juce::Array<juce::var> hashes;
    for (const auto& hash : missing) {
        hashes.add(hash);
    }
    data->setProperty("missingAudios", juce::var(hashes));
    sendLine(responseLine(line.uuid, okEnvelope(juce::var(data))));
    pullQueue.clear();
    for (const auto& hash : missing) {
        pullQueue.push_back(hash);
    }
    if (pullUuid.isEmpty()) {
        startNextPull();
    }
}

void BridgeServer::handleNotification(const ControlLine& line) {
    if (line.messageKind == msg::updateUstx) {
        state.setUstx(line.payload.getProperty("ustx", juce::String()).toString());
        return;
    }
    if (line.messageKind == msg::updateTracks) {
        std::vector<TrackInfo> tracks;
        if (const auto* list = line.payload.getProperty("tracks", juce::var()).getArray()) {
            tracks.reserve(static_cast<size_t>(list->size()));
            for (const auto& item : *list) {
                TrackInfo track;
                track.name = item.getProperty("name", juce::String()).toString();
                track.volumeDb = static_cast<double>(item.getProperty("volume", 0.0));
                track.pan = static_cast<double>(item.getProperty("pan", 0.0));
                track.muted = static_cast<bool>(item.getProperty("muted", false));
                track.singer = item.getProperty("singer", juce::String()).toString();
                track.engine = item.getProperty("engine", juce::String()).toString();
                tracks.push_back(std::move(track));
            }
        }
        state.setTracks(std::move(tracks));
        return;
    }
    if (line.messageKind == msg::updateProjectInfo) {
        state.setProjectInfo(line.payload.getProperty("name", juce::String()).toString(),
                             static_cast<bool>(line.payload.getProperty("saved", false)));
        return;
    }
    // §5.1: unknown notifications are ignored.
}

// ---- outbound -----------------------------------------------------------------

bool BridgeServer::sendLine(const juce::String& line) {
    if (activeSocket == nullptr) {
        return false;
    }
    const juce::String withNewline = line + "\n";
    const char* utf8 = withNewline.toRawUTF8();
    const int total = static_cast<int>(strlen(utf8));
    int written = 0;
    while (written < total) {
        const int sent = activeSocket->write(utf8 + written, total - written);
        if (sent <= 0) {
            return false;
        }
        written += sent;
    }
    return true;
}

void BridgeServer::startNextPull() {
    while (pullUuid.isEmpty() && !pullQueue.empty()) {
        const juce::String hash = pullQueue.front();
        pullQueue.pop_front();
        std::uint64_t parsed = 0;
        if (tryParseHash(hash, parsed) && state.hasAudio(parsed)) {
            continue;  // arrived through an earlier pull of the same content
        }
        pullUuid = juce::Uuid().toString();
        pullHash = hash;
        pullStartedMs = juce::Time::getMillisecondCounter();
        auto* payload = new juce::DynamicObject();
        payload->setProperty("hash", hash);
        sendLine(requestLine(pullUuid, msg::getAudio, juce::var(payload)));
    }
}

void BridgeServer::drainOutbox() {
    const auto nowMs = juce::Time::getMillisecondCounter();
    if (nowMs - lastPingMs >= kPingIntervalMs) {
        lastPingMs = nowMs;
        sendLine(notificationLine(msg::ping, juce::var(new juce::DynamicObject())));
    }
    const int startedCount = outbox.playbackStartedCount.load(std::memory_order_acquire);
    while (sentPlaybackStarted < startedCount) {
        ++sentPlaybackStarted;
        sendLine(notificationLine(msg::playbackStarted, juce::var(new juce::DynamicObject())));
    }
    const int playheadSeq = outbox.playheadSeq.load(std::memory_order_acquire);
    if (playheadSeq != sentPlayheadSeq) {
        sentPlayheadSeq = playheadSeq;
        auto* payload = new juce::DynamicObject();
        payload->setProperty("positionMs", outbox.playheadMs.load(std::memory_order_relaxed));
        payload->setProperty("playing", outbox.playheadPlaying.load(std::memory_order_relaxed));
        sendLine(notificationLine(msg::playhead, juce::var(payload)));
    }
    const int bpmSeq = outbox.bpmSeq.load(std::memory_order_acquire);
    if (bpmSeq != sentBpmSeq) {
        sentBpmSeq = bpmSeq;
        auto* payload = new juce::DynamicObject();
        payload->setProperty("bpm", outbox.bpm.load(std::memory_order_relaxed));
        sendLine(notificationLine(msg::bpm, juce::var(payload)));
    }
}

}  // namespace oubridge

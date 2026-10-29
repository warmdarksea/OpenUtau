#include "BridgeState.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>
#include <cstring>

namespace oubridge {

namespace {

// The wire trim (§8): OpenUtau pans constant-power, so pre-fader wire audio is
// scaled by cos(pi/4) per channel. Reproducing the mixer unwinds it first.
const float kWireTrim = std::sqrt(0.5f);

constexpr juce::uint32 kStateMagic = 0x4f554252;  // "OUBR"
constexpr int kStateVersion = 1;
// Above this the audio cache stays out of the DAW session chunk; OpenUtau
// re-sends audio on reconnect, so persistence is a convenience, not a need.
constexpr juce::int64 kMaxPersistedAudioBytes = juce::int64(256) << 20;

}  // namespace

BridgeState::BridgeState() {
    currentPlaylist = std::make_shared<const Playlist>();
}

void BridgeState::setUstx(const juce::String& ustx) {
    const juce::ScopedLock guard(lock);
    ustxDocument = ustx;
    bumpRevision();
}

void BridgeState::setTracks(std::vector<TrackInfo> tracks) {
    const juce::ScopedLock guard(lock);
    trackList = std::move(tracks);
    rebuildPlaylistLocked();
}

void BridgeState::setProjectInfo(const juce::String& newName, bool nowSaved) {
    const juce::ScopedLock guard(lock);
    name = newName;
    saved = nowSaved;
    bumpRevision();
}

juce::StringArray BridgeState::setLayout(std::vector<PartLayout> layout) {
    const juce::ScopedLock guard(lock);
    layoutList = std::move(layout);
    juce::StringArray missing;
    for (const auto& part : layoutList) {
        if (audioCache.find(part.audioHash) == audioCache.end()) {
            const auto hash = formatHash(part.audioHash);
            if (!missing.contains(hash)) {
                missing.add(hash);
            }
        }
    }
    rebuildPlaylistLocked();
    return missing;
}

bool BridgeState::hasAudio(std::uint64_t hash) const {
    const juce::ScopedLock guard(lock);
    return audioCache.find(hash) != audioCache.end();
}

void BridgeState::storeAudio(std::uint64_t hash, const void* bytes, int numBytes) {
    const int numFrames = numBytes / (kChannels * static_cast<int>(sizeof(float)));
    juce::AudioBuffer<float> buffer(kChannels, juce::jmax(1, numFrames));
    buffer.clear();
    const auto* data = static_cast<const juce::uint8*>(bytes);
    for (int frame = 0; frame < numFrames; ++frame) {
        for (int channel = 0; channel < kChannels; ++channel) {
            float sample;
            std::memcpy(&sample, data + (frame * kChannels + channel) * sizeof(float), sizeof(float));
            // Wire format is little-endian; so is every platform this builds on.
            buffer.setSample(channel, frame, sample);
        }
    }
    const juce::ScopedLock guard(lock);
    auto& entry = audioCache[hash];
    entry.source = std::move(buffer);
    entry.resampled.reset();
    entry.resampledRate = 0.0;
    rebuildPlaylistLocked();
}

void BridgeState::setTargetSampleRate(double rate) {
    const juce::ScopedLock guard(lock);
    if (rate > 0 && std::abs(rate - targetRate) > 0.001) {
        targetRate = rate;
        rebuildPlaylistLocked();
    }
}

void BridgeState::setTrackFilter(int trackNo) {
    trackFilterNo.store(trackNo);
    const juce::ScopedLock guard(lock);
    rebuildPlaylistLocked();
}

std::shared_ptr<const Playlist> BridgeState::playlist() const {
    const juce::ScopedLock guard(lock);
    return currentPlaylist;
}

juce::String BridgeState::projectName() const {
    const juce::ScopedLock guard(lock);
    return name;
}

bool BridgeState::projectSaved() const {
    const juce::ScopedLock guard(lock);
    return saved;
}

std::vector<TrackInfo> BridgeState::tracks() const {
    const juce::ScopedLock guard(lock);
    return trackList;
}

int BridgeState::numParts() const {
    const juce::ScopedLock guard(lock);
    return static_cast<int>(layoutList.size());
}

int BridgeState::numCachedParts() const {
    const juce::ScopedLock guard(lock);
    int cached = 0;
    for (const auto& part : layoutList) {
        if (audioCache.find(part.audioHash) != audioCache.end()) {
            ++cached;
        }
    }
    return cached;
}

std::shared_ptr<const juce::AudioBuffer<float>> BridgeState::resampledLocked(CachedAudio& entry, double rate) {
    if (entry.resampled != nullptr && std::abs(entry.resampledRate - rate) < 0.001) {
        return entry.resampled;
    }
    const int sourceFrames = entry.source.getNumSamples();
    if (std::abs(rate - kSampleRate) < 0.001) {
        auto copy = std::make_shared<juce::AudioBuffer<float>>(entry.source);
        entry.resampled = copy;
        entry.resampledRate = rate;
        return entry.resampled;
    }
    const double speedRatio = kSampleRate / rate;
    const int outFrames = juce::jmax(1, static_cast<int>(std::floor(sourceFrames / speedRatio)));
    // The interpolator reads a few samples past the end; a padded copy keeps it in bounds.
    juce::AudioBuffer<float> padded(kChannels, sourceFrames + 8);
    padded.clear();
    for (int channel = 0; channel < kChannels; ++channel) {
        padded.copyFrom(channel, 0, entry.source, channel, 0, sourceFrames);
    }
    auto out = std::make_shared<juce::AudioBuffer<float>>(kChannels, outFrames);
    for (int channel = 0; channel < kChannels; ++channel) {
        juce::LagrangeInterpolator interpolator;
        interpolator.process(speedRatio, padded.getReadPointer(channel),
                             out->getWritePointer(channel), outFrames);
    }
    entry.resampled = out;
    entry.resampledRate = rate;
    return entry.resampled;
}

void BridgeState::rebuildPlaylistLocked() {
    auto next = std::make_shared<Playlist>();
    next->sampleRate = targetRate;
    const int filter = trackFilterNo.load();
    for (const auto& part : layoutList) {
        if (filter >= 0 && part.trackNo != filter) {
            continue;
        }
        auto cached = audioCache.find(part.audioHash);
        if (cached == audioCache.end()) {
            continue;
        }
        float gainLeft = 1.0f;
        float gainRight = 1.0f;
        if (filter < 0) {
            // All-tracks mode reproduces OpenUtau's own mix: effective mute, volume
            // in dB and the constant-power pan law, applied to the un-trimmed signal.
            if (part.trackNo >= 0 && part.trackNo < static_cast<int>(trackList.size())) {
                const auto& track = trackList[static_cast<size_t>(part.trackNo)];
                if (track.muted) {
                    continue;
                }
                const float gain = juce::Decibels::decibelsToGain(static_cast<float>(track.volumeDb));
                const double theta = (juce::jlimit(-100.0, 100.0, track.pan) + 100.0) / 200.0
                    * juce::MathConstants<double>::halfPi;
                gainLeft = gain * static_cast<float>(std::cos(theta)) / kWireTrim;
                gainRight = gain * static_cast<float>(std::sin(theta)) / kWireTrim;
            }
        }
        PlaylistEntry entry;
        entry.startSample = static_cast<juce::int64>(std::llround(part.startMs / 1000.0 * targetRate));
        entry.audio = resampledLocked(cached->second, targetRate);
        entry.gainLeft = gainLeft;
        entry.gainRight = gainRight;
        next->lengthSamples = juce::jmax(next->lengthSamples,
                                         entry.startSample + entry.audio->getNumSamples());
        next->entries.push_back(std::move(entry));
    }
    currentPlaylist = std::move(next);
    bumpRevision();
}

void BridgeState::bumpRevision() {
    revisionCounter.fetch_add(1, std::memory_order_release);
}

// ---- DAW session persistence ------------------------------------------------

void BridgeState::save(juce::MemoryBlock& destination) const {
    const juce::ScopedLock guard(lock);
    juce::MemoryOutputStream out(destination, false);
    out.writeInt(static_cast<int>(kStateMagic));
    out.writeInt(kStateVersion);
    out.writeInt(trackFilterNo.load());
    out.writeString(name);
    out.writeBool(saved);
    out.writeString(ustxDocument);
    out.writeInt(static_cast<int>(trackList.size()));
    for (const auto& track : trackList) {
        out.writeString(track.name);
        out.writeDouble(track.volumeDb);
        out.writeDouble(track.pan);
        out.writeBool(track.muted);
        out.writeString(track.singer);
        out.writeString(track.engine);
    }
    out.writeInt(static_cast<int>(layoutList.size()));
    for (const auto& part : layoutList) {
        out.writeInt(part.trackNo);
        out.writeDouble(part.startMs);
        out.writeDouble(part.endMs);
        out.writeInt64(static_cast<juce::int64>(part.audioHash));
    }
    juce::int64 audioBytes = 0;
    for (const auto& [hash, cached] : audioCache) {
        audioBytes += juce::int64(cached.source.getNumSamples()) * kChannels * sizeof(float);
    }
    if (audioBytes > kMaxPersistedAudioBytes) {
        // The session stays small; OpenUtau re-sends the audio on reconnect.
        out.writeInt(0);
        return;
    }
    out.writeInt(static_cast<int>(audioCache.size()));
    for (const auto& [hash, cached] : audioCache) {
        out.writeInt64(static_cast<juce::int64>(hash));
        const int numFrames = cached.source.getNumSamples();
        out.writeInt(numFrames);
        // Interleave back to the wire layout; GZIP squeezes the zero runs.
        juce::MemoryOutputStream pcm;
        {
            juce::GZIPCompressorOutputStream zipped(pcm);
            for (int frame = 0; frame < numFrames; ++frame) {
                for (int channel = 0; channel < kChannels; ++channel) {
                    zipped.writeFloat(cached.source.getSample(channel, frame));
                }
            }
        }
        out.writeInt(static_cast<int>(pcm.getDataSize()));
        out.write(pcm.getData(), pcm.getDataSize());
    }
}

bool BridgeState::restore(const void* data, size_t size) {
    juce::MemoryInputStream in(data, size, false);
    if (in.readInt() != static_cast<int>(kStateMagic) || in.readInt() != kStateVersion) {
        return false;
    }
    const juce::ScopedLock guard(lock);
    trackFilterNo.store(in.readInt());
    name = in.readString();
    saved = in.readBool();
    ustxDocument = in.readString();
    trackList.clear();
    const int numTracks = in.readInt();
    for (int i = 0; i < numTracks && !in.isExhausted(); ++i) {
        TrackInfo track;
        track.name = in.readString();
        track.volumeDb = in.readDouble();
        track.pan = in.readDouble();
        track.muted = in.readBool();
        track.singer = in.readString();
        track.engine = in.readString();
        trackList.push_back(std::move(track));
    }
    layoutList.clear();
    const int numParts = in.readInt();
    for (int i = 0; i < numParts && !in.isExhausted(); ++i) {
        PartLayout part;
        part.trackNo = in.readInt();
        part.startMs = in.readDouble();
        part.endMs = in.readDouble();
        part.audioHash = static_cast<std::uint64_t>(in.readInt64());
        layoutList.push_back(part);
    }
    audioCache.clear();
    const int numAudio = in.readInt();
    for (int i = 0; i < numAudio && !in.isExhausted(); ++i) {
        const auto hash = static_cast<std::uint64_t>(in.readInt64());
        const int numFrames = in.readInt();
        const int zippedBytes = in.readInt();
        if (numFrames < 0 || zippedBytes < 0 ||
            juce::int64(numFrames) * kChannels * sizeof(float) > juce::int64(kMaxFrameBytes)) {
            return false;
        }
        juce::MemoryBlock zipped;
        if (in.readIntoMemoryBlock(zipped, zippedBytes) != static_cast<size_t>(zippedBytes)) {
            return false;
        }
        juce::MemoryInputStream zippedIn(zipped, false);
        juce::GZIPDecompressorInputStream pcm(&zippedIn, false);
        juce::AudioBuffer<float> buffer(kChannels, juce::jmax(1, numFrames));
        buffer.clear();
        for (int frame = 0; frame < numFrames; ++frame) {
            for (int channel = 0; channel < kChannels; ++channel) {
                buffer.setSample(channel, frame, pcm.readFloat());
            }
        }
        auto& entry = audioCache[hash];
        entry.source = std::move(buffer);
        entry.resampled.reset();
        entry.resampledRate = 0.0;
    }
    rebuildPlaylistLocked();
    return true;
}

}  // namespace oubridge

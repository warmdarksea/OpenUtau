// Synchronized project state: what OpenUtau pushed (tracks, part layout, audio
// by hash) plus the plugin's own playback settings, compiled into an immutable
// playlist snapshot the audio thread reads without locking.
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "BridgeProtocol.h"

namespace oubridge {

struct TrackInfo {
    juce::String name;
    double volumeDb = 0.0;  // OpenUtau scale: dB, 0 = unity
    double pan = 0.0;       // OpenUtau scale: -100 .. +100
    bool muted = false;     // effective mute, solo already resolved (§6.1)
    juce::String singer;    // v1.2, informational
    juce::String engine;    // v1.2, informational
};

struct PartLayout {
    int trackNo = 0;
    double startMs = 0.0;
    double endMs = 0.0;
    std::uint64_t audioHash = 0;
};

// One placed, resampled, gain-baked piece of audio on the shared timeline.
struct PlaylistEntry {
    juce::int64 startSample = 0;
    std::shared_ptr<const juce::AudioBuffer<float>> audio;  // stereo, playlist rate
    float gainLeft = 1.0f;
    float gainRight = 1.0f;
};

struct Playlist {
    double sampleRate = kSampleRate;
    std::vector<PlaylistEntry> entries;
    juce::int64 lengthSamples = 0;
};

class BridgeState {
public:
    BridgeState();

    // ---- written from the connection thread -------------------------------
    void setUstx(const juce::String& ustx);
    void setTracks(std::vector<TrackInfo> tracks);
    void setProjectInfo(const juce::String& name, bool saved);
    // Replaces the layout; returns the hashes not yet cached, in layout order.
    juce::StringArray setLayout(std::vector<PartLayout> layout);
    bool hasAudio(std::uint64_t hash) const;
    // Interleaved stereo float32 LE wire payload -> cached 44.1 kHz buffer.
    void storeAudio(std::uint64_t hash, const void* bytes, int numBytes);

    // ---- playback settings (message thread / state restore) ---------------
    void setTargetSampleRate(double rate);
    // -1 = all tracks through the OpenUtau mixer model; >= 0 = that track, raw pre-fader.
    void setTrackFilter(int trackNo);
    int trackFilter() const { return trackFilterNo.load(); }

    // ---- audio thread ------------------------------------------------------
    std::shared_ptr<const Playlist> playlist() const;

    // ---- UI -----------------------------------------------------------------
    int revision() const { return revisionCounter.load(std::memory_order_acquire); }
    juce::String projectName() const;
    bool projectSaved() const;
    std::vector<TrackInfo> tracks() const;
    int numParts() const;
    int numCachedParts() const;

    // ---- DAW session persistence -------------------------------------------
    void save(juce::MemoryBlock& destination) const;
    bool restore(const void* data, size_t size);

private:
    struct CachedAudio {
        juce::AudioBuffer<float> source;      // stereo, 44.1 kHz
        std::shared_ptr<const juce::AudioBuffer<float>> resampled;
        double resampledRate = 0.0;
    };

    void rebuildPlaylistLocked();
    std::shared_ptr<const juce::AudioBuffer<float>> resampledLocked(CachedAudio& entry, double rate);
    void bumpRevision();

    mutable juce::CriticalSection lock;
    juce::String ustxDocument;
    juce::String name;
    bool saved = false;
    std::vector<TrackInfo> trackList;
    std::vector<PartLayout> layoutList;
    std::map<std::uint64_t, CachedAudio> audioCache;
    double targetRate = kSampleRate;

    std::shared_ptr<const Playlist> currentPlaylist;
    std::atomic<int> trackFilterNo{-1};
    std::atomic<int> revisionCounter{0};
};

}  // namespace oubridge

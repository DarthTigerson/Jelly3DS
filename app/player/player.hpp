#pragma once

#include "../gfx.hpp"
#include "../tasks.hpp"
#include "decoders.hpp"

#include "curl_http.hpp"
#include "jelly/platform.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
}

namespace app {

class ByteSource;

/// Thread-safe FIFO of demuxed packets with FLUSH/EOF markers.
class PacketQueue {
public:
    enum class Kind { Packet, Flush, Eof };
    struct Entry {
        Kind kind;
        AVPacket* pkt;
    };
    void push(Entry e);
    /// Waits up to timeoutMs; false on timeout or abort.
    bool pop(Entry& out, int timeoutMs);
    void clear();
    void abort();
    /// Clears the abort flag so the queue can be reused by the next open().
    void reset();
    size_t size() const;

private:
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<Entry> q_;
    bool aborted_ = false;
};

/// Plays one MPEG-TS/MP4 (H.264 + AAC) from the SD card or an HTTP stream.
/// Threads: demux (I/O), video (decode + A/V sync), audio (decode → NDSP). Audio is the master clock.
class Player {
public:
    struct Source {
        std::string path;  // local file, or
        std::string url;   // progressive HTTP stream
        jelly::Headers headers;
        double timeOffset = 0;  // media time of the stream's first sample (streams started mid-way)
        bool verifyTls = false;
        std::string caFile;
    };

    Player();
    ~Player();

    /// Phase 1, safe on a worker thread: opens the file/stream and probes it (no linear-memory use).
    bool open(const Source& src, double startAt, std::string& error);
    /// Phase 2, main thread only: decoders, frame buffers, texture and audio live in linear memory, whose
    /// allocator is not thread-safe. Starts the playback threads.
    bool start(bool useHardwareDecoder, bool fastSoftware, std::string& error);
    void close();

    void setPaused(bool paused);
    bool paused() const { return paused_; }
    /// Local files only; streams are re-opened at the new offset by the caller.
    void seek(double seconds);
    bool seekable() const { return seekable_; }

    double position() const;
    double duration() const { return duration_; }
    bool ended() const;
    bool failed() const { return failed_; }
    std::string error() const;
    std::string decoderName() const { return decoderName_; }
    bool hasAudio() const { return audioStream_ >= 0; }
    bool buffering() const;

    /// Main thread, outside of a GPU frame: uploads the newest decoded frame. Returns true if it changed.
    bool updateTexture();
    const Texture& texture() const { return tex_; }
    int videoWidth() const { return displayW_; }
    int videoHeight() const { return displayH_; }

    /// Volume boost (1 = as decoded). Takes effect immediately.
    void setGain(float gain) { gain_ = gain; }

    static bool initAudio();
    static void exitAudio();

private:
    void demuxLoop();
    void videoLoop();
    void audioLoop();
    double audioClock() const;
    bool audioIdle() const;
    double clock() const;
    double toSeconds(int64_t ts, AVRational tb) const;
    void setError(const std::string& e);
    void submitAudio(const int16_t* samples, int frames, double pts);
    void resetAudioOutput();

    std::unique_ptr<ByteSource> io_;
    AVFormatContext* fmt_ = nullptr;
    AVIOContext* avio_ = nullptr;
    AVCodecContext* audioCtx_ = nullptr;
    std::unique_ptr<VideoDecoder> video_;
    int videoStream_ = -1, audioStream_ = -1;
    AVRational videoTb_{1, 1}, audioTb_{1, 1};
    double startTime_ = 0;  // container start_time in seconds
    double timeOffset_ = 0;
    double startAt_ = 0;
    double duration_ = 0;
    bool seekable_ = false;
    std::string decoderName_;

    PacketQueue videoQ_, audioQ_;
    Thread demuxThread_, videoThread_, audioThread_;
    std::atomic_bool stop_{false};
    std::atomic_bool paused_{false};
    std::atomic_bool failed_{false};
    std::atomic_bool demuxEof_{false}, videoDone_{false}, audioDone_{false};
    std::atomic_bool seekRequest_{false};
    std::atomic<double> seekTarget_{0};
    std::atomic<double> skipUntil_{-1};
    mutable std::mutex errorMutex_;
    std::string error_;

    // Video frames: decoded into two linear-memory slots, newest one handed to the main thread.
    uint16_t* slots_[2] = {nullptr, nullptr};
    uint16_t* staging_ = nullptr;
    std::mutex frameMutex_;
    int readySlot_ = -1;
    int decodeW_ = 0, decodeH_ = 0;
    int displayW_ = 0, displayH_ = 0;
    Texture tex_;
    std::atomic<double> lastVideoPts_{0};

    // Audio output (NDSP channel 0)
    static constexpr int AUDIO_BUFS = 8;
    static constexpr int AUDIO_FRAMES = 2048;
    ndspWaveBuf waveBufs_[AUDIO_BUFS]{};
    int16_t* audioMem_ = nullptr;
    double bufPts_[AUDIO_BUFS] = {};
    int nextBuf_ = 0;
    int sampleRate_ = 32000;
    std::atomic<float> gain_{1.0f};
    mutable std::atomic<double> lastAudioClock_{0};
    std::atomic<int> flushGen_{0};
    static bool audioAvailable_;
    std::atomic_bool audioStarted_{false};

    // Fallback clock when there is no audio track
    u64 wallStartMs_ = 0;
    double wallStartPos_ = 0;
    mutable std::mutex wallMutex_;
};

}  // namespace app

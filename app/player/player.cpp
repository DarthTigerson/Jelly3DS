#include "player.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
#include <libavutil/channel_layout.h>
}

namespace app {

bool Player::audioAvailable_ = false;

// =============================================================================================
// Byte sources (custom AVIO): buffered SD file, or curl → ring buffer for streams
// =============================================================================================

class ByteSource {
public:
    virtual ~ByteSource() = default;
    virtual int read(uint8_t* buf, int size) = 0;
    virtual int64_t seek(int64_t offset, int whence) = 0;
    virtual bool seekable() const = 0;
    virtual void abort() {}
    virtual size_t buffered() const { return SIZE_MAX; }
};

namespace {

class FileSource : public ByteSource {
public:
    bool open(const std::string& path) {
        f_ = std::fopen(path.c_str(), "rb");
        if (!f_) return false;
        buf_.resize(256 * 1024);  // big reads keep the SD card from stalling playback
        std::setvbuf(f_, buf_.data(), _IOFBF, buf_.size());
        std::fseek(f_, 0, SEEK_END);
        size_ = std::ftell(f_);
        std::fseek(f_, 0, SEEK_SET);
        return true;
    }
    ~FileSource() override {
        if (f_) std::fclose(f_);
    }
    int read(uint8_t* buf, int size) override {
        size_t n = std::fread(buf, 1, static_cast<size_t>(size), f_);
        if (n == 0) return AVERROR_EOF;
        return static_cast<int>(n);
    }
    int64_t seek(int64_t offset, int whence) override {
        if (whence == AVSEEK_SIZE) return size_;
        if (std::fseek(f_, static_cast<long>(offset), whence & ~AVSEEK_FORCE) != 0) return -1;
        return std::ftell(f_);
    }
    bool seekable() const override { return true; }

private:
    FILE* f_ = nullptr;
    std::vector<char> buf_;
    int64_t size_ = 0;
};

class HttpSource : public ByteSource {
public:
    static constexpr size_t RING = 3 * 1024 * 1024;

    bool open(const std::string& url, const jelly::Headers& headers, const jelly::CurlOptions& options) {
        ring_.resize(RING);
        http_ = std::make_unique<jelly::CurlHttp>(options);
        url_ = url;
        headers_ = headers;
        return thread_.start([this] { run(); }, 0x2E, -2, 128 * 1024);
    }
    ~HttpSource() override {
        abort();
        thread_.join();
    }
    void abort() override {
        cancel_ = true;
        cv_.notify_all();
    }
    int read(uint8_t* buf, int size) override {
        std::unique_lock<std::mutex> lock(m_);
        cv_.wait(lock, [&] { return count_ > 0 || done_ || cancel_; });
        if (cancel_) return AVERROR_EXIT;
        if (count_ == 0) return error_.empty() ? AVERROR_EOF : AVERROR(EIO);
        size_t n = std::min(static_cast<size_t>(size), count_);
        for (size_t i = 0; i < n; ++i) buf[i] = ring_[(head_ + i) % RING];
        head_ = (head_ + n) % RING;
        count_ -= n;
        cv_.notify_all();
        return static_cast<int>(n);
    }
    int64_t seek(int64_t, int) override { return -1; }
    bool seekable() const override { return false; }
    size_t buffered() const override {
        std::lock_guard<std::mutex> lock(m_);
        return count_;
    }
    std::string error() const {
        std::lock_guard<std::mutex> lock(m_);
        return error_;
    }

private:
    void run() {
        auto sink = [this](const char* data, size_t len) {
            size_t off = 0;
            while (off < len) {
                std::unique_lock<std::mutex> lock(m_);
                cv_.wait(lock, [&] { return count_ < RING || cancel_; });
                if (cancel_) return false;
                size_t n = std::min(len - off, RING - count_);
                size_t tail = (head_ + count_) % RING;
                for (size_t i = 0; i < n; ++i) ring_[(tail + i) % RING] = static_cast<uint8_t>(data[off + i]);
                count_ += n;
                off += n;
                cv_.notify_all();
            }
            return true;
        };
        auto r = http_->stream(url_, headers_, sink, cancel_);
        std::lock_guard<std::mutex> lock(m_);
        if (!r.error.empty() && r.error != "cancelled")
            error_ = r.error;
        else if (r.status >= 400)
            error_ = "Server returned HTTP " + std::to_string(r.status);
        done_ = true;
        cv_.notify_all();
    }

    std::unique_ptr<jelly::CurlHttp> http_;
    std::string url_;
    jelly::Headers headers_;
    Thread thread_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::vector<uint8_t> ring_;
    size_t head_ = 0, count_ = 0;
    bool done_ = false;
    std::atomic_bool cancel_{false};
    std::string error_;
};

int avioRead(void* opaque, uint8_t* buf, int size) { return static_cast<ByteSource*>(opaque)->read(buf, size); }
int64_t avioSeek(void* opaque, int64_t offset, int whence) {
    return static_cast<ByteSource*>(opaque)->seek(offset, whence);
}

// No vertical flip: texture memory row 0 is the top of the image (sampled at v = 1.0).
constexpr u32 TRANSFER_FLAGS = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) |
                               GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) |
                               GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);

inline int16_t toS16(float v) {
    v *= 32767.0f;
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return static_cast<int16_t>(v);
}

/// Converts any decoded audio frame to interleaved stereo s16.
void appendStereo(const AVFrame* f, std::vector<int16_t>& out) {
    const int n = f->nb_samples;
    const int ch = f->ch_layout.nb_channels;
    const int l = 0, r = ch > 1 ? 1 : 0;
    out.reserve(out.size() + n * 2);
    switch (f->format) {
    case AV_SAMPLE_FMT_FLTP: {
        auto* a = reinterpret_cast<const float*>(f->data[l]);
        auto* b = reinterpret_cast<const float*>(f->data[r]);
        for (int i = 0; i < n; ++i) {
            out.push_back(toS16(a[i]));
            out.push_back(toS16(b[i]));
        }
        break;
    }
    case AV_SAMPLE_FMT_FLT: {
        auto* a = reinterpret_cast<const float*>(f->data[0]);
        for (int i = 0; i < n; ++i) {
            out.push_back(toS16(a[i * ch + l]));
            out.push_back(toS16(a[i * ch + r]));
        }
        break;
    }
    case AV_SAMPLE_FMT_S16P: {
        auto* a = reinterpret_cast<const int16_t*>(f->data[l]);
        auto* b = reinterpret_cast<const int16_t*>(f->data[r]);
        for (int i = 0; i < n; ++i) {
            out.push_back(a[i]);
            out.push_back(b[i]);
        }
        break;
    }
    case AV_SAMPLE_FMT_S16: {
        auto* a = reinterpret_cast<const int16_t*>(f->data[0]);
        for (int i = 0; i < n; ++i) {
            out.push_back(a[i * ch + l]);
            out.push_back(a[i * ch + r]);
        }
        break;
    }
    default:
        out.insert(out.end(), static_cast<size_t>(n) * 2, 0);
    }
}

}  // namespace

// =============================================================================================
// PacketQueue
// =============================================================================================

void PacketQueue::push(Entry e) {
    std::lock_guard<std::mutex> lock(m_);
    if (aborted_) {
        if (e.pkt) av_packet_free(&e.pkt);
        return;
    }
    q_.push_back(e);
    cv_.notify_one();
}

bool PacketQueue::pop(Entry& out, int timeoutMs) {
    std::unique_lock<std::mutex> lock(m_);
    if (!cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return !q_.empty() || aborted_; }))
        return false;
    if (aborted_ || q_.empty()) return false;
    out = q_.front();
    q_.pop_front();
    return true;
}

void PacketQueue::clear() {
    std::lock_guard<std::mutex> lock(m_);
    for (auto& e : q_)
        if (e.pkt) av_packet_free(&e.pkt);
    q_.clear();
}

void PacketQueue::abort() {
    std::lock_guard<std::mutex> lock(m_);
    aborted_ = true;
    cv_.notify_all();
}

void PacketQueue::reset() {
    std::lock_guard<std::mutex> lock(m_);
    aborted_ = false;
}

size_t PacketQueue::size() const {
    std::lock_guard<std::mutex> lock(m_);
    return q_.size();
}

// =============================================================================================
// Player
// =============================================================================================

bool Player::initAudio() {
    audioAvailable_ = R_SUCCEEDED(ndspInit());
    if (audioAvailable_) ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    return audioAvailable_;
}

void Player::exitAudio() {
    if (audioAvailable_) ndspExit();
    audioAvailable_ = false;
}

Player::Player() = default;
Player::~Player() { close(); }

void Player::setError(const std::string& e) {
    std::lock_guard<std::mutex> lock(errorMutex_);
    if (error_.empty()) error_ = e;
    failed_ = true;
}

std::string Player::error() const {
    std::lock_guard<std::mutex> lock(errorMutex_);
    return error_;
}

double Player::toSeconds(int64_t ts, AVRational tb) const {
    if (ts == AV_NOPTS_VALUE) return 0;
    return ts * av_q2d(tb) - startTime_ + timeOffset_;
}

bool Player::open(const Source& src, double startAt, std::string& error) {
    close();
    stop_ = false;
    failed_ = false;
    demuxEof_ = videoDone_ = audioDone_ = false;
    error_.clear();
    timeOffset_ = src.timeOffset;

    // --- I/O
    if (!src.path.empty()) {
        auto f = std::make_unique<FileSource>();
        if (!f->open(src.path)) {
            error = "Cannot open " + src.path;
            return false;
        }
        io_ = std::move(f);
    } else {
        auto h = std::make_unique<HttpSource>();
        jelly::CurlOptions o;
        o.verifyTls = src.verifyTls;
        o.caFile = src.caFile;
        o.stallTimeoutSec = 600;  // pausing a stream leaves the connection idle for a while
        if (!h->open(src.url, src.headers, o)) {
            error = "Cannot start the network stream";
            return false;
        }
        io_ = std::move(h);
    }
    seekable_ = io_->seekable();

    const int ioSize = 64 * 1024;
    auto* ioBuf = static_cast<unsigned char*>(av_malloc(ioSize));
    avio_ = avio_alloc_context(ioBuf, ioSize, 0, io_.get(), avioRead, nullptr, seekable_ ? avioSeek : nullptr);
    avio_->seekable = seekable_ ? AVIO_SEEKABLE_NORMAL : 0;

    fmt_ = avformat_alloc_context();
    fmt_->pb = avio_;
    fmt_->flags |= AVFMT_FLAG_CUSTOM_IO;
    fmt_->probesize = seekable_ ? 2 * 1024 * 1024 : 512 * 1024;
    fmt_->max_analyze_duration = 3 * AV_TIME_BASE;
    if (avformat_open_input(&fmt_, nullptr, nullptr, nullptr) < 0) {
        fmt_ = nullptr;  // freed by avformat_open_input on failure
        auto* h = dynamic_cast<HttpSource*>(io_.get());
        error = h && !h->error().empty() ? h->error() : "Unsupported or damaged video file";
        close();
        return false;
    }
    if (avformat_find_stream_info(fmt_, nullptr) < 0) {
        error = "Could not read the video streams";
        close();
        return false;
    }
    videoStream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    audioStream_ = audioAvailable_ ? av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0) : -1;
    if (videoStream_ < 0) {
        error = "No video track";
        close();
        return false;
    }
    for (unsigned i = 0; i < fmt_->nb_streams; ++i)
        if (static_cast<int>(i) != videoStream_ && static_cast<int>(i) != audioStream_)
            fmt_->streams[i]->discard = AVDISCARD_ALL;

    startTime_ = fmt_->start_time != AV_NOPTS_VALUE ? fmt_->start_time / static_cast<double>(AV_TIME_BASE) : 0;
    duration_ = (seekable_ && fmt_->duration > 0) ? fmt_->duration / static_cast<double>(AV_TIME_BASE) + timeOffset_ : 0;

    AVStream* vs0 = fmt_->streams[videoStream_];
    displayW_ = vs0->codecpar->width;
    displayH_ = vs0->codecpar->height;
    if (displayW_ <= 0 || displayH_ <= 0 || displayW_ > 512 || displayH_ > 256) {
        error = "Video is " + std::to_string(displayW_) + "x" + std::to_string(displayH_) +
                "; the 3DS needs it converted by the server (download it again)";
        close();
        return false;
    }
    startAt_ = startAt;
    return true;
}

bool Player::start(bool useHardwareDecoder, bool fastSoftware, std::string& error) {
    if (!fmt_) {
        error = "Nothing to play";
        return false;
    }
    const double startAt = startAt_;
    // --- Video decoder
    AVStream* vs = fmt_->streams[videoStream_];
    videoTb_ = vs->time_base;
    std::string decErr;
    if (useHardwareDecoder) {
        video_ = makeMvdDecoder();
        if (!video_->open(vs->codecpar, decErr)) video_.reset();
    }
    if (!video_) {
        video_ = makeSoftwareDecoder(fastSoftware);
        if (!video_->open(vs->codecpar, decErr)) {
            error = decErr;
            close();
            return false;
        }
    }
    decoderName_ = video_->name();
    decodeW_ = video_->outWidth();
    decodeH_ = video_->outHeight();
    for (auto& s : slots_) {
        s = static_cast<uint16_t*>(linearMemAlign(static_cast<size_t>(decodeW_) * decodeH_ * 2, 0x80));
        if (!s) {
            error = "Out of memory for video frames";
            close();
            return false;
        }
    }
    if (!tex_.create(displayW_, displayH_)) {
        error = "Out of video memory";
        close();
        return false;
    }
    staging_ = static_cast<uint16_t*>(linearMemAlign(static_cast<size_t>(tex_.tex.width) * tex_.tex.height * 2, 0x80));
    if (!staging_) {
        error = "Out of memory";
        close();
        return false;
    }
    std::memset(staging_, 0, static_cast<size_t>(tex_.tex.width) * tex_.tex.height * 2);

    // --- Audio
    if (audioStream_ >= 0) {
        AVStream* as = fmt_->streams[audioStream_];
        audioTb_ = as->time_base;
        const AVCodec* codec = avcodec_find_decoder(as->codecpar->codec_id);
        audioCtx_ = codec ? avcodec_alloc_context3(codec) : nullptr;
        if (!audioCtx_ || avcodec_parameters_to_context(audioCtx_, as->codecpar) < 0 ||
            avcodec_open2(audioCtx_, codec, nullptr) < 0) {
            if (audioCtx_) avcodec_free_context(&audioCtx_);
            audioStream_ = -1;  // play silently rather than fail
        }
    }
    if (audioStream_ >= 0) {
        sampleRate_ = audioCtx_->sample_rate > 0 ? audioCtx_->sample_rate : 32000;
        audioMem_ = static_cast<int16_t*>(linearAlloc(AUDIO_BUFS * AUDIO_FRAMES * 2 * sizeof(int16_t)));
        ndspChnReset(0);
        ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE);
        ndspChnSetRate(0, static_cast<float>(sampleRate_));
        ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
        float mix[12] = {1.0f, 1.0f};
        ndspChnSetMix(0, mix);
        for (int i = 0; i < AUDIO_BUFS; ++i) {
            waveBufs_[i] = {};
            waveBufs_[i].data_pcm16 = audioMem_ + i * AUDIO_FRAMES * 2;
            waveBufs_[i].status = NDSP_WBUF_FREE;
        }
        nextBuf_ = 0;
        ndspChnSetPaused(0, false);
    }

    // --- Initial position
    lastAudioClock_ = startAt > timeOffset_ ? startAt : timeOffset_;
    wallStartPos_ = lastAudioClock_;
    wallStartMs_ = osGetTime();
    audioStarted_ = false;
    paused_ = false;
    if (seekable_ && startAt > 1.0) {
        int64_t ts = static_cast<int64_t>((startAt - timeOffset_ + startTime_) * AV_TIME_BASE);
        av_seek_frame(fmt_, -1, ts, AVSEEK_FLAG_BACKWARD);
        skipUntil_ = startAt;
    } else {
        skipUntil_ = -1;
    }

    // --- Threads. Video decode sits just below the UI thread; audio gets the second core.
    demuxThread_.start([this] { demuxLoop(); }, 0x2F, -2, 64 * 1024);
    videoThread_.start([this] { videoLoop(); }, 0x31, -2, 128 * 1024);
    if (audioStream_ >= 0) audioThread_.start([this] { audioLoop(); }, 0x30, 1, 128 * 1024);
    return true;
}

void Player::close() {
    stop_ = true;
    videoQ_.abort();
    audioQ_.abort();
    if (io_) io_->abort();
    demuxThread_.join();
    videoThread_.join();
    audioThread_.join();
    videoQ_.clear();
    audioQ_.clear();
    videoQ_.reset();
    audioQ_.reset();

    if (audioMem_) {
        ndspChnReset(0);
        linearFree(audioMem_);
        audioMem_ = nullptr;
    }
    if (audioCtx_) avcodec_free_context(&audioCtx_);
    video_.reset();
    if (fmt_) avformat_close_input(&fmt_);
    if (avio_) {
        av_freep(&avio_->buffer);
        avio_context_free(&avio_);
    }
    io_.reset();
    for (auto& s : slots_) {
        if (s) linearFree(s);
        s = nullptr;
    }
    if (staging_) linearFree(staging_);
    staging_ = nullptr;
    tex_.destroy();
    readySlot_ = -1;
    videoStream_ = audioStream_ = -1;
}

void Player::setPaused(bool p) {
    if (p == paused_) return;
    {
        std::lock_guard<std::mutex> lock(wallMutex_);
        if (p) {
            wallStartPos_ = clock();
        } else {
            wallStartMs_ = osGetTime();
        }
    }
    paused_ = p;
    if (audioStream_ >= 0) ndspChnSetPaused(0, p);
}

void Player::seek(double seconds) {
    if (!seekable_) return;
    if (seconds < 0) seconds = 0;
    if (duration_ > 0 && seconds > duration_ - 1) seconds = duration_ - 1;
    seekTarget_ = seconds;
    seekRequest_ = true;
}

bool Player::audioIdle() const {
    for (const auto& wb : waveBufs_)
        if (wb.status == NDSP_WBUF_QUEUED || wb.status == NDSP_WBUF_PLAYING) return false;
    return true;
}

double Player::audioClock() const {
    if (!audioStarted_) return lastAudioClock_;
    u16 seq = ndspChnGetWaveBufSeq(0);
    for (int i = 0; i < AUDIO_BUFS; ++i) {
        const auto& wb = waveBufs_[i];
        if (wb.status == NDSP_WBUF_PLAYING && wb.sequence_id == seq) {
            double c = bufPts_[i] + static_cast<double>(ndspChnGetSamplePos(0)) / sampleRate_;
            lastAudioClock_ = c;
            return c;
        }
    }
    return lastAudioClock_;
}

double Player::clock() const {
    if (audioStream_ >= 0) return audioClock();
    std::lock_guard<std::mutex> lock(wallMutex_);
    if (paused_) return wallStartPos_;
    return wallStartPos_ + (osGetTime() - wallStartMs_) / 1000.0;
}

double Player::position() const {
    double p = clock();
    if (duration_ > 0 && p > duration_) p = duration_;
    return p < 0 ? 0 : p;
}

bool Player::ended() const { return demuxEof_ && videoDone_ && (audioStream_ < 0 || audioDone_); }

bool Player::buffering() const {
    if (paused_ || demuxEof_ || audioStream_ < 0) return false;
    return audioStarted_ && audioIdle() && audioQ_.size() == 0;
}

// --- demux ------------------------------------------------------------------------------------

void Player::demuxLoop() {
    while (!stop_) {
        if (seekRequest_) {
            double target = seekTarget_;
            int64_t ts = static_cast<int64_t>((target - timeOffset_ + startTime_) * AV_TIME_BASE);
            av_seek_frame(fmt_, -1, ts, AVSEEK_FLAG_BACKWARD);
            videoQ_.clear();
            audioQ_.clear();
            skipUntil_ = target;
            lastAudioClock_ = target;
            {
                std::lock_guard<std::mutex> lock(wallMutex_);
                wallStartPos_ = target;
                wallStartMs_ = osGetTime();
            }
            ++flushGen_;
            videoQ_.push({PacketQueue::Kind::Flush, nullptr});
            if (audioStream_ >= 0) audioQ_.push({PacketQueue::Kind::Flush, nullptr});
            demuxEof_ = videoDone_ = audioDone_ = false;
            seekRequest_ = false;
        }
        if (demuxEof_ || videoQ_.size() > 90 || audioQ_.size() > 300) {
            svcSleepThread(5 * 1000 * 1000);
            continue;
        }
        AVPacket* pkt = av_packet_alloc();
        int ret = av_read_frame(fmt_, pkt);
        if (ret < 0) {
            av_packet_free(&pkt);
            if (stop_) break;
            if (ret != AVERROR_EOF) {
                auto* h = dynamic_cast<HttpSource*>(io_.get());
                if (h && !h->error().empty()) setError("Stream interrupted: " + h->error());
            }
            videoQ_.push({PacketQueue::Kind::Eof, nullptr});
            if (audioStream_ >= 0) audioQ_.push({PacketQueue::Kind::Eof, nullptr});
            demuxEof_ = true;
            continue;
        }
        if (pkt->stream_index == videoStream_)
            videoQ_.push({PacketQueue::Kind::Packet, pkt});
        else if (pkt->stream_index == audioStream_)
            audioQ_.push({PacketQueue::Kind::Packet, pkt});
        else
            av_packet_free(&pkt);
    }
}

// --- video ------------------------------------------------------------------------------------

void Player::videoLoop() {
    bool waitKey = false;
    int target = 0;
    while (!stop_) {
        PacketQueue::Entry e{};
        if (!videoQ_.pop(e, 20)) continue;
        if (e.kind == PacketQueue::Kind::Flush) {
            video_->flush();
            waitKey = false;
            continue;
        }
        if (e.kind == PacketQueue::Kind::Eof) {
            videoDone_ = true;
            continue;
        }
        AVPacket* pkt = e.pkt;
        int gen = flushGen_;
        int64_t raw = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
        double ppts = toSeconds(raw, videoTb_);
        bool key = pkt->flags & AV_PKT_FLAG_KEY;
        double now = clock();

        // A decoder that fell more than a second behind drops to the next keyframe to catch up.
        if (waitKey && !key) {
            av_packet_free(&pkt);
            continue;
        }
        waitKey = false;
        bool seeking = ppts < skipUntil_;
        if (!seeking && audioStarted_ && !paused_ && now - ppts > 1.0 && !key) {
            waitKey = true;
            av_packet_free(&pkt);
            continue;
        }
        bool late = !seeking && audioStarted_ && now - ppts > 0.12;

        {
            std::lock_guard<std::mutex> lock(frameMutex_);
            target = readySlot_ == 0 ? 1 : 0;
        }
        int64_t fpts = AV_NOPTS_VALUE;
        int r = video_->decode(pkt, slots_[target], &fpts, seeking || late);
        av_packet_free(&pkt);
        if (r != 1) continue;
        double fsec = fpts != AV_NOPTS_VALUE ? toSeconds(fpts, videoTb_) : ppts;

        // Present on time: sleep until the audio clock reaches the frame.
        while (!stop_ && gen == flushGen_) {
            double c = clock();
            if (c >= fsec - 0.008) break;
            double wait = std::min(fsec - c, 0.02);
            svcSleepThread(static_cast<s64>(wait * 1e9));
        }
        if (stop_ || gen != flushGen_) continue;
        std::lock_guard<std::mutex> lock(frameMutex_);
        readySlot_ = target;
        lastVideoPts_ = fsec;
    }
}

bool Player::updateTexture() {
    if (!staging_ || !tex_.valid) return false;
    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        if (readySlot_ < 0) return false;
        uint16_t* src = slots_[readySlot_];
        GSPGPU_InvalidateDataCache(src, static_cast<u32>(decodeW_) * decodeH_ * 2);
        const int texW = tex_.tex.width;
        for (int y = 0; y < displayH_; ++y)
            std::memcpy(staging_ + y * texW, src + y * decodeW_, static_cast<size_t>(displayW_) * 2);
        readySlot_ = -1;
    }
    const u32 bytes = static_cast<u32>(tex_.tex.width) * tex_.tex.height * 2;
    GSPGPU_FlushDataCache(staging_, bytes);
    C3D_SyncDisplayTransfer(reinterpret_cast<u32*>(staging_), GX_BUFFER_DIM(tex_.tex.width, tex_.tex.height),
                            static_cast<u32*>(tex_.tex.data), GX_BUFFER_DIM(tex_.tex.width, tex_.tex.height),
                            TRANSFER_FLAGS);
    return true;
}

// --- audio ------------------------------------------------------------------------------------

void Player::resetAudioOutput() {
    ndspChnWaveBufClear(0);
    for (auto& wb : waveBufs_) wb.status = NDSP_WBUF_FREE;
    nextBuf_ = 0;
    audioStarted_ = false;
    ndspChnSetPaused(0, paused_);
}

void Player::submitAudio(const int16_t* samples, int frames, double pts) {
    int gen = flushGen_;
    ndspWaveBuf& wb = waveBufs_[nextBuf_];
    while (wb.status == NDSP_WBUF_QUEUED || wb.status == NDSP_WBUF_PLAYING) {
        if (stop_ || gen != flushGen_) return;
        svcSleepThread(2 * 1000 * 1000);
    }
    std::memcpy(wb.data_pcm16, samples, static_cast<size_t>(frames) * 2 * sizeof(int16_t));
    wb.nsamples = static_cast<u32>(frames);
    DSP_FlushDataCache(wb.data_pcm16, static_cast<u32>(frames) * 2 * sizeof(int16_t));
    bufPts_[nextBuf_] = pts;
    ndspChnWaveBufAdd(0, &wb);
    nextBuf_ = (nextBuf_ + 1) % AUDIO_BUFS;
    audioStarted_ = true;
}

void Player::audioLoop() {
    AVFrame* frame = av_frame_alloc();
    std::vector<int16_t> pending;
    double pendingPts = 0;
    bool eofSeen = false;

    auto drain = [&](bool all) {
        const size_t chunk = AUDIO_FRAMES * 2;
        size_t off = 0;
        while (pending.size() - off >= chunk || (all && pending.size() > off)) {
            size_t n = std::min(chunk, pending.size() - off);
            submitAudio(pending.data() + off, static_cast<int>(n / 2), pendingPts);
            pendingPts += static_cast<double>(n / 2) / sampleRate_;
            off += n;
        }
        pending.erase(pending.begin(), pending.begin() + off);
    };

    while (!stop_) {
        PacketQueue::Entry e{};
        if (!audioQ_.pop(e, 20)) {
            if (eofSeen && !audioDone_ && audioIdle()) audioDone_ = true;
            continue;
        }
        if (e.kind == PacketQueue::Kind::Flush) {
            avcodec_flush_buffers(audioCtx_);
            pending.clear();
            resetAudioOutput();
            eofSeen = false;
            continue;
        }
        if (e.kind == PacketQueue::Kind::Eof) {
            drain(true);
            eofSeen = true;
            continue;
        }
        if (avcodec_send_packet(audioCtx_, e.pkt) >= 0) {
            while (avcodec_receive_frame(audioCtx_, frame) >= 0) {
                double pts = toSeconds(frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp
                                                                                       : e.pkt->pts,
                                       audioTb_);
                if (pts < skipUntil_) {
                    av_frame_unref(frame);
                    continue;
                }
                if (pending.empty()) pendingPts = pts;
                appendStereo(frame, pending);
                av_frame_unref(frame);
            }
        }
        av_packet_free(&e.pkt);
        drain(false);
    }
    av_frame_free(&frame);
}

}  // namespace app

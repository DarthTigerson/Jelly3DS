#include "decoders.hpp"

#include <cstring>
#include <vector>

namespace app {

namespace {

int align16(int v) { return (v + 15) & ~15; }

// ---------------------------------------------------------------------------------------------
// MVD (New 3DS)
// ---------------------------------------------------------------------------------------------

class MvdDecoder : public VideoDecoder {
public:
    ~MvdDecoder() override {
        if (initialised_) mvdstdExit();
        if (nalBuf_) linearFree(nalBuf_);
    }

    bool open(const AVCodecParameters* par, std::string& error) override {
        if (par->codec_id != AV_CODEC_ID_H264) {
            error = "MVD only decodes H.264";
            return false;
        }
        width_ = align16(par->width);
        height_ = align16(par->height);
        nalBuf_ = static_cast<u8*>(linearMemAlign(NAL_BUF_SIZE, 0x40));
        if (!nalBuf_) {
            error = "Out of linear memory";
            return false;
        }
        Result r = mvdstdInit(MVDMODE_VIDEOPROCESSING, MVD_INPUT_H264, MVD_OUTPUT_BGR565, MVD_DEFAULT_WORKBUF_SIZE,
                              nullptr);
        if (R_FAILED(r)) {
            error = "mvdstdInit failed";
            return false;
        }
        initialised_ = true;
        mvdstdGenerateDefaultConfig(&config_, width_, height_, width_, height_, nullptr, nullptr, nullptr);

        // MP4-style extradata (avcC) carries SPS/PPS out of band; TS streams have them in-band.
        if (par->extradata && par->extradata_size > 7 && par->extradata[0] == 1) feedAvcC(par->extradata, par->extradata_size);
        return true;
    }

    int decode(const AVPacket* pkt, uint16_t* out, int64_t* pts, bool discard) override {
        bool frame = false;
        const u8* d = pkt->data;
        size_t n = static_cast<size_t>(pkt->size);
        size_t i = 0;
        // Split the Annex-B packet on start codes (00 00 01 or 00 00 00 01).
        size_t start = SIZE_MAX;
        while (i + 3 <= n) {
            if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
                if (start != SIZE_MAX) frame |= feed(d + start, trimZeros(d, start, i));
                start = i + 3;
                i += 3;
            } else {
                ++i;
            }
        }
        if (start != SIZE_MAX && start < n) frame |= feed(d + start, n - start);
        if (start == SIZE_MAX && n > 4) {
            // Length-prefixed (MP4) packet.
            size_t p = 0;
            while (p + 4 <= n) {
                size_t len = (d[p] << 24) | (d[p + 1] << 16) | (d[p + 2] << 8) | d[p + 3];
                p += 4;
                if (len == 0 || p + len > n) break;
                frame |= feed(d + p, len);
                p += len;
            }
        }
        if (!frame) return 0;
        *pts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
        if (discard) return 0;
        config_.physaddr_outdata0 = osConvertVirtToPhys(out);
        Result r = mvdstdRenderVideoFrame(&config_, true);
        if (r != MVD_STATUS_OK) return -1;
        return 1;
    }

    void flush() override {}
    int outWidth() const override { return width_; }
    int outHeight() const override { return height_; }
    const char* name() const override { return "MVD (hardware)"; }

private:
    static constexpr size_t NAL_BUF_SIZE = 0x100000;

    static size_t trimZeros(const u8* d, size_t start, size_t end) {
        // A 4-byte start code leaves a trailing 0 on the previous NAL.
        while (end > start && d[end - 1] == 0) --end;
        return end - start;
    }

    void feedAvcC(const u8* x, int size) {
        int p = 5;
        int nSps = x[p++] & 0x1F;
        for (int k = 0; k < nSps && p + 2 <= size; ++k) {
            int len = (x[p] << 8) | x[p + 1];
            p += 2;
            if (p + len > size) return;
            feed(x + p, len);
            p += len;
        }
        if (p >= size) return;
        int nPps = x[p++];
        for (int k = 0; k < nPps && p + 2 <= size; ++k) {
            int len = (x[p] << 8) | x[p + 1];
            p += 2;
            if (p + len > size) return;
            feed(x + p, len);
            p += len;
        }
    }

    /// Feeds one NAL unit (without start code). Returns true when it completed a picture.
    bool feed(const u8* nal, size_t len) {
        if (len == 0 || len + 3 > NAL_BUF_SIZE) return false;
        nalBuf_[0] = 0;
        nalBuf_[1] = 0;
        nalBuf_[2] = 1;
        std::memcpy(nalBuf_ + 3, nal, len);
        GSPGPU_FlushDataCache(nalBuf_, len + 3);
        Result r = mvdstdProcessVideoFrame(nalBuf_, len + 3, 0, nullptr);
        if (!MVD_CHECKNALUPROC_SUCCESS(r)) return false;
        int type = nal[0] & 0x1F;
        bool slice = type == 1 || type == 5;
        return slice && r != MVD_STATUS_PARAMSET && r != MVD_STATUS_INCOMPLETEPROCESSING;
    }

    MVDSTD_Config config_{};
    u8* nalBuf_ = nullptr;
    bool initialised_ = false;
    int width_ = 0, height_ = 0;
};

// ---------------------------------------------------------------------------------------------
// FFmpeg + Y2R (all models)
// ---------------------------------------------------------------------------------------------

class SoftwareDecoder : public VideoDecoder {
public:
    explicit SoftwareDecoder(bool fast) : fast_(fast) {}
    ~SoftwareDecoder() override {
        if (frame_) av_frame_free(&frame_);
        if (ctx_) avcodec_free_context(&ctx_);
        if (y2r_) y2rExit();
    }

    bool open(const AVCodecParameters* par, std::string& error) override {
        const AVCodec* codec = avcodec_find_decoder(par->codec_id);
        if (!codec) {
            error = "No decoder for this video codec";
            return false;
        }
        ctx_ = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(ctx_, par);
        ctx_->thread_count = 1;
        if (fast_) {
            // The Old 3DS has ~268 MHz to spare: skip the deblocking filter, allow non-spec speedups.
            ctx_->skip_loop_filter = AVDISCARD_ALL;
            ctx_->flags2 |= AV_CODEC_FLAG2_FAST;
        }
        if (avcodec_open2(ctx_, codec, nullptr) < 0) {
            error = "Could not open the video decoder";
            return false;
        }
        frame_ = av_frame_alloc();
        width_ = (par->width + 7) & ~7;
        height_ = (par->height + 7) & ~7;
        if (R_FAILED(y2rInit())) {
            error = "Y2R init failed";
            return false;
        }
        y2r_ = true;
        Y2RU_SetTransferEndInterrupt(true);
        Y2RU_GetTransferEndEvent(&doneEvent_);
        return true;
    }

    int decode(const AVPacket* pkt, uint16_t* out, int64_t* pts, bool discard) override {
        if (avcodec_send_packet(ctx_, pkt) < 0) return -1;
        int ret = avcodec_receive_frame(ctx_, frame_);
        if (ret == AVERROR(EAGAIN)) return 0;
        if (ret < 0) return -1;
        *pts = frame_->best_effort_timestamp;
        int result = 0;
        if (!discard && frame_->format == AV_PIX_FMT_YUV420P) result = convert(out) ? 1 : -1;
        av_frame_unref(frame_);
        return result;
    }

    void flush() override {
        if (ctx_) avcodec_flush_buffers(ctx_);
    }
    int outWidth() const override { return width_; }
    int outHeight() const override { return height_; }
    const char* name() const override { return "FFmpeg (software)"; }

private:
    bool convert(uint16_t* out) {
        Y2RU_ConversionParams params{};
        params.input_format = INPUT_YUV420_INDIV_8;
        params.output_format = OUTPUT_RGB_16_565;
        params.rotation = ROTATION_NONE;
        params.block_alignment = BLOCK_LINE;
        params.input_line_width = static_cast<s16>(width_);
        params.input_lines = static_cast<s16>(height_);
        params.standard_coefficient = COEFFICIENT_ITU_R_BT_601_SCALING;
        params.alpha = 0xFF;
        if (R_FAILED(Y2RU_SetConversionParams(&params))) return false;

        const int w = width_, h = height_;
        // Y2R reads the planes by DMA: push them out of the CPU cache first.
        svcFlushProcessDataCache(CUR_PROCESS_HANDLE, reinterpret_cast<u32>(frame_->data[0]), frame_->linesize[0] * h);
        svcFlushProcessDataCache(CUR_PROCESS_HANDLE, reinterpret_cast<u32>(frame_->data[1]), frame_->linesize[1] * h / 2);
        svcFlushProcessDataCache(CUR_PROCESS_HANDLE, reinterpret_cast<u32>(frame_->data[2]), frame_->linesize[2] * h / 2);
        Y2RU_SetSendingY(frame_->data[0], w * h, w, frame_->linesize[0] - w);
        Y2RU_SetSendingU(frame_->data[1], w * h / 4, w / 2, frame_->linesize[1] - w / 2);
        Y2RU_SetSendingV(frame_->data[2], w * h / 4, w / 2, frame_->linesize[2] - w / 2);
        Y2RU_SetReceiving(out, w * h * 2, w * 8 * 2, 0);
        Y2RU_StartConversion();
        Result r = svcWaitSynchronization(doneEvent_, 50 * 1000 * 1000);  // 50 ms
        if (R_FAILED(r) || r == static_cast<Result>(0x09401BFE)) {  // timeout
            Y2RU_StopConversion();
            return false;
        }
        return true;
    }

    bool fast_;
    AVCodecContext* ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    bool y2r_ = false;
    Handle doneEvent_ = 0;
    int width_ = 0, height_ = 0;
};

}  // namespace

std::unique_ptr<VideoDecoder> makeMvdDecoder() { return std::make_unique<MvdDecoder>(); }
std::unique_ptr<VideoDecoder> makeSoftwareDecoder(bool fast) { return std::make_unique<SoftwareDecoder>(fast); }

}  // namespace app

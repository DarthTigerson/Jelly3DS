#pragma once

#include <3ds.h>

#include <cstdint>
#include <memory>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace app {

/// H.264 → RGB565 raster (outWidth()×outHeight(), tightly packed) in linear memory.
class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;
    virtual bool open(const AVCodecParameters* par, std::string& error) = 0;
    /// Returns 1 when a frame was written to `out` (and *pts set, in stream time-base units), 0 when no frame is
    /// ready yet, -1 on a decode error. With `discard`, the frame is decoded for reference but not converted.
    virtual int decode(const AVPacket* pkt, uint16_t* out, int64_t* pts, bool discard) = 0;
    virtual void flush() = 0;
    virtual int outWidth() const = 0;
    virtual int outHeight() const = 0;
    virtual const char* name() const = 0;
};

/// New 3DS hardware decoder (MVDSTD). Feeds Annex-B NAL units, renders BGR565 straight into `out`.
std::unique_ptr<VideoDecoder> makeMvdDecoder();
/// FFmpeg software decoder + Y2R hardware colour conversion (works on every model).
std::unique_ptr<VideoDecoder> makeSoftwareDecoder(bool fast);

}  // namespace app

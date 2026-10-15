#include "gfx.hpp"

#include <cstring>
#include <vector>

namespace app {

namespace {

int nextPow2(int v) {
    int p = 8;
    while (p < v) p <<= 1;
    return p;
}

// Offset of pixel (x, y) inside 8×8 Morton-ordered GPU tiles.
inline u32 tileOffset(u32 x, u32 y, u32 texWidth) {
    return (((y >> 3) * (texWidth >> 3) + (x >> 3)) << 6) +
           ((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3));
}

}  // namespace

bool Texture::create(int w, int h) {
    destroy();
    int tw = nextPow2(w), th = nextPow2(h);
    if (tw > 1024 || th > 1024) return false;
    if (!C3D_TexInit(&tex, tw, th, GPU_RGB565)) return false;
    C3D_TexSetFilter(&tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    std::memset(tex.data, 0, tex.size);
    width = w;
    height = h;
    sub = {static_cast<u16>(w), static_cast<u16>(h), 0.0f, 1.0f, static_cast<float>(w) / tw,
           1.0f - static_cast<float>(h) / th};
    image = {&tex, &sub};
    valid = true;
    return true;
}

void Texture::uploadRgb565(const uint16_t* pixels, int w, int h) {
    if (!valid) return;
    auto* dst = static_cast<uint16_t*>(tex.data);
    // Texture memory row 0 is the top of the image (sampled at v = 1.0, matching sub.top = 1.0).
    for (int y = 0; y < h && y < tex.height; ++y)
        for (int x = 0; x < w && x < tex.width; ++x) dst[tileOffset(x, y, tex.width)] = pixels[y * w + x];
    C3D_TexFlush(&tex);
}

void Texture::destroy() {
    if (valid) C3D_TexDelete(&tex);
    valid = false;
}

bool Gfx::init() {
    gfxInitDefault();
    gfxSet3D(false);
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) return false;
    if (!C2D_Init(C2D_DEFAULT_MAX_OBJECTS * 2)) return false;
    C2D_Prepare();
    top_ = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    bottom_ = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    buf_ = C2D_TextBufNew(8192);
    measureBuf_ = C2D_TextBufNew(1024);
    return top_ && bottom_ && buf_ && measureBuf_;
}

void Gfx::exit() {
    if (buf_) C2D_TextBufDelete(buf_);
    if (measureBuf_) C2D_TextBufDelete(measureBuf_);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
}

void Gfx::beginFrame() {
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TextBufClear(buf_);
}

void Gfx::beginTop() {
    C2D_TargetClear(top_, color::bg);
    C2D_SceneBegin(top_);
}

void Gfx::beginBottom() {
    C2D_TargetClear(bottom_, color::bg);
    C2D_SceneBegin(bottom_);
}

void Gfx::endFrame() {
    C3D_FrameEnd(0);
    if (fitCache_.size() > 512) fitCache_.clear();
}

void Gfx::rect(float x, float y, float w, float h, u32 c) { C2D_DrawRectSolid(x, y, 0.5f, w, h, c); }

void Gfx::roundRect(float x, float y, float w, float h, u32 c) {
    C2D_DrawRectSolid(x + 2, y, 0.5f, w - 4, h, c);
    C2D_DrawRectSolid(x, y + 2, 0.5f, 2, h - 4, c);
    C2D_DrawRectSolid(x + w - 2, y + 2, 0.5f, 2, h - 4, c);
}

float Gfx::textWidth(const std::string& s, float size) {
    if (s.empty()) return 0;
    // Separate scratch buffer so measuring never eats into this frame's glyph budget.
    C2D_TextBufClear(measureBuf_);
    C2D_Text t;
    C2D_TextParse(&t, measureBuf_, s.c_str());
    float w = 0, h = 0;
    C2D_TextGetDimensions(&t, size, size, &w, &h);
    return w;
}

std::string Gfx::fit(const std::string& s, float size, float maxWidth) {
    std::string key = s + '\x01' + std::to_string(static_cast<int>(size * 100)) + '\x01' +
                      std::to_string(static_cast<int>(maxWidth));
    auto it = fitCache_.find(key);
    if (it != fitCache_.end()) return it->second;

    std::string out = s;
    if (textWidth(s, size) > maxWidth) {
        // Largest number of whole UTF-8 characters k such that prefix(k) + "…" fits.
        std::vector<size_t> starts;
        for (size_t i = 0; i < s.size(); ++i)
            if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) starts.push_back(i);
        auto prefix = [&](size_t k) { return s.substr(0, k < starts.size() ? starts[k] : s.size()) + "\xE2\x80\xA6"; };
        size_t lo = 0, hi = starts.size() - 1;
        while (lo < hi) {
            size_t mid = (lo + hi + 1) / 2;
            if (textWidth(prefix(mid), size) <= maxWidth)
                lo = mid;
            else
                hi = mid - 1;
        }
        out = prefix(lo);
    }
    fitCache_[key] = out;
    return out;
}

float Gfx::text(const std::string& s, float x, float y, float size, u32 c, Align align, float maxWidth) {
    if (s.empty()) return 0;
    std::string str = maxWidth > 0 ? fit(s, size, maxWidth) : s;
    C2D_Text t;
    C2D_TextParse(&t, buf_, str.c_str());
    C2D_TextOptimize(&t);
    float w = 0, h = 0;
    C2D_TextGetDimensions(&t, size, size, &w, &h);
    if (align == Align::Center) x -= w / 2;
    if (align == Align::Right) x -= w;
    C2D_DrawText(&t, C2D_WithColor, x, y, 0.5f, size, size, c);
    return w;
}

void Gfx::wrapped(const std::string& s, float x, float y, float size, u32 c, float width, int maxLines) {
    if (s.empty()) return;
    // Greedy word wrap so we can cap the number of lines (citro2d's wrap has no line limit).
    float lineH = 30.0f * size;
    std::string line, word;
    int lines = 0;
    auto flush = [&](bool last) {
        if (lines >= maxLines) return;
        text(line, x, y + lines * lineH, size, c, Align::Left, last || lines == maxLines - 1 ? width : 0);
        ++lines;
        line.clear();
    };
    for (size_t i = 0; i <= s.size(); ++i) {
        char ch = i < s.size() ? s[i] : ' ';
        if (ch == '\n' || ch == ' ' || ch == '\r') {
            if (!word.empty()) {
                std::string cand = line.empty() ? word : line + " " + word;
                if (textWidth(cand, size) > width && !line.empty()) {
                    if (lines == maxLines - 1) {
                        line = cand;  // truncated with an ellipsis by flush
                        flush(true);
                        return;
                    }
                    flush(false);
                    line = word;
                } else {
                    line = cand;
                }
                word.clear();
            }
            if (ch == '\n' && !line.empty()) flush(false);
        } else {
            word.push_back(ch);
        }
    }
    if (!line.empty()) flush(true);
}

void Gfx::image(const Texture& t, float x, float y, float w, float h) {
    if (!t.valid || t.width == 0 || t.height == 0) return;
    C2D_DrawImageAt(t.image, x, y, 0.5f, nullptr, w / t.width, h / t.height);
}

void Gfx::imageCover(const Texture& t, float x, float y, float w, float h) {
    if (!t.valid || t.width == 0 || t.height == 0) return;
    // Crop the source rectangle to the box's aspect ratio, centred.
    float srcW = t.width, srcH = t.height;
    if (srcW / srcH > w / h)
        srcW = srcH * w / h;
    else
        srcH = srcW * h / w;
    float ox = (t.width - srcW) / 2, oy = (t.height - srcH) / 2;
    const float tw = t.tex.width, th = t.tex.height;
    Tex3DS_SubTexture sub = {static_cast<u16>(srcW), static_cast<u16>(srcH), ox / tw, 1.0f - oy / th,
                             (ox + srcW) / tw, 1.0f - (oy + srcH) / th};
    C2D_Image img = {const_cast<C3D_Tex*>(&t.tex), &sub};
    C2D_DrawImageAt(img, x, y, 0.5f, nullptr, w / srcW, h / srcH);
}

void Gfx::gradient(float x, float y, float w, float h, u32 tl, u32 tr, u32 bl, u32 br) {
    C2D_DrawRectangle(x, y, 0.5f, w, h, tl, tr, bl, br);
}

void Gfx::outline(float x, float y, float w, float h, u32 c, float t) {
    rect(x, y, w, t, c);
    rect(x, y + h - t, w, t, c);
    rect(x, y, t, h, c);
    rect(x + w - t, y, t, h, c);
}

void Gfx::circle(float cx, float cy, float r, u32 c) { C2D_DrawCircleSolid(cx, cy, 0.5f, r, c); }

void Gfx::triangle(float x0, float y0, float x1, float y1, float x2, float y2, u32 c) {
    C2D_DrawTriangle(x0, y0, c, x1, y1, c, x2, y2, c, 0.5f);
}

void Gfx::speaker(float x, float y, float s, u32 c) {
    // Box + cone, then two "sound waves" as bars of growing height.
    rect(x, y + s * 0.35f, s * 0.22f, s * 0.3f, c);
    triangle(x + s * 0.2f, y + s * 0.35f, x + s * 0.5f, y + s * 0.08f, x + s * 0.5f, y + s * 0.5f, c);
    triangle(x + s * 0.2f, y + s * 0.65f, x + s * 0.5f, y + s * 0.5f, x + s * 0.5f, y + s * 0.92f, c);
    rect(x + s * 0.62f, y + s * 0.38f, s * 0.09f, s * 0.24f, c);
    rect(x + s * 0.8f, y + s * 0.25f, s * 0.09f, s * 0.5f, c);
}

void Gfx::progressBar(float x, float y, float w, float h, float fraction, u32 fill, u32 back) {
    if (fraction < 0) fraction = 0;
    if (fraction > 1) fraction = 1;
    rect(x, y, w, h, back);
    if (fraction > 0) rect(x, y, w * fraction, h, fill);
}

}  // namespace app

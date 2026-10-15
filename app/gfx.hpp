#pragma once

#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>

#include <cstdint>
#include <string>
#include <unordered_map>

namespace app {

constexpr int TOP_W = 400;
constexpr int BOTTOM_W = 320;
constexpr int SCREEN_H = 240;

namespace color {
constexpr u32 rgba(u8 r, u8 g, u8 b, u8 a = 255) { return r | (g << 8) | (b << 16) | (static_cast<u32>(a) << 24); }
constexpr u32 bg = rgba(16, 18, 26);
constexpr u32 panel = rgba(28, 31, 44);
constexpr u32 panelHi = rgba(44, 49, 70);
constexpr u32 accent = rgba(170, 92, 255);   // Jellyfin purple
constexpr u32 accent2 = rgba(0, 164, 220);   // Jellyfin blue
constexpr u32 text = rgba(236, 238, 245);
constexpr u32 dim = rgba(150, 156, 175);
constexpr u32 good = rgba(80, 200, 120);
constexpr u32 warn = rgba(240, 180, 60);
constexpr u32 bad = rgba(235, 90, 90);
constexpr u32 black = rgba(0, 0, 0);
constexpr u32 shade = rgba(0, 0, 0, 170);
}  // namespace color

enum class Align { Left, Center, Right };

/// A GPU texture holding an RGB565 image of up to 1024×1024 pixels.
struct Texture {
    C3D_Tex tex{};
    Tex3DS_SubTexture sub{};
    C2D_Image image{};
    int width = 0;
    int height = 0;
    bool valid = false;

    /// Allocates a power-of-two texture big enough for w×h.
    bool create(int w, int h);
    /// Uploads tightly packed RGB565 pixels (w×h ≤ the created size) by swizzling into GPU tile order.
    void uploadRgb565(const uint16_t* pixels, int w, int h);
    void destroy();
    ~Texture() { destroy(); }
};

class Gfx {
public:
    bool init();
    void exit();

    void beginFrame();
    void beginTop();
    void beginBottom();
    void endFrame();

    void rect(float x, float y, float w, float h, u32 c);
    void roundRect(float x, float y, float w, float h, u32 c);  // slightly bevelled corners
    /// Draws one line of text, truncated with "…" to fit maxWidth when > 0. Returns the drawn width.
    float text(const std::string& s, float x, float y, float size, u32 c, Align align = Align::Left,
               float maxWidth = 0);
    /// Word-wrapped text clipped to maxLines.
    void wrapped(const std::string& s, float x, float y, float size, u32 c, float width, int maxLines);
    float textWidth(const std::string& s, float size);
    void image(const Texture& t, float x, float y, float w, float h);
    /// Fills the box with the image, cropping the overflow (CSS object-fit: cover).
    void imageCover(const Texture& t, float x, float y, float w, float h);
    /// Four corner colours: top-left, top-right, bottom-left, bottom-right.
    void gradient(float x, float y, float w, float h, u32 tl, u32 tr, u32 bl, u32 br);
    void outline(float x, float y, float w, float h, u32 c, float thickness = 1);
    void circle(float cx, float cy, float r, u32 c);
    void triangle(float x0, float y0, float x1, float y1, float x2, float y2, u32 c);
    /// Small speaker glyph (volume), drawn with primitives so it never depends on the font.
    void speaker(float x, float y, float size, u32 c);
    void progressBar(float x, float y, float w, float h, float fraction, u32 fill, u32 back = color::panelHi);

private:
    std::string fit(const std::string& s, float size, float maxWidth);

    C3D_RenderTarget* top_ = nullptr;
    C3D_RenderTarget* bottom_ = nullptr;
    C2D_TextBuf buf_ = nullptr;
    C2D_TextBuf measureBuf_ = nullptr;
    std::unordered_map<std::string, std::string> fitCache_;
};

}  // namespace app

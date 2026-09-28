#include "Global.h"
#include "Duels.h"
#include "DuelsCapture.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace Duels
{
    static std::string g_capturePath;
    static bool g_rendering = false;          // inside CApp::OnRender
    static bool g_frameBufferBound = false;   // FTL bound its frame buffer in this frame

    void RequestCapture(const std::string &path)
    {
        g_capturePath = path;
    }

    // graphics_read_pixels returns RGBA rows from the bottom up, the order BMP files store them in.
    static bool WriteBmp(const std::string &path, int width, int height, const std::vector<uint8_t> &rgba)
    {
        size_t stride = ((size_t)width * 3 + 3) & ~(size_t)3;
        std::vector<uint8_t> file(54 + stride * height, 0);
        auto put16 = [&](size_t at, uint32_t value) {
            for (int i = 0; i < 2; ++i) file[at + i] = (uint8_t)(value >> (8 * i));
        };
        auto put32 = [&](size_t at, uint32_t value) {
            for (int i = 0; i < 4; ++i) file[at + i] = (uint8_t)(value >> (8 * i));
        };
        file[0] = 'B';
        file[1] = 'M';
        put32(2, (uint32_t)file.size());
        put32(10, 54);   // pixel data offset
        put32(14, 40);   // BITMAPINFOHEADER
        put32(18, (uint32_t)width);
        put32(22, (uint32_t)height);   // positive: bottom-up
        put16(26, 1);
        put16(28, 24);
        put32(34, (uint32_t)(stride * height));
        for (int y = 0; y < height; ++y)
        {
            const uint8_t *from = &rgba[(size_t)y * width * 4];
            uint8_t *to = &file[54 + (size_t)y * stride];
            for (int x = 0; x < width; ++x, from += 4, to += 3)
            {
                to[0] = from[2];
                to[1] = from[1];
                to[2] = from[0];
            }
        }
        FILE *out = fopen(path.c_str(), "wb");
        if (!out) return false;
        bool written = fwrite(file.data(), file.size(), 1, out) == 1;
        fclose(out);
        return written;
    }

    // Reads the bound frame buffer (or the display) now.
    static void Capture(int width, int height, const char *source)
    {
        std::string path = g_capturePath;
        g_capturePath.clear();
        std::vector<uint8_t> rgba((size_t)width * height * 4);
        bool read = graphics_read_pixels(0, 0, width, height, rgba.data()) != 0;
        bool saved = read && WriteBmp(path, width, height, rgba);
        Log("screenshot %s: %dx%d from %s, %s", path.c_str(), width, height, source,
            saved ? "saved" : read ? "could not write the file" : "could not read the pixels");
    }

    static void BeginRender()
    {
        g_rendering = true;
        g_frameBufferBound = false;
    }

    static void EndRender(CApp *app)
    {
        g_rendering = false;
        // FTL draws straight into the window when it doesn't scale: the frame is there, not shown yet.
        if (!g_capturePath.empty() && !g_frameBufferBound) Capture(app->screen_x, app->screen_y, "the window");
    }

    static void OnBindFrameBuffer(GL_FrameBuffer *frameBuffer)
    {
        CApp *app = G_->GetCApp();
        if (!g_rendering || !app || !app->framebuffer) return;
        if (frameBuffer == app->framebuffer)
        {
            g_frameBufferBound = true;
        }
        else if (!frameBuffer && g_frameBufferBound && !g_capturePath.empty())
        {
            // FTL is done drawing the frame and switches to the window to show it; its frame buffer is still bound.
            Capture(1280, 720, "FTL's frame buffer");
        }
    }
}

HOOK_METHOD_PRIORITY(CApp, OnRender, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CApp::OnRender -> Begin (DuelsCapture.cpp)\n")
    Duels::BeginRender();
    super();
    Duels::EndRender(this);
}

HOOK_STATIC_PRIORITY(CSurface, GL_BindFrameBuffer, -2000, (GL_FrameBuffer *fb) -> void)
{
    LOG_HOOK("HOOK_STATIC_PRIORITY -> CSurface::GL_BindFrameBuffer -> Begin (DuelsCapture.cpp)\n")
    Duels::OnBindFrameBuffer(fb);
    super(fb);
}

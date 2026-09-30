#include "Global.h"
#include "Duels.h"
#include "DuelsScreen.h"
#include "DuelsView.h"
#include "DuelsWin32.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace Duels
{
    namespace Screen
    {
        static const char *const WINDOW_TITLE = "FTL: Duels";

        static bool g_hiRes = true;
        static GL_FrameBuffer *g_buffer = nullptr;   // ours, of the window's size
        static int g_bufferW = 0, g_bufferH = 0;
        static GL_FrameBuffer *g_ftlBuffer = nullptr;  // FTL's own, while ours stands in for it

        static bool g_rendering = false;   // inside CApp::OnRender
        static bool g_hiResFrame = false;  // this frame is drawn into our frame buffer
        static bool g_pass = false;        // FTL's (or our) frame buffer is bound: the frame is being drawn
        static bool g_bound = false;       // it was bound in this frame
        static int g_frameW = 1280, g_frameH = 720;

        static std::string g_capturePath;

        void SetHiRes(bool on) { g_hiRes = on; }
        bool HiRes() { return g_hiRes; }

        std::string Describe()
        {
            char buffer[160];
            if (g_bufferW > 0)
            {
                snprintf(buffer, sizeof(buffer), "hires %s: frames %s at %d x %d (FTL's 1280 x 720 scaled %.2f)",
                         g_hiRes ? "on" : "off", g_hiResFrame || g_ftlBuffer ? "drawn" : "last drawn", g_bufferW, g_bufferH,
                         g_bufferW / 1280.f);
            }
            else
            {
                snprintf(buffer, sizeof(buffer), "hires %s: frames drawn at FTL's 1280 x 720 (the window is %s)",
                         g_hiRes ? "on" : "off", g_hiRes ? "not larger, or a whole multiple" : "not checked");
            }
            return buffer;
        }

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

        // Reads the bound frame buffer (or the window) now.
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

        static void BeginRender(CApp *app)
        {
            g_rendering = true;
            g_pass = false;
            g_bound = false;
            g_hiResFrame = false;
            g_frameW = 1280;
            g_frameH = 720;
            if (!g_hiRes || !app->useFrameBuffer || !app->framebuffer) return;

            // The window's drawing area, without FTL's black bars (it keeps 16:9).
            int w = app->screen_x - 2 * app->x_bar, h = app->screen_y - 2 * app->y_bar;
            if (w <= 1280 || h <= 720 || (w % 1280 == 0 && h % 720 == 0)) return;
            if (!g_buffer || w != g_bufferW || h != g_bufferH)
            {
                // (A buffer of an older size is left alive; windows are resized rarely.)
                GL_FrameBuffer *buffer = CSurface::GL_CreateFrameBuffer(w, h);
                if (!buffer) return;
                g_buffer = buffer;
                g_bufferW = w;
                g_bufferH = h;
                Log("Screen: drawing frames at %d x %d", w, h);
            }
            g_ftlBuffer = app->framebuffer;
            app->framebuffer = g_buffer;
            g_hiResFrame = true;
            g_frameW = w;
            g_frameH = h;
        }

        static void EndRender(CApp *app)
        {
            g_rendering = false;
            g_pass = false;
            // FTL draws straight into the window when it doesn't scale: the frame is there, not shown yet.
            if (!g_capturePath.empty() && !g_bound) Capture(app->screen_x, app->screen_y, "the window");
            if (g_hiResFrame)
            {
                app->framebuffer = g_ftlBuffer;
                g_ftlBuffer = nullptr;
                g_hiResFrame = false;
            }
            View::SetFrameSmoothing(false);
        }

        static void OnBindFrameBuffer(GL_FrameBuffer *frameBuffer)
        {
            CApp *app = G_->GetCApp();
            if (!g_rendering || !app || !app->framebuffer) return;
            if (frameBuffer == app->framebuffer)
            {
                g_pass = true;
                g_bound = true;
            }
            else if (!frameBuffer && g_pass)
            {
                // FTL is done drawing the frame and switches to the window to show it; its frame buffer is still bound.
                if (!g_capturePath.empty()) Capture(g_frameW, g_frameH, g_hiResFrame ? "the window-size frame buffer" : "FTL's frame buffer");
                g_pass = false;
                View::SetFrameSmoothing(false);
            }
        }

        // FTL sets its frame buffer's viewport to 1280 x 720: ours has the window's size, in the same coordinates.
        static void AdjustViewPort(int left, int bottom, int &w, int &h)
        {
            if (!g_hiResFrame || !g_pass || left != 0 || bottom != 0 || w != 1280 || h != 720) return;
            w = g_frameW;
            h = g_frameH;
            View::SetFrameSmoothing(true);
        }

        // Clip regions are in the frame buffer's pixels.
        static void AdjustScissor(int &x, int &y, int &w, int &h)
        {
            if (!g_hiResFrame || !g_pass) return;
            float sx = g_frameW / 1280.f, sy = g_frameH / 720.f;
            x = (int)(x * sx + 0.5f);
            y = (int)(y * sy + 0.5f);
            w = (int)(w * sx + 0.5f);
            h = (int)(h * sy + 0.5f);
        }

        static int g_versionFont = -1;

        bool VersionLabel(int fontSize, float x, float y, const std::string &text, std::string &label)
        {
            if (x != 1280.f || y != 0.f || text.compare(0, 3, "HS-") != 0) return false;
            g_versionFont = fontSize;
            label = std::string("FTL:Duels ") + VERSION + " (HS " + text.substr(3) + ")";
            return true;
        }

        int VersionFont()
        {
            return g_versionFont;
        }

        void OnFrame()
        {
            // FTL names its window when it creates it, before our hooks are in place.
            static bool titled = false;
            if (!titled)
            {
                std::string details;
                titled = SetGameWindowTitle(WINDOW_TITLE, details);
            }
        }
    }
}

HOOK_METHOD_PRIORITY(CApp, OnRender, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CApp::OnRender -> Begin (DuelsScreen.cpp)\n")
    Duels::Screen::BeginRender(this);
    super();
    Duels::Screen::EndRender(this);
}

HOOK_STATIC_PRIORITY(CSurface, GL_BindFrameBuffer, -2000, (GL_FrameBuffer *fb) -> void)
{
    LOG_HOOK("HOOK_STATIC_PRIORITY -> CSurface::GL_BindFrameBuffer -> Begin (DuelsScreen.cpp)\n")
    Duels::Screen::OnBindFrameBuffer(fb);
    super(fb);
}

HOOK_STATIC_PRIORITY(CSurface, GL_SetViewPort, -2000, (int left, int bottom, int h, int w) -> bool)
{
    LOG_HOOK("HOOK_STATIC_PRIORITY -> CSurface::GL_SetViewPort -> Begin (DuelsScreen.cpp)\n")
    // (The parameter names in the .zhl are swapped: the third one is the width.)
    Duels::Screen::AdjustViewPort(left, bottom, h, w);
    return super(left, bottom, h, w);
}

HOOK_STATIC_PRIORITY(CSurface, GL_SetScissor, -2000, (int x, int y, int w, int h) -> void)
{
    LOG_HOOK("HOOK_STATIC_PRIORITY -> CSurface::GL_SetScissor -> Begin (DuelsScreen.cpp)\n")
    Duels::Screen::AdjustScissor(x, y, w, h);
    super(x, y, w, h);
}

// Later names of the window (SIL renames it when the display mode changes).
HOOK_GLOBAL_PRIORITY(sys_graphics_set_window_title, -2000, (char *title) -> void)
{
    LOG_HOOK("HOOK_GLOBAL_PRIORITY -> sys_graphics_set_window_title -> Begin (DuelsScreen.cpp)\n")
    static char name[] = "FTL: Duels";
    super(name);
}

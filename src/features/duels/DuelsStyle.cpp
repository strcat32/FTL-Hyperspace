#include "Global.h"
#include "DuelsStyle.h"
#include "DuelsTrace.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace Duels
{
    namespace Style
    {
        static GL_Color Rgb(int r, int g, int b, float a = 1.f)
        {
            return GL_Color(r / 255.f, g / 255.f, b / 255.f, a);
        }

        // TextButton's colours in FTL 1.6 (TextButton::InitColors): off, on, under the mouse; the letters.
        GL_Color ButtonBody(Look look)
        {
            if (look == Look::Pressed) return Rgb(196, 204, 190);
            switch (look)
            {
            case Look::Hover: return Rgb(255, 230, 94);
            case Look::Off: return Rgb(164, 171, 160);
            default: return Rgb(235, 245, 229);
            }
        }

        GL_Color ButtonText()
        {
            return Rgb(25, 49, 51);
        }

        GL_Color FrameLine()
        {
            return Rgb(53, 75, 89);   // the dark line in FTL's button frames
        }

        GL_Color Red(float alpha)
        {
            return GL_Color(1.f, 0.35f, 0.28f, alpha);
        }

        GL_Color Blue(float alpha)
        {
            return GL_Color(0.4f, 0.68f, 1.f, alpha);
        }

        GL_Color Mix(const GL_Color &a, const GL_Color &b, float t)
        {
            t = std::max(0.f, std::min(1.f, t));
            return GL_Color(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
        }

        void CutRect(float x, float y, float w, float h, float cut, const GL_Color &colour)
        {
            int c = (int)cut;
            if (c <= 0 || w <= 2.f * c || h <= 2.f * c)
            {
                CSurface::GL_DrawRect(x, y, w, h, colour);
                return;
            }
            CSurface::GL_DrawRect(x + c, y, w - 2.f * c, h, colour);
            for (int i = 0; i < c; ++i)
            {
                float inset = (float)(c - i);   // the column i pixels in from the edge is that much shorter at each end
                CSurface::GL_DrawRect(x + i, y + inset, 1.f, h - 2.f * inset, colour);
                CSurface::GL_DrawRect(x + w - 1.f - i, y + inset, 1.f, h - 2.f * inset, colour);
            }
        }

        void Blend(float x, float y, float w, float h, const GL_Color &left, const GL_Color &right, float cut)
        {
            int c = std::max(0, (int)cut);
            float sx = 0.f;
            while (sx < w)
            {
                // Pixel columns where the corners are cut, broader strips between them.
                bool edge = sx < c || sx >= w - c;
                float strip = edge ? 1.f : std::min(4.f, std::max(1.f, w - c - sx));
                float fromEdge = std::min(sx, w - 1.f - sx);
                float inset = edge ? (float)c - fromEdge : 0.f;
                GL_Color colour = Mix(left, right, (sx + strip / 2.f) / w);
                if (h - 2.f * inset > 0.f) CSurface::GL_DrawRect(x + sx, y + inset, strip, h - 2.f * inset, colour);
                sx += strip;
            }
        }

        float LineHeight(int font)
        {
            return freetype::easy_measurePrintLines(font, 0.f, 0.f, 9999, "A").y;
        }

        void Button(float x, float y, float w, float h, const std::string &label, int font, Look look, const GL_Color *body)
        {
            // Outside in: the glow (three fading rings), the light border (3 px), the dark line (2 px), the body. Held
            // down: no glow, a thicker dark line and a darker body, the letters a pixel lower.
            const bool pressed = look == Look::Pressed;
            const GL_Color glow = Rgb(121, 242, 253);
            for (int i = 3; i >= 1 && !pressed; --i)
            {
                GL_Color ring(glow.r, glow.g, glow.b, 0.07f * (4 - i));
                CutRect(x - i, y - i, w + 2.f * i, h + 2.f * i, 4.f + i, ring);
            }
            CutRect(x, y, w, h, 4.f, ButtonBody(Look::Idle));
            CutRect(x + 3.f, y + 3.f, w - 6.f, h - 6.f, 3.f, FrameLine());
            float inset = pressed ? 6.f : 5.f;
            CutRect(x + inset, y + inset, w - 2.f * inset, h - 2.f * inset, 2.f, body ? *body : ButtonBody(look));
            GL_Color text = ButtonText();
            if (look == Look::Off) text.a = 0.55f;
            CSurface::GL_SetColor(text);
            // FTL's line of a font has room for letters below the line: the capitals of font 12 sat 2 px above the
            // middle (roadmap AI).
            float lower = (font == 12 ? 3.f : 1.f) + (pressed ? 1.f : 0.f);
            freetype::easy_printCenter(font, x + w / 2.f, y + std::floor((h - LineHeight(font)) / 2.f) + lower, label);
            CSurface::GL_SetColor(COLOR_WHITE);
        }

        bool TopFrame(float x, float y, float w, const GL_Color &tint)
        {
            GL_Texture *texture = G_->GetResources()->GetImageId("statusUI/top_store_base.png");
            if (!texture || texture->width_ <= 0) return false;
            // The image is even along its length between its corners (x 12 to 89): 20 px kept at each end, the middle
            // stretched (or squeezed) to the width.
            const float iw = (float)texture->width_, ih = (float)texture->height_, cap = 20.f;
            float middle = std::max(0.f, w - 2.f * cap);
            CSurface::GL_BlitImagePartial(texture, x, y, cap, ih, 0.f, cap / iw, 0.f, 1.f, 1.f, tint, false);
            CSurface::GL_BlitImagePartial(texture, x + cap, y, middle, ih, cap / iw, (iw - cap) / iw, 0.f, 1.f, 1.f, tint, false);
            CSurface::GL_BlitImagePartial(texture, x + cap + middle, y, cap, ih, (iw - cap) / iw, 1.f, 0.f, 1.f, 1.f, tint, false);
            return true;
        }

        void WindowOutline(int x, int y, int w, int h)
        {
            // FTL builds a frame's primitives for its place and size; one for each (the window has one or two).
            static std::map<std::tuple<int, int, int, int>, WindowFrame *> frames;
            WindowFrame *&frame = frames[std::make_tuple(x, y, w, h)];
            if (!frame) frame = new WindowFrame(x, y, w, h);
            frame->DrawOutline();
        }

        void TitleTab(float frameX, float frameY, const std::string &title)
        {
            GL_Texture *texture = G_->GetResources()->GetImageId("box_options_configure_tab.png");
            float textWidth = (float)freetype::easy_measureWidth(63, title);
            if (texture && texture->width_ > 0)
            {
                // As FTL's controls screen: the tab's image 7 px left of the frame and 42 px above it, its left 39 px
                // and right 51 px kept, the pixel column between them stretched under the title.
                const float iw = (float)texture->width_, ih = (float)texture->height_;
                float x = frameX - 7.f, y = frameY - 42.f, middle = std::max(1.f, textWidth - 16.f);
                CSurface::GL_BlitImagePartial(texture, x, y, 39.f, ih, 0.f, 39.f / iw, 0.f, 1.f, 1.f, COLOR_WHITE, false);
                CSurface::GL_BlitImagePartial(texture, x + 39.f, y, middle, ih, 39.f / iw, 40.f / iw, 0.f, 1.f, 1.f, COLOR_WHITE, false);
                CSurface::GL_BlitImagePartial(texture, x + 39.f + middle, y, 51.f, ih, 40.f / iw, 1.f, 0.f, 1.f, 1.f, COLOR_WHITE, false);
            }
            CSurface::GL_SetColor(ButtonText());
            freetype::easy_print(63, frameX + 24.f, frameY - 31.f, title);
            CSurface::GL_SetColor(COLOR_WHITE);
        }

        float Label(float x, float y, const std::string &title)
        {
            const float h = 20.f;
            float w = (float)freetype::easy_measureWidth(62, title) + 16.f;
            GL_Color light = ButtonBody(Look::Idle);
            CSurface::GL_DrawRect(x, y, w, h, light);
            CSurface::GL_DrawTriangle(Point((int)(x + w), (int)y), Point((int)(x + w + h), (int)(y + h)), Point((int)(x + w), (int)(y + h)), light);
            CSurface::GL_SetColor(ButtonText());
            freetype::easy_print(62, x + 8.f, y + std::floor((h - LineHeight(62)) / 2.f) + 1.f, title);
            CSurface::GL_SetColor(COLOR_WHITE);
            return h;
        }

        void Dialog(float x, float y, float w, float h, const std::string &title, bool dim)
        {
            if (dim) CSurface::GL_DrawRect(0.f, 0.f, 1280.f, 720.f, GL_Color(0.f, 0.f, 0.f, 0.55f));
            Blend(x + 2.f, y + 2.f, w - 4.f, h - 4.f, Rgb(40, 13, 17), Rgb(12, 20, 44), 9.f);
            WindowOutline((int)x, (int)y, (int)w, (int)h);
            TitleTab(x, y, title);
        }

        void CheckBox(float x, float y, bool on, bool hover)
        {
            const float s = CHECK_SIZE;
            CutRect(x, y, s, s, 2.f, ButtonBody(hover ? Look::Hover : Look::Idle));
            CutRect(x + 3.f, y + 3.f, s - 6.f, s - 6.f, 1.f, Rgb(8, 10, 14));
            if (on) CutRect(x + 6.f, y + 6.f, s - 12.f, s - 12.f, 0.f, ButtonBody(Look::Idle));
        }

        void Field(float x, float y, float w, float h, bool focus)
        {
            CutRect(x, y, w, h, 3.f, ButtonBody(focus ? Look::Hover : Look::Idle));
            CutRect(x + 2.f, y + 2.f, w - 4.f, h - 4.f, 2.f, Rgb(8, 10, 14));
        }

        void TextField(float x, float y, float w, float h, const std::string &text, int caret, bool focus, bool stars)
        {
            const int font = 12;
            Field(x, y, w, h, focus);
            std::string shown = stars ? std::string(text.size(), '*') : text;
            // Font 12's capitals sit in the middle of its line 2 px lower than the line's middle (as on the buttons).
            float ty = y + std::floor((h - LineHeight(font)) / 2.f) + 3.f;
            CSurface::GL_SetColor(Rgb(255, 255, 255));
            freetype::easy_print(font, x + 10.f, ty, shown);
            if (focus && std::fmod(WallMs(), 1000.0) < 600.0)
            {
                int at = std::max(0, std::min(caret, (int)shown.size()));
                float cx = x + 10.f + (float)freetype::easy_measureWidth(font, shown.substr(0, (size_t)at)) + 1.f;
                CSurface::GL_DrawRect(cx, y + 6.f, 1.f, h - 12.f, Rgb(255, 255, 255));
            }
            CSurface::GL_SetColor(COLOR_WHITE);
        }
    }
}

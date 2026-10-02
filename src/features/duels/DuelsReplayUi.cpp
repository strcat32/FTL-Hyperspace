#include "Global.h"
#include "Duels.h"
#include "DuelsDemo.h"
#include "DuelsReplayUi.h"
#include "DuelsStyle.h"
#include "DuelsTrace.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace Duels
{
    namespace ReplayUi
    {
        // Over the weapons and drone bars (the user's place for them), clear of the feed at the bottom left (it reaches
        // x 335) and of the enemy window (from x 833); above FTL's weapon charge numbers (y 603).
        static const float BAR_X = 345.f, BAR_Y = 572.f, H = 26.f, GAP = 6.f;
        // The buttons show a media player's symbols, not words (roadmap BC); the time line has the room they left.
        static const float W_ICON = 30.f, W_SPEED = 42.f, W_LINE = 212.f;
        static const double STEP_MS = 10000.0;
        // Under the enemy window (its bottom stays where FTL has it, DuelsView.cpp), above the drone box (from y 615) and
        // left of the subsystems (from x 1030): whose side is shown, and full sensors (roadmap BA).
        static const float SIDE_X = 852.f, SIDE_Y = 586.f, SIDE_H = 24.f, W_VIEW = 118.f, W_SENSORS = 134.f;   // (BZ: its label filled it)

        enum class Icon { Stop, Back, Play, Pause, On };

        struct State
        {
            int mouseX = 0, mouseY = 0;
            Style::Box stop, back, play, on, speed, line, view, sensors;
            double lengthMs = 0.0;
            bool shown = false;
        };

        static State g;

        static bool InGame()
        {
            WorldManager *world = G_->GetWorld();
            CApp *app = G_->GetCApp();
            return world && world->commandGui && world->playerShip && app && !app->menu.bOpen;
        }

        // m:ss
        static std::string Clock(double ms)
        {
            int seconds = std::max(0, (int)(ms / 1000.0));
            char text[16];
            snprintf(text, sizeof(text), "%d:%02d", seconds / 60, seconds % 60);
            return text;
        }

        static std::string SpeedText(double speed)
        {
            if (speed < 1.0) return "1/2x";
            char text[8];
            snprintf(text, sizeof(text), "%dx", (int)speed);
            return text;
        }

        static void Button(Style::Box &box, float x, float w, const std::string &label)
        {
            box.x = x;
            box.y = BAR_Y;
            box.w = w;
            box.h = H;
            bool hover = box.Contains(g.mouseX, g.mouseY);
            Style::Button(x, BAR_Y, w, H, label, 12, hover ? Style::Look::Hover : Style::Look::Idle);
        }

        // A button under the enemy window (BA): off (grey) when the demo has one side only, held down while on.
        static void SideButton(Style::Box &box, float x, float w, const std::string &label, bool available, bool on)
        {
            box.x = x;
            box.y = SIDE_Y;
            box.w = w;
            box.h = SIDE_H;
            bool hover = box.Contains(g.mouseX, g.mouseY);
            Style::Look look = !available ? Style::Look::Off : on ? Style::Look::Pressed : hover ? Style::Look::Hover : Style::Look::Idle;
            Style::Button(x, SIDE_Y, w, SIDE_H, label, 12, look);
        }

        // A button with a symbol in FTL's dark button letters: a box (stop), two triangles to the left (back), one
        // to the right (play), two bars (pause), two triangles to the right (on).
        static void IconButton(Style::Box &box, float x, Icon icon)
        {
            Button(box, x, W_ICON, "");
            GL_Color ink = Style::ButtonText();
            int cx = (int)(x + W_ICON / 2.f), cy = (int)(BAR_Y + H / 2.f);
            switch (icon)
            {
            case Icon::Stop:
                CSurface::GL_DrawRect((float)(cx - 4), (float)(cy - 4), 9.f, 9.f, ink);
                break;
            case Icon::Pause:
                CSurface::GL_DrawRect((float)(cx - 4), (float)(cy - 5), 3.f, 11.f, ink);
                CSurface::GL_DrawRect((float)(cx + 2), (float)(cy - 5), 3.f, 11.f, ink);
                break;
            case Icon::Play:
                CSurface::GL_DrawTriangle(Point(cx - 3, cy - 6), Point(cx + 5, cy), Point(cx - 3, cy + 6), ink);
                break;
            case Icon::Back:
                // (Each triangle's corners in the same turn as the others', as DuelsStyle.cpp's.)
                CSurface::GL_DrawTriangle(Point(cx, cy - 5), Point(cx, cy + 5), Point(cx - 7, cy), ink);
                CSurface::GL_DrawTriangle(Point(cx + 7, cy - 5), Point(cx + 7, cy + 5), Point(cx, cy), ink);
                break;
            case Icon::On:
                CSurface::GL_DrawTriangle(Point(cx - 7, cy - 5), Point(cx, cy), Point(cx - 7, cy + 5), ink);
                CSurface::GL_DrawTriangle(Point(cx, cy - 5), Point(cx + 7, cy), Point(cx, cy + 5), ink);
                break;
            }
        }

        // The demo's time line: its whole length, the part played, a mark where it is ("SEEKING" while it runs there).
        static void Line(float x, double fraction, bool seeking)
        {
            Style::Box &box = g.line;
            box.x = x;
            box.y = BAR_Y;
            box.w = W_LINE;
            box.h = H;
            Style::CutRect(x, BAR_Y, W_LINE, H, 4.f, Style::ButtonBody(Style::Look::Idle));
            Style::CutRect(x + 3.f, BAR_Y + 3.f, W_LINE - 6.f, H - 6.f, 3.f, Style::FrameLine());
            float inner = W_LINE - 12.f;
            float played = (float)(inner * std::max(0.0, std::min(1.0, fraction)));
            CSurface::GL_DrawRect(x + 6.f, BAR_Y + 10.f, played, H - 20.f, GL_Color(1.f, 0.84f, 0.3f, 0.9f));
            CSurface::GL_DrawRect(x + 6.f + played - 2.f, BAR_Y + 6.f, 4.f, H - 12.f, GL_Color(1.f, 1.f, 1.f, 1.f));
            if (seeking)
            {
                CSurface::GL_SetColor(GL_Color(1.f, 1.f, 1.f, 1.f));
                freetype::easy_printCenter(10, x + W_LINE / 2.f, BAR_Y + 7.f, "SEEKING");
                CSurface::GL_SetColor(COLOR_WHITE);
            }
        }

        void Render()
        {
            g.shown = false;
            g.stop.w = g.back.w = g.play.w = g.on.w = g.speed.w = g.line.w = g.view.w = g.sensors.w = 0.f;
            Demo::ReplayView v = Demo::GetReplayView();
            if (!v.active || !InGame()) return;
            g.shown = true;
            g.lengthMs = v.lengthMs;
            float x = BAR_X;
            IconButton(g.stop, x, Icon::Stop);
            x += W_ICON + GAP;
            IconButton(g.back, x, Icon::Back);
            x += W_ICON + GAP;
            IconButton(g.play, x, v.paused && !v.seeking ? Icon::Play : Icon::Pause);
            x += W_ICON + GAP;
            IconButton(g.on, x, Icon::On);
            x += W_ICON + GAP;
            Button(g.speed, x, W_SPEED, SpeedText(v.speed));
            x += W_SPEED + GAP;
            Line(x, v.lengthMs > 0.0 ? v.positionMs / v.lengthMs : 0.0, v.seeking);
            x += W_LINE + GAP;
            // The time and the demo's length, on FTL's dark backing as its other numbers.
            std::string time = Clock(v.positionMs) + " / " + Clock(v.lengthMs);
            float width = (float)freetype::easy_measureWidth(12, time);
            CSurface::GL_DrawRect(x, BAR_Y + 2.f, width + 12.f, H - 4.f, GL_Color(0.f, 0.f, 0.f, 0.65f));
            CSurface::GL_SetColor(GL_Color(1.f, 1.f, 1.f, 1.f));
            freetype::easy_print(12, x + 6.f, BAR_Y + 8.f, time);
            CSurface::GL_SetColor(COLOR_WHITE);
            // Whose side the screen shows, and full sensors: both need both players' full states (BA).
            SideButton(g.view, SIDE_X, W_VIEW, v.viewedHost ? "HOST'S VIEW" : "GUEST'S VIEW", v.bothSides, false);
            SideButton(g.sensors, SIDE_X + W_VIEW + GAP, W_SENSORS, "FULL SENSORS", v.bothSides, v.fullSensors);
        }

        void RenderSeekCover()
        {
            Demo::ReplayView v = Demo::GetReplayView();
            if (!v.active || !v.covering || !InGame()) return;
            CSurface::GL_DrawRect(0.f, 0.f, 1280.f, 720.f, GL_Color(6.f / 255.f, 8.f / 255.f, 12.f / 255.f, 1.f));
            CSurface::GL_SetColor(GL_Color(226.f / 255.f, 230.f / 255.f, 236.f / 255.f, 1.f));
            freetype::easy_printCenter(24, 640.f, 300.f, "REPLAY");   // font 24: its letters 15 px lower
            freetype::easy_printCenter(12, 640.f, 352.f, v.coverText);
            const float bw = 420.f, bx = 640.f - bw / 2.f, by = 384.f;
            CSurface::GL_DrawRect(bx, by, bw, 10.f, GL_Color(0.2f, 0.22f, 0.26f, 1.f));
            CSurface::GL_DrawRect(bx, by, (float)(bw * v.coverProgress), 10.f, GL_Color(1.f, 0.84f, 0.3f, 1.f));
            CSurface::GL_SetColor(COLOR_WHITE);
        }

        bool LButtonDown(int x, int y)
        {
            if (Demo::GetReplayView().covering) return true;   // nothing to click behind the cover
            if (!g.shown) return false;
            if (g.stop.Contains(x, y)) Demo::ReplayStop();
            else if (g.back.Contains(x, y)) Demo::ReplayStep(-STEP_MS);
            else if (g.play.Contains(x, y)) Demo::ReplayPlayPause();
            else if (g.on.Contains(x, y)) Demo::ReplayStep(STEP_MS);
            else if (g.speed.Contains(x, y)) Demo::ReplaySpeedStep(0);
            else if (g.view.Contains(x, y)) Demo::ReplaySwitchView();
            else if (g.sensors.Contains(x, y)) Demo::ReplaySetFullSensors(!Demo::ReplayFullSensors());
            else if (g.line.Contains(x, y))
            {
                double fraction = (x - (g.line.x + 6.f)) / std::max(1.f, g.line.w - 12.f);
                Demo::ReplaySeekTo(std::max(0.0, std::min(1.0, fraction)) * g.lengthMs);
            }
            else return false;
            return true;
        }

        void MouseMove(int x, int y)
        {
            g.mouseX = x;
            g.mouseY = y;
        }

        bool KeyDown(int key)
        {
            Demo::ReplayView v = Demo::GetReplayView();
            if (!v.active) return false;
            if (v.covering) return key != SDLK_ESCAPE;   // behind the cover only FTL's menu
            switch (key)
            {
            case SDLK_SPACE: Demo::ReplayPlayPause(); return true;
            case SDLK_LEFT: Demo::ReplayStep(-STEP_MS); return true;
            case SDLK_RIGHT: Demo::ReplayStep(STEP_MS); return true;
            case SDLK_UP: Demo::ReplaySpeedStep(1); return true;
            case SDLK_DOWN: Demo::ReplaySpeedStep(-1); return true;
            case SDLK_HOME: Demo::ReplaySeekTo(0.0); return true;
            case SDLK_v: Demo::ReplaySwitchView(); return true;
            case SDLK_f: Demo::ReplaySetFullSensors(!Demo::ReplayFullSensors()); return true;
            case SDLK_ESCAPE: return false;   // FTL's menu
            default: return true;             // nothing else reaches the game in a replay
            }
        }

        bool ControlCentre(const std::string &name, int &x, int &y)
        {
            const Style::Box *box = name == "stop" ? &g.stop : name == "back" ? &g.back : name == "play" ? &g.play
                                  : name == "on" ? &g.on : name == "speed" ? &g.speed : name == "line" ? &g.line
                                  : name == "view" ? &g.view : name == "sensors" ? &g.sensors : nullptr;
            if (!g.shown || !box || box->w <= 0.f) return false;
            x = (int)(box->x + box->w / 2.f);
            y = (int)(box->y + box->h / 2.f);
            return true;
        }
    }
}

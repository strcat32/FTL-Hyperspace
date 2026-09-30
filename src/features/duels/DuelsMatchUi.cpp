#include "Global.h"
#include "Duels.h"
#include "DuelsMatchUi.h"
#include "DuelsRounds.h"
#include "DuelsTrace.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace Duels
{
    namespace MatchUi
    {
        // Where things go (FTL's 1280 x 720; docs/design/match-ui.md).
        static const float PANEL_CENTRE = 430.f, PANEL_TOP = 48.f, PANEL_MAX_WIDTH = 188.f;   // under the scrap
        static const float PREP_CENTRE = 1105.f, PREP_TOP = 430.f;      // the preparation: right of the store
        // The fight: between the weapons bar and the enemy window, above the drone systems (clear of the feed at the
        // bottom left): the countdown ends at FIGHT_SPLIT, the buttons start there.
        static const float FIGHT_SPLIT = 604.f, FIGHT_TOP = 574.f;
        static const float SPLASH_MIDDLE = 250.f;                       // the splashes' middle line
        static const double ARMED_MS = 3000.0;                          // "Sure?" on Concede

        struct Box
        {
            float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
            bool shown = false;

            bool Contains(int px, int py) const
            {
                return shown && px >= x && px < x + w && py >= y && py < y + h;
            }
        };

        struct SplashState
        {
            bool active = false;
            std::string text;
            Colour colour = WHITE;
            double start = 0.0, ms = 0.0;
            bool names = false;
        };

        struct State
        {
            int mouseX = 0, mouseY = 0;
            Box ready, draw, concede;
            double concedeArmedUntil = 0.0;
            SplashState splash;
            std::string beepLabel;          // the countdown last beeped for this label and whole second
            int beepSecond = -1;
        };

        static State g;

        static bool InGame()
        {
            WorldManager *world = G_->GetWorld();
            CApp *app = G_->GetCApp();
            return world && world->commandGui && world->playerShip && app && !app->menu.bOpen;
        }

        static GL_Color ColourOf(Colour colour, float alpha)
        {
            switch (colour)
            {
            case RED: return GL_Color(1.f, 0.35f, 0.28f, alpha);
            case BLUE: return GL_Color(0.4f, 0.68f, 1.f, alpha);
            case GOLD: return GL_Color(1.f, 0.84f, 0.3f, alpha);
            default: return GL_Color(1.f, 1.f, 1.f, alpha);
            }
        }

        static float Width(int font, const std::string &text)
        {
            return (float)freetype::easy_measureWidth(font, text);
        }

        // The text cut to fit a width ("Captain_Kaz..").
        static std::string Fit(int font, const std::string &text, float width)
        {
            if (Width(font, text) <= width) return text;
            std::string cut = text;
            while (!cut.empty() && Width(font, cut + "..") > width) cut.pop_back();
            return cut + "..";
        }

        static void Print(int font, float x, float y, const std::string &text, const GL_Color &colour)
        {
            CSurface::GL_SetColor(colour);
            freetype::easy_print(font, x, y, text);
        }

        static void PrintCentre(int font, float x, float y, const std::string &text, const GL_Color &colour)
        {
            CSurface::GL_SetColor(colour);
            freetype::easy_printCenter(font, x, y, text);
        }

        static void Panel(float x, float y, float w, float h)
        {
            CSurface::GL_DrawRect(x, y, w, h, GL_Color(0.f, 0.f, 0.f, 0.7f));
            CSurface::GL_DrawRectOutline((int)x, (int)y, (int)w, (int)h, GL_Color(0.85f, 0.85f, 0.85f, 0.8f), 2.f);
        }

        static std::string Clock(double ms)
        {
            int seconds = (int)std::ceil(std::max(0.0, ms) / 1000.0);
            char text[16];
            snprintf(text, sizeof(text), "%d:%02d", seconds / 60, seconds % 60);
            return text;
        }

        // ---------------------------------------------------------------------------------------------------------
        // The score panel (AB): the red player's name, the points, the blue player's name; the phase in its colour
        // ---------------------------------------------------------------------------------------------------------

        static void RenderPanel(const Rounds::Summary &s)
        {
            const int font = 12;
            const float line = 17.f;
            std::string points = s.points[0] + " : " + s.points[1];
            float nameWidth = (PANEL_MAX_WIDTH - Width(font, points) - 28.f) / 2.f;
            std::string red = Fit(font, s.names[0], nameWidth), blue = Fit(font, s.names[1], nameWidth);

            std::string phase;
            GL_Color phaseColour(1.f, 1.f, 1.f, 1.f);
            switch (s.phase)
            {
            case Rounds::Phase::Prep: phase = "PREPARATION"; phaseColour = GL_Color(0.55f, 1.f, 0.5f, 1.f); break;
            case Rounds::Phase::Starting:
            case Rounds::Phase::Fight: phase = "FIGHT"; phaseColour = ColourOf(RED, 1.f); break;
            case Rounds::Phase::Ending:
            case Rounds::Phase::RoundOver: phase = "END"; phaseColour = GL_Color(0.62f, 0.72f, 0.88f, 1.f); break;
            case Rounds::Phase::MatchOver: phase = "MATCH OVER"; phaseColour = ColourOf(GOLD, 1.f); break;
            default: break;
            }
            if (s.paused)
            {
                phase = "PAUSED";
                phaseColour = GL_Color(1.f, 0.9f, 0.35f, 1.f);
            }
            if (!s.free && s.phase != Rounds::Phase::MatchOver) phase = "ROUND " + std::to_string(s.round) + "   " + phase;

            float top = Width(font, red) + Width(font, points) + Width(font, blue) + 24.f;
            float width = std::max(top, Width(font, phase)) + 18.f;
            float x = PANEL_CENTRE - width / 2.f;
            Panel(x, PANEL_TOP - 4.f, width, line * 2.f + 8.f);

            float lineX = PANEL_CENTRE - top / 2.f;
            Print(font, lineX, PANEL_TOP, red, ColourOf(RED, 1.f));
            lineX += Width(font, red) + 12.f;
            Print(font, lineX, PANEL_TOP, points, ColourOf(WHITE, 1.f));
            lineX += Width(font, points) + 12.f;
            Print(font, lineX, PANEL_TOP, blue, ColourOf(BLUE, 1.f));
            PrintCentre(font, PANEL_CENTRE, PANEL_TOP + line, phase, phaseColour);
        }

        // ---------------------------------------------------------------------------------------------------------
        // The countdown (AB): larger than the rest; its last 5 s flash red with a beep each second. The preparation's
        // last 5 s are the big numbers of the splash instead.
        // ---------------------------------------------------------------------------------------------------------

        static void Beep(const std::string &label, double ms)
        {
            int second = (int)std::ceil(ms / 1000.0);
            if (label == g.beepLabel && second == g.beepSecond) return;
            g.beepLabel = label;
            g.beepSecond = second;
            if (G_->GetSoundControl()) G_->GetSoundControl()->PlaySoundMix("powerUpSystem", -1.f, false);
        }

        static void RenderCountdown(const Rounds::Summary &s, bool prepLayout)
        {
            if (s.countdownMs < 0.0 || s.countdownLabel.empty()) return;
            bool preparation = s.countdownLabel == "Fight in";
            if (preparation && s.countdownMs <= 5000.0) return;   // the splash counts the last five
            bool urgent = s.countdownMs <= 5000.0;
            if (urgent) Beep(s.countdownLabel, s.countdownMs);
            bool blink = urgent && ((long long)(WallMs() / 250.0) % 2 == 0);
            GL_Color colour = urgent ? (blink ? ColourOf(RED, 1.f) : ColourOf(RED, 0.45f)) : ColourOf(WHITE, 1.f);
            std::string time = Clock(s.countdownMs);
            if (prepLayout)
            {
                Panel(PREP_CENTRE - 75.f, PREP_TOP - 6.f, 150.f, 56.f);
                PrintCentre(12, PREP_CENTRE, PREP_TOP, s.countdownLabel, ColourOf(WHITE, 0.9f));
                PrintCentre(24, PREP_CENTRE, PREP_TOP + 16.f, time, colour);
            }
            else
            {
                // The panel ends left of the buttons; the time sits right-aligned in it (FTL measures font 24 a little
                // short), the label at its left.
                float width = std::max(150.f, Width(12, s.countdownLabel) + 1.3f * Width(24, time) + 40.f);
                float right = FIGHT_SPLIT - 12.f, left = right - width;
                Panel(left, FIGHT_TOP - 4.f, width, 38.f);
                Print(12, left + 10.f, FIGHT_TOP + 9.f, s.countdownLabel, ColourOf(WHITE, 0.9f));
                CSurface::GL_SetColor(colour);
                freetype::easy_printRightAlign(24, right - 10.f, FIGHT_TOP - 1.f, time);
                if (s.paused && !s.pausedText.empty()) Print(10, left + 10.f, FIGHT_TOP + 36.f, s.pausedText, ColourOf(WHITE, 0.85f));
            }
        }

        // ---------------------------------------------------------------------------------------------------------
        // The buttons (R): Ready in the preparation, Draw and Concede in the fight; a small line under Ready and Draw
        // for the opponent's side
        // ---------------------------------------------------------------------------------------------------------

        static void Button(Box &box, float x, float y, float w, const std::string &label, bool enabled, bool highlight,
                           const GL_Color &highlightColour)
        {
            box.x = x;
            box.y = y;
            box.w = w;
            box.h = 26.f;
            box.shown = true;
            bool hover = enabled && box.Contains(g.mouseX, g.mouseY);
            GL_Color fill = hover ? GL_Color(0.35f, 0.33f, 0.12f, 0.95f) : GL_Color(0.08f, 0.09f, 0.1f, 0.9f);
            GL_Color edge = !enabled ? GL_Color(0.45f, 0.45f, 0.45f, 1.f)
                          : hover ? GL_Color(1.f, 0.9f, 0.37f, 1.f)       // FTL's yellow for the button under the mouse
                          : highlight ? highlightColour : GL_Color(0.92f, 0.92f, 0.92f, 1.f);
            CSurface::GL_DrawRect(box.x, box.y, box.w, box.h, fill);
            CSurface::GL_DrawRectOutline((int)box.x, (int)box.y, (int)box.w, (int)box.h, edge, 2.f);
            GL_Color text = !enabled ? GL_Color(0.5f, 0.5f, 0.5f, 1.f) : highlight ? highlightColour : GL_Color(1.f, 1.f, 1.f, 1.f);
            PrintCentre(12, box.x + box.w / 2.f, box.y + 6.f, label, text);
        }

        static void RenderButtons(const Rounds::Summary &s, bool prepLayout)
        {
            g.ready.shown = g.draw.shown = g.concede.shown = false;
            bool running = s.inMatch && s.phase != Rounds::Phase::MatchOver && !s.paused;
            if (!running) return;
            const std::string them = s.names[s.me == 0 ? 1 : 0];
            const GL_Color green(0.55f, 1.f, 0.5f, 1.f), grey(0.75f, 0.75f, 0.75f, 1.f);

            if (s.phase == Rounds::Phase::Prep && !s.free)
            {
                float x = prepLayout ? PREP_CENTRE - 65.f : FIGHT_SPLIT;
                float y = prepLayout ? PREP_TOP + 58.f : FIGHT_TOP;
                Button(g.ready, x, y, 130.f, s.ready ? "NOT READY" : "READY", s.canReady || s.canUnready, s.ready, green);
                std::string line = s.opponentReady ? them + " is ready" : them + " is preparing";
                if (s.ready) line = "You are ready.  " + line;
                PrintCentre(10, x + 65.f, y + 29.f, line, s.opponentReady ? green : grey);
            }
            else if (s.phase == Rounds::Phase::Starting || s.phase == Rounds::Phase::Fight)
            {
                float x = FIGHT_SPLIT, y = FIGHT_TOP;
                bool blink = (long long)(WallMs() / 400.0) % 2 == 0;
                std::string drawLabel = s.drawToAnswer ? "ACCEPT DRAW" : "DRAW";
                Button(g.draw, x, y, 110.f, drawLabel, s.canOfferRoundDraw || s.drawToAnswer, s.drawToAnswer && blink, ColourOf(GOLD, 1.f));
                if (s.drawToAnswer) PrintCentre(10, x + 55.f, y + 29.f, them + " offers a draw", ColourOf(GOLD, 1.f));
                else if (s.weOfferDraw) PrintCentre(10, x + 55.f, y + 29.f, "You offer a draw", grey);
                bool armed = WallMs() < g.concedeArmedUntil;
                Button(g.concede, x + 116.f, y, 94.f, armed ? "SURE?" : "CONCEDE", s.canConcede, armed, ColourOf(RED, 1.f));
            }
        }

        // ---------------------------------------------------------------------------------------------------------
        // The splashes (AC)
        // ---------------------------------------------------------------------------------------------------------

        void Splash(const std::string &text, Colour colour, double ms, const std::string &sound, bool withNames)
        {
            g.splash.active = true;
            g.splash.text = text;
            g.splash.colour = colour;
            g.splash.start = WallMs();
            g.splash.ms = ms;
            g.splash.names = withNames;
            if (!sound.empty() && G_->GetSoundControl()) G_->GetSoundControl()->PlaySoundMix(sound, -1.f, false);
            Log("MatchUi: splash '%s'", text.c_str());
        }

        // Big letters: font 63 (FTL's broad title letters) scaled up, centred on x, their middle on y.
        static void PrintBig(int font, float x, float y, float scale, const std::string &text, const GL_Color &colour)
        {
            float height = freetype::easy_measurePrintLines(font, 0.f, 0.f, 2000, text).y;
            CSurface::GL_PushMatrix();
            CSurface::GL_Translate(x, y - height * scale / 2.f, 0.f);
            CSurface::GL_Scale(scale, scale, 1.f);
            CSurface::GL_SetColor(colour);
            freetype::easy_printCenter(font, 0.f, 0.f, text);
            CSurface::GL_PopMatrix();
        }

        void RenderSplash()
        {
            SplashState &sp = g.splash;
            if (!sp.active || !InGame()) return;
            double t = WallMs() - sp.start;
            if (t > sp.ms)
            {
                sp.active = false;
                return;
            }
            float alpha = t < sp.ms * 0.7 ? 1.f : (float)(1.0 - (t - sp.ms * 0.7) / (sp.ms * 0.3));
            float pop = t < 150.0 ? 1.f + 0.4f * (float)(1.0 - t / 150.0) : 1.f;   // it arrives a little larger
            bool number = sp.text.size() <= 2 && std::isdigit((unsigned char)sp.text[0]);
            float scale = (number ? 5.f : 3.f) * pop;
            CSurface::GL_DrawRect(0.f, SPLASH_MIDDLE - 58.f, 1280.f, sp.names ? 150.f : 116.f, GL_Color(0.f, 0.f, 0.f, 0.45f * alpha));
            PrintBig(63, 640.f + 4.f, SPLASH_MIDDLE + 4.f, scale, sp.text, GL_Color(0.f, 0.f, 0.f, 0.8f * alpha));
            PrintBig(63, 640.f, SPLASH_MIDDLE, scale, sp.text, ColourOf(sp.colour, alpha));
            if (sp.names)
            {
                Rounds::Summary s = Rounds::GetSummary();
                const int font = 24;
                float y = SPLASH_MIDDLE + 48.f;
                PrintCentre(font, 640.f, y, "VS", ColourOf(WHITE, alpha));
                CSurface::GL_SetColor(ColourOf(RED, alpha));
                freetype::easy_printRightAlign(font, 640.f - 30.f, y, s.names[0]);
                Print(font, 640.f + 30.f, y, s.names[1], ColourOf(BLUE, alpha));
            }
            CSurface::GL_SetColor(COLOR_WHITE);
        }

        // ---------------------------------------------------------------------------------------------------------
        // Entry points
        // ---------------------------------------------------------------------------------------------------------

        void Render()
        {
            g.ready.shown = g.draw.shown = g.concede.shown = false;
            if (!InGame()) return;
            Rounds::Summary s = Rounds::GetSummary();
            if (!s.inMatch) return;
            // The preparation has no enemy window, and the store covers the middle: its countdown and Ready go right.
            bool prepLayout = G_->GetShipManager(1) == nullptr;
            RenderPanel(s);
            RenderCountdown(s, prepLayout);
            RenderButtons(s, prepLayout);
            CSurface::GL_SetColor(COLOR_WHITE);
        }

        void MouseMove(int x, int y)
        {
            g.mouseX = x;
            g.mouseY = y;
        }

        bool LButtonDown(int x, int y)
        {
            std::string command, message;
            Rounds::Summary s = Rounds::GetSummary();
            if (g.ready.Contains(x, y)) command = s.ready ? "ready off" : "ready";
            else if (g.draw.Contains(x, y)) command = s.drawToAnswer ? "draw yes" : "draw round";
            else if (g.concede.Contains(x, y))
            {
                if (WallMs() >= g.concedeArmedUntil)
                {
                    g.concedeArmedUntil = WallMs() + ARMED_MS;   // "Sure?": the next click concedes
                    return true;
                }
                g.concedeArmedUntil = 0.0;
                command = "concede";
            }
            else return false;
            bool ok = Rounds::Act(command, message);
            Log("MatchUi: %s -> %s%s", command.c_str(), ok ? "" : "refused: ", message.c_str());
            return true;
        }
    }
}

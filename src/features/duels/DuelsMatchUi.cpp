#include "Global.h"
#include "Duels.h"
#include "DuelsHud.h"
#include "DuelsMatchUi.h"
#include "DuelsRounds.h"
#include "DuelsStyle.h"
#include "DuelsWindow.h"
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
        // Under the scrap, between the hull's number (it ends at x 361) and the FTL drive's box (from x 528).
        static const float PANEL_CENTRE = 445.f, PANEL_TOP = 50.f, PANEL_MAX_WIDTH = 164.f;
        static const float PREP_CENTRE = 1105.f, PREP_TOP = 430.f;      // the preparation: right of the store
        // The fight: between the weapons bar and the enemy window, above the drone systems (clear of the feed at the
        // bottom left): the countdown ends at FIGHT_SPLIT, the buttons start there.
        static const float FIGHT_SPLIT = 604.f, FIGHT_TOP = 574.f;
        static const float SPLASH_MIDDLE = 250.f;                       // the splashes' middle line
        static const double ARMED_MS = 3000.0;                          // "Sure?" on Concede
        static const float BUTTON_H = 28.f;                             // FTL's button look (DuelsStyle.cpp)

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
            case RED: return Style::Red(alpha);
            case BLUE: return Style::Blue(alpha);
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

        // A box as FTL's own on its interface (the scrap's): a light border with cut corners around a dark field.
        static void Panel(float x, float y, float w, float h)
        {
            Style::CutRect(x, y, w, h, 4.f, Style::ButtonBody(Style::Look::Idle));
            Style::CutRect(x + 2.f, y + 2.f, w - 4.f, h - 4.f, 3.f, GL_Color(0.03f, 0.04f, 0.06f, 0.94f));
        }

        static std::string Clock(double ms)
        {
            int seconds = (int)std::ceil(std::max(0.0, ms) / 1000.0);
            char text[16];
            snprintf(text, sizeof(text), "%d:%02d", seconds / 60, seconds % 60);
            return text;
        }

        // ---------------------------------------------------------------------------------------------------------
        // The score panel (AB, AG): three rows around the middle's colon, as a scoreboard: the host's name (red) and
        // the guest's (blue), the points, the round and its phase in its colour
        // ---------------------------------------------------------------------------------------------------------

        static void RenderPanel(const Rounds::Summary &s)
        {
            const int font = 12;
            const float line = 17.f, inner = PANEL_MAX_WIDTH - 12.f;
            const std::string colon = " : ";
            // The names each keep to their half of the panel; in the smaller font when they don't fit in the larger.
            float half = (inner - Width(font, colon)) / 2.f;
            int nameFont = Width(font, s.names[0]) <= half && Width(font, s.names[1]) <= half ? font : 10;
            float nameHalf = (inner - Width(nameFont, colon)) / 2.f;
            std::string red = Fit(nameFont, s.names[0], nameHalf), blue = Fit(nameFont, s.names[1], nameHalf);

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
            if (!s.free && s.phase != Rounds::Phase::MatchOver) phase = "ROUND " + std::to_string(s.round) + "  " + phase;
            // The phase in the smaller font when it doesn't fit ("ROUND 2  PREPARATION").
            int phaseFont = Width(font, phase) <= PANEL_MAX_WIDTH - 12.f ? font : 10;

            // Symmetric about the colon: as wide as the wider side of each row needs.
            float sideNames = std::max(Width(nameFont, red), Width(nameFont, blue)) * 2.f + Width(nameFont, colon);
            float sidePoints = std::max(Width(font, s.points[0]), Width(font, s.points[1])) * 2.f + Width(font, colon);
            float width = std::min(PANEL_MAX_WIDTH, std::max({sideNames, sidePoints, Width(phaseFont, phase)}) + 12.f);
            float x = PANEL_CENTRE - width / 2.f;
            Panel(x, PANEL_TOP - 4.f, width, line * 3.f + 8.f);

            // Each row: the left side right-aligned to the colon, the right side after it.
            auto row = [&](int rowFont, float y, const std::string &left, const std::string &right, const GL_Color &leftColour,
                           const GL_Color &rightColour)
            {
                float colonWidth = Width(rowFont, colon);
                CSurface::GL_SetColor(leftColour);
                freetype::easy_printRightAlign(rowFont, PANEL_CENTRE - colonWidth / 2.f, y, left);
                PrintCentre(rowFont, PANEL_CENTRE, y, colon, ColourOf(WHITE, 1.f));
                Print(rowFont, PANEL_CENTRE + colonWidth / 2.f, y, right, rightColour);
            };
            row(nameFont, PANEL_TOP + (nameFont == font ? 0.f : 2.f), red, blue, ColourOf(RED, 1.f), ColourOf(BLUE, 1.f));
            row(font, PANEL_TOP + line, s.points[0], s.points[1], ColourOf(WHITE, 1.f), ColourOf(WHITE, 1.f));
            PrintCentre(phaseFont, PANEL_CENTRE, PANEL_TOP + 2.f * line + (phaseFont == font ? 0.f : 2.f), phase, phaseColour);
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
                Panel(PREP_CENTRE - 75.f, PREP_TOP - 6.f, 150.f, 58.f);
                PrintCentre(12, PREP_CENTRE, PREP_TOP, s.countdownLabel, ColourOf(WHITE, 0.9f));
                PrintCentre(24, PREP_CENTRE, PREP_TOP + 12.f, time, colour);   // font 24 draws 15 px below its y
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
                freetype::easy_printRightAlign(24, right - 10.f, FIGHT_TOP - 10.f, time);   // its letters 15 px lower, in the middle
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
            box.h = BUTTON_H;
            box.shown = true;
            // FTL's button: its light body, yellow under the mouse, grey when off; a highlight colours the body.
            bool hover = enabled && box.Contains(g.mouseX, g.mouseY);
            Style::Look look = !enabled ? Style::Look::Off : hover ? Style::Look::Hover : Style::Look::Idle;
            Style::Button(x, y, w, BUTTON_H, label, 12, look, highlight && enabled && !hover ? &highlightColour : nullptr);
        }

        static void RenderButtons(const Rounds::Summary &s, bool prepLayout)
        {
            g.ready.shown = g.draw.shown = g.concede.shown = false;
            bool running = s.inMatch && s.phase != Rounds::Phase::MatchOver && !s.paused;
            if (!running) return;
            const std::string them = s.names[s.me == 0 ? 1 : 0];
            const GL_Color green(0.55f, 1.f, 0.5f, 1.f), grey(0.75f, 0.75f, 0.75f, 1.f);
            const GL_Color greenBody(0.59f, 0.93f, 0.53f, 1.f), goldBody(1.f, 0.84f, 0.35f, 1.f), redBody(1.f, 0.5f, 0.43f, 1.f);

            if (s.phase == Rounds::Phase::Prep && !s.free)
            {
                float x = prepLayout ? PREP_CENTRE - 65.f : FIGHT_SPLIT;
                float y = prepLayout ? PREP_TOP + 58.f : FIGHT_TOP;
                Button(g.ready, x, y, 130.f, s.ready ? "NOT READY" : "READY", s.canReady || s.canUnready, s.ready, greenBody);
                std::string line = s.opponentReady ? them + " is ready" : them + " is preparing";
                if (s.ready) line = "You are ready.  " + line;
                PrintCentre(10, x + 65.f, y + BUTTON_H + 4.f, line, s.opponentReady ? green : grey);
            }
            else if (s.phase == Rounds::Phase::Starting || s.phase == Rounds::Phase::Fight)
            {
                float x = FIGHT_SPLIT, y = FIGHT_TOP;
                // A draw offer (AH): ours keeps the button pressed down while it is open; the other player's makes it
                // "DRAW?", flashing (a click accepts). No text under it: the chat log has the offer.
                bool blink = (long long)(WallMs() / 400.0) % 2 == 0;
                if (s.weOfferDraw)
                {
                    g.draw.x = x;
                    g.draw.y = y;
                    g.draw.w = 110.f;
                    g.draw.h = BUTTON_H;
                    g.draw.shown = true;
                    Style::Button(x, y, 110.f, BUTTON_H, "DRAW", 12, Style::Look::Pressed);
                }
                else Button(g.draw, x, y, 110.f, s.drawToAnswer ? "DRAW?" : "DRAW", s.canOfferRoundDraw || s.drawToAnswer,
                            s.drawToAnswer && blink, goldBody);
                bool armed = WallMs() < g.concedeArmedUntil;
                Button(g.concede, x + 116.f, y, 94.f, armed ? "SURE?" : "CONCEDE", s.canConcede, armed, redBody);
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
            // They sit on FTL's interface, so they shake with it (the buttons' hit boxes stay: FTL's do too).
            CSurface::GL_PushMatrix();
            CSurface::GL_Translate(Hud::ShakeX(), Hud::ShakeY(), 0.f);
            if (!Window::IsOpen()) RenderPanel(s);   // the window shows the score itself, and its tab covers the panel
            RenderCountdown(s, prepLayout);
            RenderButtons(s, prepLayout);
            CSurface::GL_PopMatrix();
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

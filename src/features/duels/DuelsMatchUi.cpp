#include "Global.h"
#include "Duels.h"
#include "DuelsHud.h"
#include "DuelsLobby.h"
#include "DuelsMatchUi.h"
#include "DuelsNet.h"
#include "DuelsRounds.h"
#include "DuelsShips.h"
#include "DuelsStyle.h"
#include "DuelsWindow.h"
#include "DuelsTrace.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <vector>

namespace Duels
{
    namespace MatchUi
    {
        // Where things go (FTL's 1280 x 720; docs/design/match-ui.md).
        // Under the scrap, between the hull's number (it ends at x 361) and the FTL drive's box (from x 528).
        static const float PANEL_CENTRE = 445.f, PANEL_TOP = 50.f, PANEL_MAX_WIDTH = 164.f;
        static const float PREP_CENTRE = 1105.f, PREP_TOP = 430.f;      // the preparation: right of the store
        // The fight: between the weapons bar and the enemy window, above the drone systems (clear of the feed at the
        // bottom left): the countdown ends at FIGHT_SPLIT. Its DRAW and CONCEDE are under the crew's save-position
        // buttons (roadmap AY).
        static const float FIGHT_SPLIT = 604.f, FIGHT_TOP = 574.f;
        static const float CREW_BUTTONS_GAP = 6.f;                      // under the save-position buttons, and between ours
        static const float CREW_BUTTON_FRAME = 5.f;                     // FTL's base around each of those buttons, past its hitbox
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

        // The end screen (roadmap 3.5, part 5): after the match's splash, until LOBBY or STAY.
        struct EndScreen
        {
            double overSince = -1.0;   // when the match was seen over, -1 while it runs
            bool dismissed = false;    // STAY
            bool shown = false;
            Box lobby, stay;
        };

        // The ship choice's window (roadmap 3.9, part 2): its tiles as last drawn, and the one marked by a first click.
        struct ChoiceTile
        {
            Box box;
            int value = -1;          // the type (the bans), the offer's index (the pick)
            bool pick = false;
            bool enabled = false;
        };

        struct ChoiceScreen
        {
            bool shown = false;
            std::vector<ChoiceTile> tiles;
            int marked = -1;
            bool markedPick = false;
            double markedUntil = 0.0;
        };

        struct State
        {
            int mouseX = 0, mouseY = 0;
            EndScreen end;
            ChoiceScreen choice;
            Box ready, draw, concede;
            double concedeArmedUntil = 0.0;
            std::string crewButtonsLogged;  // where DRAW and CONCEDE went last (logged when it changes)
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

        // (On whole pixels: FTL's letters blur between them.)
        static void Print(int font, float x, float y, const std::string &text, const GL_Color &colour)
        {
            CSurface::GL_SetColor(colour);
            freetype::easy_print(font, std::floor(x), std::floor(y), text);
        }

        static void PrintCentre(int font, float x, float y, const std::string &text, const GL_Color &colour)
        {
            CSurface::GL_SetColor(colour);
            freetype::easy_printCenter(font, std::floor(x), std::floor(y), text);
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
            case Rounds::Phase::Choice: phase = "SHIP CHOICE"; phaseColour = ColourOf(GOLD, 1.f); break;
            default: break;
            }
            if (s.paused)
            {
                phase = "PAUSED";
                phaseColour = GL_Color(1.f, 0.9f, 0.35f, 1.f);
            }
            if (!s.free && s.phase != Rounds::Phase::MatchOver && s.phase != Rounds::Phase::Choice)
            {
                phase = "ROUND " + std::to_string(s.round) + "  " + phase;
            }
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
                // A replay's controls take this row (roadmap AW): the countdown goes up a row then.
                float top = FIGHT_TOP - (Net::Replaying() ? 44.f : 0.f);
                float width = std::max(150.f, Width(12, s.countdownLabel) + 1.3f * Width(24, time) + 40.f);
                float right = FIGHT_SPLIT - 12.f, left = right - width;
                Panel(left, top - 4.f, width, 38.f);
                Print(12, left + 10.f, top + 9.f, s.countdownLabel, ColourOf(WHITE, 0.9f));
                CSurface::GL_SetColor(colour);
                freetype::easy_printRightAlign(24, right - 10.f, top - 10.f, time);   // its letters 15 px lower, in the middle
                if (s.paused && !s.pausedText.empty()) Print(10, left + 10.f, top + 36.f, s.pausedText, ColourOf(WHITE, 0.85f));
            }
        }

        // ---------------------------------------------------------------------------------------------------------
        // The buttons (R): Ready in the preparation, Draw and Concede in the fight; a small line under Ready and Draw
        // for the opponent's side
        // ---------------------------------------------------------------------------------------------------------

        static void Button(Box &box, float x, float y, float w, const std::string &label, bool enabled, bool highlight,
                           const GL_Color &highlightColour, int font = 12)
        {
            box.x = x;
            box.y = y;
            box.w = w;
            box.h = BUTTON_H;
            box.shown = true;
            // FTL's button: its light body, yellow under the mouse, grey when off; a highlight colours the body.
            bool hover = enabled && box.Contains(g.mouseX, g.mouseY);
            Style::Look look = !enabled ? Style::Look::Off : hover ? Style::Look::Hover : Style::Look::Idle;
            Style::Button(x, y, w, BUTTON_H, label, font, look, highlight && enabled && !hover ? &highlightColour : nullptr);
        }

        // The fight's DRAW and CONCEDE (roadmap AY): under the crew's save-position buttons, one above the other, as
        // wide as the two of those together (with the frames FTL draws around them); they go down with them as the crew
        // list grows. With no crew listed FTL puts those buttons off the screen: ours stay where the list would begin.
        static void UnderCrewButtons(float &x, float &y, float &w)
        {
            x = 19.f;
            y = 152.f;
            w = 69.f;
            WorldManager *world = G_->GetWorld();
            CommandGui *gui = world ? world->commandGui : nullptr;
            if (!gui) return;
            const Globals::Rect &save = gui->crewControl.saveStations.hitbox;
            const Globals::Rect &back = gui->crewControl.returnStations.hitbox;
            if (save.x < 0 || back.x < 0 || save.w <= 0 || back.w <= 0 || save.y < 0 || save.y > 720) return;
            x = (float)save.x - CREW_BUTTON_FRAME;
            w = (float)(back.x + back.w - save.x) + 2.f * CREW_BUTTON_FRAME;
            y = (float)std::max(save.y + save.h, back.y + back.h) + CREW_BUTTON_FRAME + CREW_BUTTONS_GAP;
        }

        static void RenderButtons(const Rounds::Summary &s, bool prepLayout)
        {
            g.ready.shown = g.draw.shown = g.concede.shown = false;
            if (Net::Replaying()) return;   // a replay can't be played (roadmap AW)
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
                float x, y, w;
                UnderCrewButtons(x, y, w);
                char where[80];
                snprintf(where, sizeof(where), "%.0f,%.0f width %.0f", x, y, w);
                if (g.crewButtonsLogged != where)
                {
                    g.crewButtonsLogged = where;
                    Log("MatchUi: DRAW and CONCEDE under the crew's buttons at %s", where);
                }
                // FTL's button letters when "CONCEDE" fits the width inside the button's frame (5 px a side and a margin),
                // else FTL's smaller letters for both.
                int font = Width(12, "CONCEDE") + 16.f <= w ? 12 : 10;
                // A draw offer (AH): ours keeps the button pressed down while it stands (a click takes it back, AT); the
                // other player's makes it "DRAW?", flashing (a click accepts). No text under it: the chat log has it.
                bool blink = (long long)(WallMs() / 400.0) % 2 == 0;
                if (s.weOfferDraw)
                {
                    g.draw.x = x;
                    g.draw.y = y;
                    g.draw.w = w;
                    g.draw.h = BUTTON_H;
                    g.draw.shown = true;
                    Style::Button(x, y, w, BUTTON_H, "DRAW", font, Style::Look::Pressed);
                }
                else Button(g.draw, x, y, w, s.drawToAnswer ? "DRAW?" : "DRAW", s.canOfferRoundDraw || s.drawToAnswer,
                            s.drawToAnswer && blink, goldBody, font);
                bool armed = WallMs() < g.concedeArmedUntil;
                Button(g.concede, x, y + BUTTON_H + CREW_BUTTONS_GAP, w, armed ? "SURE?" : "CONCEDE", s.canConcede, armed, redBody, font);
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
        // The end screen (roadmap 3.5, part 5): the match's end, a forfeit's too: both names and the points, who won
        // and why, the damage score, each round's result; LOBBY (the run left for the room list) or STAY (the run kept
        // for a look and a chat: the Duels window has LOBBY then)
        // ---------------------------------------------------------------------------------------------------------

        static const float EW = 660.f, EH = 450.f, EX = (1280.f - EW) / 2.f, EY = 120.f;
        static const double END_AFTER_MS = 3000.0;   // the match's splash first

        static void EndButton(Box &box, float x, float y, float w, const std::string &label, const GL_Color *body)
        {
            box.x = x;
            box.y = y;
            box.w = w;
            box.h = 34.f;
            box.shown = true;
            bool hover = box.Contains(g.mouseX, g.mouseY);
            Style::Button(x, y, w, 34.f, label, 12, hover ? Style::Look::Hover : Style::Look::Idle, hover ? nullptr : body);
        }

        static void RenderEnd(const Rounds::Summary &s)
        {
            EndScreen &e = g.end;
            e.lobby.shown = e.stay.shown = false;
            if (s.phase != Rounds::Phase::MatchOver) return;
            if (Net::Replaying())
            {
                e.shown = false;   // a replay ends on its own controls (roadmap AW)
                return;
            }
            double now = WallMs();
            if (e.overSince < 0.0) e.overSince = now;
            e.shown = !e.dismissed && now - e.overSince >= END_AFTER_MS;
            if (!e.shown) return;
            if (Window::IsOpen()) Window::Close();

            Style::Dialog(EX, EY, EW, EH, "MATCH OVER");
            // The band: the host's name on the red side, the guest's on the blue side, the points between them.
            float bx = EX + 24.f, bw = EW - 48.f, by = EY + 26.f;
            Style::Blend(bx, by, bw, 56.f, Style::Mix(Style::Red(1.f), GL_Color(0.f, 0.f, 0.f, 1.f), 0.45f),
                         Style::Mix(Style::Blue(1.f), GL_Color(0.f, 0.f, 0.f, 1.f), 0.45f), 4.f);
            std::string points = s.points[0] + " : " + s.points[1];
            float side = (bw - Width(24, points)) / 2.f - 30.f;
            PrintCentre(24, bx + bw / 2.f, by - 5.f, points, ColourOf(WHITE, 1.f));
            Print(24, bx + 16.f, by - 5.f, Fit(24, s.names[0], side), GL_Color(1.f, 0.77f, 0.74f, 1.f));
            CSurface::GL_SetColor(GL_Color(0.77f, 0.85f, 1.f, 1.f));
            freetype::easy_printRightAlign(24, bx + bw - 16.f, by - 5.f, Fit(24, s.names[1], side));

            // Who won, and why ("match over: you win (the other player forfeited)").
            std::string verdict = "A DRAW";
            GL_Color colour = ColourOf(WHITE, 1.f);
            if (s.state.find("you win") != std::string::npos)
            {
                verdict = "YOU WIN";
                colour = ColourOf(GOLD, 1.f);
            }
            else if (s.state.find("you lose") != std::string::npos)
            {
                verdict = "YOU LOSE";
                colour = GL_Color(1.f, 0.55f, 0.5f, 1.f);
            }
            float y = by + 70.f;
            PrintCentre(24, EX + EW / 2.f, y - 12.f, verdict, colour);
            y += 40.f;
            size_t open = s.state.find('('), close = s.state.rfind(')');
            if (open != std::string::npos && close != std::string::npos && close > open)
            {
                PrintCentre(12, EX + EW / 2.f, y, s.state.substr(open + 1, close - open - 1), ColourOf(WHITE, 0.9f));
            }
            y += 26.f;
            std::string score = s.score;
            if (!score.empty()) score[0] = (char)std::toupper((unsigned char)score[0]);
            PrintCentre(10, EX + EW / 2.f, y, score, GL_Color(0.75f, 0.78f, 0.82f, 1.f));
            y += 28.f;

            // Each round.
            float lx = EX + 40.f;
            y += Style::Label(lx, y, "THE ROUNDS") + 10.f;
            size_t first = s.results.size() > 7 ? s.results.size() - 7 : 0;
            if (s.results.empty()) Print(10, lx, y, "No round was finished.", GL_Color(0.75f, 0.78f, 0.82f, 1.f));
            for (size_t i = first; i < s.results.size(); ++i)
            {
                Print(10, lx, y, s.results[i], GL_Color(0.87f, 0.89f, 0.92f, 1.f));
                y += 17.f;
            }

            // LOBBY or STAY.
            float fy = EY + EH - 58.f;
            CSurface::GL_SetColor(GL_Color(0.75f, 0.78f, 0.82f, 1.f));
            freetype::easy_printAutoNewlines(10, lx, fy - 4.f, 300, "LOBBY leaves the run for the room list. STAY keeps it for a "
                                                                   "look and a chat; the DUELS window has LOBBY then.");
            const GL_Color goldBody(1.f, 0.84f, 0.35f, 1.f);
            EndButton(e.stay, EX + EW - 30.f - 170.f - 12.f - 120.f, fy, 120.f, "STAY", nullptr);
            EndButton(e.lobby, EX + EW - 30.f - 170.f, fy, 170.f, "LOBBY", &goldBody);
            CSurface::GL_SetColor(COLOR_WHITE);
        }

        // ---------------------------------------------------------------------------------------------------------
        // The ship choice (roadmap 3.9, part 2): a window over the game while the ships are chosen. The bans: the ten
        // ship types as FTL's hangar shows them, crossed out in the banner's colour as they go; a click marks a type,
        // a second click bans it. The pick: the ships left, larger, with their weapons, systems and crew; a click marks
        // one, a second click picks it. Then both ships, until round 1's preparation begins
        // ---------------------------------------------------------------------------------------------------------

        static const float CW = 1100.f, CH = 540.f, CX = (1280.f - CW) / 2.f, CY = 108.f;
        static const float MINI_W = 191.f, MINI_H = 121.f;   // FTL's hangar images (customizeUI/miniship_*.png)
        static const double MARK_MS = 4000.0;                 // a marked tile waits this long for its second click

        static bool Over(float x, float y, float w, float h)
        {
            return g.mouseX >= x && g.mouseX < x + w && g.mouseY >= y && g.mouseY < y + h;
        }

        // A ship's hangar image at a scale in a dark cell, framed in FTL's light border (or a colour).
        static void ShipTile(const std::string &image, float x, float y, float scale, const GL_Color &border, float alpha)
        {
            x = std::floor(x);
            y = std::floor(y);
            float w = std::round(MINI_W * scale), h = std::round(MINI_H * scale);
            Style::CutRect(x - 4.f, y - 4.f, w + 8.f, h + 8.f, 4.f, border);
            Style::CutRect(x - 2.f, y - 2.f, w + 4.f, h + 4.f, 3.f, GL_Color(0.03f, 0.04f, 0.06f, 0.94f));
            GL_Texture *texture = image.empty() ? nullptr : G_->GetResources()->GetImageId(image);
            if (texture) CSurface::GL_BlitImage(texture, x, y, w, h, 0.f, GL_Color(1.f, 1.f, 1.f, alpha), false);
        }

        // A ship's facts as lines that fit a width: its weapons and drones, its systems (in two lines if need be, broken
        // after a comma) and its crew.
        static std::vector<std::string> Facts(const std::string &blueprint, float width)
        {
            std::vector<std::string> lines;
            for (const std::string &fact : {Ships::Arms(blueprint), Ships::Systems(blueprint), Ships::Augments(blueprint), Ships::CrewText(blueprint)})
            {
                if (fact.empty()) continue;
                std::string line = fact;
                while (Width(10, line) > width && lines.size() < 6)
                {
                    // The longest start that fits, ending at a comma.
                    size_t cut = std::string::npos;
                    for (size_t comma = line.find(", "); comma != std::string::npos; comma = line.find(", ", comma + 1))
                    {
                        if (Width(10, line.substr(0, comma + 1)) <= width) cut = comma;
                    }
                    if (cut == std::string::npos) break;
                    lines.push_back(line.substr(0, cut + 1));
                    line = line.substr(cut + 2);
                }
                lines.push_back(Fit(10, line, width));
            }
            return lines;
        }

        static void Cross(float x, float y, float w, float h, const GL_Color &colour)
        {
            CSurface::GL_DrawLine(x + 10.f, y + 8.f, x + w - 10.f, y + h - 8.f, 5.f, colour);
            CSurface::GL_DrawLine(x + w - 10.f, y + 8.f, x + 10.f, y + h - 8.f, 5.f, colour);
        }

        // The ten types: each in the pool, banned (by whom) or free; ours to ban when it is our turn.
        static void RenderBans(const Rounds::Summary &s, ChoiceScreen &c, bool ourTurn, const GL_Color &ours)
        {
            const Rounds::Summary::Choice &ch = s.choice;
            const float gap = 20.f, cellW = MINI_W, rowH = MINI_H + 44.f;
            const float gx = CX + (CW - (5.f * cellW + 4.f * gap)) / 2.f, gy = CY + 138.f;
            const GL_Color idle = Style::ButtonBody(Style::Look::Idle), hover = Style::ButtonBody(Style::Look::Hover);
            bool blink = (long long)(WallMs() / 350.0) % 2 == 0;
            for (int type = 0; type < Ships::TYPE_COUNT; ++type)
            {
                float x = gx + (type % 5) * (cellW + gap), y = gy + (type / 5) * rowH;
                bool inPool = (ch.pool >> type) & 1;
                int ban = -1;
                for (size_t i = 0; i < ch.banned.size(); ++i)
                {
                    if (ch.banned[i] == type) ban = (int)i;
                }
                bool open = inPool && ban < 0;
                ChoiceTile tile;
                tile.box.x = x - 4.f;
                tile.box.y = y - 4.f;
                tile.box.w = cellW + 8.f;
                tile.box.h = MINI_H + 8.f;
                tile.box.shown = true;
                tile.value = type;
                tile.enabled = open && ourTurn;
                bool marked = tile.enabled && c.marked == type && !c.markedPick;
                bool hovering = tile.enabled && Over(tile.box.x, tile.box.y, tile.box.w, tile.box.h);
                GL_Color border = marked ? ours : hovering ? hover : open ? idle : GL_Color(idle.r, idle.g, idle.b, 0.35f);
                ShipTile(Ships::Image(Ships::TypeBlueprint(type)), x, y, 1.f, border, open ? 1.f : 0.28f);
                std::string label = Ships::TypeName(type);
                GL_Color colour = ColourOf(WHITE, 0.95f);
                if (!inPool)
                {
                    label += ": not in the pool";
                    colour = GL_Color(0.6f, 0.62f, 0.66f, 1.f);
                }
                else if (ban >= 0)
                {
                    GL_Color by = ch.bannedBy[ban] == 0 ? Style::Red(1.f) : Style::Blue(1.f);
                    Cross(x, y, cellW, MINI_H, by);
                    label += ch.byServer[ban] ? ": banned (time)" : ": banned";
                    colour = ch.bannedBy[ban] == 0 ? GL_Color(1.f, 0.62f, 0.58f, 1.f) : GL_Color(0.62f, 0.75f, 1.f, 1.f);
                }
                else if (marked)
                {
                    label += ": ban?";
                    colour = blink ? ours : ColourOf(WHITE, 0.95f);
                }
                PrintCentre(12, x + cellW / 2.f, y + MINI_H + 8.f, label, colour);
                c.tiles.push_back(tile);
            }
        }

        // The ships offered: up to three large with what they carry, more (a host's list) smaller, by name only.
        static void RenderOffer(const Rounds::Summary &s, ChoiceScreen &c, bool ourTurn, const GL_Color &ours)
        {
            const Rounds::Summary::Choice &ch = s.choice;
            const size_t n = ch.offer.size();
            const float scale = n <= 3 ? 1.4f : n <= 10 ? 1.f : 0.7f;
            const int columns = n <= 3 ? (int)n : n <= 10 ? 5 : 7;
            const bool facts = n <= 3;
            const float gap = n <= 3 ? 44.f : 20.f, cellW = MINI_W * scale, rowH = MINI_H * scale + (facts ? 96.f : 40.f);
            const int used = std::min(columns, (int)n);
            const float gx = CX + (CW - (used * cellW + (used - 1) * gap)) / 2.f, gy = CY + 140.f;
            const GL_Color idle = Style::ButtonBody(Style::Look::Idle), hover = Style::ButtonBody(Style::Look::Hover), gold = ColourOf(GOLD, 1.f);
            const GL_Color soft(0.78f, 0.81f, 0.85f, 1.f);
            bool blink = (long long)(WallMs() / 350.0) % 2 == 0;
            for (size_t i = 0; i < n; ++i)
            {
                float x = gx + (i % columns) * (cellW + gap), y = gy + (i / columns) * rowH;
                ChoiceTile tile;
                tile.box.x = x - 4.f;
                tile.box.y = y - 4.f;
                tile.box.w = cellW + 8.f;
                tile.box.h = MINI_H * scale + 8.f;
                tile.box.shown = true;
                tile.value = (int)i;
                tile.pick = true;
                tile.enabled = ourTurn;
                bool ourShip = ch.picked[s.me] && ch.ourPick == (int)i;
                bool marked = tile.enabled && c.marked == (int)i && c.markedPick;
                bool hovering = tile.enabled && Over(tile.box.x, tile.box.y, tile.box.w, tile.box.h);
                GL_Color border = ourShip ? gold : marked ? ours : hovering ? hover : idle;
                ShipTile(Ships::Image(ch.offer[i]), x, y, scale, border, ch.picked[s.me] && !ourShip ? 0.35f : 1.f);
                std::string label = Ships::Title(ch.offer[i]);
                GL_Color colour = ColourOf(WHITE, 0.95f);
                if (ourShip)
                {
                    label += ": your ship";
                    colour = gold;
                }
                else if (marked)
                {
                    label += ": pick?";
                    colour = blink ? ours : ColourOf(WHITE, 0.95f);
                }
                float ty = y + MINI_H * scale + 8.f;
                PrintCentre(12, x + cellW / 2.f, ty, label, colour);
                if (facts)
                {
                    ty += 22.f;
                    for (const std::string &line : Facts(ch.offer[i], cellW + 30.f))
                    {
                        PrintCentre(10, x + cellW / 2.f, ty, line, soft);
                        ty += 16.f;
                    }
                }
                c.tiles.push_back(tile);
            }
        }

        // Both ships, the host's on the red side, the guest's on the blue.
        static void RenderShips(const Rounds::Summary &s)
        {
            const Rounds::Summary::Choice &ch = s.choice;
            const float scale = 1.5f, w = MINI_W * scale, h = MINI_H * scale, y = CY + 168.f;
            const GL_Color soft(0.78f, 0.81f, 0.85f, 1.f);
            for (int player = 0; player < 2; ++player)
            {
                float x = player == 0 ? CX + CW / 2.f - 70.f - w : CX + CW / 2.f + 70.f;
                GL_Color colour = player == 0 ? Style::Red(1.f) : Style::Blue(1.f);
                std::string whose = player == s.me ? "YOUR SHIP" : Fit(12, s.names[player], w - 20.f);
                PrintCentre(12, x + w / 2.f, y - 30.f, whose, player == 0 ? GL_Color(1.f, 0.62f, 0.58f, 1.f) : GL_Color(0.62f, 0.75f, 1.f, 1.f));
                ShipTile(Ships::Image(ch.ships[player]), x, y, scale, colour, 1.f);
                float ty = y + h + 6.f;
                PrintCentre(24, x + w / 2.f, ty - 14.f, Ships::Title(ch.ships[player]), ColourOf(WHITE, 1.f));
                ty += 30.f;
                for (const std::string &line : Facts(ch.ships[player], w + 40.f))
                {
                    PrintCentre(10, x + w / 2.f, ty, line, soft);
                    ty += 16.f;
                }
            }
            PrintCentre(24, CX + CW / 2.f, y + h / 2.f - 26.f, "VS", ColourOf(WHITE, 1.f));
        }

        static void RenderChoice(const Rounds::Summary &s)
        {
            ChoiceScreen &c = g.choice;
            c.tiles.clear();
            c.shown = s.inMatch && s.phase == Rounds::Phase::Choice;
            if (!c.shown)
            {
                c.marked = -1;
                return;
            }
            const Rounds::Summary::Choice &ch = s.choice;
            const uint8_t them = s.me == 0 ? 1 : 0;
            double now = WallMs();
            if (now > c.markedUntil) c.marked = -1;
            bool revealed = !ch.ships[0].empty();
            bool banning = !revealed && ch.banner < 2;
            bool picking = !revealed && !banning && !ch.offer.empty();
            bool ourTurn = !s.paused && ((banning && ch.banner == s.me) || (picking && !ch.picked[s.me]));
            const GL_Color ours = s.me == 0 ? Style::Red(1.f) : Style::Blue(1.f);

            Style::Dialog(CX, CY, CW, CH, "SHIP CHOICE");
            // The band: the host's name on the red side, the guest's on the blue side, the stage between them.
            float bx = CX + 24.f, bw = CW - 48.f, by = CY + 26.f;
            Style::Blend(bx, by, bw, 56.f, Style::Mix(Style::Red(1.f), GL_Color(0.f, 0.f, 0.f, 1.f), 0.45f),
                         Style::Mix(Style::Blue(1.f), GL_Color(0.f, 0.f, 0.f, 1.f), 0.45f), 4.f);
            std::string stage = revealed ? "THE SHIPS" : banning ? "BAN " + std::to_string(ch.banned.size() + 1) + " OF " + std::to_string(ch.bansTotal) : "PICK";
            float side = (bw - Width(24, stage)) / 2.f - 30.f;
            PrintCentre(24, bx + bw / 2.f, by - 5.f, stage, ColourOf(WHITE, 1.f));
            Print(24, bx + 16.f, by - 5.f, Fit(24, s.names[0], side), GL_Color(1.f, 0.77f, 0.74f, 1.f));
            CSurface::GL_SetColor(GL_Color(0.77f, 0.85f, 1.f, 1.f));
            freetype::easy_printRightAlign(24, bx + bw - 16.f, by - 5.f, Fit(24, s.names[1], side));

            // What to do now, and the time left for it.
            float sy = by + 72.f;
            std::string line;
            GL_Color lineColour = ColourOf(WHITE, 0.92f);
            if (s.paused) line = s.pausedText;
            else if (revealed) line = "The first round's preparation begins with these ships.";
            else if (banning && ourTurn) line = "Your ban: click a ship type, and again to ban it.";
            else if (banning) line = s.names[them] + " bans a ship type.";
            else if (picking && ourTurn) line = "Your ship for the match: click one, and again to pick it.";
            else if (picking) line = "You have picked. " + s.names[them] + " picks; you see their ship when both have.";
            if (ourTurn) lineColour = ColourOf(GOLD, 1.f);
            Print(12, bx + 6.f, sy, line, lineColour);
            if (s.countdownMs >= 0.0)
            {
                bool urgent = ourTurn && s.countdownMs <= 5000.0;
                if (urgent) Beep("choice", s.countdownMs);
                bool blink = urgent && (long long)(now / 250.0) % 2 == 0;
                CSurface::GL_SetColor(urgent ? (blink ? ColourOf(RED, 1.f) : ColourOf(RED, 0.45f)) : ColourOf(WHITE, 1.f));
                freetype::easy_printRightAlign(24, bx + bw - 6.f, sy - 16.f, Clock(s.countdownMs));
            }

            if (revealed) RenderShips(s);
            else if (picking) RenderOffer(s, c, ourTurn, ours);
            else RenderBans(s, c, ourTurn, ours);

            // The rules of it, at the bottom.
            std::string rule;
            if (ch.bans && !revealed)
            {
                int pool = 0;
                for (int type = 0; type < Ships::TYPE_COUNT; ++type) pool += (ch.pool >> type) & 1;
                rule = std::to_string(ch.bansTotal) + " bans in turn (" + s.names[ch.firstBanner & 1] + " first) leave " +
                       std::to_string(pool - ch.bansTotal) +
                       " ship types, each with a layout drawn for it. Time up: a ban or a pick is drawn for you. Both may pick the same ship.";
            }
            else if (!revealed) rule = "Pick any ship of the host's list. Time up: a pick is drawn for you. Both may pick the same ship.";
            if (!rule.empty()) PrintCentre(10, CX + CW / 2.f, CY + CH - 34.f, Fit(10, rule, CW - 60.f), GL_Color(0.7f, 0.73f, 0.78f, 1.f));
            CSurface::GL_SetColor(COLOR_WHITE);
        }

        // A click on a tile marks it; a second click on it bans or picks. The window takes the clicks on it; the rest of
        // the screen (the DUELS button) answers as usual.
        static bool ChoiceClick(int x, int y)
        {
            ChoiceScreen &c = g.choice;
            if (!c.shown) return false;
            for (const ChoiceTile &tile : c.tiles)
            {
                if (!tile.enabled || !tile.box.Contains(x, y)) continue;
                if (c.marked == tile.value && c.markedPick == tile.pick && WallMs() <= c.markedUntil)
                {
                    std::string command = tile.pick ? "pick " + std::to_string(tile.value + 1) : std::string("ban ") + Ships::TypeWord(tile.value);
                    std::string message;
                    bool ok = Rounds::Act(command, message);
                    Log("MatchUi: the ship choice: %s -> %s%s", command.c_str(), ok ? "" : "refused: ", message.c_str());
                    c.marked = -1;
                }
                else
                {
                    c.marked = tile.value;
                    c.markedPick = tile.pick;
                    c.markedUntil = WallMs() + MARK_MS;
                }
                return true;
            }
            return x >= CX && x < CX + CW && y >= CY - 42.f && y < CY + CH;
        }

        // ---------------------------------------------------------------------------------------------------------
        // Entry points
        // ---------------------------------------------------------------------------------------------------------

        void Render()
        {
            g.ready.shown = g.draw.shown = g.concede.shown = false;
            g.choice.shown = false;
            if (!InGame()) return;
            Rounds::Summary s = Rounds::GetSummary();
            if (s.phase != Rounds::Phase::MatchOver) g.end = EndScreen();   // a new match, or none
            if (!s.inMatch) return;
            // The preparation has no enemy window, and the store covers the middle: its countdown and Ready go right.
            bool prepLayout = G_->GetShipManager(1) == nullptr;
            // They sit on FTL's interface, so they shake with it (the buttons' hit boxes stay: FTL's do too).
            CSurface::GL_PushMatrix();
            CSurface::GL_Translate(Hud::ShakeX(), Hud::ShakeY(), 0.f);
            if (!Window::IsOpen()) RenderPanel(s);   // the window shows the score itself, and its tab covers the panel
            if (s.phase != Rounds::Phase::Choice) RenderCountdown(s, prepLayout);   // the choice's window has its time
            RenderButtons(s, prepLayout);
            CSurface::GL_PopMatrix();
            RenderChoice(s);
            RenderEnd(s);   // over the rest, the whole screen dimmed
            CSurface::GL_SetColor(COLOR_WHITE);
        }

        void MouseMove(int x, int y)
        {
            g.mouseX = x;
            g.mouseY = y;
        }

        bool Covering()
        {
            return InGame() && (g.choice.shown || g.end.shown);
        }

        bool ButtonCentre(const std::string &name, int &x, int &y)
        {
            const Box *box = name == "draw" ? &g.draw : name == "concede" ? &g.concede : name == "ready" ? &g.ready : nullptr;
            if (!box || !box->shown) return false;
            x = (int)(box->x + box->w / 2.f);
            y = (int)(box->y + box->h / 2.f);
            return true;
        }

        bool LButtonDown(int x, int y)
        {
            // The end screen takes every click while it is up: LOBBY or STAY.
            if (g.end.shown)
            {
                if (g.end.lobby.Contains(x, y))
                {
                    Log("MatchUi: the end screen: LOBBY");
                    g.end.shown = false;
                    g.end.dismissed = true;
                    Lobby::ToLobby();
                }
                else if (g.end.stay.Contains(x, y))
                {
                    Log("MatchUi: the end screen: STAY");
                    g.end.shown = false;
                    g.end.dismissed = true;
                }
                return true;
            }
            if (ChoiceClick(x, y)) return true;
            std::string command, message;
            Rounds::Summary s = Rounds::GetSummary();
            if (g.ready.Contains(x, y)) command = s.ready ? "ready off" : "ready";
            else if (g.draw.Contains(x, y)) command = s.drawToAnswer ? "draw yes" : s.weOfferDraw ? "draw back" : "draw round";
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

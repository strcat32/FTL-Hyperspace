#include "Global.h"
#include "Duels.h"
#include "DuelsHud.h"
#include "DuelsMatch.h"
#include "DuelsNet.h"
#include "DuelsRounds.h"
#include "DuelsStyle.h"
#include "DuelsTrace.h"
#include "DuelsWindow.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace Duels
{
    namespace Window
    {
        struct Box
        {
            float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
            bool Contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
        };

        struct Action
        {
            std::string label;
            std::string command;     // the round module's verb (Rounds::Act)
            bool enabled = false;
            bool confirm = false;    // a second click makes it happen (forfeit, concede)
            Box box;
        };

        struct WindowState
        {
            bool open = false;
            Box button;              // the DUELS button: its frame's visible part (clicks)
            float frameX = 0.f, frameY = 0.f, frameW = 0.f;   // its frame image
            int labelFont = 62;
            Box window;
            Box close;
            int mouseX = -1, mouseY = -1;
            std::vector<Action> actions;
            std::string armed;       // the action waiting for its second click
            double armedUntil = 0.0;
            std::string message;     // what the last action said
            double messageUntil = 0.0;
            bool loggedButton = false, loggedWindow = false;   // the layout's numbers, once (tests)
        };

        static WindowState g_win;

        // The window (roadmap S): FTL's window outline and title tab, over FTL: Duels' red and blue, in the middle of the
        // screen below the top bar. Two columns under the scoreboard: the match and how to win; the bay icons.
        static const float WIDTH = 760.f, HEIGHT = 600.f, TOP = 100.f, PAD = 20.f;
        static const float BAND_H = 58.f, ACTION_H = 34.f;
        static const int FONT = 10, TEXT = 12, BIG = 24;
        static const std::string TITLE = "FTL: DUELS";

        // The icons Duels draws in the weapon and drone bays (tools/make-bay-icons.py), and what they stand for.
        struct Legend
        {
            const char *icon;
            const char *label;
        };

        static const Legend WEAPONS[] = {{"laser", "Laser"},     {"ion", "Ion"},   {"missile", "Missile"}, {"beam", "Beam"},
                                         {"fire", "Fire weapon"}, {"bomb", "Bomb"}, {"flak", "Flak"},       {"crystal", "Crystal"}};
        static const Legend DRONES[] = {{"drone_laser", "Combat, laser"},  {"drone_missile", "Combat, missile"},
                                        {"drone_ion", "Combat, ion"},      {"drone_beam", "Beam"},
                                        {"drone_fire", "Combat, fire"},    {"drone_defense", "Defense"},
                                        {"drone_antidrone", "Anti-drone"}, {"drone_shield", "Shield"},
                                        {"drone_hull", "Hull repair"},     {"drone_repair", "System repair"},
                                        {"drone_battle", "Anti-personnel"}, {"drone_boarder", "Boarding"}};

        static const char *const HOW_TO_WIN =
            "A round won is 1 point, a drawn round half a point for each player. Win more points than the opponent can "
            "still reach; with points equal at the end, the higher damage score wins. A round is won by destroying the "
            "other ship or its whole crew; if both go down at once, the round's "
            "damage score decides. A player who jumps away (the FTL drive charged) gives the other half a point. Each "
            "round begins with a timed preparation: repairs, the round's scrap, the shop and upgrades (only then). If "
            "neither ship's hull or crew reaches a new low for a while, the lows decide the round.";

        static bool InGame()
        {
            WorldManager *world = G_->GetWorld();
            CApp *app = G_->GetCApp();
            return world && world->playerShip && world->commandGui && app && !app->menu.bOpen;
        }

        static GL_Color Rgb(int r, int g, int b, float a = 1.f)
        {
            return GL_Color(r / 255.f, g / 255.f, b / 255.f, a);
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

        static void Text(int font, float x, float y, const std::string &text, GL_Color color)
        {
            CSurface::GL_SetColor(color);
            freetype::easy_print(font, x, y, text);
        }

        static void TextCentre(int font, float x, float y, const std::string &text, GL_Color color)
        {
            CSurface::GL_SetColor(color);
            freetype::easy_printCenter(font, x, y, text);
        }

        // Wrapped text; returns its height.
        static float Paragraph(int font, float x, float y, float width, const std::string &text, GL_Color color)
        {
            CSurface::GL_SetColor(color);
            Pointf end = freetype::easy_printAutoNewlines(font, x, y, (int)width, text);
            return end.y - y;
        }

        // The bay icons are FTL-style 64 px system icons with the glyph (26 px) in the middle: drawn at their own size,
        // crisp and as large as in the game's panels (the user: too small to make out at 22 px).
        static const float ICON_IMAGE = 64.f, ICON_GLYPH = 26.f, ICON_INSET = 19.f;

        static void Icon(const std::string &kind, float glyphX, float glyphY)
        {
            GL_Texture *texture = G_->GetResources()->GetImageId("icons/s_bay_" + kind + "_green1.png");
            if (texture) CSurface::GL_BlitImage(texture, glyphX - ICON_INSET, glyphY - ICON_INSET, ICON_IMAGE, ICON_IMAGE, 0.f, COLOR_WHITE, false);
        }

        // The actions the window offers now, and whether each can be used.
        static void MakeActions(const Rounds::Summary &s)
        {
            std::vector<Action> actions;
            auto add = [&](const std::string &label, const std::string &command, bool enabled, bool confirm)
            {
                Action action;
                action.label = label;
                action.command = command;
                action.enabled = enabled;
                action.confirm = confirm;
                actions.push_back(action);
            };
            bool unready = s.ready && s.canUnready;   // ready in the preparation: a click takes it back
            add(unready ? "NOT READY" : "READY", unready ? "ready off" : "ready", unready || s.canReady, false);
            add("CONCEDE", "concede", s.canConcede, true);
            if (s.drawToAnswer)
            {
                add("ACCEPT DRAW", "draw yes", true, false);
                add("DECLINE", "draw no", true, false);
            }
            else
            {
                add("DRAW ROUND", "draw round", s.canOfferRoundDraw, false);
                add("DRAW MATCH", "draw match", s.canOfferMatchDraw, false);
            }
            add("FORFEIT", "forfeit", s.canForfeit, true);
            g_win.actions.swap(actions);
        }

        // ---------------------------------------------------------------------------------------------------------
        // The DUELS button: FTL's STORE button frame, right of the options button, its body in Duels' red and blue
        // ---------------------------------------------------------------------------------------------------------

        // Where the button goes: after the options button as the next of FTL's top buttons would (Hyperspace places
        // them 3 px apart by their hit boxes; FTL draws the STORE frame 5 px left of its hit box, and 12 px above and
        // left of the button's own place). Its field is as wide as its label needs (FTL's STORE: 78 px).
        static void PlaceButton()
        {
            CommandGui *gui = G_->GetWorld()->commandGui;
            const Globals::Rect &options = gui->optionsButton.hitbox;
            const std::string label = "DUELS";
            g_win.labelFont = 62;
            float field = std::max(40.f, Width(g_win.labelFont, label) + 10.f);
            g_win.frameW = field + 2.f * Style::TOP_FIELD_INSET;
            g_win.frameX = (float)(options.x + options.w + 3 - 5);
            g_win.frameY = (float)gui->storeButton.position.y - Style::TOP_FIELD_INSET;
            Box &b = g_win.button;
            b.x = g_win.frameX + Style::TOP_FRAME_GLOW;
            b.y = g_win.frameY + Style::TOP_FRAME_GLOW;
            b.w = g_win.frameW - 2.f * Style::TOP_FRAME_GLOW;
            b.h = Style::TOP_FRAME_HEIGHT - 2.f * Style::TOP_FRAME_GLOW;
            if (!g_win.loggedButton)
            {
                g_win.loggedButton = true;
                const Globals::Rect &store = gui->storeButton.hitbox;
                Log("Window: the DUELS button's frame at %.0f,%.0f, %.0f wide (visible %.0f-%.0f x %.0f-%.0f); 'DUELS' in font 62 "
                    "is %.0f px wide, a line %.0f high; the options button's hit box %d,%d %dx%d, the store button at %d,%d "
                    "(hit box %d,%d %dx%d)",
                    g_win.frameX, g_win.frameY, g_win.frameW, b.x, b.x + b.w, b.y, b.y + b.h, Width(62, label),
                    Style::LineHeight(62), options.x, options.y, options.w, options.h, gui->storeButton.position.x,
                    gui->storeButton.position.y, store.x, store.y, store.w, store.h);
                const char *samples[] = {"STORE", "CONCEDE", "DRAW ROUND", "ACCEPT DRAW", "NOT READY", "FORFEIT", "THE MATCH"};
                for (const char *sample : samples)
                    Log("Window: '%s' in font 62 %.0f px, font 63 %.0f px, font 12 %.0f px", sample, Width(62, sample),
                        Width(63, sample), Width(12, sample));
                Log("Window: line heights: font 10 %.0f, 12 %.0f, 13 %.0f, 24 %.0f, 62 %.0f, 63 %.0f", Style::LineHeight(10),
                    Style::LineHeight(12), Style::LineHeight(13), Style::LineHeight(24), Style::LineHeight(62), Style::LineHeight(63));
            }
        }

        static void RenderButton()
        {
            PlaceButton();
            bool hover = g_win.button.Contains(g_win.mouseX, g_win.mouseY);
            bool attention = Rounds::GetSummary().drawToAnswer && ((long long)(WallMs() / 500.0) % 2 == 0);
            float x = g_win.frameX, y = g_win.frameY, w = g_win.frameW;
            Style::TopFrame(x, y, w, COLOR_WHITE);
            // The body in FTL's field: FTL's yellow under the mouse and while the window is open, gold while the
            // opponent offers a draw; otherwise Duels' own shade of FTL's light body, red to blue.
            float fx = x + Style::TOP_FIELD_INSET, fy = y + Style::TOP_FIELD_INSET;
            float fw = w - 2.f * Style::TOP_FIELD_INSET, fh = Style::TOP_FIELD_HEIGHT;
            if (hover || g_win.open) Style::CutRect(fx, fy, fw, fh, 2.f, Style::ButtonBody(Style::Look::Hover));
            else if (attention) Style::CutRect(fx, fy, fw, fh, 2.f, Rgb(255, 196, 70));
            else Style::Blend(fx, fy, fw, fh, Rgb(240, 182, 172), Rgb(172, 198, 240), 2.f);
            CSurface::GL_SetColor(Style::ButtonText());
            freetype::easy_printCenter(g_win.labelFont, fx + fw / 2.f, fy + std::floor((fh - Style::LineHeight(g_win.labelFont)) / 2.f) + 1.f, "DUELS");
            CSurface::GL_SetColor(COLOR_WHITE);
        }

        // ---------------------------------------------------------------------------------------------------------
        // The window
        // ---------------------------------------------------------------------------------------------------------

        // The round and its phase, as the score panel on the screen shows them (DuelsMatchUi.cpp).
        static std::string PhaseLine(const Rounds::Summary &s, GL_Color &colour)
        {
            std::string phase;
            colour = Rgb(255, 255, 255);
            switch (s.phase)
            {
            case Rounds::Phase::Prep: phase = "PREPARATION"; colour = GL_Color(0.55f, 1.f, 0.5f, 1.f); break;
            case Rounds::Phase::Starting:
            case Rounds::Phase::Fight: phase = "FIGHT"; colour = GL_Color(1.f, 0.5f, 0.42f, 1.f); break;
            case Rounds::Phase::Ending:
            case Rounds::Phase::RoundOver: phase = "END"; colour = GL_Color(0.7f, 0.8f, 0.95f, 1.f); break;
            case Rounds::Phase::MatchOver: phase = "MATCH OVER"; colour = GL_Color(1.f, 0.84f, 0.3f, 1.f); break;
            default: break;
            }
            if (s.paused)
            {
                phase = "PAUSED";
                colour = GL_Color(1.f, 0.9f, 0.35f, 1.f);
            }
            if (s.free || s.phase == Rounds::Phase::MatchOver || s.rounds <= 0) return phase;
            return "ROUND " + std::to_string(s.round) + " OF " + std::to_string(s.rounds) + "     " + phase;
        }

        // The scoreboard across the top: FTL: Duels' red and blue, the host's name on the red side, the guest's on the
        // blue side, the points between them, and the round and its phase. (Font 24 draws its letters 15 px below the
        // y it is given, font 12 about 1 px: the big line's letters take the band's rows 8 to 29, the small line's
        // rows 37 to 48.)
        static void RenderScoreboard(const Rounds::Summary &s, float x, float y, float w)
        {
            Style::Blend(x, y, w, BAND_H, Rgb(122, 28, 32), Rgb(26, 50, 124), 4.f);
            float middle = x + w / 2.f, bigY = y - 7.f, smallY = y + 36.f;
            if (s.inMatch)
            {
                std::string points = s.points[0] + " : " + s.points[1];
                float side = (w - Width(BIG, points)) / 2.f - 34.f;
                TextCentre(BIG, middle, bigY, points, Rgb(255, 255, 255));
                Text(BIG, x + 16.f, bigY, Fit(BIG, s.names[0], side), Rgb(255, 196, 188));
                CSurface::GL_SetColor(Rgb(196, 218, 255));
                freetype::easy_printRightAlign(BIG, x + w - 16.f, bigY, Fit(BIG, s.names[1], side));
                GL_Color colour;
                std::string phase = PhaseLine(s, colour);
                TextCentre(TEXT, middle, smallY, phase, colour);
            }
            else
            {
                std::string opponent = Net::IsConnected() ? Net::PeerName() : "";
                TextCentre(BIG, middle, bigY, opponent.empty() ? "NOT IN A DUEL" : "VS  " + opponent, Rgb(255, 255, 255));
                TextCentre(TEXT, middle, smallY, opponent.empty() ? "Host or join one from the console (Tab)" : "Waiting for the match", Rgb(220, 224, 230));
            }
        }

        // The left column: the match (who, where, the settings, its state and results) and how to win; returns its end.
        static float RenderMatchColumn(const Rounds::Summary &s, float x, float y, float w)
        {
            const GL_Color white = Rgb(255, 255, 255), soft = Rgb(206, 210, 216), gold = Rgb(255, 235, 170);
            y += Style::Label(x, y, "THE MATCH") + 8.f;
            std::string opponent = Net::IsConnected() ? Net::PeerName() : "";
            if (opponent.empty())
            {
                y += Paragraph(FONT, x, y, w, "Not in a duel. Host or join one from the console (Tab): host relay, lobby, join <code>.", soft) + 6.f;
            }
            else
            {
                Text(FONT, x, y, "Opponent: " + Match::ScreenName(opponent), gold);
                y += 15.f;
                Text(FONT, x, y, Net::UsesRelay() ? "Room: " + Net::RelayCode() : "Direct connection", soft);
                y += 17.f;
            }
            y += Paragraph(FONT, x, y, w, "Settings: " + s.settings, soft) + 5.f;
            if (s.inMatch)
            {
                Text(FONT, x, y, "Now: " + s.state, white);
                y += 15.f;
                if (!s.score.empty())
                {
                    Text(FONT, x, y, std::string(1, (char)toupper(s.score[0])) + s.score.substr(1), white);
                    y += 15.f;
                }
                if (!s.environment.empty()) y += Paragraph(FONT, x, y, w, s.environment, white) + 2.f;
                size_t first = s.results.size() > 5 ? s.results.size() - 5 : 0;
                for (size_t i = first; i < s.results.size(); ++i) y += Paragraph(FONT, x + 10.f, y, w - 10.f, s.results[i], soft) + 1.f;
                if (!s.drawText.empty()) y += Paragraph(FONT, x, y, w, s.drawText, gold) + 2.f;
            }
            y += 12.f;
            y += Style::Label(x, y, "HOW TO WIN") + 8.f;
            y += Paragraph(FONT, x, y, w, HOW_TO_WIN, soft);
            return y;
        }

        // The right column: the icons Duels draws in the weapon and drone bays; returns its end.
        static float RenderBaysColumn(float x, float y, float w)
        {
            const GL_Color white = Rgb(255, 255, 255), soft = Rgb(206, 210, 216), cream = Style::ButtonBody(Style::Look::Idle);
            y += Style::Label(x, y, "WEAPON AND DRONE BAYS") + 8.f;
            y += Paragraph(FONT, x, y, w, "Each weapon and each drone has a room of its own; its icon shows the kind.", soft) + 8.f;
            const float cell = w / 2.f, row = ICON_GLYPH + 6.f;
            auto grid = [&](const Legend *items, size_t count)
            {
                for (size_t i = 0; i < count; ++i)
                {
                    float cx = x + 4.f + cell * (i % 2), cy = y + row * (i / 2);
                    Icon(items[i].icon, cx, cy);
                    Text(FONT, cx + ICON_GLYPH + 10.f, cy + 7.f, items[i].label, white);
                }
                y += row * ((count + 1) / 2);
            };
            Text(TEXT, x, y, "Weapons", cream);
            y += 20.f;
            grid(WEAPONS, sizeof(WEAPONS) / sizeof(WEAPONS[0]));
            y += 8.f;
            Text(TEXT, x, y, "Drones", cream);
            y += 20.f;
            grid(DRONES, sizeof(DRONES) / sizeof(DRONES[0]));
            return y;
        }

        static void RenderWindow()
        {
            Rounds::Summary s = Rounds::GetSummary();
            MakeActions(s);
            Box &w = g_win.window;
            w.w = WIDTH;
            w.h = HEIGHT;
            w.x = std::floor((1280.f - WIDTH) / 2.f);
            w.y = TOP;

            // Opaque, dark red on the left to dark blue on the right (FTL: Duels' red and blue), inside FTL's window
            // outline, with FTL's title tab.
            Style::Blend(w.x + 2.f, w.y + 2.f, w.w - 4.f, w.h - 4.f, Rgb(40, 13, 17), Rgb(12, 20, 44), 9.f);
            Style::WindowOutline((int)w.x, (int)w.y, (int)w.w, (int)w.h);
            Style::TitleTab(w.x, w.y, TITLE);

            float x = w.x + PAD, inner = w.w - 2.f * PAD;
            Box &close = g_win.close;
            close.w = 30.f;
            close.h = 30.f;
            close.x = w.x + w.w - PAD - close.w;
            close.y = w.y + 16.f;
            Style::Button(close.x, close.y, close.w, close.h, "X", TEXT,
                          close.Contains(g_win.mouseX, g_win.mouseY) ? Style::Look::Hover : Style::Look::Idle);
            RenderScoreboard(s, x, w.y + 16.f, inner - close.w - 10.f);

            float top = w.y + 16.f + BAND_H + 16.f, column = (inner - PAD) / 2.f;
            float leftEnd = RenderMatchColumn(s, x, top, column);
            float rightEnd = RenderBaysColumn(x + column + PAD, top, column);

            // The actions, along the bottom, in FTL's buttons (FTL's broad button letters don't fit "DRAW ROUND").
            double now = WallMs();
            if (!g_win.armed.empty() && now > g_win.armedUntil) g_win.armed.clear();
            float gap = 10.f, bw = std::floor((inner - gap * (g_win.actions.size() - 1)) / (float)g_win.actions.size());
            float by = w.y + w.h - 22.f - ACTION_H;
            const int font = TEXT;
            for (size_t i = 0; i < g_win.actions.size(); ++i)
            {
                Action &action = g_win.actions[i];
                action.box.x = x + (bw + gap) * i;
                action.box.y = by;
                action.box.w = bw;
                action.box.h = ACTION_H;
                bool armed = g_win.armed == action.command;
                bool hover = action.enabled && action.box.Contains(g_win.mouseX, g_win.mouseY);
                Style::Look look = !action.enabled ? Style::Look::Off : hover ? Style::Look::Hover : Style::Look::Idle;
                GL_Color alarm = Rgb(255, 128, 110), gold = Rgb(255, 214, 90), green = Rgb(150, 236, 136);
                const GL_Color *body = !action.enabled ? nullptr
                                     : armed ? &alarm
                                     : action.command == "draw yes" && !hover && (long long)(now / 400.0) % 2 == 0 ? &gold
                                     : action.command == "ready off" && !hover ? &green : nullptr;
                Style::Button(action.box.x, action.box.y, action.box.w, action.box.h, armed ? "SURE?" : action.label, font, look, body);
            }
            if (!g_win.message.empty() && now < g_win.messageUntil) TextCentre(FONT, w.x + w.w / 2.f, by + ACTION_H + 5.f, g_win.message, Rgb(255, 235, 170));

            if (!g_win.loggedWindow)
            {
                g_win.loggedWindow = true;
                Log("Window: %.0f x %.0f at %.0f,%.0f; the columns end at y %.0f and %.0f, the actions start at y %.0f (%s); "
                    "the first action at %.0f,%.0f %.0f x %.0f, one every %.0f px",
                    w.w, w.h, w.x, w.y, leftEnd, rightEnd, by, leftEnd > by - 8.f || rightEnd > by - 8.f ? "overlap" : "clear",
                    x, by, bw, ACTION_H, bw + gap);
            }
            CSurface::GL_SetColor(COLOR_WHITE);
        }

        void Render()
        {
            if (!InGame()) return;
            // The button is part of FTL's top bar: it shakes with it.
            CSurface::GL_PushMatrix();
            CSurface::GL_Translate(Hud::ShakeX(), Hud::ShakeY(), 0.f);
            RenderButton();
            CSurface::GL_PopMatrix();
            if (g_win.open) RenderWindow();
            CSurface::GL_SetColor(COLOR_WHITE);
        }

        float ButtonsRight()
        {
            if (!InGame()) return 0.f;
            PlaceButton();
            return g_win.button.x + g_win.button.w;
        }

        bool MouseMove(int x, int y)
        {
            g_win.mouseX = x;
            g_win.mouseY = y;
            return g_win.open && g_win.window.Contains(x, y);
        }

        bool LButtonDown(int x, int y)
        {
            if (!InGame()) return false;
            if (g_win.button.Contains(x, y))
            {
                g_win.open = !g_win.open;
                g_win.armed.clear();
                return true;
            }
            if (!g_win.open) return false;
            if (g_win.close.Contains(x, y))
            {
                g_win.open = false;
                return true;
            }
            for (const Action &action : g_win.actions)
            {
                if (!action.box.Contains(x, y)) continue;
                if (!action.enabled) return true;
                if (action.confirm && g_win.armed != action.command)
                {
                    g_win.armed = action.command;
                    g_win.armedUntil = WallMs() + 3000.0;
                    return true;
                }
                g_win.armed.clear();
                std::string message;
                Rounds::Act(action.command, message);
                g_win.message = message;
                g_win.messageUntil = WallMs() + 5000.0;
                Log("Window: %s -> %s", action.command.c_str(), message.c_str());
                return true;
            }
            // Clicks in the window stay there; outside it they go to the game (the window stays open).
            return g_win.window.Contains(x, y);
        }

        bool KeyDown(int key)
        {
            if (g_win.open && key == SDLK_ESCAPE)
            {
                g_win.open = false;
                return true;
            }
            return false;
        }

        bool IsOpen()
        {
            return g_win.open;
        }

        static bool Click(int x, int y, std::string &message)
        {
            MouseMove(x, y);
            bool taken = LButtonDown(x, y);
            message = "click at " + std::to_string(x) + "," + std::to_string(y) + (taken ? ": the Duels button or window took it" : ": passed to the game") +
                      (g_win.message.empty() ? "" : " (" + g_win.message + ")");
            return true;
        }

        bool RunVerb(const std::vector<std::string> &args, std::string &message)
        {
            if (args.size() >= 2 && (args[1] == "open" || args[1] == "close"))
            {
                g_win.open = args[1] == "open";
                message = std::string("Duels window ") + (g_win.open ? "open" : "closed");
                return true;
            }
            if (args.size() >= 4 && args[1] == "click") return Click(std::atoi(args[2].c_str()), std::atoi(args[3].c_str()), message);
            if (args.size() >= 3 && args[1] == "press")
            {
                // duels press <action>: a click in the middle of the window's button for that action (its verb, as
                // "ready", "draw match", "draw yes"), wherever the layout puts it; the window must have been drawn open.
                std::string command = args[2];
                for (size_t i = 3; i < args.size(); ++i) command += " " + args[i];
                for (const Action &action : g_win.actions)
                    if (action.command == command && g_win.open)
                        return Click((int)(action.box.x + action.box.w / 2.f), (int)(action.box.y + action.box.h / 2.f), message);
                message = "no button for '" + command + "' in the open window";
                return false;
            }
            message = "usage: duels open|close | duels click <x> <y> | duels press <action>";
            return false;
        }
    }
}

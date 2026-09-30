#include "Global.h"
#include "Duels.h"
#include "DuelsNet.h"
#include "DuelsRounds.h"
#include "DuelsTrace.h"
#include "DuelsWindow.h"

#include <algorithm>
#include <cctype>
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
            Box button;              // the Duels button
            Box window;
            Box close;
            int mouseX = -1, mouseY = -1;
            std::vector<Action> actions;
            std::string armed;       // the action waiting for its second click
            double armedUntil = 0.0;
            std::string message;     // what the last action said
            double messageUntil = 0.0;
        };

        static WindowState g_win;

        static const float WIDTH = 620.f, HEIGHT = 636.f, PAD = 14.f;   // five results, the environment and a draw offer fit
        static const int FONT = 10, HEADING = 13, TITLE = 24;

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
            "damage score decides. Each round begins with a timed preparation: repairs, the round's scrap, the shop and "
            "upgrades (only then). If neither ship's hull or crew reaches a new low for a while, the lows decide the round.";

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

        static void Text(int font, float x, float y, const std::string &text, GL_Color color)
        {
            CSurface::GL_SetColor(color);
            freetype::easy_print(font, x, y, text);
        }

        // Wrapped text; returns its height.
        static float Paragraph(int font, float x, float y, float width, const std::string &text, GL_Color color)
        {
            CSurface::GL_SetColor(color);
            Pointf end = freetype::easy_printAutoNewlines(font, x, y, (int)width, text);
            return end.y - y;
        }

        static void Frame(const Box &box, GL_Color fill, GL_Color edge)
        {
            CSurface::GL_DrawRect(box.x, box.y, box.w, box.h, fill);
            CSurface::GL_DrawRectOutline((int)box.x, (int)box.y, (int)box.w, (int)box.h, edge, 2.f);
        }

        static void DrawButton(const Box &box, const std::string &label, bool enabled, bool hover, bool alarm)
        {
            GL_Color fill = !enabled ? Rgb(30, 30, 30) : alarm ? Rgb(150, 40, 40) : hover ? Rgb(90, 90, 90) : Rgb(50, 50, 50);
            GL_Color edge = enabled ? Rgb(220, 220, 220) : Rgb(90, 90, 90);
            Frame(box, fill, edge);
            CSurface::GL_SetColor(enabled ? Rgb(255, 255, 255) : Rgb(110, 110, 110));
            freetype::easy_printCenter(FONT, box.x + box.w / 2.f, box.y + (box.h - 12.f) / 2.f, label);
        }

        static void Icon(const std::string &kind, float x, float y, float size)
        {
            GL_Texture *texture = G_->GetResources()->GetImageId("icons/s_bay_" + kind + "_green1.png");
            if (texture) CSurface::GL_BlitImage(texture, x, y, size, size, 0.f, COLOR_WHITE, false);
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
            add(s.ready ? "Ready: waiting" : "Ready", "ready", s.canReady, false);
            add("Concede round", "concede", s.canConcede, true);
            if (s.drawToAnswer)
            {
                add("Accept draw", "draw yes", true, false);
                add("Decline draw", "draw no", true, false);
            }
            else
            {
                add("Draw: round", "draw round", s.canOfferRoundDraw, false);
                add("Draw: match", "draw match", s.canOfferMatchDraw, false);
            }
            add("Forfeit", "forfeit", s.canForfeit, true);
            g_win.actions.swap(actions);
        }

        static void RenderButton()
        {
            CommandGui *gui = G_->GetWorld()->commandGui;
            // Right of the options button, wherever it is (it moves right when the store button shows).
            const Globals::Rect &options = gui->optionsButton.hitbox;
            Box &b = g_win.button;
            b.x = (float)(options.x + options.w + 6);
            b.y = (float)(options.y + 4);
            b.w = 62.f;   // clear of the enemy window (it starts at x 822 in a duel)
            b.h = (float)std::max(28, options.h - 8);
            bool hover = b.Contains(g_win.mouseX, g_win.mouseY);
            bool attention = Rounds::GetSummary().drawToAnswer && ((int)(WallMs() / 500.0) % 2 == 0);
            GL_Color fill = attention ? Rgb(150, 110, 30) : hover || g_win.open ? Rgb(90, 90, 90) : Rgb(35, 35, 35);
            Frame(b, fill, Rgb(235, 235, 235));
            CSurface::GL_SetColor(COLOR_WHITE);
            freetype::easy_printCenter(HEADING, b.x + b.w / 2.f, b.y + (b.h - 16.f) / 2.f, "DUELS");
        }

        static void RenderWindow()
        {
            Rounds::Summary s = Rounds::GetSummary();
            MakeActions(s);
            Box &w = g_win.window;
            w.w = WIDTH;
            w.h = HEIGHT;
            w.x = (1280.f - WIDTH) / 2.f;
            w.y = 72.f;
            Frame(w, Rgb(20, 22, 26), Rgb(235, 235, 235));   // opaque: the match display sits behind it

            GL_Color white = Rgb(255, 255, 255), soft = Rgb(200, 205, 210), gold = Rgb(255, 235, 170), heading = Rgb(150, 210, 255);
            float x = w.x + PAD, y = w.y + 10.f, inner = w.w - 2.f * PAD;
            Text(TITLE, x, y, "FTL: DUELS", white);
            Box &close = g_win.close;
            close.w = 26.f;
            close.h = 22.f;
            close.x = w.x + w.w - close.w - 10.f;
            close.y = w.y + 10.f;
            DrawButton(close, "X", true, close.Contains(g_win.mouseX, g_win.mouseY), false);
            y += 36.f;

            // Who and where.
            std::string opponent = Net::IsConnected() ? Net::PeerName() : "";
            if (opponent.empty())
            {
                y += Paragraph(FONT, x, y, inner, "Not in a duel. Host or join one from the console (Tab): host relay, lobby, join <code>.", soft) + 6.f;
            }
            else
            {
                Text(FONT, x, y, "Opponent: " + opponent + "      " + (Net::UsesRelay() ? "Room: " + Net::RelayCode() : "Direct connection"), gold);
                y += 18.f;
            }

            // The match.
            y += Paragraph(FONT, x, y, inner, "Match: " + s.settings, soft) + 4.f;
            if (s.inMatch)
            {
                Text(FONT, x, y, "Now: " + s.state, white);
                y += 16.f;
                Text(FONT, x, y, s.score.empty() ? "" : std::string(1, (char)toupper(s.score[0])) + s.score.substr(1), white);
                y += 16.f;
                if (!s.environment.empty())
                {
                    Text(FONT, x, y, s.environment, white);
                    y += 16.f;
                }
                size_t first = s.results.size() > 5 ? s.results.size() - 5 : 0;
                for (size_t i = first; i < s.results.size(); ++i)
                {
                    Text(FONT, x + 10.f, y, s.results[i], soft);
                    y += 15.f;
                }
                if (!s.drawText.empty())
                {
                    Text(FONT, x, y, s.drawText, gold);
                    y += 16.f;
                }
            }
            y += 6.f;

            Text(HEADING, x, y, "How to win", heading);
            y += 20.f;
            y += Paragraph(FONT, x, y, inner, HOW_TO_WIN, soft) + 8.f;

            // The icons of the weapon and drone bays.
            Text(HEADING, x, y, "The weapon and drone bays", heading);
            y += 20.f;
            y += Paragraph(FONT, x, y, inner, "Each weapon and each drone has a room of its own; its icon shows the kind. "
                                              "Weapons:", soft) + 4.f;
            const float cell = inner / 4.f, size = 22.f, row = 26.f;
            for (size_t i = 0; i < sizeof(WEAPONS) / sizeof(WEAPONS[0]); ++i)
            {
                float cx = x + cell * (i % 4), cy = y + row * (i / 4);
                Icon(WEAPONS[i].icon, cx, cy, size);
                Text(FONT, cx + size + 6.f, cy + 5.f, WEAPONS[i].label, white);
            }
            y += row * 2.f + 4.f;
            Text(FONT, x, y, "Drones:", soft);
            y += 16.f;
            for (size_t i = 0; i < sizeof(DRONES) / sizeof(DRONES[0]); ++i)
            {
                float cx = x + cell * (i % 4), cy = y + row * (i / 4);
                Icon(DRONES[i].icon, cx, cy, size);
                Text(FONT, cx + size + 6.f, cy + 5.f, DRONES[i].label, white);
            }

            // The actions, along the bottom.
            double now = WallMs();
            if (!g_win.armed.empty() && now > g_win.armedUntil) g_win.armed.clear();
            float gap = 8.f, bw = (inner - gap * (g_win.actions.size() - 1)) / (float)g_win.actions.size(), by = w.y + w.h - 46.f;
            for (size_t i = 0; i < g_win.actions.size(); ++i)
            {
                Action &action = g_win.actions[i];
                action.box.x = x + (bw + gap) * i;
                action.box.y = by;
                action.box.w = bw;
                action.box.h = 28.f;
                bool armed = g_win.armed == action.command;
                DrawButton(action.box, armed ? "Sure? Click again" : action.label, action.enabled,
                           action.box.Contains(g_win.mouseX, g_win.mouseY), armed);
            }
            if (!g_win.message.empty() && now < g_win.messageUntil) Text(FONT, x, w.y + w.h - 14.f, g_win.message, gold);
            CSurface::GL_SetColor(COLOR_WHITE);
        }

        void Render()
        {
            if (!InGame()) return;
            RenderButton();
            if (g_win.open) RenderWindow();
            CSurface::GL_SetColor(COLOR_WHITE);
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

        bool RunVerb(const std::vector<std::string> &args, std::string &message)
        {
            if (args.size() >= 2 && (args[1] == "open" || args[1] == "close"))
            {
                g_win.open = args[1] == "open";
                message = std::string("Duels window ") + (g_win.open ? "open" : "closed");
                return true;
            }
            if (args.size() >= 4 && args[1] == "click")
            {
                int x = std::atoi(args[2].c_str()), y = std::atoi(args[3].c_str());
                MouseMove(x, y);
                bool taken = LButtonDown(x, y);
                message = "click at " + args[2] + "," + args[3] + (taken ? ": the Duels button or window took it" : ": passed to the game") +
                          (g_win.message.empty() ? "" : " (" + g_win.message + ")");
                return true;
            }
            message = "usage: duels open|close | duels click <x> <y>";
            return false;
        }
    }
}

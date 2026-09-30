#include "Global.h"
#include "CommandConsole.h"
#include "Duels.h"
#include "DuelsConsole.h"
#include "DuelsTrace.h"
#include "DuelsWindow.h"

#include <algorithm>
#include <cctype>
#include <deque>
#include <set>
#include <sstream>
#include <vector>

namespace Duels
{
    namespace Console
    {
        static const size_t SHOWN_LINES = 12;     // the last messages, while the console is open
        static const size_t KEPT_LINES = 200;
        static const size_t KEPT_COMMANDS = 50;
        static const double FEED_MS = 10000.0;     // a feed line stays this long (the last 2 s fading)
        static const size_t FEED_SHOWN = 5;        // lines shown while the feed is closed
        static const size_t FEED_OPEN_SHOWN = 12;  // and while it is open for chat
        static const double ECHO_MS = 6000.0;      // debug mode: the log's lines at the top left this long
        static const size_t ECHO_SHOWN = 8;

        struct Timed
        {
            std::string text;
            double at;
            bool chat;
        };

        static Timed MakeTimed(const std::string &text, bool chat)
        {
            Timed timed;
            timed.text = text;
            timed.at = WallMs();
            timed.chat = chat;
            return timed;
        }

        struct ConsoleState
        {
            bool open = false;
            bool chat = false;             // open as the feed's chat line, not as the console
            struct TextInput *input = nullptr;    // "struct": InputBox has a TextInput() method
            std::deque<std::string> lines;
            std::deque<Timed> feed;        // the feed's lines (chat and what matters)
            std::deque<Timed> echo;        // debug mode: the log's latest lines, shown for a moment
            std::vector<std::string> commands;
            size_t commandPos = 0;
            int openKey = 0;               // the key that opened it, which may still arrive as a typed character
            double openKeyUntilMs = 0.0;
        };

        static ConsoleState g_console;

        // The verbs a line can start with, "DUEL" left out (DuelsDriver.cpp and DuelsShipControl.cpp).
        static const std::set<std::string> VERBS = {
            "ai", "aimcheck", "arm", "augment", "autofire", "battery", "cloak", "hack", "mind", "teleport", "console", "crew", "crewpower", "debug", "describe", "door", "drone",
            "droneparts", "dronepower", "export", "fire", "host", "import", "install", "ionize", "join", "keys", "leave", "lobby",
            "name", "nebula", "net", "netsim", "netstats", "nopause", "note", "pausetest", "power", "quit", "relay", "rooms", "say", "screenshot", "swap",
            "script", "spawn", "status", "stop", "supershield", "trace", "tracepower", "upgrade", "version", "view",
            "weapon", "window", "xp", "match", "ready", "forfeit", "concede", "draw", "hull", "kill", "duels", "fonttest", "mouse", "click", "chatflood", "shake", "drag", "hotkey", "rclick", "escape", "ftlcharge", "menu"};

        static std::string Lower(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            return text;
        }

        static std::string Trim(const std::string &text)
        {
            size_t start = text.find_first_not_of(" \t");
            if (start == std::string::npos) return "";
            size_t end = text.find_last_not_of(" \t");
            return text.substr(start, end - start + 1);
        }

        static void Remember(const std::string &line)
        {
            g_console.lines.push_back(line);
            while (g_console.lines.size() > KEPT_LINES) g_console.lines.pop_front();
        }

        void Print(const std::string &line)
        {
            Remember(line);
            if (!GetState().debug) return;
            g_console.echo.push_back(MakeTimed(line, false));
            while (g_console.echo.size() > ECHO_SHOWN) g_console.echo.pop_front();
        }

        static void AddFeed(const std::string &line, bool chat)
        {
            g_console.feed.push_back(MakeTimed(line, chat));
            while (g_console.feed.size() > KEPT_LINES) g_console.feed.pop_front();
        }

        void Feed(const std::string &line)
        {
            Remember(line);
            AddFeed(line, false);
        }

        void Chat(const std::string &from, const std::string &text)
        {
            Remember(from + ": " + text);
            AddFeed(from + ": " + text, true);
        }

        bool IsOpen()
        {
            return g_console.open;
        }

        static void StartInput(const std::string &text);
        static void Open(bool chat);
        static bool CanOpen(CommandGui *gui);

        bool OpenWith(CommandGui *gui, const std::string &text)
        {
            if (!g_console.open)
            {
                if (!CanOpen(gui)) return false;
                Open(false);
            }
            StartInput(text);
            return true;
        }

        // The conditions of Hyperspace's own console key (CommandConsole.cpp).
        static bool CanOpen(CommandGui *gui)
        {
            CommandConsole *console = CommandConsole::GetInstance();
            if (!gui || !console->enabled || gui->inputBox.bOpen) return false;
            if (gui->writeErrorDialog.bOpen || gui->menuBox.bOpen || gui->gameOverScreen.bOpen) return false;
            if (gui->shipComplete && gui->shipComplete->shipManager && gui->shipComplete->shipManager->bJumping) return false;
            for (FocusWindow *window : gui->focusWindows)
            {
                if (window->bOpen) return false;
            }
            return true;
        }

        // Starts (again) the game's text entry, which delivers typed characters and editing keys as text events.
        static void StartInput(const std::string &text)
        {
            if (!g_console.input) g_console.input = new struct TextInput(200, TextInput::ALLOW_ANY, "");
            g_console.input->Start();
            g_console.input->SetText(text);
            g_console.input->pos = (int)g_console.input->text.size();
        }

        static void Open(bool chat)
        {
            StartInput("");
            g_console.open = true;
            g_console.chat = chat;
            g_console.commandPos = g_console.commands.size();
        }

        static void Close()
        {
            if (g_console.input) g_console.input->Stop();
            g_console.open = false;
            g_console.chat = false;
        }

        // A line typed: DUEL commands with or without "DUEL", "HS <command>" for Hyperspace's commands, anything else
        // as typed.
        static void Run(CommandGui *gui, const std::string &line)
        {
            Remember("> " + line);
            g_console.commands.push_back(line);
            if (g_console.commands.size() > KEPT_COMMANDS) g_console.commands.erase(g_console.commands.begin());
            g_console.commandPos = g_console.commands.size();

            std::istringstream words(line);
            std::string first;
            words >> first;
            std::string command = line;
            if (Lower(first) == "hs") command = Trim(line.substr(2));
            else if (VERBS.count(Lower(first))) command = "DUEL " + line;
            if (!command.empty()) gui->RunCommand(command);
        }

        // The console's key is Hyperspace's "console" hotkey (Options > Controls; Tab by default in FTL: Duels). F1
        // only while that is unbound: FTL selects crew member 1 with F1. The chat key opens it with "say " typed in.
        static bool IsConsoleKey(int key)
        {
            int bound = (int)Settings::GetHotkey("console");
            return bound > 0 ? key == bound : key == SDLK_F1;
        }

        void MigrateKeys()
        {
            // Hyperspace's console key used to be backslash, which many keyboards (German ones, for example) can't
            // type; a saved backslash becomes Tab, FTL: Duels' default.
            SettingValues *settings = G_->GetSettings();
            if (!settings) return;
            for (std::vector<HotkeyDesc> &page : settings->hotkeys)
            {
                for (HotkeyDesc &hotkey : page)
                {
                    if (hotkey.name == "console" && hotkey.key == SDLK_BACKSLASH)
                    {
                        hotkey.key = SDLK_TAB;
                        Log("Console key: backslash changed to Tab (Options > Controls)");
                    }
                }
            }
        }

        static bool IsChatKey(int key)
        {
            int bound = (int)Settings::GetHotkey("duels_chat");
            return bound > 0 && key == bound;
        }

        bool KeyDown(CommandGui *gui, int key)
        {
            if (!g_console.open)
            {
                bool chat = IsChatKey(key);
                if ((!chat && !IsConsoleKey(key)) || !CanOpen(gui)) return false;
                Open(chat);
                g_console.openKey = key;
                g_console.openKeyUntilMs = WallMs() + 150.0;
                return true;
            }
            if ((!g_console.chat && IsConsoleKey(key)) || key == SDLK_ESCAPE)
            {
                Close();
                return true;
            }
            // Up and Down go through the lines typed before.
            std::vector<std::string> &commands = g_console.commands;
            if (key == SDLK_UP && g_console.commandPos > 0)
            {
                StartInput(commands[--g_console.commandPos]);
            }
            else if (key == SDLK_DOWN && g_console.commandPos < commands.size())
            {
                ++g_console.commandPos;
                StartInput(g_console.commandPos < commands.size() ? commands[g_console.commandPos] : "");
            }
            return true;   // the game's own keys are off while typing
        }

        bool TextInput(int ch)
        {
            if (!g_console.open || !g_console.input) return false;
            // The key that just opened the console isn't typed into it.
            int openKey = g_console.openKey;
            g_console.openKey = 0;
            bool sameKey = ch == openKey || (openKey >= 'a' && openKey <= 'z' && ch == openKey - 'a' + 'A');
            if (openKey && sameKey && WallMs() < g_console.openKeyUntilMs) return true;
            g_console.input->OnTextInput(ch);
            return true;
        }

        bool TextEvent(CommandGui *gui, int event)
        {
            if (!g_console.open || !g_console.input) return false;
            switch (event)
            {
            case CEvent::TEXT_CONFIRM:
            {
                std::string line = Trim(g_console.input->GetText());
                if (g_console.chat)
                {
                    // The feed's chat line: said to the other player, then closed.
                    Close();
                    std::string command = "DUEL say " + line;
                    if (!line.empty()) gui->RunCommand(command);
                    break;
                }
                if (!line.empty()) Run(gui, line);
                StartInput("");   // Enter ends the game's text entry; the console stays open
                break;
            }
            case CEvent::TEXT_CANCEL:
                Close();
                break;
            default:
                g_console.input->OnTextEvent((CEvent::TextEvent)event);
                break;
            }
            return true;
        }

        // A line of text on a dark backdrop.
        static void Backdrop(float x, float y, float width, float height, float alpha)
        {
            CSurface::GL_DrawRect(x - 6.f, y - 4.f, width + 12.f, height + 8.f, GL_Color(0.f, 0.f, 0.f, 0.6f * alpha));
        }

        // The input line with its caret (console or chat), at x, y.
        static void InputLine(int font, float x, float y, const std::string &prompt)
        {
            struct TextInput *input = g_console.input;
            input->OnLoop();   // the caret's blink
            std::string typed = input->GetText();
            freetype::easy_print(font, x, y, prompt + typed);
            if ((long long)(WallMs() / 500.0) % 2 == 0)
            {
                // The caret: after the characters before the cursor.
                std::string before = prompt + input->GetUTF8(0, input->pos);
                float caretX = x + (float)freetype::easy_measureWidth(font, before);
                CSurface::GL_DrawRect(caretX, y + 1.f, 1.f, 11.f, COLOR_WHITE);
            }
        }

        static bool InGame()
        {
            WorldManager *world = G_->GetWorld();
            CApp *app = G_->GetCApp();
            return world && world->playerShip && world->commandGui && app && !app->menu.bOpen;
        }

        // The feed at the bottom left, above the power bars and systems: its recent lines fading, or (open for chat)
        // more lines and the input line.
        // A line cut into rows no wider than the width, at spaces (a word longer than the width gets a row of its own).
        static std::vector<std::string> WrapRows(int font, const std::string &text, float width)
        {
            std::vector<std::string> rows;
            std::string row;
            std::istringstream words(text);
            std::string word;
            while (words >> word)
            {
                std::string longer = row.empty() ? word : row + " " + word;
                if (!row.empty() && (float)freetype::easy_measureWidth(font, longer) > width)
                {
                    rows.push_back(row);
                    row = word;
                }
                else row = longer;
            }
            if (!row.empty() || rows.empty()) rows.push_back(row);
            return rows;
        }

        static void RenderFeed()
        {
            // Its rows wrap before x 336: in the preparation the store starts right of that, in a fight the match's
            // countdown and buttons (roadmap S; DuelsMatchUi.cpp).
            const int font = 10;
            const float x = 16.f, bottom = 588.f, lineHeight = 14.f, maxWidth = 318.f;
            double now = WallMs();
            bool typing = g_console.open && g_console.chat && g_console.input;
            // The Duels window shows the match itself, and the feed would run over its left side.
            if (!typing && Window::IsOpen()) return;
            std::vector<const Timed*> shown;
            for (auto it = g_console.feed.rbegin(); it != g_console.feed.rend(); ++it)
            {
                if (shown.size() >= (typing ? FEED_OPEN_SHOWN : FEED_SHOWN)) break;
                if (!typing && now - it->at > FEED_MS) break;
                shown.push_back(&*it);
            }
            if (shown.empty() && !typing) return;

            // The rows, oldest first, each with its line's fading.
            struct FeedRow
            {
                std::string text;
                bool chat;
                float alpha;
            };
            std::vector<FeedRow> rows;
            for (auto it = shown.rbegin(); it != shown.rend(); ++it)
            {
                const Timed &line = **it;
                float lineAlpha = typing ? 1.f : (float)std::min(1.0, std::max(0.0, (FEED_MS - (now - line.at)) / 2000.0));
                for (const std::string &text : WrapRows(font, line.text, maxWidth)) rows.push_back(FeedRow{text, line.chat, lineAlpha});
            }
            float count = (float)rows.size() + (typing ? 1.f : 0.f);
            float top = bottom - count * lineHeight;
            // Fading with its newest line.
            float alpha = 1.f;
            if (!typing && !shown.empty()) alpha = (float)std::min(1.0, std::max(0.0, (FEED_MS - (now - shown.front()->at)) / 2000.0));
            // As wide as its longest row (the chat's input line gets the whole width).
            float width = typing ? maxWidth : 0.f;
            for (const FeedRow &row : rows) width = std::max(width, (float)freetype::easy_measureWidth(font, row.text));
            Backdrop(x, top, std::min(width, maxWidth), count * lineHeight, typing ? 1.f : alpha);
            float y = top;
            for (const FeedRow &row : rows)
            {
                CSurface::GL_SetColor(row.chat ? GL_Color(0.75f, 0.95f, 1.f, row.alpha) : GL_Color(1.f, 0.95f, 0.8f, row.alpha));
                freetype::easy_print(font, x, y, row.text);
                y += lineHeight;
            }
            if (typing)
            {
                CSurface::GL_SetColor(COLOR_WHITE);
                InputLine(font, x, y, "Say: ");
            }
            CSurface::GL_SetColor(COLOR_WHITE);
        }

        // Debug mode: the log's latest lines at the top left for a moment, on a dark backdrop.
        static void RenderEcho(float x, float y, int font, int lineLength)
        {
            double now = WallMs();
            std::string text;
            for (const Timed &line : g_console.echo)
            {
                if (now - line.at > ECHO_MS) continue;
                text += (text.empty() ? "" : "\n") + line.text;
            }
            if (text.empty()) return;
            float height = freetype::easy_measurePrintLines(font, x, y, lineLength, text).y;
            Backdrop(x, y, (float)lineLength, height, 1.f);
            CSurface::GL_SetColor(COLOR_WHITE);
            freetype::easy_printAutoNewlines(font, x, y, lineLength, text);
        }

        bool Render()
        {
            if (!InGame()) return false;
            PrintHelper *printer = PrintHelper::GetInstance();
            float x = (float)printer->x;
            float y = (float)printer->y;
            int font = printer->font;
            RenderFeed();
            bool console = g_console.open && !g_console.chat && g_console.input;
            if (!console)
            {
                if (GetState().debug && !Window::IsOpen()) RenderEcho(x, y, font, printer->lineLength);   // not over the Duels window
                return false;
            }

            // The recent messages, then the input line under them with the caret, on a faint backdrop.
            std::string text;
            size_t first = g_console.lines.size() > SHOWN_LINES ? g_console.lines.size() - SHOWN_LINES : 0;
            for (size_t i = first; i < g_console.lines.size(); ++i) text += (i > first ? "\n" : "") + g_console.lines[i];
            float textHeight = text.empty() ? 0.f : freetype::easy_measurePrintLines(font, x, y, printer->lineLength, text).y;
            const float lineHeight = 14.f;
            CSurface::GL_DrawRect(x - 6.f, y - 5.f, (float)printer->lineLength + 12.f, textHeight + lineHeight + 10.f,
                                  GL_Color(0.f, 0.f, 0.f, 0.7f));
            CSurface::GL_SetColor(COLOR_WHITE);
            if (!text.empty())
            {
                freetype::easy_printAutoNewlines(font, x, y, printer->lineLength, text);
                y += textHeight;
            }
            InputLine(font, x, y, "> ");
            return true;
        }
    }
}

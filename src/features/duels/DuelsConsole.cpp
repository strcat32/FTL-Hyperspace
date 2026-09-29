#include "Global.h"
#include "CommandConsole.h"
#include "Duels.h"
#include "DuelsConsole.h"
#include "DuelsTrace.h"

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

        struct ConsoleState
        {
            bool open = false;
            struct TextInput *input = nullptr;    // "struct": InputBox has a TextInput() method
            std::deque<std::string> lines;
            std::vector<std::string> commands;
            size_t commandPos = 0;
            int openKey = 0;               // the key that opened it, which may still arrive as a typed character
            double openKeyUntilMs = 0.0;
        };

        static ConsoleState g_console;

        // The verbs a line can start with, "DUEL" left out (DuelsDriver.cpp and DuelsShipControl.cpp).
        static const std::set<std::string> VERBS = {
            "ai", "aimcheck", "arm", "augment", "autofire", "battery", "cloak", "console", "crew", "debug", "describe", "door", "drone",
            "droneparts", "dronepower", "export", "fire", "host", "import", "install", "ionize", "join", "keys", "leave",
            "name", "nebula", "net", "netsim", "netstats", "nopause", "note", "pausetest", "power", "quit", "relay", "rooms", "say", "screenshot", "swap",
            "script", "spawn", "status", "stop", "supershield", "trace", "tracepower", "upgrade", "version", "view",
            "weapon", "window", "xp"};

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
            PrintHelper::GetInstance()->AddMessage(line);
            Remember(line);
        }

        bool IsOpen()
        {
            return g_console.open;
        }

        static void StartInput(const std::string &text);
        static void Open();
        static bool CanOpen(CommandGui *gui);

        bool OpenWith(CommandGui *gui, const std::string &text)
        {
            if (!g_console.open)
            {
                if (!CanOpen(gui)) return false;
                Open();
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

        static void Open()
        {
            StartInput("");
            g_console.open = true;
            g_console.commandPos = g_console.commands.size();
        }

        static void Close()
        {
            if (g_console.input) g_console.input->Stop();
            g_console.open = false;
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
                Open();
                if (chat) StartInput("say ");
                g_console.openKey = key;
                g_console.openKeyUntilMs = WallMs() + 150.0;
                return true;
            }
            if (IsConsoleKey(key) || key == SDLK_ESCAPE)
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

        bool Render()
        {
            if (!g_console.open || !g_console.input) return false;
            PrintHelper *printer = PrintHelper::GetInstance();
            float x = (float)printer->x;
            float y = (float)printer->y;
            int font = printer->font;

            // The recent messages, then the input line under them with the caret, on a faint backdrop.
            std::string text;
            size_t first = g_console.lines.size() > SHOWN_LINES ? g_console.lines.size() - SHOWN_LINES : 0;
            for (size_t i = first; i < g_console.lines.size(); ++i) text += (i > first ? "\n" : "") + g_console.lines[i];
            float textHeight = text.empty() ? 0.f : freetype::easy_measurePrintLines(font, x, y, printer->lineLength, text).y;
            const float lineHeight = 14.f;
            CSurface::GL_DrawRect(x - 6.f, y - 5.f, (float)printer->lineLength + 12.f, textHeight + lineHeight + 10.f,
                                  GL_Color(0.f, 0.f, 0.f, 0.55f));
            CSurface::GL_SetColor(COLOR_WHITE);
            if (!text.empty())
            {
                freetype::easy_printAutoNewlines(font, x, y, printer->lineLength, text);
                y += textHeight;
            }

            struct TextInput *input = g_console.input;
            input->OnLoop();   // the caret's blink
            std::string typed = input->GetText();
            std::string prompt = "> ";
            freetype::easy_print(font, x, y, prompt + typed);
            if ((long long)(WallMs() / 500.0) % 2 == 0)
            {
                // The caret: after the characters before the cursor.
                std::string before = prompt + input->GetUTF8(0, input->pos);
                float caretX = x + (float)freetype::easy_measureWidth(font, before);
                CSurface::GL_DrawRect(caretX, y + 1.f, 1.f, 11.f, COLOR_WHITE);
            }
            return true;
        }
    }
}

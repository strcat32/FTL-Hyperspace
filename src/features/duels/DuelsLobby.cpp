#include "Global.h"
#include "Duels.h"
#include "DuelsConfig.h"
#include "DuelsConsole.h"
#include "DuelsEnvironment.h"
#include "DuelsLobby.h"
#include "DuelsMatch.h"
#include "DuelsRelay.h"
#include "DuelsRounds.h"
#include "DuelsStyle.h"
#include "DuelsTrace.h"
#include "DuelsWindow.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>

namespace Duels
{
    namespace Lobby
    {
        // A line to type in: FTL's TextInput (the crew renaming's) keeps the text and the caret; Style::TextField draws
        // it (a password as stars).
        struct Field
        {
            struct TextInput *input = nullptr;
            bool stars = false;
            Style::Box box;

            void Make(int maxChars, bool hidden)
            {
                if (!input) input = new struct TextInput(maxChars, TextInput::ALLOW_ASCII, "");
                stars = hidden;
            }
            std::string Text() const { return input ? input->GetText() : std::string(); }
        };

        enum class Window
        {
            None,
            Host,
            Join
        };

        enum class Pending
        {
            None,
            Host,   // a room opens when the run begins
            Join    // the room is joined when the run begins
        };

        struct LobbyState
        {
            Window open = Window::None;
            int mouseX = -1, mouseY = -1;
            Field name, password;                // the Host window's room
            Field code, joinPassword;            // the Join window's
            Field *focus = nullptr;              // the field with the keyboard
            bool listed = true;
            Rounds::NextDuel next;
            std::string message;                 // under the buttons: why CHOOSE SHIP didn't go on
            Style::Box listedBox, roundsLess, roundsMore, prepLess, prepMore, stallLess, stallMore, permadeathBox;
            Style::Box hazardBoxes[Environment::KIND_COUNT];
            Style::Box cancel, choose;

            // After CHOOSE SHIP: the room waits for the run.
            Pending pending = Pending::None;
            std::string roomName, roomPassword, roomCode;
            bool roomListed = true;
            bool sawHangar = false;

            // FTL's first message box at a run's start.
            bool inRun = false;
            double closeBoxUntilMs = 0.0;
        };

        static LobbyState g;

        static const int FONT = 10, TEXT = 12;
        static const size_t ROOM_NAME_MAX = 32;   // the relay keeps 32 bytes of it
        static const int PASSWORD_MAX = 24;

        // The hazards in the Host window's order, with their names there.
        static const uint8_t HAZARDS[] = {Environment::SUN, Environment::PULSAR, Environment::ASTEROIDS,
                                          Environment::NEBULA, Environment::STORM, Environment::BATTERY};
        static const char *const HAZARD_NAMES[] = {"Sun", "Pulsar", "Asteroids", "Nebula", "Ion storm", "Anti-ship battery"};

        static GL_Color Rgb(int r, int g, int b, float a = 1.f)
        {
            return GL_Color(r / 255.f, g / 255.f, b / 255.f, a);
        }

        static void Text(int font, float x, float y, const std::string &text, const GL_Color &colour)
        {
            CSurface::GL_SetColor(colour);
            freetype::easy_print(font, x, y, text);
        }

        static float Paragraph(int font, float x, float y, float width, const std::string &text, const GL_Color &colour)
        {
            CSurface::GL_SetColor(colour);
            return freetype::easy_printAutoNewlines(font, x, y, (int)width, text).y - y;
        }

        static bool Hover(const Style::Box &box)
        {
            return box.Contains(g.mouseX, g.mouseY);
        }

        static void ButtonAt(Style::Box &box, float x, float y, float w, float h, const std::string &label, bool enabled = true)
        {
            box.x = x;
            box.y = y;
            box.w = w;
            box.h = h;
            Style::Button(x, y, w, h, label, TEXT, !enabled ? Style::Look::Off : Hover(box) ? Style::Look::Hover : Style::Look::Idle);
        }

        // A check box and its label; the label clicks too.
        static void CheckAt(Style::Box &box, float x, float y, bool on, const std::string &label)
        {
            box.x = x;
            box.y = y;
            box.w = Style::CHECK_SIZE + 10.f + (float)freetype::easy_measureWidth(FONT, label);
            box.h = Style::CHECK_SIZE;
            Style::CheckBox(x, y, on, Hover(box));
            Text(FONT, x + Style::CHECK_SIZE + 10.f, y + std::floor((Style::CHECK_SIZE - Style::LineHeight(FONT)) / 2.f) + 2.f, label,
                 Rgb(226, 230, 236));
        }

        static void RenderField(Field &field, float x, float y, float w)
        {
            field.box.x = x;
            field.box.y = y;
            field.box.w = w;
            field.box.h = 30.f;
            Style::TextField(x, y, w, 30.f, field.Text(), field.input ? field.input->pos : 0, g.focus == &field, field.stars);
        }

        static void Focus(Field *field)
        {
            for (Field *f : {&g.name, &g.password, &g.code, &g.joinPassword})
            {
                if (f != field && f->input) f->input->Stop();
            }
            g.focus = field;
            if (field && field->input) field->input->Start();
        }

        // The next field of the open window (Tab, Enter).
        static void FocusNext()
        {
            if (g.open == Window::Host) Focus(g.focus == &g.name ? &g.password : &g.name);
            else if (g.open == Window::Join) Focus(g.focus == &g.code ? &g.joinPassword : &g.code);
        }

        static std::string Seconds(int seconds)
        {
            if (seconds % 60 == 0 && seconds >= 120) return std::to_string(seconds / 60) + " min";
            return std::to_string(seconds) + " s";
        }

        static void Close()
        {
            Focus(nullptr);
            g.open = Window::None;
        }

        // CHOOSE SHIP: the room waits for the run, and FTL's hangar opens (as its NEW GAME opens it).
        static void OpenHangar(Pending pending)
        {
            g.pending = pending;
            g.sawHangar = false;
            Close();
            CApp *app = G_->GetCApp();
            if (app) app->menu.shipBuilder.Open();
        }

        // ---------------------------------------------------------------------------------------------------------
        // The Host window
        // ---------------------------------------------------------------------------------------------------------

        static const float HW = 800.f, HH = 540.f, HX = (1280.f - HW) / 2.f, HY = 90.f;

        void OpenHost()
        {
            g.name.Make((int)ROOM_NAME_MAX, false);
            g.password.Make(PASSWORD_MAX, true);
            std::string roomName = Config::Value("room_name");
            if (roomName.empty()) roomName = Match::PlayerName() + "'s duel";
            if (roomName.size() > ROOM_NAME_MAX) roomName.resize(ROOM_NAME_MAX);
            g.name.input->SetText(roomName);
            g.password.input->SetText("");
            g.listed = Config::Value("room_listed") != "off";
            g.next = Rounds::GetNextDuel();
            g.message.clear();
            g.open = Window::Host;
            Focus(&g.name);
            Log("Lobby: the Host window (room '%s', %s)", roomName.c_str(), g.listed ? "listed" : "unlisted");
        }

        static void ChooseHost()
        {
            std::string message;
            if (!Rounds::SetNextDuel(g.next, message))
            {
                g.message = message;
                return;
            }
            g.roomName = g.name.Text();
            g.roomPassword = g.password.Text();
            g.roomListed = g.listed;
            if (SettingsFromConfig())
            {
                Config::SaveValue("room_name", g.roomName);
                Config::SaveValue("room_listed", g.listed ? "on" : "off");
            }
            Log("Lobby: choose a ship; the room '%s' (%s%s) opens with the run: %s", g.roomName.c_str(),
                g.roomListed ? "listed" : "unlisted", g.roomPassword.empty() ? "" : ", with a password", message.c_str());
            OpenHangar(Pending::Host);
        }

        static void RenderHost()
        {
            Style::Dialog(HX, HY, HW, HH, "HOST DUEL");
            const GL_Color white = Rgb(255, 255, 255), soft = Rgb(190, 196, 204), light = Rgb(226, 230, 236), gold = Rgb(255, 235, 170);

            // The room.
            float lx = HX + 30.f, lw = 340.f, y = HY + 24.f;
            y += Style::Label(lx, y, "THE ROOM") + 14.f;
            Text(FONT, lx, y, "Its name, in the room list:", light);
            RenderField(g.name, lx, y + 20.f, lw);
            y += 64.f;
            Text(FONT, lx, y, "A password (empty: none):", light);
            RenderField(g.password, lx, y + 20.f, lw);
            y += 66.f;
            CheckAt(g.listedBox, lx, y, g.listed, "In the room list (JOIN DUEL)");
            y += 40.f;
            Paragraph(FONT, lx, y, lw, "CHOOSE SHIP opens FTL's hangar. Its START begins the run and opens the room at the relay. "
                                       "The room's code is in the Duels window (the DUELS button at the top): the other player "
                                       "joins with it, or finds the room in JOIN DUEL's list.", soft);

            // The match.
            float rx = HX + 410.f, rw = HW - 410.f - 30.f;
            y = HY + 24.f;
            y += Style::Label(rx, y, "THE MATCH") + 14.f;
            const Rounds::NextDuel &n = g.next;
            auto stepper = [&](const std::string &label, const std::string &value, Style::Box &less, Style::Box &more, bool canLess,
                               bool canMore)
            {
                Text(TEXT, rx, y + 6.f, label, light);
                float bx = rx + rw - 2.f * 30.f - 110.f;
                ButtonAt(less, bx, y, 30.f, 28.f, "-", canLess);
                CSurface::GL_SetColor(white);
                freetype::easy_printCenter(TEXT, bx + 30.f + 55.f, y + 6.f, value);
                ButtonAt(more, bx + 30.f + 110.f, y, 30.f, 28.f, "+", canMore);
                y += 38.f;
            };
            stepper("Rounds", "best of " + std::to_string(n.rounds), g.roundsLess, g.roundsMore, n.rounds > 1, n.rounds < 15);
            stepper("Preparation", Seconds(n.prepSeconds), g.prepLess, g.prepMore, n.prepSeconds > 30, n.prepSeconds < 300);
            stepper("Anti-stall", n.stallSeconds == 0 ? "off" : Seconds(n.stallSeconds), g.stallLess, g.stallMore, n.stallSeconds > 0,
                    n.stallSeconds < 600);
            CheckAt(g.permadeathBox, rx, y, n.permadeath, "Permanent death (the dead stay dead)");
            y += 38.f;
            Text(FONT, rx, y, "Hazards, by chance from round 3:", light);
            y += 22.f;
            for (int i = 0; i < 6; ++i)
            {
                float cx = rx + (i % 2) * (rw / 2.f), cy = y + (i / 2) * 30.f;
                CheckAt(g.hazardBoxes[HAZARDS[i]], cx, cy, (n.hazards & (1 << HAZARDS[i])) != 0, HAZARD_NAMES[i]);
            }
            y += 3 * 30.f + 4.f;
            if (n.env != Environment::MODE_AUTO)
            {
                Paragraph(FONT, rx, y, rw, n.env == Environment::MODE_OFF ? std::string("No hazards at all (the console's match env off).")
                                                                           : std::string("Every round: ") + Environment::ModeName(n.env) +
                                                                                 " (the console's match env).",
                          gold);
            }
            else Paragraph(FONT, rx, y, rw, "The anti-ship battery comes from round 5 on.", soft);

            // The buttons.
            float by = HY + HH - 58.f;
            if (!g.message.empty()) Paragraph(FONT, HX + 30.f, by + 8.f, 380.f, g.message, Rgb(255, 140, 120));
            ButtonAt(g.cancel, HX + HW - 30.f - 190.f - 12.f - 130.f, by, 130.f, 34.f, "CANCEL");
            ButtonAt(g.choose, HX + HW - 30.f - 190.f, by, 190.f, 34.f, "CHOOSE SHIP");
        }

        static void ClickHost(int x, int y)
        {
            Rounds::NextDuel &n = g.next;
            if (g.name.box.Contains(x, y)) Focus(&g.name);
            else if (g.password.box.Contains(x, y)) Focus(&g.password);
            else if (g.listedBox.Contains(x, y)) g.listed = !g.listed;
            else if (g.roundsLess.Contains(x, y) && n.rounds > 1) n.rounds = n.rounds % 2 == 0 ? n.rounds - 1 : n.rounds - 2;
            else if (g.roundsMore.Contains(x, y) && n.rounds < 15) n.rounds = n.rounds % 2 == 0 ? n.rounds + 1 : n.rounds + 2;
            else if (g.prepLess.Contains(x, y) && n.prepSeconds > 30) n.prepSeconds = std::max(30, (n.prepSeconds - 1) / 15 * 15);
            else if (g.prepMore.Contains(x, y) && n.prepSeconds < 300) n.prepSeconds = std::min(300, n.prepSeconds / 15 * 15 + 15);
            else if (g.stallLess.Contains(x, y) && n.stallSeconds > 0) n.stallSeconds = n.stallSeconds <= 120 ? 0 : (n.stallSeconds - 1) / 60 * 60;
            else if (g.stallMore.Contains(x, y) && n.stallSeconds < 600) n.stallSeconds = n.stallSeconds < 120 ? 120 : n.stallSeconds / 60 * 60 + 60;
            else if (g.permadeathBox.Contains(x, y)) n.permadeath = !n.permadeath;
            else if (g.cancel.Contains(x, y))
            {
                Close();
                Log("Lobby: the Host window closed");
            }
            else if (g.choose.Contains(x, y)) ChooseHost();
            else
            {
                for (uint8_t kind : HAZARDS)
                {
                    if (g.hazardBoxes[kind].Contains(x, y)) n.hazards ^= (uint8_t)(1 << kind);
                }
            }
        }

        // ---------------------------------------------------------------------------------------------------------
        // The Join window (a room's code; the room list comes with part 3)
        // ---------------------------------------------------------------------------------------------------------

        static const float JW = 560.f, JH = 330.f, JX = (1280.f - JW) / 2.f, JY = 180.f;

        void OpenJoin()
        {
            g.code.Make(6, false);
            g.joinPassword.Make(PASSWORD_MAX, true);
            g.code.input->SetText("");
            g.joinPassword.input->SetText("");
            g.message.clear();
            g.open = Window::Join;
            Focus(&g.code);
            Log("Lobby: the Join window");
        }

        static void ChooseJoin()
        {
            std::string code = g.code.Text();
            std::transform(code.begin(), code.end(), code.begin(), [](unsigned char c) { return (char)std::toupper(c); });
            if (!Relay::Client::IsRoomCode(code))
            {
                g.message = "A room's code has 6 letters and digits (the host's Duels window shows it).";
                return;
            }
            g.roomCode = code;
            g.roomPassword = g.joinPassword.Text();
            Log("Lobby: choose a ship; the room %s is joined with the run%s", code.c_str(), g.roomPassword.empty() ? "" : " (with a password)");
            OpenHangar(Pending::Join);
        }

        static void RenderJoin()
        {
            Style::Dialog(JX, JY, JW, JH, "JOIN DUEL");
            const GL_Color soft = Rgb(190, 196, 204), light = Rgb(226, 230, 236);
            float x = JX + 30.f, w = JW - 60.f, y = JY + 24.f;
            y += Style::Label(x, y, "A ROOM BY ITS CODE") + 14.f;
            Text(FONT, x, y, "The room's code (6 letters and digits):", light);
            RenderField(g.code, x, y + 20.f, 160.f);
            y += 64.f;
            Text(FONT, x, y, "Its password, if it has one:", light);
            RenderField(g.joinPassword, x, y + 20.f, 300.f);
            y += 64.f;
            Paragraph(FONT, x, y, w, "CHOOSE SHIP opens FTL's hangar; its START begins the run and joins the room. The list of "
                                     "open rooms comes here soon; until then the console's lobby command shows it.", soft);
            float by = JY + JH - 58.f;
            if (!g.message.empty()) Paragraph(FONT, x, by - 20.f, w, g.message, Rgb(255, 140, 120));
            ButtonAt(g.cancel, JX + JW - 30.f - 190.f - 12.f - 130.f, by, 130.f, 34.f, "CANCEL");
            ButtonAt(g.choose, JX + JW - 30.f - 190.f, by, 190.f, 34.f, "CHOOSE SHIP");
        }

        static void ClickJoin(int x, int y)
        {
            if (g.code.box.Contains(x, y)) Focus(&g.code);
            else if (g.joinPassword.box.Contains(x, y)) Focus(&g.joinPassword);
            else if (g.cancel.Contains(x, y))
            {
                Close();
                Log("Lobby: the Join window closed");
            }
            else if (g.choose.Contains(x, y)) ChooseJoin();
        }

        // ---------------------------------------------------------------------------------------------------------
        // Entry points
        // ---------------------------------------------------------------------------------------------------------

        bool IsOpen()
        {
            return g.open != Window::None;
        }

        void Render()
        {
            if (g.open == Window::Host) RenderHost();
            else if (g.open == Window::Join) RenderJoin();
            CSurface::GL_SetColor(COLOR_WHITE);
        }

        void MouseMove(int x, int y)
        {
            g.mouseX = x;
            g.mouseY = y;
        }

        bool MouseClick(int x, int y)
        {
            if (g.open == Window::Host) ClickHost(x, y);
            else if (g.open == Window::Join) ClickJoin(x, y);
            else return false;
            return true;
        }

        bool TextInput(int ch)
        {
            if (!IsOpen()) return false;
            if (g.focus && g.focus->input) g.focus->input->OnTextInput(ch);
            g.message.clear();
            return true;
        }

        // Enter goes to the next field (it can come as a key and as the text's confirmation: only the text's counts);
        // Tab too; Escape closes the window.
        bool TextEvent(int event)
        {
            if (!IsOpen()) return false;
            if (event == CEvent::TEXT_CONFIRM) FocusNext();
            else if (event != CEvent::TEXT_CANCEL && g.focus && g.focus->input) g.focus->input->OnTextEvent((CEvent::TextEvent)event);
            return true;
        }

        bool KeyDown(int key)
        {
            if (!IsOpen()) return false;
            if (key == SDLK_TAB) FocusNext();
            else if (key == SDLK_ESCAPE)
            {
                Close();
                Log("Lobby: the window closed (Escape)");
            }
            return true;
        }

        // ---------------------------------------------------------------------------------------------------------
        // Into the run
        // ---------------------------------------------------------------------------------------------------------

        static bool InRun()
        {
            CApp *app = G_->GetCApp();
            WorldManager *world = G_->GetWorld();
            return app && !app->menu.bOpen && world && world->bStartedGame && world->playerShip && world->commandGui;
        }

        void OnFrame()
        {
            CApp *app = G_->GetCApp();
            if (!app) return;
            bool inRun = InRun();
            if (inRun && !g.inRun) g.closeBoxUntilMs = WallMs() + 5000.0;   // a run begins
            g.inRun = inRun;
            if (inRun && WallMs() < g.closeBoxUntilMs)
            {
                // FTL's first message box (its story): the tutorial box explained the duel instead. Its only choice, as
                // the player's click.
                ChoiceBox &box = G_->GetWorld()->commandGui->choiceBox;
                if (box.bOpen && box.choices.size() == 1)
                {
                    box.KeyDown(SDLK_1);
                    g.closeBoxUntilMs = 0.0;
                    Log("Lobby: FTL's first message box closed");
                }
            }

            if (g.pending == Pending::None) return;
            if (app->menu.bOpen)
            {
                // Back from the hangar without its START: no room.
                if (app->menu.shipBuilder.bOpen) g.sawHangar = true;
                else if (g.sawHangar && !app->menu.shipBuilder.bDone)
                {
                    g.pending = Pending::None;
                    Log("Lobby: back from the hangar: no room");
                }
                return;
            }
            if (!inRun) return;
            Pending pending = g.pending;
            g.pending = Pending::None;
            std::string message;
            bool ok = pending == Pending::Host ? HostRoom(g.roomName, g.roomPassword, g.roomListed, message)
                                               : JoinRoom(g.roomCode, g.roomPassword, message);
            Log("Lobby: the run began: %s%s", ok ? "" : "FAILED: ", message.c_str());
            Console::Feed(ok ? message : (pending == Pending::Host ? "No room opened: " : "Not joined: ") + message);
            if (ok) ::Duels::Window::Open();   // the room's code, or the joining
        }

        // A click on the hangar's START, through the menu's input as a player's click goes.
        static bool ClickStart(std::string &message)
        {
            CApp *app = G_->GetCApp();
            if (!app || !app->menu.bOpen || !app->menu.shipBuilder.bOpen)
            {
                message = "the hangar isn't open";
                return false;
            }
            const Globals::Rect &r = app->menu.shipBuilder.startButton.hitbox;
            int x = r.x + r.w / 2, y = r.y + r.h / 2;
            app->menu.MouseMove(x, y);
            app->menu.MouseClick(x, y);
            app->menu.MouseUp(x, y);
            message = "clicked the hangar's START at " + std::to_string(x) + "," + std::to_string(y);
            return true;
        }

        static bool SetField(Field &field, Window window, const std::string &text, std::string &message)
        {
            if (g.open != window || !field.input)
            {
                message = "that window isn't open";
                return false;
            }
            field.input->SetText(text);
            return true;
        }

        bool RunVerb(const std::vector<std::string> &args, std::string &message)
        {
            const std::string what = args.size() > 1 ? args[1] : "";
            if (what == "host") OpenHost();
            else if (what == "join") OpenJoin();
            else if (what == "choose" && g.open == Window::Host) ChooseHost();
            else if (what == "choose" && g.open == Window::Join) ChooseJoin();
            else if (what == "cancel" && IsOpen()) Close();
            else if (what == "start") return ClickStart(message);
            else if (what == "code" && args.size() > 2)
            {
                // "@file": the code is in that file of the game folder (the test runner copies the host's code there).
                std::string code = args[2];
                if (!code.empty() && code[0] == '@')
                {
                    std::ifstream file(code.substr(1).c_str());
                    if (!(file >> code))
                    {
                        message = "no room code in " + args[2].substr(1);
                        return false;
                    }
                }
                if (!SetField(g.code, Window::Join, code, message)) return false;
            }
            else if (what == "password" && args.size() > 2)
            {
                if (!IsOpen())
                {
                    message = "no window of ours is open";
                    return false;
                }
                if (!SetField(g.open == Window::Join ? g.joinPassword : g.password, g.open, args[2], message)) return false;
            }
            else return false;
            const char *names[] = {"no window", "the Host window", "the Join window"};
            message = std::string(names[(int)g.open]) + " open" +
                      (g.pending == Pending::Host ? "; a room opens with the run" : g.pending == Pending::Join ? "; a room is joined with the run" : "");
            return true;
        }
    }
}

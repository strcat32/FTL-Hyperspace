#include "Global.h"
#include "Duels.h"
#include "DuelsConfig.h"
#include "DuelsAi.h"
#include "DuelsConsole.h"
#include "DuelsEnvironment.h"
#include "DuelsLobby.h"
#include "DuelsMatch.h"
#include "DuelsNet.h"
#include "DuelsRelay.h"
#include "DuelsRounds.h"
#include "DuelsStyle.h"
#include "DuelsTrace.h"
#include "DuelsWindow.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <set>

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
            Join,   // the room is joined when the run begins
            Ai      // the match against FTL's AI begins with the run (roadmap 3.6)
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
            bool vsAi = false;                   // the opponent: FTL's AI, not another player (roadmap 3.6)
            int aiShip = 0;                      // the AI's ship: 0 random, else PlayerShips()[aiShip - 1]
            Style::Box playerBox, aiBox, aiShipLess, aiShipMore;
            Style::Box listedBox, recordBox, roundsLess, roundsMore, prepLess, prepMore, stallLess, stallMore, permadeathBox;
            Style::Box hazardBoxes[Environment::KIND_COUNT];
            Style::Box cancel, choose;

            // The Join window's list: the relays asked, the ones filtered out, the room picked (its relay and code), the
            // page; the rows and the relays' check boxes as drawn.
            std::vector<Net::RelayAddress> relays;
            std::set<std::string> hiddenRelays;
            std::string pickedRelay, pickedCode;
            int page = 0;
            std::vector<Style::Box> rows;
            std::vector<std::string> rowKeys;         // each row's relay and code ("relay|code")
            std::vector<Style::Box> relayBoxes;
            Style::Box refresh, pagePrev, pageNext;

            // After CHOOSE SHIP: the room waits for the run, then the relays are tried in order (part 4).
            Pending pending = Pending::None;
            std::string roomName, roomPassword, roomCode;
            bool roomListed = true;
            bool sawHangar = false;
            std::vector<Net::RelayAddress> attemptRelays;
            size_t attemptIndex = 0;
            bool attempting = false;

            // FTL's first message box at a run's start: its choice taken until the box is gone.
            bool inRun = false;
            double closeBoxUntilMs = 0.0, nextBoxKeyMs = 0.0;
            int boxKeys = 0;

            // LOBBY: FTL's main menu asked for, then the room list there.
            bool toMenu = false, listOnMenu = false;
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

        static const float HW = 800.f, HH = 610.f, HX = (1280.f - HW) / 2.f, HY = 55.f;

        // The AI's ship as HOST DUEL's window shows it ("Random", "Kestrel Cruiser A").
        static std::string AiShipBlueprint()
        {
            std::vector<std::string> ships = Ai::PlayerShips();
            return g.aiShip > 0 && g.aiShip <= (int)ships.size() ? ships[g.aiShip - 1] : std::string();
        }

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
            if (g.vsAi)
            {
                // Against FTL's AI (roadmap 3.6): no room; the match begins with the run.
                Log("Lobby: choose a ship; the match against the AI (%s) begins with the run: %s",
                    AiShipBlueprint().empty() ? "a random ship" : AiShipBlueprint().c_str(), message.c_str());
                OpenHangar(Pending::Ai);
                return;
            }
            // The room opens at the first relay of the list that answers (part 4).
            g.attemptRelays = Net::RelayList();
            Log("Lobby: choose a ship; the room '%s' (%s%s) opens with the run, at the first of %u relay(s) that answers: %s",
                g.roomName.c_str(), g.roomListed ? "listed" : "unlisted", g.roomPassword.empty() ? "" : ", with a password",
                (unsigned)g.attemptRelays.size(), message.c_str());
            OpenHangar(Pending::Host);
        }

        static void RenderHost()
        {
            Style::Dialog(HX, HY, HW, HH, "HOST DUEL");
            const GL_Color white = Rgb(255, 255, 255), soft = Rgb(190, 196, 204), light = Rgb(226, 230, 236), gold = Rgb(255, 235, 170);

            // The opponent: another player (a room at a relay), or FTL's AI on this computer (roadmap 3.6).
            float lx = HX + 30.f, lw = 340.f, y = HY + 24.f;
            y += Style::Label(lx, y, "THE OPPONENT") + 14.f;
            CheckAt(g.playerBox, lx, y, !g.vsAi, "Another player (a room at a relay)");
            y += 30.f;
            CheckAt(g.aiBox, lx, y, g.vsAi, "FTL's AI (on this computer, unranked)");
            y += 34.f;
            if (g.vsAi)
            {
                std::vector<std::string> ships = Ai::PlayerShips();
                Text(FONT, lx, y + 6.f, "The AI's ship:", light);
                ButtonAt(g.aiShipLess, lx + 110.f, y, 30.f, 28.f, "<");
                CSurface::GL_SetColor(white);
                std::string shipName = g.aiShip == 0 ? std::string("Random") : Ai::ShipTitle(AiShipBlueprint());
                freetype::easy_printCenter(FONT, lx + 110.f + 30.f + 85.f, y + 6.f, shipName);
                ButtonAt(g.aiShipMore, lx + 110.f + 30.f + 170.f, y, 30.f, 28.f, ">");
            }
            else g.aiShipLess.w = g.aiShipMore.w = 0.f;
            y += 44.f;

            // The room (not for a match against the AI).
            y += Style::Label(lx, y, "THE ROOM") + 14.f;
            const GL_Color labels = g.vsAi ? soft : light;
            Text(FONT, lx, y, "Its title (a few words):", labels);
            RenderField(g.name, lx, y + 20.f, lw);
            y += 64.f;
            Text(FONT, lx, y, "A password (empty: none):", labels);
            RenderField(g.password, lx, y + 20.f, lw);
            y += 66.f;
            CheckAt(g.listedBox, lx, y, !g.listed, "Private: only who has its code can join");
            y += 32.f;
            CheckAt(g.recordBox, lx, y, g.next.record, "Public recording (off: the match is unranked)");
            y += 40.f;
            Paragraph(FONT, lx, y, lw, g.vsAi ? "CHOOSE SHIP opens FTL's hangar. Its START begins the run and the match against "
                                                "the AI, on this computer: no room, nothing over the network."
                                              : "CHOOSE SHIP opens FTL's hangar. Its START begins the run and opens the room. Its "
                                                "code is in the Duels window (DUELS at the top): the other player joins with it, or "
                                                "finds a room that isn't private in JOIN DUEL's list.", soft);

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
            int ships = (int)Ai::PlayerShips().size();
            if (g.playerBox.Contains(x, y)) g.vsAi = false;
            else if (g.aiBox.Contains(x, y)) g.vsAi = true;
            else if (g.aiShipLess.Contains(x, y)) g.aiShip = (g.aiShip + ships) % (ships + 1);
            else if (g.aiShipMore.Contains(x, y)) g.aiShip = (g.aiShip + 1) % (ships + 1);
            else if (g.name.box.Contains(x, y)) Focus(&g.name);
            else if (g.password.box.Contains(x, y)) Focus(&g.password);
            else if (g.listedBox.Contains(x, y)) g.listed = !g.listed;
            else if (g.recordBox.Contains(x, y)) n.record = !n.record;
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
        // The Join window (parts 3 and 4): the open rooms of every relay on the list (a filter by relay; a click on a
        // room shows it on the right), or a room by its code, tried at every relay
        // ---------------------------------------------------------------------------------------------------------

        static const float JW = 960.f, JH = 580.f, JX = (1280.f - JW) / 2.f, JY = 76.f;
        static const int ROWS = 11;
        static const float ROW_H = 24.f;

        static std::string Upper(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::toupper(c); });
            return text;
        }

        static std::string Key(const Net::FoundRoom &room)
        {
            return room.relay.name + "|" + room.code;
        }

        // The text cut to fit a width ("Captain_Kaz..").
        static std::string Fit(int font, const std::string &text, float width)
        {
            if ((float)freetype::easy_measureWidth(font, text) <= width) return text;
            std::string cut = text;
            while (!cut.empty() && (float)freetype::easy_measureWidth(font, cut + "..") > width) cut.pop_back();
            return cut + "..";
        }

        // Where the list's rows are: under the title, the relays' line and the list's head row.
        static const float LIST_X = JX + 30.f, LIST_W = 580.f, LIST_Y = JY + 94.f;

        static Style::Box RowBox(int index)
        {
            Style::Box box;
            box.x = LIST_X + 3.f;
            box.y = LIST_Y + 4.f + ROW_H * (index + 1);
            box.w = LIST_W - 6.f;
            box.h = ROW_H;
            return box;
        }

        // The found rooms the relay filter lets through, in the order they came.
        static std::vector<Net::FoundRoom> Shown()
        {
            std::vector<Net::FoundRoom> shown;
            for (const Net::FoundRoom &room : Net::Search().rooms)
            {
                if (!g.hiddenRelays.count(room.relay.name)) shown.push_back(room);
            }
            return shown;
        }

        static bool Picked(Net::FoundRoom &picked)
        {
            for (const Net::FoundRoom &room : Shown())
            {
                if (room.relay.name == g.pickedRelay && room.code == g.pickedCode)
                {
                    picked = room;
                    return true;
                }
            }
            return false;
        }

        static void Refresh()
        {
            g.relays = Net::RelayList();
            Net::SearchRooms(g.relays);
            g.page = 0;
            Log("Lobby: the room list asks %u relay(s)", (unsigned)g.relays.size());
        }

        void OpenJoin()
        {
            g.code.Make(6, false);
            g.joinPassword.Make(PASSWORD_MAX, true);
            g.code.input->SetText("");
            g.joinPassword.input->SetText("");
            g.message.clear();
            g.pickedRelay.clear();
            g.pickedCode.clear();
            g.open = Window::Join;
            Focus(nullptr);
            Refresh();
            Log("Lobby: the Join window");
        }

        static void ChooseJoin()
        {
            std::string code = Upper(g.code.Text());
            g.roomPassword = g.joinPassword.Text();
            if (!code.empty())
            {
                // A room by its code: every relay of the list is asked for it in turn.
                if (!Relay::Client::IsRoomCode(code))
                {
                    g.message = "A room's code has 6 letters and digits (the host's Duels window shows it).";
                    return;
                }
                g.roomCode = code;
                g.attemptRelays = Net::RelayList();
                Log("Lobby: choose a ship; the room %s is joined with the run (looked for at %u relay(s))%s", code.c_str(),
                    (unsigned)g.attemptRelays.size(), g.roomPassword.empty() ? "" : " (with a password)");
                OpenHangar(Pending::Join);
                return;
            }
            Net::FoundRoom room;
            if (!Picked(room))
            {
                g.message = "Pick a room in the list, or type a room's code.";
                return;
            }
            if (room.version != Net::Version())
            {
                g.message = "That room's game is version " + room.version + ", yours " + Net::Version() + ": a duel needs the same.";
                return;
            }
            if (room.password && g.roomPassword.empty())
            {
                g.message = "That room needs its password.";
                Focus(&g.joinPassword);
                return;
            }
            g.roomCode = room.code;
            g.attemptRelays = {room.relay};
            Log("Lobby: choose a ship; the room %s ('%s', %s's) at %s is joined with the run", room.code.c_str(), room.roomName.c_str(),
                room.hostName.c_str(), room.relay.name.c_str());
            OpenHangar(Pending::Join);
        }

        static void RenderJoin()
        {
            Style::Dialog(JX, JY, JW, JH, "JOIN DUEL");
            const GL_Color soft = Rgb(190, 196, 204), light = Rgb(226, 230, 236), gold = Rgb(255, 235, 170), white = Rgb(255, 255, 255),
                           red = Rgb(255, 140, 120);
            const Net::RoomSearch &search = Net::Search();

            // The list's head: its title, REFRESH, and the relays as a filter.
            float lx = JX + 30.f, lw = 580.f, y = JY + 24.f;
            Style::Label(lx, y, "OPEN ROOMS");
            ButtonAt(g.refresh, lx + lw - 110.f, y - 4.f, 110.f, 28.f, "REFRESH", !search.Busy());
            y += 36.f;
            g.relayBoxes.resize(g.relays.size());
            float rx = lx;
            for (size_t i = 0; i < g.relays.size(); ++i)
            {
                const std::string &name = g.relays[i].name;
                CheckAt(g.relayBoxes[i], rx, y, !g.hiddenRelays.count(name), name);
                rx += g.relayBoxes[i].w + 18.f;
            }
            y += 34.f;

            // The rooms: a head row, then a page of them. The code (the relay's, unique) and the title (the host's) have a
            // column each (AR).
            const float cCode = lx + 10.f, cName = lx + 82.f, cHost = lx + 258.f, cRelay = lx + 380.f, cLock = lx + 498.f;
            float listY = LIST_Y, listH = ROW_H * (ROWS + 1) + 8.f;
            Style::Field(lx, listY, lw, listH, false);
            Text(FONT, cCode, listY + 6.f, "CODE", soft);
            Text(FONT, cName, listY + 6.f, "TITLE", soft);
            Text(FONT, cHost, listY + 6.f, "HOST", soft);
            Text(FONT, cRelay, listY + 6.f, "RELAY", soft);
            std::vector<Net::FoundRoom> shown = Shown();
            int pages = std::max(1, ((int)shown.size() + ROWS - 1) / ROWS);
            g.page = std::max(0, std::min(g.page, pages - 1));
            g.rows.clear();
            g.rowKeys.clear();
            for (int i = 0; i < ROWS && g.page * ROWS + i < (int)shown.size(); ++i)
            {
                const Net::FoundRoom &room = shown[g.page * ROWS + i];
                Style::Box box = RowBox(i);
                bool picked = room.relay.name == g.pickedRelay && room.code == g.pickedCode;
                if (picked) CSurface::GL_DrawRect(box.x, box.y, box.w, box.h, Rgb(255, 230, 94, 0.28f));
                else if (Hover(box)) CSurface::GL_DrawRect(box.x, box.y, box.w, box.h, Rgb(255, 255, 255, 0.08f));
                bool sameVersion = room.version == Net::Version();
                float ty = box.y + 4.f;
                Text(FONT, cCode, ty, Fit(FONT, room.code, 66.f), sameVersion ? light : soft);
                Text(FONT, cName, ty, Fit(FONT, room.roomName.empty() ? "(no title)" : room.roomName, 170.f), sameVersion ? white : soft);
                Text(FONT, cHost, ty, Fit(FONT, Match::ScreenName(room.hostName), 116.f), sameVersion ? light : soft);
                Text(FONT, cRelay, ty, Fit(FONT, room.relay.name, 110.f), soft);
                if (!sameVersion) Text(FONT, cLock, ty, "v" + Fit(FONT, room.version, 56.f), red);
                else if (room.password) Text(FONT, cLock, ty, "PASSWORD", gold);
                g.rows.push_back(box);
                g.rowKeys.push_back(Key(room));
            }

            // Under the list: how the search goes, and the pages.
            float sy = listY + listH + 8.f;
            std::string status;
            if (search.Busy()) status = "Asking " + std::to_string(search.relays - search.answered - search.failed) + " of " +
                                        std::to_string(search.relays) + " relays...";
            else if (shown.empty()) status = "No open rooms. A private room is joined by its code, below.";
            else status = std::to_string(shown.size()) + (shown.size() == 1 ? " room" : " rooms") + " open.";
            Text(FONT, lx, sy, Fit(FONT, status, lw - 170.f), light);
            if (!search.errors.empty()) Text(FONT, lx, sy + 16.f, Fit(FONT, search.errors.front(), lw - 170.f), red);
            ButtonAt(g.pagePrev, lx + lw - 160.f, sy - 2.f, 36.f, 26.f, "<", g.page > 0);
            CSurface::GL_SetColor(light);
            freetype::easy_printCenter(FONT, lx + lw - 80.f, sy + 3.f, std::to_string(g.page + 1) + " / " + std::to_string(pages));
            ButtonAt(g.pageNext, lx + lw - 36.f, sy - 2.f, 36.f, 26.f, ">", g.page + 1 < pages);

            // The room picked, on the right.
            float dx = JX + 640.f, dw = JW - 640.f - 30.f, dy = JY + 24.f;
            dy += Style::Label(dx, dy, "THE ROOM") + 14.f;
            Net::FoundRoom room;
            if (Picked(room))
            {
                dy += Paragraph(TEXT, dx, dy, dw, room.roomName.empty() ? "(no title)" : room.roomName, white) + 8.f;
                auto line = [&](const std::string &label, const std::string &value, const GL_Color &colour)
                {
                    Text(FONT, dx, dy, label, soft);
                    dy += Paragraph(FONT, dx + 84.f, dy, dw - 84.f, value, colour) + 4.f;
                };
                line("Host", room.hostName, light);
                line("Relay", room.relay.name, light);
                line("Code", room.code, light);
                bool sameVersion = room.version == Net::Version();
                line("Version", room.version + (sameVersion ? "" : " (yours: " + Net::Version() + ")"), sameVersion ? light : red);
                line("Password", room.password ? "needed: type it below" : "none", room.password ? gold : light);
                dy += 6.f;
                Paragraph(FONT, dx, dy, dw, "Whether it is ranked, needs Steam or runs debug mode, and its match settings come with "
                                            "the relay's next version.", soft);
            }
            else Paragraph(FONT, dx, dy, dw, "A click on a room in the list shows it here.", soft);

            // A room by its code (a private one, say), and the password for either.
            float by = JY + JH - 58.f, fy = by - 72.f;
            Text(FONT, lx, fy, "Or a room by its code:", light);
            RenderField(g.code, lx, fy + 18.f, 130.f);
            Text(FONT, lx + 160.f, fy, "The room's password, if it has one:", light);
            RenderField(g.joinPassword, lx + 160.f, fy + 18.f, 260.f);
            if (!g.message.empty()) Paragraph(FONT, dx, by - 40.f, dw, g.message, red);
            ButtonAt(g.cancel, JX + JW - 30.f - 190.f - 12.f - 130.f, by, 130.f, 34.f, "CANCEL");
            ButtonAt(g.choose, JX + JW - 30.f - 190.f, by, 190.f, 34.f, "CHOOSE SHIP");
        }

        static void ClickJoin(int x, int y)
        {
            for (size_t i = 0; i < g.rows.size(); ++i)
            {
                if (!g.rows[i].Contains(x, y)) continue;
                size_t bar = g.rowKeys[i].find('|');
                g.pickedRelay = g.rowKeys[i].substr(0, bar);
                g.pickedCode = g.rowKeys[i].substr(bar + 1);
                if (g.code.input) g.code.input->SetText("");   // the list's room, not a typed code
                g.message.clear();
                Log("Lobby: picked the room %s at %s", g.pickedCode.c_str(), g.pickedRelay.c_str());
                return;
            }
            for (size_t i = 0; i < g.relayBoxes.size() && i < g.relays.size(); ++i)
            {
                if (!g.relayBoxes[i].Contains(x, y)) continue;
                const std::string &name = g.relays[i].name;
                if (g.hiddenRelays.count(name)) g.hiddenRelays.erase(name);
                else g.hiddenRelays.insert(name);
                g.page = 0;
                return;
            }
            if (g.code.box.Contains(x, y)) Focus(&g.code);
            else if (g.joinPassword.box.Contains(x, y)) Focus(&g.joinPassword);
            else if (g.refresh.Contains(x, y) && !Net::Search().Busy()) Refresh();
            else if (g.pagePrev.Contains(x, y) && g.page > 0) --g.page;
            else if (g.pageNext.Contains(x, y)) ++g.page;   // kept to the pages there are when drawn
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

        // The room at the relay of this attempt: opened (the host) or joined (the guest), as "host relay" and "join" do.
        // A relay that can't be reached at all is skipped.
        static bool StartAttempt()
        {
            while (g.attemptIndex < g.attemptRelays.size())
            {
                const Net::RelayAddress &relay = g.attemptRelays[g.attemptIndex];
                std::string message;
                bool ok = g.pending == Pending::Host
                              ? Match::HostRelay(relay.server, relay.port, g.roomName, g.roomPassword, g.roomListed, message)
                              : Match::JoinRelay(relay.server, relay.port, g.roomCode, g.roomPassword, message);
                Log("Lobby: %s at %s: %s", g.pending == Pending::Host ? "the room" : "joining", relay.name.c_str(), message.c_str());
                if (ok) return true;
                ++g.attemptIndex;
            }
            return false;
        }

        // How the attempt goes: 1 done (the room is open, or joined), 0 still going, -1 failed; `retry` when the next
        // relay may do better (no answer, the relay full or busy, and for a code: no such room there).
        static int AttemptState(bool &retry, std::string &why)
        {
            if (g.pending == Pending::Host && Net::GetPhase() == Net::Phase::Hosting && !Net::RelayCode().empty()) return 1;
            if (g.pending == Pending::Join && Net::IsConnected()) return 1;
            if (Net::GetPhase() != Net::Phase::Idle) return 0;
            std::string text;
            int error = Net::LastRelayError(&text);
            why = text.empty() ? "the connection failed" : text;
            retry = error == Relay::Event::NO_ANSWER || error == 4 || error == 6 || (g.pending == Pending::Join && error == 2);
            return -1;
        }

        static void Finish(bool ok, const std::string &why)
        {
            bool host = g.pending == Pending::Host;
            std::string relay = g.attemptIndex < g.attemptRelays.size() ? g.attemptRelays[g.attemptIndex].name : std::string("-");
            g.pending = Pending::None;
            g.attempting = false;
            if (ok)
            {
                std::string text = host ? "Room " + Net::RelayCode() + " is open at " + relay + ": waiting for a guest"
                                        : "Joined " + Net::PeerName() + "'s room at " + relay;
                Log("Lobby: %s", text.c_str());
                Console::Feed(text);
            }
            else
            {
                Log("Lobby: %s: %s", host ? "no room opened" : "not joined", why.c_str());
                Console::Feed((host ? "No room opened: " : "Not joined: ") + why);
            }
            ::Duels::Window::Open();   // the room's code, the opponent, or why not
        }

        void ToLobby()
        {
            Match::Leave();
            g.toMenu = true;
            g.listOnMenu = true;
            Log("Lobby: to the lobby: the duel left, FTL's main menu, then the room list");
        }

        bool FirstBoxAnswered()
        {
            return g.inRun && g.boxKeys > 0;
        }

        bool TakeMenuRequest()
        {
            bool asked = g.toMenu;
            g.toMenu = false;
            return asked;
        }

        void OnFrame()
        {
            CApp *app = G_->GetCApp();
            if (!app) return;
            if (g.listOnMenu && app->menu.bOpen && !app->menu.shipBuilder.bOpen)
            {
                g.listOnMenu = false;
                OpenJoin();
            }
            bool inRun = InRun();
            if (inRun && !g.inRun)
            {
                // A run begins.
                g.closeBoxUntilMs = WallMs() + 10000.0;
                g.nextBoxKeyMs = 0.0;
                g.boxKeys = 0;
            }
            g.inRun = inRun;
            if (inRun && WallMs() < g.closeBoxUntilMs)
            {
                // FTL's first message box (its story): the tutorial box explained the duel instead. Its only choice, as
                // the player's key, again until the box is gone. (FTL answers a message box only while its game is
                // paused: a match against the AI has no pause once the box is gone, DuelsAi.cpp.)
                ChoiceBox &box = G_->GetWorld()->commandGui->choiceBox;
                if (box.bOpen && box.choices.size() == 1 && WallMs() >= g.nextBoxKeyMs)
                {
                    box.KeyDown(SDLK_1);
                    g.nextBoxKeyMs = WallMs() + 300.0;
                    ++g.boxKeys;
                }
                else if (!box.bOpen && g.boxKeys > 0)
                {
                    Log("Lobby: FTL's first message box closed (%d key%s)", g.boxKeys, g.boxKeys == 1 ? "" : "s");
                    g.closeBoxUntilMs = 0.0;
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
                    g.attempting = false;
                    Log("Lobby: back from the hangar: no room");
                }
                return;
            }
            if (!inRun) return;
            if (g.pending == Pending::Ai)
            {
                g.pending = Pending::None;
                Ai::Start(AiShipBlueprint());
                Console::Feed("A match against " + Ai::Name() + " (FTL's AI, on this computer)");
                return;
            }
            if (!g.attempting)
            {
                g.attempting = true;
                g.attemptIndex = 0;
                if (!StartAttempt()) Finish(false, "no relay of the list can be reached");
                return;
            }
            bool retry = false;
            std::string why;
            int state = AttemptState(retry, why);
            if (state == 0) return;
            if (state > 0)
            {
                Finish(true, "");
                return;
            }
            if (retry && g.attemptIndex + 1 < g.attemptRelays.size())
            {
                Log("Lobby: %s: %s; the next relay", g.attemptRelays[g.attemptIndex].name.c_str(), why.c_str());
                ++g.attemptIndex;
                if (!StartAttempt()) Finish(false, why);
                return;
            }
            Finish(false, why);
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
            else if (what == "refresh" && g.open == Window::Join) Refresh();
            else if (what == "ai" && args.size() > 2 && g.open == Window::Host)
            {
                // ai on|off: the opponent, FTL's AI or another player.
                g.vsAi = args[2] == "on";
            }
            else if (what == "aiship" && args.size() > 2 && g.open == Window::Host)
            {
                // aiship <blueprint>|random: the AI's ship.
                std::vector<std::string> ships = Ai::PlayerShips();
                auto found = std::find(ships.begin(), ships.end(), Upper(args[2]));   // the verb's words come in lower case
                if (args[2] != "random" && found == ships.end())
                {
                    message = "no player ship " + Upper(args[2]) + " in the hangar (" + std::to_string(ships.size()) + ":";
                    for (const std::string &ship : ships) message += " " + ship;
                    message += ")";
                    return false;
                }
                g.aiShip = args[2] == "random" ? 0 : (int)(found - ships.begin()) + 1;
            }
            else if (what == "pick" && args.size() > 2 && g.open == Window::Join)
            {
                // pick <code|@file>: a click on that room's row in the list (on its page).
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
                std::vector<Net::FoundRoom> shown = Shown();
                for (size_t i = 0; i < shown.size(); ++i)
                {
                    if (shown[i].code != Upper(code)) continue;
                    // Its page, then a click on its row there (the rows are laid out as the next frame draws them).
                    g.page = (int)i / ROWS;
                    Style::Box box = RowBox((int)i % ROWS);
                    g.rows.assign(1, box);
                    g.rowKeys.assign(1, Key(shown[i]));
                    int x = (int)(box.x + box.w / 2.f), y = (int)(box.y + box.h / 2.f);
                    MouseClick(x, y);
                    message = "picked the room " + shown[i].code + " at " + shown[i].relay.name + " (clicked at " + std::to_string(x) +
                              "," + std::to_string(y) + ")";
                    return true;
                }
                message = "no room " + code + " in the list (" + std::to_string(shown.size()) + " shown)";
                return false;
            }
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

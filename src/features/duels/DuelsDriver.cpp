#include "Global.h"
#include "CommandConsole.h"
#include "Duels.h"
#include "DuelsConfig.h"
#include "DuelsConsole.h"
#include "DuelsDemo.h"
#include "DuelsFair.h"
#include "DuelsHud.h"
#include "DuelsMatch.h"
#include "DuelsMatchUi.h"
#include "DuelsReplayUi.h"
#include "DuelsMenu.h"
#include "DuelsNet.h"
#include "DuelsRelay.h"
#include "DuelsRounds.h"
#include "DuelsScreen.h"
#include "DuelsShipControl.h"
#include "DuelsTrace.h"
#include "DuelsTune.h"
#include "DuelsAccount.h"
#include "DuelsView.h"
#include "DuelsWindow.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

namespace Duels
{
    static bool ParseOnOff(const Command &cmd, size_t index, bool &out)
    {
        if (ArgIs(cmd, index, "on")) { out = true; return true; }
        if (ArgIs(cmd, index, "off")) { out = false; return true; }
        return false;
    }

    static bool ParseShipId(const Command &cmd, size_t index, int &shipId, std::string &message)
    {
        if (!ArgInt(cmd, index, shipId) || shipId < 0 || shipId > 1)
        {
            message = "ship id must be 0 (player) or 1 (enemy)";
            return false;
        }
        return true;
    }

    static std::string DescribeState()
    {
        const State &state = GetState();
        std::ostringstream out;
        out << "nopause=" << (state.noPause ? "on" : "off")
            << " ai0=" << (state.aiOff[0] ? "off" : "on")
            << " ai1=" << (state.aiOff[1] ? "off" : "on")
            << " trace=" << (state.trace ? "on" : "off")
            << " script=" << (state.scriptStartMs >= 0.0 ? "running" : "idle")
            << " enemy=" << (G_->GetShipManager(1) ? "present" : "none");
        return out.str();
    }

    // Moves the mouse over the center of every room of both ships, as they are drawn now, through the game's own mouse
    // handling (CommandGui::MouseMove), and checks what the game makes of it: the opponent's room it would target, and
    // the room of our ship under the mouse for crew orders and doors (CrewControl) and for clicks
    // (GetWorldCoordinates). "full" starts at the window's pixels and goes through CApp::TranslateMouse (window size,
    // scaling) first.
    static bool AimCheck(bool full, std::string &message)
    {
        CApp *app = G_->GetCApp();
        CommandGui *gui = app ? app->gui : nullptr;
        ShipManager *enemy = G_->GetShipManager(1);
        ShipManager *own = G_->GetShipManager(0);
        if (!gui || !enemy || !own)
        {
            message = "needs an enemy ship";
            return false;
        }
        CombatControl &combat = gui->combatControl;
        Pointf savedMouse = combat.lastMouse;

        // The screen pixel under a point, as the game gets it; with `full`, from the window pixel that
        // CApp::TranslateMouse maps to it (the inverse of its arithmetic).
        auto moveTo = [&](float x, float y) {
            Point at((int)std::floor(x), (int)std::floor(y));
            if (full)
            {
                int windowX = (int)std::floor((at.x + 0.5f + app->modifier_x) / app->mouseModifier_x + app->x_bar);
                int windowY = (int)std::floor((at.y + 0.5f + app->modifier_y) / app->mouseModifier_y + app->y_bar);
                at = app->TranslateMouse(windowX, windowY);
            }
            gui->MouseMove(at.x, at.y);
            return at;
        };

        int rooms = (int)enemy->ship.vRoomList.size();
        int hits = 0;
        std::ostringstream misses;
        for (int room = 0; room < rooms; ++room)
        {
            Pointf center = enemy->GetRoomCenter(room);
            float x, y;
            if (!View::ShipToScreen(1, center.x, center.y, x, y)) continue;
            Point at = moveTo(x, y);
            if (combat.selectedRoom == room) ++hits;
            else misses << " " << room << "->" << combat.selectedRoom << "@" << at.x << "," << at.y;
        }

        int ownRooms = (int)own->ship.vRoomList.size();
        int ownHits = 0;
        std::ostringstream ownMisses;
        for (int room = 0; room < ownRooms; ++room)
        {
            Pointf center = own->GetRoomCenter(room);
            float x, y;
            if (!View::ShipToScreen(0, center.x, center.y, x, y)) continue;
            Point at = moveTo(x, y);
            Point mouse = gui->crewControl.worldCurrentMouse;
            Point click = gui->GetWorldCoordinates(at, false);
            int underMouse = own->ship.GetSelectedRoomId(mouse.x, mouse.y, true);
            int underClick = own->ship.GetSelectedRoomId(click.x, click.y, true);
            if (underMouse == room && underClick == room) ++ownHits;
            else ownMisses << " " << room << "->" << underMouse << "/" << underClick << "@" << at.x << "," << at.y;
        }
        gui->MouseMove((int)savedMouse.x, (int)savedMouse.y);

        std::ostringstream out;
        out << (full ? "aimcheck (window pixels): " : "aimcheck: ") << "opponent " << hits << "/" << rooms << " rooms"
            << (hits == rooms ? "" : " (misses:" + misses.str() + ")") << ", ours " << ownHits << "/" << ownRooms << " rooms"
            << (ownHits == ownRooms ? "" : " (misses:" + ownMisses.str() + ")") << " | " << View::Describe();
        message = out.str();
        Log("%s", message.c_str());
        return hits == rooms && ownHits == ownRooms;
    }

    // The relay server for "host relay" and "join relay" ("relay <server>[:port]").
    static std::string g_relayServer;
    static bool g_relayChosen = false;   // the relay command picked it (it comes first for the menu's rooms)
    static int g_relayPort = Relay::DEFAULT_PORT;

    // The options of a relay room after "host relay [server]": name <room title...>, password <password>, unlisted (or
    // private: not in the room list, roadmap AR).
    static bool IsRoomOption(const std::string &word)
    {
        return word == "name" || word == "password" || word == "unlisted" || word == "private";
    }

    static bool RoomOptions(const Command &cmd, size_t from, std::string &roomName, std::string &password, bool &listed,
                            std::string &message)
    {
        listed = true;
        for (size_t i = from; i < cmd.raw.size(); ++i)
        {
            const std::string &word = cmd.args[i];
            if (word == "unlisted" || word == "private")
            {
                listed = false;
            }
            else if (word == "password" && i + 1 < cmd.raw.size())
            {
                password = cmd.raw[++i];
            }
            else if (word == "name" && i + 1 < cmd.raw.size())
            {
                roomName.clear();
                while (i + 1 < cmd.raw.size() && !IsRoomOption(cmd.args[i + 1])) roomName += (roomName.empty() ? "" : " ") + cmd.raw[++i];
            }
            else
            {
                message = "usage: host relay [server[:port]] [name <room name>] [password <password>] [unlisted]";
                return false;
            }
        }
        return true;
    }

    // "name", "name:port", "1.2.3.4:port" or "[ipv6]:port".
    static bool ParseServer(const std::string &text, std::string &server, int &port)
    {
        port = Relay::DEFAULT_PORT;
        std::string portText;
        if (!text.empty() && text[0] == '[')
        {
            size_t close = text.find(']');
            if (close == std::string::npos) return false;
            server = text.substr(1, close - 1);
            if (close + 1 < text.size())
            {
                if (text[close + 1] != ':') return false;
                portText = text.substr(close + 2);
            }
        }
        else if (std::count(text.begin(), text.end(), ':') == 1)
        {
            size_t colon = text.find(':');
            server = text.substr(0, colon);
            portText = text.substr(colon + 1);
        }
        else
        {
            server = text;   // a host name, IPv4, or an IPv6 address without a port
        }
        if (!portText.empty())
        {
            char *end = nullptr;
            long value = std::strtol(portText.c_str(), &end, 10);
            if (*end != '\0' || value <= 0 || value > 65535) return false;
            port = (int)value;
        }
        return !server.empty();
    }

    // Test verb "mouse": FTL's mouse held at a point for a while.
    struct HeldMouse
    {
        int x = 0, y = 0;
        double untilMs = 0.0;
    };

    static HeldMouse g_heldMouse;

    void HeldMouseOnFrame()
    {
        if (WallMs() >= g_heldMouse.untilMs) return;
        MouseControl *mouse = G_->GetMouseControl();
        CApp *app = G_->GetCApp();
        if (mouse) mouse->position = Point(g_heldMouse.x, g_heldMouse.y);
        // In the main menu FTL's menu takes the mouse (ours over it); the game's interface isn't there.
        if (app && app->menu.bOpen) app->menu.MouseMove(g_heldMouse.x, g_heldMouse.y);
        else if (app && app->gui) app->gui->MouseMove(g_heldMouse.x, g_heldMouse.y);
    }

    // Commands any player may use. The others change ships or automate play, so they need debug mode.
    static bool IsPlayerVerb(const std::string &verb)
    {
        static const std::set<std::string> verbs = {
            "console", "debug", "host", "join", "leave", "lobby", "name", "net", "netstats", "note", "quit", "relay", "say",
            "screenshot", "status", "stop", "trace", "tracepower", "version", "window", "xp",
            "match", "ready", "forfeit", "concede", "draw", "ban", "pick", "timeout", "demo", "replay", "tune", "preset", "account",
            "master"};
        return verbs.count(verb) != 0;
    }

    // A test scenario without debug mode (@nodebug, a ranked match's test: debug mode makes a match unranked) may still
    // drive the menus as a player's mouse does: these only click and look.
    static bool IsUiVerb(const std::string &verb)
    {
        return verb == "menu" || verb == "click" || verb == "describe";
    }

    void EnableDebug(const char *why)
    {
        State &state = GetState();
        if (state.debug) return;
        state.debug = true;
        Match::SetDebug(true);
        Log("Debug mode on (%s): test commands work, and the duels of this game are debug duels", why);
    }

    // The first relay in duels.cfg, until "relay" picks another.
    static void UseConfiguredRelay()
    {
        if (!g_relayServer.empty() || Config::Relays().empty()) return;
        std::string server;
        int port;
        if (ParseServer(Config::Relays()[0], server, port))
        {
            g_relayServer = server;
            g_relayPort = port;
        }
    }

    // The relays the menu's HOST DUEL and JOIN DUEL use (roadmap 3.5, part 4), in order: one the relay command chose
    // (tests: the relay on this computer), then duels.cfg's and the project's public relay; each once.
    std::vector<Net::RelayAddress> Net::RelayList()
    {
        std::vector<Net::RelayAddress> list;
        auto add = [&](const std::string &text)
        {
            std::string server;
            int port;
            if (!ParseServer(text, server, port)) return;
            for (const Net::RelayAddress &known : list)
            {
                if (known.server == server && known.port == port) return;
            }
            Net::RelayAddress address;
            address.server = server;
            address.port = (uint16_t)port;
            address.name = Config::IsPublicRelay(server) && port == Relay::DEFAULT_PORT ? "public relay" : text;
            list.push_back(address);
        };
        if (g_relayChosen && !g_relayServer.empty())
        {
            bool v6 = g_relayServer.find(':') != std::string::npos;
            add((v6 ? "[" + g_relayServer + "]" : g_relayServer) + ":" + std::to_string(g_relayPort));
        }
        for (const std::string &relay : Config::Relays()) add(relay);
        return list;
    }

    bool Execute(const Command &cmd, std::string &message)
    {
        if (cmd.args.empty())
        {
            message = "empty command";
            return false;
        }

        State &state = GetState();
        const std::string &verb = cmd.args[0];

        if (!state.debug && !IsPlayerVerb(verb) && !(AutotestActive() && IsUiVerb(verb)))
        {
            message = "'" + verb + "' is a test command: it needs debug mode (debug on)";
            return false;
        }
        if (verb == "debug")
        {
            if (ArgIs(cmd, 1, "on"))
            {
                if (!state.debug && Net::IsConnected())
                {
                    message = "debug mode can't start in the middle of a duel: leave it first";
                    return false;
                }
                EnableDebug("debug on");
            }
            else if (ArgIs(cmd, 1, "off") && state.debug)
            {
                message = "debug mode stays on until the game restarts";
                return false;
            }
            else if (cmd.args.size() > 1 && !ArgIs(cmd, 1, "off"))
            {
                message = "usage: debug [on|off]";
                return false;
            }
            message = state.debug ? "debug mode on: test commands work, and the duels of this game are debug duels "
                                    "(until the game restarts)"
                                  : "debug mode off";
            return true;
        }

        if (verb == "status")
        {
            message = DescribeState();
            return true;
        }
        if (verb == "version")
        {
            message = std::string("FTL:Duels ") + VERSION;
            return true;
        }
        if (verb == "nopause")
        {
            if (!ParseOnOff(cmd, 1, state.noPause)) { message = "usage: nopause on|off"; return false; }
            message = std::string("no-pause ") + (state.noPause ? "on" : "off");
            return true;
        }
        if (verb == "trace")
        {
            if (!ParseOnOff(cmd, 1, state.trace)) { message = "usage: trace on|off"; return false; }
            message = std::string("trace ") + (state.trace ? "on" : "off");
            return true;
        }
        if (verb == "tracepower")
        {
            if (!ParseOnOff(cmd, 1, state.tracePower)) { message = "usage: tracepower on|off"; return false; }
            message = std::string("power tracing ") + (state.tracePower ? "on" : "off");
            return true;
        }
        if (verb == "ai")
        {
            int shipId;
            bool on;
            if (!ParseShipId(cmd, 1, shipId, message)) return false;
            if (!ParseOnOff(cmd, 2, on)) { message = "usage: ai <ship> on|off"; return false; }
            state.aiOff[shipId] = !on;
            message = "ship " + std::to_string(shipId) + " AI " + (on ? "on" : "off");
            return true;
        }
        if (verb == "script")
        {
            if (cmd.raw.size() < 2) { message = "usage: script <file>"; return false; }
            return StartScript(cmd.raw[1], message);  // original spelling: paths are case-sensitive on Linux
        }
        if (verb == "stop")
        {
            state.scriptStartMs = -1.0;
            message = "script stopped";
            return true;
        }
        if (verb == "note")
        {
            // Free text marker for the log, e.g. "note opening upgrade screen now".
            message = cmd.text;
            return true;
        }

        // Enemy window and console
        if (verb == "view")
        {
            if (ArgIs(cmd, 1, "auto")) View::SetMode(View::Mode::Auto);
            else if (ArgIs(cmd, 1, "fit"))
            {
                // Any enemy, for checks in single player; a spawned player ship gets its shields placed as in a duel.
                View::SetMode(View::Mode::Fit);
                View::UsePlayerShieldPosition(G_->GetShipManager(1));
            }
            else if (ArgIs(cmd, 1, "off")) View::SetMode(View::Mode::Off);
            else if (ArgIs(cmd, 1, "equal") || ArgIs(cmd, 1, "hires") || ArgIs(cmd, 1, "icons"))
            {
                bool on;
                if (!ParseOnOff(cmd, 2, on)) { message = "usage: view equal|hires|icons on|off"; return false; }
                if (ArgIs(cmd, 1, "equal")) View::SetEqualSize(on);
                else if (ArgIs(cmd, 1, "hires")) Screen::SetHiRes(on);
                else View::SetUnmirrorIcons(on);
            }
            else if (cmd.args.size() > 1) { message = "usage: view [auto|fit|off] | view equal|hires on|off"; return false; }
            // The layout follows at the next frame.
            View::OnFrame();
            message = View::Describe() + " | " + Screen::Describe();
            Log("%s", message.c_str());
            return true;
        }
        if (verb == "aimcheck") return AimCheck(ArgIs(cmd, 1, "full"), message);
        if (verb == "console")
        {
            // console [text]: opens the console with that text typed in (for screenshots and tests).
            CApp *app = G_->GetCApp();
            std::string text = cmd.text.substr(cmd.text.find("console") + 7);
            size_t start = text.find_first_not_of(' ');
            text = start == std::string::npos ? "" : text.substr(start);
            if (!app || !app->gui || !Console::OpenWith(app->gui, text))
            {
                message = "the console can't open now";
                return false;
            }
            message = "console open";
            return true;
        }

        // Network duel
        if (verb == "xp")
        {
            // xp [factor]: how fast crew skills grow in a duel (the host's setting counts; 1 is FTL's own pace).
            if (cmd.raw.size() < 2)
            {
                message = Match::CrewXpStatus();
                return true;
            }
            return Match::SetCrewXp((float)std::atof(cmd.raw[1].c_str()), message);
        }
        if (verb == "name")
        {
            // name <player name>: saved in duels.cfg (the rest of the line: a name may have spaces, as the menu's
            // name prompt allows). A test scenario leaves the file alone unless it says @config.
            std::string name = cmd.text.substr(cmd.text.find("name") + 4);
            size_t start = name.find_first_not_of(' '), end = name.find_last_not_of(' ');
            name = start == std::string::npos ? "" : name.substr(start, end - start + 1);
            if (name.empty()) { message = "usage: name <player name>"; return false; }
            Match::SetPlayerName(name);
            if (SettingsFromConfig()) Config::SavePlayerName(Match::PlayerName());
            message = "player name " + Match::PlayerName();
            return true;
        }
        if (verb == "relay" || verb == "host" || verb == "join" || verb == "lobby") UseConfiguredRelay();
        if (verb == "relay")
        {
            // relay [server[:port]]: the relay server for "host relay" and "join relay".
            if (cmd.raw.size() > 1)
            {
                std::string server;
                int port;
                if (!ParseServer(cmd.raw[1], server, port))
                {
                    message = "usage: relay <server>[:port]";
                    return false;
                }
                g_relayServer = server;
                g_relayPort = port;
                g_relayChosen = true;
            }
            message = g_relayServer.empty() ? "no relay server set (relay <server>[:port])"
                                            : "relay server " + g_relayServer + " port " + std::to_string(g_relayPort);
            return true;
        }
        if (verb == "host" && ArgIs(cmd, 1, "relay"))
        {
            // host relay [server[:port]] [name <room name>] [password <password>] [unlisted]: a room at the relay; its
            // code goes to the other player, or they find it in the relay's room list (lobby) unless it is unlisted.
            std::string server = g_relayServer;
            int port = g_relayPort;
            size_t next = 2;
            if (cmd.raw.size() > next && !IsRoomOption(cmd.args[next]))
            {
                if (!ParseServer(cmd.raw[next], server, port))
                {
                    message = "usage: host relay [server[:port]] [name <room name>] [password <password>] [unlisted]";
                    return false;
                }
                ++next;
            }
            std::string roomName, password;
            bool listed = true;
            if (!RoomOptions(cmd, next, roomName, password, listed, message)) return false;
            if (server.empty())
            {
                message = "no relay server: host relay <server>[:port], or relay <server> first";
                return false;
            }
            return Match::HostRelay(server, (uint16_t)port, roomName, password, listed, message);
        }
        if (verb == "lobby")
        {
            // lobby [page]: the rooms at the relay waiting for a guest.
            int page = 1;
            if (cmd.args.size() > 1 && (!ArgInt(cmd, 1, page) || page < 1))
            {
                message = "usage: lobby [page]";
                return false;
            }
            if (g_relayServer.empty())
            {
                message = "no relay server: relay <server>[:port] first";
                return false;
            }
            return Net::ListRelayRooms(g_relayServer, (uint16_t)g_relayPort, page - 1, message);
        }
        if (verb == "join" && ArgIs(cmd, 1, "relay"))
        {
            // join relay <code> [server[:port]] [password <password>]
            std::string server = g_relayServer;
            int port = g_relayPort;
            std::string password;
            size_t next = 3;
            if (cmd.raw.size() > next && cmd.args[next] != "password")
            {
                if (!ParseServer(cmd.raw[next], server, port))
                {
                    message = "usage: join relay <code> [server[:port]] [password <password>]";
                    return false;
                }
                ++next;
            }
            if (cmd.raw.size() > next + 1 && cmd.args[next] == "password") password = cmd.raw[next + 1];
            if (cmd.raw.size() < 3)
            {
                message = "usage: join relay <code> [server[:port]] [password <password>]";
                return false;
            }
            if (server.empty())
            {
                message = "no relay server: join relay <code> <server>[:port], or relay <server> first";
                return false;
            }
            // "@file": the code is in that file of the game folder (tests: the runner copies the host's code there).
            std::string code = cmd.raw[2];
            if (!code.empty() && code[0] == '@')
            {
                std::ifstream file(code.substr(1).c_str());
                if (!(file >> code))
                {
                    message = "no room code in " + cmd.raw[2].substr(1);
                    return false;
                }
            }
            return Match::JoinRelay(server, (uint16_t)port, code, password, message);
        }
        if (verb == "host")
        {
            // host [port] [local]: "local" accepts only a second game on this computer (no firewall prompt).
            int port = Net::DEFAULT_PORT;
            size_t next = 1;
            if (ArgInt(cmd, next, port)) ++next;
            bool local = ArgIs(cmd, next, "local");
            if (port <= 0 || port > 65535) { message = "usage: host [port] [local]"; return false; }
            return Match::Host((uint16_t)port, local, message);
        }
        if (verb == "join" && cmd.raw.size() >= 2 && !g_relayServer.empty() && Relay::Client::IsRoomCode(cmd.raw[1]))
        {
            // join <code> [password]: a room at the relay (a room code, not an address).
            std::string password = cmd.raw.size() > 2 ? cmd.raw[2] : "";
            return Match::JoinRelay(g_relayServer, (uint16_t)g_relayPort, cmd.raw[1], password, message);
        }
        if (verb == "join")
        {
            int port = Net::DEFAULT_PORT;
            if (cmd.raw.size() < 2 || (cmd.args.size() > 2 && !ArgInt(cmd, 2, port)) || port <= 0 || port > 65535)
            {
                message = "usage: join <address> [port]";
                return false;
            }
            return Match::Join(cmd.raw[1], (uint16_t)port, message);
        }
        if (verb == "leave")
        {
            Match::Leave();
            message = "left";
            return true;
        }
        if (verb == "net")
        {
            message = Match::Status();
            Log("%s", message.c_str());
            return true;
        }
        if (verb == "netstats")
        {
            bool on;
            if (!ParseOnOff(cmd, 1, on)) { message = "usage: netstats on|off"; return false; }
            Hud::SetNetStats(on);
            message = std::string("network numbers ") + (on ? "shown (bottom left)" : "hidden");
            return true;
        }
        if (verb == "netsim")
        {
            // netsim <delay ms> [jitter ms] [loss %]: test conditions for our outgoing packets; netsim cut on|off: none
            // go out or come in (a pulled cable)
            if (ArgIs(cmd, 1, "cut"))
            {
                bool on;
                if (!ParseOnOff(cmd, 2, on)) { message = "usage: netsim cut on|off"; return false; }
                Net::SimulateCut(on);
                message = on ? "network cut" : "network back";
                return true;
            }
            if (cmd.args.size() < 2) { message = "usage: netsim <delay ms> [jitter ms] [loss %] | netsim cut on|off"; return false; }
            double delay = std::atof(cmd.args[1].c_str());
            double jitter = cmd.args.size() > 2 ? std::atof(cmd.args[2].c_str()) : 0.0;
            double loss = cmd.args.size() > 3 ? std::atof(cmd.args[3].c_str()) : 0.0;
            Net::Simulate(delay, jitter, loss);
            message = "simulating " + cmd.args[1] + " ms delay";
            return true;
        }
        if (verb == "say")
        {
            std::string text = cmd.text.substr(cmd.text.find("say") + 3);
            size_t start = text.find_first_not_of(' ');
            text = start == std::string::npos ? "" : text.substr(start);
            return Match::Say(text, message);
        }

        if (verb == "chatflood")
        {
            int count;
            if (!ArgInt(cmd, 1, count) || count < 1 || count > 50) { message = "usage: chatflood <count>"; return false; }
            message = std::to_string(Match::ChatFlood(count)) + " chat lines sent past the limits";
            return true;
        }
        if (verb == "click")
        {
            // click <x> <y>: a left click there through the game's whole input (the Duels window, the match's
            // buttons, then FTL), in its 1280 x 720 coordinates, for tests of the buttons; in the main menu, through
            // the menu's (ours over FTL's).
            // click draw|timeout|concede|ready: the middle of that match button, wherever its layout puts it (roadmap AY,
            // BF).
            int x = 0, y = 0;
            const char *button = ArgIs(cmd, 1, "draw") ? "draw" : ArgIs(cmd, 1, "timeout") ? "timeout" : ArgIs(cmd, 1, "concede") ? "concede"
                               : ArgIs(cmd, 1, "ready") ? "ready" : nullptr;
            if (button)
            {
                if (!MatchUi::ButtonCentre(button, x, y)) { message = std::string("no ") + button + " button on the screen"; return false; }
            }
            else if (ArgIs(cmd, 1, "replay"))
            {
                // click replay stop|back|play|on|speed|line|view|sensors: a replay's control (roadmap AW, BC, BA).
                std::string control = cmd.args.size() > 2 ? cmd.args[2] : "";
                if (!ReplayUi::ControlCentre(control, x, y)) { message = "no replay control '" + control + "' on the screen"; return false; }
            }
            else if (!ArgInt(cmd, 1, x) || !ArgInt(cmd, 2, y)) { message = "usage: click <x> <y> | click draw|timeout|concede|ready"; return false; }
            CApp *app = G_->GetCApp();
            if (app && app->menu.bOpen)
            {
                app->menu.MouseMove(x, y);
                app->menu.MouseClick(x, y);
                app->menu.MouseUp(x, y);
                message = "clicked at " + std::to_string(x) + "," + std::to_string(y) + " in the main menu";
                return true;
            }
            if (!app || !app->gui) { message = "not in the game"; return false; }
            app->gui->MouseMove(x, y);
            app->gui->LButtonDown(x, y, false);
            app->gui->LButtonUp(x, y, false);
            message = "clicked at " + std::to_string(x) + "," + std::to_string(y);
            return true;
        }
        if (verb == "rclick")
        {
            // rclick <x> <y>: a right click there through the game's whole input (FTL's upgrade screen takes a level
            // back with it).
            int x, y;
            if (!ArgInt(cmd, 1, x) || !ArgInt(cmd, 2, y)) { message = "usage: rclick <x> <y>"; return false; }
            CApp *app = G_->GetCApp();
            if (!app || !app->gui) { message = "not in the game"; return false; }
            app->gui->MouseMove(x, y);
            app->gui->RButtonDown(x, y, false);
            app->gui->RButtonUp(x, y, false);
            message = "right-clicked at " + std::to_string(x) + "," + std::to_string(y);
            return true;
        }
        if (verb == "drag")
        {
            // drag <x1> <y1> <x2> <y2>: press the left button at the first point, move to the second (in steps) and
            // let go there, through the game's whole input (moving weapons, drones and crew in FTL's screens).
            int x1, y1, x2, y2;
            if (!ArgInt(cmd, 1, x1) || !ArgInt(cmd, 2, y1) || !ArgInt(cmd, 3, x2) || !ArgInt(cmd, 4, y2))
            {
                message = "usage: drag <x1> <y1> <x2> <y2>";
                return false;
            }
            CApp *app = G_->GetCApp();
            if (!app || !app->gui) { message = "not in the game"; return false; }
            app->gui->MouseMove(x1, y1);
            app->gui->LButtonDown(x1, y1, false);
            for (int step = 1; step <= 8; ++step) app->gui->MouseMove(x1 + (x2 - x1) * step / 8, y1 + (y2 - y1) * step / 8);
            app->gui->LButtonUp(x2, y2, false);
            message = "dragged from " + std::to_string(x1) + "," + std::to_string(y1) + " to " + std::to_string(x2) + "," + std::to_string(y2);
            return true;
        }
        if (verb == "hotkey")
        {
            // hotkey <name>: the key set for one of FTL's controls (Options > Controls), as pressed: ship_info (U),
            // ship_crew (C), ship_inv (I), store, ...
            CApp *app = G_->GetCApp();
            if (cmd.args.size() < 2 || !app || !app->gui) { message = "usage: hotkey <control name>"; return false; }
            SDLKey key = Settings::GetHotkey(cmd.args[1]);
            app->gui->KeyDown(key, false);
            message = "pressed the key of " + cmd.args[1] + " (" + std::to_string((int)key) + ")";
            return true;
        }
        if (verb == "mouse")
        {
            // mouse <x> <y> [seconds]: FTL's mouse stays there (in its 1280 x 720 coordinates), for tooltips in
            // screenshots (FTL shows one after the mouse rests on a thing for a moment).
            int x, y;
            if (!ArgInt(cmd, 1, x) || !ArgInt(cmd, 2, y)) { message = "usage: mouse <x> <y> [seconds]"; return false; }
            double seconds = cmd.args.size() > 3 ? std::atof(cmd.args[3].c_str()) : 3.0;
            g_heldMouse.x = x;
            g_heldMouse.y = y;
            g_heldMouse.untilMs = WallMs() + seconds * 1000.0;
            HeldMouseOnFrame();
            message = "mouse at " + std::to_string(x) + "," + std::to_string(y);
            return true;
        }
        if (verb == "ftlcharge")
        {
            // ftlcharge: our FTL drive charged at once (tests of running away, roadmap AD).
            ShipManager *own = G_->GetShipManager(0);
            if (!own) { message = "no ship"; return false; }
            own->jump_timer.first = own->jump_timer.second;
            message = "the FTL drive is charged";
            return true;
        }
        if (verb == "shake")
        {
            // shake: FTL's screen shake at its strongest (as a hard hit starts it), for tests of what moves with it.
            CommandGui *gui = G_->GetWorld() ? G_->GetWorld()->commandGui : nullptr;
            if (!gui) { message = "not in the game"; return false; }
            gui->fShakeTimer = 1.f;
            message = "the screen shakes";
            return true;
        }
        if (verb == "fonttest")
        {
            // fonttest [seconds]: the match display's text in each of FTL's fonts, for a screenshot.
            double seconds = cmd.args.size() > 1 ? std::atof(cmd.args[1].c_str()) : 5.0;
            Hud::FontTest(seconds);
            message = "font test shown";
            return true;
        }

        // The Duels window (test verb: open, close, a click in it).
        if (verb == "duels") return Window::RunVerb(cmd.args, message);
        // Fair play (roadmap 4.1): its state; a lying verdict for tests.
        if (verb == "fair") return Fair::RunVerb(cmd, message);
        if (verb == "demo") return Demo::RunVerb(cmd, message);
        // Fine settings and presets (roadmap BE).
        if (verb == "tune") return Tune::RunVerb(cmd, message);
        // The player's account at the master (roadmap BG).
        if (verb == "account") return Account::RunVerb(cmd, message);
        if (verb == "master")
        {
            // master [<server>|<web address>]: the master server for this run (duels.cfg's stays; a test's local one).
            if (cmd.raw.size() > 1) Config::UseMaster(cmd.raw[1]);
            message = "master " + Config::Master() + " (" + Config::MasterUrl() + ")";
            return true;
        }
        if (verb == "preset") return Tune::RunPresetVerb(cmd, message);
        if (verb == "replay") return Demo::RunReplayVerb(cmd, message);
        // Ours in the main menu (test verb: the name prompt, the tutorial box, the players' guide).
        if (verb == "menu") return Menu::RunVerb(cmd.args, message);

        // The match flow (DuelsRounds.cpp): its settings, ready, forfeit, concede and draws.
        if (Rounds::IsVerb(verb)) return Rounds::RunVerb(cmd, message);

        // Everything else acts on a ship.
        return ExecuteShipCommand(cmd, message);
    }
}

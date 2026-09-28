#include "Global.h"
#include "CommandConsole.h"
#include "Duels.h"
#include "DuelsMatch.h"
#include "DuelsNet.h"
#include "DuelsScreen.h"
#include "DuelsShipControl.h"
#include "DuelsView.h"

#include <cmath>
#include <cstdlib>
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

    bool Execute(const Command &cmd, std::string &message)
    {
        if (cmd.args.empty())
        {
            message = "empty command";
            return false;
        }

        State &state = GetState();
        const std::string &verb = cmd.args[0];

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
            if (!app || !app->gui || !OpenConsole(app->gui))
            {
                message = "the console can't open now";
                return false;
            }
            std::string text = cmd.text.substr(cmd.text.find("console") + 7);
            size_t start = text.find_first_not_of(' ');
            text = start == std::string::npos ? "" : text.substr(start);
            CommandConsole *console = CommandConsole::GetInstance();
            if (console->textInput)
            {
                console->textInput->SetText(text);
                console->textInput->pos = (int)console->textInput->text.size();
            }
            app->gui->inputBox.inputText = text;
            message = "console open";
            return true;
        }

        // Network duel
        if (verb == "name")
        {
            if (cmd.raw.size() < 2) { message = "usage: name <player name>"; return false; }
            Match::SetPlayerName(cmd.raw[1]);
            message = "player name " + cmd.raw[1];
            return true;
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
        if (verb == "netsim")
        {
            // netsim <delay ms> [jitter ms] [loss %]: test conditions for our outgoing packets
            if (cmd.args.size() < 2) { message = "usage: netsim <delay ms> [jitter ms] [loss %]"; return false; }
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
            if (!Match::Say(text)) { message = "not connected"; return false; }
            message = "said: " + text;
            return true;
        }

        // Everything else acts on a ship.
        return ExecuteShipCommand(cmd, message);
    }
}

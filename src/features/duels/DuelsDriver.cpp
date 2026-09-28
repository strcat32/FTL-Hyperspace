#include "Global.h"
#include "Duels.h"
#include "DuelsMatch.h"
#include "DuelsNet.h"
#include "DuelsShipControl.h"

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

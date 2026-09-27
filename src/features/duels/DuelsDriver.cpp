#include "Global.h"
#include "Duels.h"
#include "DuelsShipControl.h"

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

        // Everything else acts on a ship.
        return ExecuteShipCommand(cmd, message);
    }
}

#pragma once

#include <string>
#include <vector>

namespace Duels
{
    // One duel command. The same format is used by test scripts, the console and (later) the network:
    //   "<verb> <args...>"          e.g. "fire 1 0 room 3"
    // Script lines are prefixed with the time in seconds since the script started:
    //   "12.5 fire 1 0 room 3"
    struct Command
    {
        double time = 0.0;              // seconds since script start (0 for console commands)
        std::vector<std::string> args;  // args[0] is the verb; all tokens are lower-cased
        std::vector<std::string> raw;   // the same tokens in their original spelling (blueprint names, paths)
        std::string text;               // original line, for logs
        int line = 0;                   // script line number (0 for console commands)
    };

    // Parses "<verb> <args...>". Returns false for blank and comment ('#') lines.
    bool ParseCommand(const std::string &text, Command &out);

    // Loads a timed script, sorted by time (stable for equal times).
    // Lines starting with '@' are harness directives and are returned separately, unparsed.
    // Returns false and fills `error` on the first malformed line.
    bool LoadScript(const std::string &path, std::vector<Command> &out, std::vector<std::string> &directives,
                    std::string &error);

    // Argument helpers; they return false when the argument is missing or malformed.
    bool ArgInt(const Command &cmd, size_t index, int &out);
    bool ArgIs(const Command &cmd, size_t index, const char *word);
}

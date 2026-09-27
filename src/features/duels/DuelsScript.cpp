#include "DuelsScript.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace Duels
{
    static std::string StripComment(const std::string &text)
    {
        size_t hash = text.find('#');
        return hash == std::string::npos ? text : text.substr(0, hash);
    }

    static std::vector<std::string> Tokenize(const std::string &text)
    {
        std::vector<std::string> tokens;
        std::istringstream stream(text);
        std::string token;
        while (stream >> token) tokens.push_back(token);
        return tokens;
    }

    static std::string Lower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return text;
    }

    static void SetTokens(Command &cmd, std::vector<std::string>::const_iterator begin,
                          std::vector<std::string>::const_iterator end)
    {
        cmd.raw.assign(begin, end);
        cmd.args.clear();
        for (const std::string &token : cmd.raw) cmd.args.push_back(Lower(token));
    }

    bool ParseCommand(const std::string &text, Command &out)
    {
        std::vector<std::string> tokens = Tokenize(StripComment(text));
        SetTokens(out, tokens.begin(), tokens.end());
        out.text = text;
        return !out.args.empty();
    }

    bool LoadScript(const std::string &path, std::vector<Command> &out, std::vector<std::string> &directives,
                    std::string &error)
    {
        std::ifstream file(path.c_str());
        if (!file)
        {
            error = "cannot open " + path;
            return false;
        }

        out.clear();
        directives.clear();
        std::string line;
        int lineNumber = 0;
        while (std::getline(file, line))
        {
            ++lineNumber;
            std::vector<std::string> tokens = Tokenize(StripComment(line));
            if (tokens.empty()) continue;

            if (tokens[0][0] == '@')
            {
                directives.push_back(StripComment(line));
                continue;
            }

            char *end = nullptr;
            double time = std::strtod(tokens[0].c_str(), &end);
            if (end == tokens[0].c_str() || *end != '\0' || time < 0.0 || tokens.size() < 2)
            {
                std::ostringstream message;
                message << path << ":" << lineNumber << ": expected '<seconds> <verb> <args...>'";
                error = message.str();
                return false;
            }

            Command cmd;
            cmd.time = time;
            SetTokens(cmd, tokens.begin() + 1, tokens.end());
            cmd.text = line;
            cmd.line = lineNumber;
            out.push_back(cmd);
        }

        std::stable_sort(out.begin(), out.end(), [](const Command &a, const Command &b) { return a.time < b.time; });
        return true;
    }

    bool ArgInt(const Command &cmd, size_t index, int &out)
    {
        if (index >= cmd.args.size()) return false;
        const std::string &arg = cmd.args[index];
        char *end = nullptr;
        long value = std::strtol(arg.c_str(), &end, 10);
        if (end == arg.c_str() || *end != '\0') return false;
        out = (int)value;
        return true;
    }

    bool ArgIs(const Command &cmd, size_t index, const char *word)
    {
        return index < cmd.args.size() && cmd.args[index] == word;
    }
}

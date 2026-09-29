#include "DuelsConfig.h"
#include "Duels.h"

#include <fstream>
#include <sstream>

namespace Duels
{
    namespace Config
    {
        static const char *const FILE_NAME = "duels.cfg";

        struct Settings
        {
            bool loaded = false;
            std::vector<std::string> lines;   // the file as it was, to write it back with one line changed
            std::string playerName;
            std::vector<std::string> relays;
        };

        static Settings g_settings;

        static std::string Trim(const std::string &text)
        {
            size_t start = text.find_first_not_of(" \t\r");
            if (start == std::string::npos) return "";
            size_t end = text.find_last_not_of(" \t\r");
            return text.substr(start, end - start + 1);
        }

        // "key value" of a setting line; false for blank lines and comments.
        static bool Split(const std::string &line, std::string &key, std::string &value)
        {
            std::string text = Trim(line);
            if (text.empty() || text[0] == '#') return false;
            size_t space = text.find_first_of(" \t");
            key = text.substr(0, space);
            value = space == std::string::npos ? "" : Trim(text.substr(space));
            return true;
        }

        void Load()
        {
            Settings &s = g_settings;
            if (s.loaded) return;
            s.loaded = true;
            std::ifstream file(FILE_NAME);
            std::string line;
            while (std::getline(file, line))
            {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                s.lines.push_back(line);
                std::string key, value;
                if (!Split(line, key, value) || value.empty()) continue;
                if (key == "name") s.playerName = value;
                else if (key == "relay") s.relays.push_back(value);
            }
            Log("Config: %s: name %s, %u relay(s)", FILE_NAME, s.playerName.empty() ? "(none)" : s.playerName.c_str(),
                (unsigned)s.relays.size());
        }

        const std::string &PlayerName()
        {
            Load();
            return g_settings.playerName;
        }

        void SavePlayerName(const std::string &name)
        {
            Load();
            Settings &s = g_settings;
            s.playerName = name;
            bool replaced = false;
            for (std::string &line : s.lines)
            {
                std::string key, value;
                if (!Split(line, key, value) || key != "name") continue;
                if (!replaced) line = "name " + name;
                else line.clear();   // an older duplicate goes
                replaced = true;
            }
            if (!replaced)
            {
                if (s.lines.empty()) s.lines.push_back("# FTL: Duels settings: name <player name>, relay <server>[:port] (any number)");
                s.lines.push_back("name " + name);
            }
            std::ofstream file(FILE_NAME, std::ios::trunc);
            for (const std::string &line : s.lines) file << line << "\n";
            if (!file) Log("Config: cannot write %s", FILE_NAME);
        }

        const std::vector<std::string> &Relays()
        {
            Load();
            return g_settings.relays;
        }
    }
}

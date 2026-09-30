#include "DuelsConfig.h"
#include "Duels.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>

namespace Duels
{
    namespace Config
    {
        static const char *const FILE_NAME = "duels.cfg";

        // The project's public relays, on every player's list after the relays of their own file (rules, section 6).
        static const char *const PUBLIC_RELAYS[] = {"18.226.104.62"};

        struct Settings
        {
            bool loaded = false;
            std::vector<std::string> lines;   // the file as it was, to write it back with one line changed
            std::string playerName;
            std::vector<std::string> relays;
            std::map<std::string, std::string> values;   // the last line of each one-value setting
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
                else s.values[key] = value;
            }
            size_t own = s.relays.size();
            for (const char *relay : PUBLIC_RELAYS)
            {
                // The same server with the default port written out is the same relay.
                std::string plain = relay, withPort = plain + ":47700";
                if (std::find(s.relays.begin(), s.relays.end(), plain) == s.relays.end() &&
                    std::find(s.relays.begin(), s.relays.end(), withPort) == s.relays.end())
                {
                    s.relays.push_back(plain);
                }
            }
            Log("Config: %s: name %s, %u relay(s) of its own, %u in all", FILE_NAME,
                s.playerName.empty() ? "(none)" : s.playerName.c_str(), (unsigned)own, (unsigned)s.relays.size());
        }

        const std::string &PlayerName()
        {
            Load();
            return g_settings.playerName;
        }

        // The setting's line replaced (older duplicates go), or added; then the file written back.
        static void WriteSetting(const std::string &name, const std::string &newValue)
        {
            Settings &s = g_settings;
            bool replaced = false;
            for (std::string &line : s.lines)
            {
                std::string key, value;
                if (!Split(line, key, value) || key != name) continue;
                if (!replaced) line = name + " " + newValue;
                else line.clear();
                replaced = true;
            }
            if (!replaced)
            {
                if (s.lines.empty())
                {
                    s.lines.push_back("# FTL: Duels settings: name <player name>, relay <server>[:port] (any number), and the host's match settings");
                }
                s.lines.push_back(name + " " + newValue);
            }
            std::ofstream file(FILE_NAME, std::ios::trunc);
            for (const std::string &line : s.lines) file << line << "\n";
            if (!file) Log("Config: cannot write %s", FILE_NAME);
        }

        void SavePlayerName(const std::string &name)
        {
            Load();
            g_settings.playerName = name;
            WriteSetting("name", name);
        }

        std::string Value(const std::string &key)
        {
            Load();
            std::map<std::string, std::string>::const_iterator it = g_settings.values.find(key);
            return it == g_settings.values.end() ? std::string() : it->second;
        }

        void SaveValue(const std::string &key, const std::string &value)
        {
            Load();
            if (Value(key) == value) return;
            g_settings.values[key] = value;
            WriteSetting(key, value);
        }

        bool IsPublicRelay(const std::string &server)
        {
            for (const char *relay : PUBLIC_RELAYS)
            {
                if (server == relay) return true;
            }
            return false;
        }

        const std::vector<std::string> &Relays()
        {
            Load();
            return g_settings.relays;
        }
    }
}

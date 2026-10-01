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

        // The master server of a duels.cfg that names none (roadmap AZ): its name, so that the server can move. It is the
        // project's public relay, on every player's list after the relays of their own file (rules, section 6).
        static const char *const DEFAULT_MASTER = "ftl-duels.link";

        struct Settings
        {
            bool loaded = false;
            std::vector<std::string> lines;   // the file as it was, to write it back with one line changed
            std::string playerName;
            std::string master;
            bool masterInFile = false;        // the file has its master line (else it is written at the first need)
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
                else if (key == "master") s.master = value;
                else s.values[key] = value;
            }
            s.masterInFile = !s.master.empty();
            if (s.master.empty()) s.master = DEFAULT_MASTER;
            size_t own = s.relays.size();
            {
                // The master after the file's own relays (the same server with the default port written out is the
                // same relay).
                std::string plain = s.master, withPort = plain + ":47700";
                if (std::find(s.relays.begin(), s.relays.end(), plain) == s.relays.end() &&
                    std::find(s.relays.begin(), s.relays.end(), withPort) == s.relays.end())
                {
                    s.relays.push_back(plain);
                }
            }
            Log("Config: %s: name %s, master %s%s, %u relay(s) of its own, %u in all", FILE_NAME,
                s.playerName.empty() ? "(none)" : s.playerName.c_str(), s.master.c_str(), s.masterInFile ? "" : " (none in the file)",
                (unsigned)own, (unsigned)s.relays.size());
        }

        const std::string &PlayerName()
        {
            Load();
            return g_settings.playerName;
        }

        static void WriteFile();

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
                    s.lines.push_back("# FTL: Duels settings: name <player name>, master <server> (the master server), relay <server>[:port] "
                                      "(any number), and the host's match settings");
                }
                s.lines.push_back(name + " " + newValue);
            }
            WriteFile();
        }

        static void WriteFile()
        {
            std::ofstream file(FILE_NAME, std::ios::trunc);
            for (const std::string &line : g_settings.lines) file << line << "\n";
            if (!file) Log("Config: cannot write %s", FILE_NAME);
        }

        void RemoveValue(const std::string &key)
        {
            Load();
            Settings &s = g_settings;
            if (!s.values.erase(key)) return;
            s.lines.erase(std::remove_if(s.lines.begin(), s.lines.end(),
                                         [&](const std::string &line)
                                         {
                                             std::string name, value;
                                             return Split(line, name, value) && name == key;
                                         }),
                          s.lines.end());
            WriteFile();
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

        // A server's host without its port ("name:47700", "name", an IPv6 address in brackets with its port).
        static std::string HostOf(const std::string &server)
        {
            if (!server.empty() && server[0] == '[')
            {
                size_t end = server.find(']');
                return end == std::string::npos ? server : server.substr(1, end - 1);
            }
            size_t colon = server.find(':');
            if (colon != std::string::npos && server.find(':', colon + 1) == std::string::npos) return server.substr(0, colon);
            return server;
        }

        const std::string &Master()
        {
            Load();
            Settings &s = g_settings;
            // In the file from now on: the master's address is a setting, not the code's (roadmap AZ).
            if (!s.masterInFile && SettingsFromConfig())
            {
                s.masterInFile = true;
                WriteSetting("master", s.master);
                Log("Config: %s names its master server now (%s)", FILE_NAME, s.master.c_str());
            }
            return s.master;
        }

        bool IsPublicRelay(const std::string &server)
        {
            Load();
            return HostOf(server) == HostOf(g_settings.master);
        }

        const std::vector<std::string> &Relays()
        {
            Master();
            return g_settings.relays;
        }
    }
}

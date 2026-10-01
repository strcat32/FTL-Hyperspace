#include "Global.h"
#include "Duels.h"
#include "DuelsConfig.h"
#include "DuelsMatch.h"
#include "DuelsNet.h"
#include "DuelsRounds.h"
#include "DuelsScript.h"
#include "DuelsTune.h"
#include "DuelsWire.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <boost/filesystem.hpp>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

namespace Duels
{
    namespace Tune
    {
        // The fine settings (docs/design/settings.md): name, default, kind, each number's range, what it is.
        static const std::vector<Setting> SETTINGS = {
            {"shop.scrap", "100,126,162", 'l', 0, 9999, "the scrap each round brings; after the list each round adds its last step"},
            {"shop.price_caps", "55,65,75,85", 'l', 0, 999, "the highest price of a weapon or drone in the shop, round by round (none after the list)"},
            {"shop.kinds", "weapons,drones,augments,systems,crew", 'w', 0, 0, "what the shop sells (weapons, drones, augments, systems, crew)"},
            {"shop.exclude", "", 'w', 0, 0, "blueprints the shop never sells (BEAM_1,DRONE_HACKING ...)"},
            {"shop.missiles", "8", 'n', 0, 99, "missiles on sale each round"},
            {"shop.drone_parts", "4", 'n', 0, 99, "drone parts on sale each round"},
            {"round.starting_s", "1.5", 'n', 0, 30, "the fight begins this long after both ships stand (s)"},
            {"round.ending_s", "3", 'n', 0, 30, "a ship's explosion before its round is decided (s; shots in the air still count)"},
            {"round.result_s", "6", 'n', 1, 60, "the round's result on screen before the next preparation (s)"},
            {"choice.ban_s", "20", 'n', 5, 120, "the ship choice: a ban's time (s)"},
            {"choice.pick_s", "30", 'n', 5, 300, "the ship choice: the pick's time (s)"},
            {"choice.reveal_s", "4", 'n', 1, 30, "the ship choice: both ships on screen before round 1 (s)"},
            {"draw.again_s", "60", 'n', 0, 600, "after a declined draw offer the same player waits this long (s)"},
            {"stall.step", "0.02", 'n', 0, 0.5, "the anti-stall timer starts again at a new low by more than this share of the round's start"},
            {"stall.draw_lead", "10", 'n', 0, 100, "an anti-stall decision closer than this (of 100) is a draw"},
            {"score.hull_weight", "0.65", 'n', 0, 1, "the damage score: the hull's weight (the crew's is the rest)"},
            {"hazard.sun_s", "28,34", 'r', 5, 300, "a sun: the time between flares (s, from, to)"},
            {"hazard.pulsar_s", "11,18", 'r', 3, 300, "a pulsar: the time between pulses (s, from, to)"},
            {"hazard.battery_s", "20,25", 'r', 5, 300, "an anti-ship battery: the time between its shots (s, from, to)"},
        };

        static const char *const PRESET_FOLDER = "presets";

        struct State
        {
            bool loaded = false;
            std::map<std::string, std::string> own;     // ours that differ from the defaults
            std::map<std::string, std::string> match;   // the match's (the host's) that differ from the defaults
            bool inMatch = false;
            // A ranked room's season stands in for ours (roadmap BG).
            const std::map<std::string, std::string> *season = nullptr;
        };

        static State g;

        const std::vector<Setting> &All()
        {
            return SETTINGS;
        }

        static const Setting *Find(const std::string &name)
        {
            for (const Setting &setting : SETTINGS)
            {
                if (name == setting.name) return &setting;
            }
            return nullptr;
        }

        static std::string Lower(std::string text)
        {
            for (char &c : text) c = (char)std::tolower((unsigned char)c);
            return text;
        }

        static std::vector<std::string> SplitList(const std::string &text)
        {
            std::vector<std::string> parts;
            std::stringstream in(text);
            std::string part;
            while (std::getline(in, part, ','))
            {
                part.erase(std::remove_if(part.begin(), part.end(), [](char c) { return std::isspace((unsigned char)c) != 0; }), part.end());
                if (!part.empty()) parts.push_back(part);
            }
            return parts;
        }

        // A value as it is kept: its parts, comma-separated without spaces; "" when it isn't one for the setting.
        static std::string Normal(const Setting &setting, const std::string &value, std::string &why)
        {
            std::vector<std::string> parts = SplitList(value);
            if (setting.kind == 'w')
            {
                if (parts.size() == 1 && Lower(parts[0]) == "none") parts.clear();   // how a file writes an empty list
                std::string text;
                for (const std::string &part : parts) text += (text.empty() ? "" : ",") + part;
                return text.empty() ? std::string(" ") : text;   // " ": an empty list, still a value
            }
            if (parts.empty() || (setting.kind == 'n' && parts.size() != 1) || (setting.kind == 'r' && parts.size() != 2))
            {
                why = setting.kind == 'n' ? "one number" : setting.kind == 'r' ? "two numbers, from and to (28,34)" : "numbers (100,126,162)";
                return "";
            }
            std::string text;
            double previous = 0.0;
            for (size_t i = 0; i < parts.size(); ++i)
            {
                char *end = nullptr;
                double number = std::strtod(parts[i].c_str(), &end);
                if (!end || *end != '\0' || !(number >= setting.min && number <= setting.max))
                {
                    std::ostringstream range;
                    range << (setting.kind == 'n' ? "a number from " : "numbers from ") << setting.min << " to " << setting.max;
                    why = range.str();
                    return "";
                }
                if (setting.kind == 'r' && i == 1 && number < previous)
                {
                    why = "from, then to: the second not below the first";
                    return "";
                }
                previous = number;
                text += (text.empty() ? "" : ",") + parts[i];
            }
            return text;
        }

        static std::string Default(const Setting &setting)
        {
            std::string why;
            return Normal(setting, setting.value, why);
        }

        // Ours from duels.cfg, once (a test scenario starts from the defaults, as the match's settings do).
        static void Load()
        {
            if (g.loaded) return;
            g.loaded = true;
            if (!SettingsFromConfig()) return;
            for (const Setting &setting : SETTINGS)
            {
                std::string text = Config::Value(setting.name), why;
                if (text.empty()) continue;
                std::string value = Normal(setting, text, why);
                if (value.empty()) Log("Tune: duels.cfg's %s isn't one (%s): its default", setting.name, why.c_str());
                else if (value != Default(setting)) g.own[setting.name] = value;
            }
            if (!g.own.empty()) Log("Tune: %u fine setting(s) of duels.cfg differ from the defaults", (unsigned)g.own.size());
        }

        std::string Text(const std::string &name)
        {
            Load();
            const Setting *setting = Find(name);
            if (!setting) return "";
            const std::map<std::string, std::string> &values = g.inMatch ? g.match : g.own;
            std::map<std::string, std::string>::const_iterator found = values.find(name);
            std::string text = found != values.end() ? found->second : Default(*setting);
            return text == " " ? std::string() : text;
        }

        std::vector<double> Numbers(const std::string &name)
        {
            std::vector<double> numbers;
            for (const std::string &part : SplitList(Text(name))) numbers.push_back(std::atof(part.c_str()));
            return numbers;
        }

        double Number(const std::string &name)
        {
            std::vector<double> numbers = Numbers(name);
            return numbers.empty() ? 0.0 : numbers[0];
        }

        bool HasWord(const std::string &name, const std::string &word)
        {
            const std::string wanted = Lower(word);
            for (const std::string &part : SplitList(Text(name)))
            {
                if (Lower(part) == wanted) return true;
            }
            return false;
        }

        bool Set(const std::string &name, const std::string &value, std::string &why)
        {
            Load();
            const Setting *setting = Find(name);
            if (!setting)
            {
                why = "no fine setting '" + name + "' (tune lists them)";
                return false;
            }
            std::string normal = Normal(*setting, value, why);
            if (normal.empty()) return false;
            if (normal == Default(*setting))
            {
                Reset(name);
                return true;
            }
            g.own[name] = normal;
            if (SettingsFromConfig()) Config::SaveValue(name, normal == " " ? std::string("none") : normal);
            return true;
        }

        void Reset(const std::string &name)
        {
            Load();
            g.own.erase(name);
            if (SettingsFromConfig()) Config::RemoveValue(name);
        }

        std::vector<std::pair<std::string, std::string>> Changed()
        {
            Load();
            std::vector<std::pair<std::string, std::string>> changed;
            for (const Setting &setting : SETTINGS)
            {
                std::map<std::string, std::string>::const_iterator found = g.own.find(setting.name);
                if (found != g.own.end()) changed.push_back(std::make_pair(found->first, found->second == " " ? std::string() : found->second));
            }
            return changed;
        }

        bool Custom()
        {
            Load();
            return g.inMatch ? !g.match.empty() : !g.own.empty();
        }

        void WriteMatch(Writer &w)
        {
            Load();
            // The host plays by its own, as it sends them (in a ranked room: the season's).
            g.match = g.season ? *g.season : g.own;
            g.inMatch = true;
            w.U8((uint8_t)std::min<size_t>(g.match.size(), 255));
            for (const std::pair<const std::string, std::string> &entry : g.match)
            {
                w.Str(entry.first);
                w.Str(entry.second);
            }
        }

        bool ReadMatch(Reader &r, std::string &note)
        {
            std::map<std::string, std::string> match;
            int count = r.U8();
            for (int i = 0; i < count && r.Ok(); ++i)
            {
                std::string name = r.Str(), value = r.Str(), why;
                const Setting *setting = Find(name);
                std::string normal = setting ? Normal(*setting, value, why) : std::string();
                if (normal.empty()) note += (note.empty() ? "" : ", ") + name + " left out";
                else if (normal != Default(*setting)) match[name] = normal;
            }
            if (!r.Ok()) return false;
            g.match = match;
            g.inMatch = true;
            if (!match.empty())
            {
                std::string list;
                for (const std::pair<const std::string, std::string> &entry : match) list += (list.empty() ? "" : ", ") + entry.first + " " + entry.second;
                Log("Tune: the match's fine settings (the host's): %s", list.c_str());
            }
            return true;
        }

        void EndMatch()
        {
            if (!g.inMatch) return;
            g.inMatch = false;
            g.match.clear();
        }

        std::string SeasonValue(const std::string &name, const std::string &value, bool &isDefault, std::string &why)
        {
            isDefault = false;
            const Setting *setting = Find(name);
            if (!setting)
            {
                why = "this game has no fine setting " + name + " (a newer game's?)";
                return "";
            }
            std::string normal = Normal(*setting, value, why);
            isDefault = !normal.empty() && normal == Default(*setting);
            return normal;
        }

        void UseSeason(const std::map<std::string, std::string> *fine)
        {
            g.season = fine;
        }

        bool MatchIs(const std::map<std::string, std::string> &fine)
        {
            return g.inMatch && g.match == fine;
        }

        std::vector<std::pair<std::string, std::string>> MatchChanged()
        {
            std::vector<std::pair<std::string, std::string>> changed;
            if (!g.inMatch) return changed;
            for (const Setting &setting : SETTINGS)
            {
                std::map<std::string, std::string>::const_iterator found = g.match.find(setting.name);
                if (found != g.match.end()) changed.push_back(*found);
            }
            return changed;
        }

        bool RunVerb(const Command &cmd, std::string &message)
        {
            Load();
            if (cmd.args.size() == 1)
            {
                std::vector<std::pair<std::string, std::string>> changed = Changed();
                std::string list;
                for (const std::pair<std::string, std::string> &entry : changed) list += (list.empty() ? "" : "; ") + entry.first + " " + entry.second;
                message = changed.empty() ? std::string("the fine settings are all at their defaults (tune <name> shows one)")
                                          : "fine settings off their defaults: " + list;
                if (g.inMatch)
                {
                    std::string match;
                    for (const std::pair<const std::string, std::string> &entry : g.match) match += (match.empty() ? "" : "; ") + entry.first + " " + entry.second;
                    message += ". This match's (the host's): " + (match.empty() ? std::string("the defaults") : match);
                }
                return true;
            }
            const std::string name = cmd.args[1];
            const Setting *setting = Find(name);
            if (!setting)
            {
                std::string names;
                for (const Setting &known : SETTINGS) names += (names.empty() ? "" : ", ") + std::string(known.name);
                message = "no fine setting '" + name + "'; there are: " + names;
                return false;
            }
            if (cmd.args.size() == 2)
            {
                message = name + " " + (Text(name).empty() ? std::string("(none)") : Text(name)) + " (default " +
                          (Default(*setting) == " " ? std::string("none") : Default(*setting)) + "): " + setting->help;
                return true;
            }
            if (Net::IsConnected())
            {
                message = "a duel is on: its fine settings are the host's from its start (change them before hosting)";
                return false;
            }
            if (cmd.args[2] == "default")
            {
                Reset(name);
                message = name + " back to its default";
                return true;
            }
            std::string value = cmd.raw[2];
            for (size_t i = 3; i < cmd.raw.size(); ++i) value += "," + cmd.raw[i];
            std::string why;
            if (!Set(name, value, why))
            {
                message = "usage: tune " + name + " <" + why + ">|default";
                return false;
            }
            message = name + " " + (Text(name).empty() ? std::string("(none)") : Text(name)) + " when you host";
            return true;
        }

        // ---------------------------------------------------------------------------------------------------------
        // Presets
        // ---------------------------------------------------------------------------------------------------------

        // A preset's file name: letters, digits, - and _ (spaces become _), 32 at most.
        static std::string PresetWord(const std::string &name)
        {
            std::string word;
            for (char c : name)
            {
                if (std::isalnum((unsigned char)c) || c == '-' || c == '_') word += c;
                else if (c == ' ' && !word.empty() && word.back() != '_') word += '_';
                if (word.size() >= 32) break;
            }
            return word;
        }

        static std::string PresetPath(const std::string &word)
        {
            return std::string(PRESET_FOLDER) + "/" + word + ".cfg";
        }

        static bool SavePreset(const std::string &name, std::string &message)
        {
            Load();
            const std::string word = PresetWord(name);
            if (word.empty())
            {
                message = "usage: preset save <name> (letters, digits, - and _)";
                return false;
            }
#ifdef _WIN32
            _mkdir(PRESET_FOLDER);
#else
            mkdir(PRESET_FOLDER, 0755);
#endif
            std::ofstream file(PresetPath(word), std::ios::trunc);
            if (!file)
            {
                message = "can't write " + PresetPath(word);
                return false;
            }
            std::time_t now = std::time(nullptr);
            char date[16];
            std::strftime(date, sizeof(date), "%Y-%m-%d", std::localtime(&now));
            file << "# FTL: Duels preset \"" << word << "\" (" << date << ")\n";
            file << "# The match, as HOST DUEL sets it (duels.cfg keeps the same lines)\n";
            for (const std::pair<std::string, std::string> &line : Rounds::SettingLines()) file << line.first << " " << line.second << "\n";
            char xp[16];
            std::snprintf(xp, sizeof(xp), "%g", Match::CrewXpSetting());
            file << "xp " << xp << "\n\n";
            file << "# Fine settings (docs/design/settings.md): a line changes one; \"#\" in front shows its default.\n";
            for (const Setting &setting : SETTINGS)
            {
                std::map<std::string, std::string>::const_iterator found = g.own.find(setting.name);
                std::string fallback = Default(setting);
                file << "# " << setting.help << "\n";
                if (found != g.own.end()) file << setting.name << " " << (found->second == " " ? std::string("none") : found->second) << "\n";
                else file << "# " << setting.name << " " << (fallback == " " ? std::string("none") : fallback) << "\n";
            }
            if (!file)
            {
                message = "can't write " + PresetPath(word);
                return false;
            }
            Log("Tune: the preset %s saved (%u fine setting(s) off their defaults)", PresetPath(word).c_str(), (unsigned)g.own.size());
            message = "preset " + word + " saved (" + PresetPath(word) + ")";
            return true;
        }

        static bool LoadPreset(const std::string &name, std::string &message)
        {
            Load();
            const std::string word = PresetWord(name);
            std::ifstream file(PresetPath(word));
            if (word.empty() || !file)
            {
                message = "no preset " + name + " (preset list shows them)";
                return false;
            }
            if (Net::IsConnected())
            {
                message = "a duel is on: its settings are the host's from its start";
                return false;
            }
            // A preset is the whole configuration: the fine settings it doesn't change are at their defaults.
            for (const Setting &setting : SETTINGS) Reset(setting.name);
            std::string line, left;
            int match = 0, fine = 0;
            while (std::getline(file, line))
            {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                size_t start = line.find_first_not_of(" \t");
                if (start == std::string::npos || line[start] == '#') continue;
                std::string text = line.substr(start);
                size_t space = text.find_first_of(" \t");
                std::string key = text.substr(0, space);
                size_t valueStart = space == std::string::npos ? std::string::npos : text.find_first_not_of(" \t", space);
                std::string value = valueStart == std::string::npos ? std::string() : text.substr(valueStart);
                std::string why;
                bool ok;
                if (key == "xp")
                {
                    std::string xpMessage;
                    ok = Match::SetCrewXp((float)std::atof(value.c_str()), xpMessage);
                    if (!ok) why = xpMessage;
                }
                else if (key.compare(0, 6, "match_") == 0)
                {
                    ok = value.empty() || Rounds::ApplySetting(key, value, why);
                    if (ok) ++match;
                }
                else
                {
                    const Setting *setting = Find(key);
                    ok = setting && Set(key, value, why);
                    if (!setting) why = "not a setting";
                    if (ok) ++fine;
                }
                if (!ok)
                {
                    left += (left.empty() ? "" : "; ") + key + " (" + why + ")";
                    Log("Tune: the preset's %s left out: %s", key.c_str(), why.c_str());
                }
            }
            Rounds::KeepSettings();
            Log("Tune: the preset %s loaded (%d match setting(s), %d fine setting(s))", PresetPath(word).c_str(), match, fine);
            message = "preset " + word + " loaded: " + std::to_string(match) + " match setting(s), " + std::to_string(fine) +
                      " fine setting(s) off their defaults" + (left.empty() ? std::string() : "; left out: " + left);
            return true;
        }

        bool RunPresetVerb(const Command &cmd, std::string &message)
        {
            if (ArgIs(cmd, 1, "save") && cmd.raw.size() > 2) return SavePreset(cmd.raw[2], message);
            if (ArgIs(cmd, 1, "load") && cmd.raw.size() > 2) return LoadPreset(cmd.raw[2], message);
            if (cmd.args.size() == 1 || ArgIs(cmd, 1, "list"))
            {
                namespace fs = boost::filesystem;
                boost::system::error_code error;
                std::vector<std::string> names;
                for (fs::directory_iterator it(PRESET_FOLDER, error), end; !error && it != end; it.increment(error))
                {
                    std::string file = it->path().filename().string();
                    if (file.size() > 4 && file.compare(file.size() - 4, 4, ".cfg") == 0) names.push_back(file.substr(0, file.size() - 4));
                }
                std::sort(names.begin(), names.end());
                std::string list;
                for (const std::string &name : names) list += (list.empty() ? "" : ", ") + name;
                message = names.empty() ? std::string("no presets (preset save <name> makes one, in presets\\)") : "presets: " + list;
                return true;
            }
            message = "usage: preset list | preset save <name> | preset load <name>";
            return false;
        }
    }
}

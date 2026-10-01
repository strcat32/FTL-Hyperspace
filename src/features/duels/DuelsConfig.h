#pragma once

#include <string>
#include <vector>

// The player's FTL: Duels settings, in duels.cfg in the game folder: one setting per line, "#" starts a comment.
//   name <player name>          the name the other player sees (set with the "name" command; later the Steam name)
//   master <server>[:port]      the master server (roadmap AZ; ftl-duels.link, written into the file the first time
//                               the relays are needed): the project's public relay, after the file's own; later it
//                               also hands out further relays, by region (the server's GET /api/relays)
//   relay <server>[:port]       a relay server (any number of lines), tried before the master's
//   match_rounds <n>, match_prep <s>, match_stall <s>, match_permadeath on|off, match_env <mode>, match_free on|off,
//   xp <factor>                 the host's settings for the next duel, as the "match" and "xp" commands last set them
//                               (roadmap U; a test scenario doesn't change them)
//   tutorial off                the main menu's tutorial box stays closed ("Don't show this again", DuelsMenu.cpp)
namespace Duels
{
    namespace Config
    {
        // Reads duels.cfg (once; later calls do nothing). A missing file is an empty one.
        void Load();

        // The saved player name, or "".
        const std::string &PlayerName();
        // Saves the player name (the file's other lines stay as they are).
        void SavePlayerName(const std::string &name);

        // Any one-value setting ("" when the file has none), and saving one (its line replaced, or added).
        std::string Value(const std::string &key);
        void SaveValue(const std::string &key, const std::string &value);
        // A setting's lines gone from the file (a fine setting back at its default, DuelsTune.cpp).
        void RemoveValue(const std::string &key);

        // The master server (roadmap AZ): duels.cfg's, or ftl-duels.link for a file without one (then written into it,
        // when the settings come from the file).
        const std::string &Master();
        // Its web address (roadmap BG): https://<master>, or the address as written when it is one ("http://127.0.0.1:8099",
        // a test's stand-in master: its host is the relay then).
        std::string MasterUrl();
        // Another master for this run only (the "master" command; a test's local master): duels.cfg keeps its own.
        void UseMaster(const std::string &master);
        // The relay servers: the file's, in its order, then the master (the project's public relay).
        const std::vector<std::string> &Relays();
        // The server is the master's (the project's public relay).
        bool IsPublicRelay(const std::string &server);
    }
}

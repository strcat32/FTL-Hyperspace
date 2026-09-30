#pragma once

#include <string>
#include <vector>

// The player's FTL: Duels settings, in duels.cfg in the game folder: one setting per line, "#" starts a comment.
//   name <player name>          the name the other player sees (set with the "name" command; later the Steam name)
//   relay <server>[:port]       a relay server (any number of lines); the project's public relay comes after them.
//                               The first one is used unless "relay" picks another (all of them with roadmap 3.5)
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

        // The relay servers: the file's, in its order, then the project's public relay.
        const std::vector<std::string> &Relays();
    }
}

#pragma once

#include <string>
#include <vector>

// The player's FTL: Duels settings, in duels.cfg in the game folder: one setting per line, "#" starts a comment.
//   name <player name>          the name the other player sees (set with the "name" command; later the Steam name)
//   relay <server>[:port]       a relay server; the first one is used unless "relay" picks another
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

        // The relay servers, in the file's order.
        const std::vector<std::string> &Relays();
    }
}

#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Hidden information (roadmap 4.5; docs/design/hidden-information.md in the FTL: Duels repository): what the opponent
// can see of our own ship, worked out by our game from what it knows of theirs, so that the state we send carries no
// more. FTL's rules: sensors at 2 show the interior (rooms and crew), at 3 the weapons' charge, at 4 the systems'
// power, when they work (powered, not damaged away, not hacked, not in a nebula or an ion storm) and our ship isn't
// cloaked; telepathic crew show the interior anyway, the Life Scanner the crew; crew aboard show the rooms they are in.
namespace Duels
{
    namespace Vision
    {
        struct Seen
        {
            bool interior = false;      // rooms and crew
            bool lifeforms = false;     // crew, without the rooms' state (the interior, or the Life Scanner)
            bool charge = false;        // the weapons' charge
            bool power = false;         // the systems' power
            std::vector<bool> rooms;    // rooms seen anyway: their crew aboard, ours under their mind control
            int sensors = 0;            // their sensors' working level

            bool Room(int room) const { return room >= 0 && room < (int)rooms.size() && rooms[room]; }
            bool RoomState(int room) const { return interior || Room(room); }
            bool Crew(int room) const { return lifeforms || Room(room); }
        };

        // Each state we send (DuelsMatch.cpp): worked out anew; a change goes to the log.
        const Seen &Update();
        const Seen &Current();
        void Reset();
        // For the trace (duels_sync.csv): "i" interior, "l" lifeforms, "c" charge, "p" power, then the rooms seen.
        std::string Signature();
        std::string Status();
    }
}

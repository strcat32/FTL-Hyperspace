#include "Global.h"
#include "Duels.h"
#include "DuelsCrew.h"
#include "DuelsEnvironment.h"
#include "DuelsMatch.h"
#include "DuelsNet.h"
#include "DuelsVision.h"

#include <algorithm>
#include <sstream>

namespace Duels
{
    namespace Vision
    {
        static Seen g_seen, g_logged;
        static bool g_loggedOnce = false;
        static int g_fullScopes = 0;

        // Everything seen: the interior, the crew, the charge, the power, every room.
        static const Seen &AllSeen()
        {
            static Seen all;
            all.interior = all.lifeforms = all.charge = all.power = true;
            ShipManager *own = G_->GetShipManager(0);
            all.rooms.assign(own ? own->ship.vRoomList.size() : 0, true);
            all.sensors = g_seen.sensors;
            return all;
        }

        FullScope::FullScope() { ++g_fullScopes; }
        FullScope::~FullScope() { --g_fullScopes; }

        // Their sensors' working level, as our copy of their ship has it from their state: its power within its damage,
        // and a level more while manned (FTL's GetEffectivePower: level 3 manned sees the power use; their state says
        // so, DG: our copy of the crew member there is hidden while we don't see inside their ship); none while our
        // hacking drone pulses there, ion-locked away, or in a nebula or an ion storm (FTL limits sensors to nothing
        // there, on both ships).
        static int TheirSensors(ShipManager *replica)
        {
            ShipSystem *sensors = replica ? replica->GetSystem(SYS_SENSORS) : nullptr;
            if (!sensors) return 0;
            uint8_t kind = Environment::ActiveKind();
            if (kind == Environment::NEBULA || kind == Environment::STORM) return 0;
            if (sensors->iHackEffect >= 2) return 0;
            int level = std::max(0, std::min(replica->GetSystemPower(SYS_SENSORS), sensors->healthState.first));
            if (level > 0 && Match::ReplicaManned(replica, SYS_SENSORS)) ++level;
            return level;
        }

        const Seen &Update()
        {
            Seen seen;
            ShipManager *own = G_->GetShipManager(0);
            ShipManager *replica = G_->GetShipManager(1);
            if (!own || !replica)
            {
                g_seen = seen;
                return g_seen;
            }
            seen.sensors = TheirSensors(replica);
            bool cloaked = own->cloakSystem && own->cloakSystem->bTurnedOn;
            // Telepathic crew of theirs, aboard their ship or ours.
            bool telepathic = false;
            for (CrewMember *crew : replica->vCrewList)
            {
                if (crew && !crew->bDead && crew->iShipId == 1 && crew->IsTelepathic()) telepathic = true;
            }
            for (const std::pair<uint16_t, CrewMember*> &guest : Crew::Guests())
            {
                if (guest.second && !guest.second->bDead && guest.second->IsTelepathic()) telepathic = true;
            }
            seen.interior = telepathic || (seen.sensors >= 2 && !cloaked);
            seen.lifeforms = seen.interior || replica->HasAugmentation("LIFE_SCANNER");
            seen.charge = seen.sensors >= 3 && !cloaked;
            seen.power = seen.sensors >= 4;
            // Rooms seen anyway: their crew (and drones) aboard our ship, ours under their mind control.
            seen.rooms.assign(own->ship.vRoomList.size(), false);
            for (CrewMember *crew : own->vCrewList)
            {
                if (!crew || crew->bDead) continue;
                bool theirs = crew->iShipId != 0 || Crew::IsGuest(crew);
                bool controlled = crew->iShipId == 0 && crew->bMindControlled;
                if ((theirs || controlled) && crew->iRoomId >= 0 && crew->iRoomId < (int)seen.rooms.size()) seen.rooms[crew->iRoomId] = true;
            }
            g_seen = seen;

            if (!g_loggedOnce || seen.interior != g_logged.interior || seen.lifeforms != g_logged.lifeforms || seen.charge != g_logged.charge ||
                seen.power != g_logged.power)
            {
                std::string who = Net::PeerName().empty() ? std::string("The opponent") : Net::PeerName();
                Log("Vision: %s sees %s (their sensors %d%s%s%s)", who.c_str(),
                    seen.interior ? (seen.power ? "our ship's interior, its weapons' charge and its power"
                                     : seen.charge ? "our ship's interior and its weapons' charge" : "our ship's interior")
                                  : seen.lifeforms ? "our crew (a Life Scanner)" : "nothing inside our ship",
                    seen.sensors, cloaked ? ", we are cloaked" : "", telepathic ? ", telepathic crew" : "",
                    Environment::ActiveKind() == Environment::NEBULA || Environment::ActiveKind() == Environment::STORM ? ", in a nebula" : "");
                g_logged = seen;
                g_loggedOnce = true;
            }
            return g_seen;
        }

        const Seen &Current()
        {
            return g_fullScopes > 0 ? AllSeen() : g_seen;
        }

        uint8_t Flags()
        {
            const Seen &seen = Current();
            return (uint8_t)((seen.interior ? SEES_INTERIOR : 0) | (seen.lifeforms ? SEES_LIFEFORMS : 0) |
                             (seen.charge ? SEES_CHARGE : 0) | (seen.power ? SEES_POWER : 0));
        }

        bool PowerHidden(int systemType, bool needsPower)
        {
            if (g_fullScopes > 0 || g_seen.power) return false;
            switch (systemType)
            {
                case SYS_SHIELDS:
                case SYS_ENGINES:
                case SYS_CLOAKING:
                case SYS_HACKING:
                case SYS_MIND:
                    return false;
                default:
                    return needsPower || systemType >= SYS_CUSTOM_FIRST;
            }
        }

        void Reset()
        {
            g_seen = Seen();
            g_logged = Seen();
            g_loggedOnce = false;
        }

        std::string Signature()
        {
            std::string text = std::string(g_seen.interior ? "i" : "") + (g_seen.lifeforms ? "l" : "") + (g_seen.charge ? "c" : "") +
                               (g_seen.power ? "p" : "");
            for (size_t room = 0; room < g_seen.rooms.size(); ++room)
            {
                if (g_seen.rooms[room]) text += " " + std::to_string(room);
            }
            return text.empty() ? "-" : text;
        }

        std::string Status()
        {
            return "vision: they see " + Signature() + " (their sensors " + std::to_string(g_seen.sensors) + ")";
        }
    }
}

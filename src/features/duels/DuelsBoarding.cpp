#include "Global.h"
#include "Duels.h"
#include "DuelsBoarding.h"
#include "DuelsCrew.h"
#include "DuelsNet.h"
#include "DuelsTrace.h"
#include "DuelsWire.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <sstream>

namespace Duels
{
    namespace Boarding
    {
        static const int SKILLS = 6;                   // piloting, engines, shields, weapons, repair, combat
        static const double RETURN_WAIT_MS = 10000.0;  // how long a return report waits for its crew member to be home

        struct Returned
        {
            uint16_t id = 0;
            bool alive = false;
            int health = 0;
            uint8_t skills[SKILLS][2] = {};
            double since = 0.0;
        };

        struct BoardingState
        {
            std::vector<Returned> returns;   // reports waiting for the crew member to arrive home (the teleport takes a moment)
            std::map<const CrewMember*, uint16_t> returning;   // ours on their way home (only compared, never read)
            uint32_t sent = 0, received = 0, recalled = 0, recallsReceived = 0, returned = 0;
        };

        static BoardingState g_board;

        void Reset()
        {
            g_board = BoardingState();
        }

        static bool InDuel()
        {
            return Net::IsConnected() && G_->GetShipManager(0) && G_->GetShipManager(1);
        }

        static void WriteSkills(Writer &w, const CrewMember *crew)
        {
            for (int skill = 0; skill < SKILLS; ++skill)
            {
                bool known = crew && skill < (int)crew->blueprint.skillLevel.size();
                w.U8((uint8_t)std::max(0, known ? crew->blueprint.skillLevel[skill].first : 0));
                w.U8((uint8_t)std::max(0, known ? crew->blueprint.skillLevel[skill].second : 0));
            }
        }

        static void ReadSkills(Reader &r, uint8_t skills[SKILLS][2])
        {
            for (int skill = 0; skill < SKILLS; ++skill)
            {
                skills[skill][0] = r.U8();
                skills[skill][1] = r.U8();
            }
        }

        static void ApplySkills(CrewMember *crew, const uint8_t skills[SKILLS][2])
        {
            for (int skill = 0; skill < SKILLS && skill < (int)crew->blueprint.skillLevel.size(); ++skill)
            {
                crew->blueprint.skillLevel[skill].first = skills[skill][0];
                crew->blueprint.skillLevel[skill].second = skills[skill][1];
            }
        }

        // ---------------------------------------------------------------------------------------------------------
        // Our side: our crew go aboard, get orders (DuelsMind.cpp), come back
        // ---------------------------------------------------------------------------------------------------------

        void OnCrewArrived(ShipManager *ship, CrewMember *crew, int room)
        {
            if (!InDuel() || !crew || crew->iShipId != 0) return;
            ShipManager *own = G_->GetShipManager(0);
            ShipManager *replica = G_->GetShipManager(1);
            if (ship == replica && Crew::AwayId(crew) < 0)
            {
                int id = Crew::BoardAway(crew);
                if (id < 0)
                {
                    Log("Boarding: a crew member of ours without an id went aboard; the opponent is not told");
                    return;
                }
                int where = crew->iRoomId >= 0 ? crew->iRoomId : room;
                Writer w;
                w.U8(1);
                w.U16((uint16_t)id);
                w.Str(crew->species);
                w.Str(crew->GetName());
                w.Bool(crew->blueprint.male);
                WriteSkills(w, crew);
                w.U16((uint16_t)std::max(0L, std::lround(crew->health.first)));
                w.I8((int8_t)where);
                Net::Send(MSG_BOARD, w, true);
                ++g_board.sent;
                Log("Boarding: our crew member %d went aboard the opponent's ship (room %d)", id, where);
            }
            else if (ship == own && g_board.returning.count(crew))
            {
                uint16_t id = g_board.returning[crew];
                g_board.returning.erase(crew);
                Crew::CameHome(id);
                Log("Boarding: our crew member %u is back home", (unsigned)id);
            }
        }

        void AfterTeleport(CompleteShip *ship, int command)
        {
            static const int TELE_ARRIVE = 2;   // Hyperspace: take crew back from the other ship
            WorldManager *world = G_->GetWorld();
            if (!InDuel() || !ship || !world || ship != world->playerShip || command != TELE_ARRIVE) return;
            std::vector<uint16_t> ids;
            for (CrewMember *crew : ship->arrivingParty)
            {
                int id = Crew::AwayId(crew);
                if (id < 0) continue;
                ids.push_back((uint16_t)id);
                g_board.returning[crew] = (uint16_t)id;
            }
            if (ids.empty()) return;
            Writer w;
            w.U8((uint8_t)ids.size());
            for (uint16_t id : ids) w.U16(id);
            Net::Send(MSG_RECALL, w, true);
            g_board.recalled += (uint32_t)ids.size();
            Log("Boarding: our teleporter takes %u crew back", (unsigned)ids.size());
        }

        static void OnReturned(Reader &r)
        {
            std::vector<Returned> reports(r.U8());
            for (Returned &report : reports)
            {
                report.id = r.U16();
                report.alive = r.Bool();
                report.health = r.U16();
                ReadSkills(r, report.skills);
                report.since = WallMs();
            }
            if (!r.Ok()) return;
            g_board.returns.insert(g_board.returns.end(), reports.begin(), reports.end());
        }

        // A return report counts once its crew member is home: how the defender's game let them go.
        static void ApplyReturns()
        {
            double now = WallMs();
            for (auto it = g_board.returns.begin(); it != g_board.returns.end();)
            {
                CrewMember *crew = Crew::OwnById(it->id);
                if (!crew)
                {
                    if (now - it->since > RETURN_WAIT_MS)
                    {
                        Log("Boarding: crew member %u never came home", (unsigned)it->id);
                        it = g_board.returns.erase(it);
                    }
                    else ++it;
                    continue;
                }
                if (!it->alive && !crew->bDead)
                {
                    // It died aboard before our teleporter's call arrived there.
                    crew->health.first = 0.f;
                    crew->Kill(true);
                    Log("Boarding: crew member %u died aboard before the teleporter took them", (unsigned)it->id);
                }
                else if (it->alive)
                {
                    crew->health.first = std::min((float)it->health, crew->health.second);
                    ApplySkills(crew, it->skills);
                    Log("Boarding: crew member %u is home with %d health", (unsigned)it->id, it->health);
                }
                ++g_board.returned;
                it = g_board.returns.erase(it);
            }
        }

        // ---------------------------------------------------------------------------------------------------------
        // Their side: their crew aboard our ship are ours to simulate (guests)
        // ---------------------------------------------------------------------------------------------------------

        static void OnBoard(Reader &r)
        {
            int count = r.U8();
            for (int i = 0; i < count; ++i)
            {
                uint16_t id = r.U16();
                std::string species = r.Str();
                std::string name = r.Str();
                bool male = r.Bool();
                uint8_t skills[SKILLS][2];
                ReadSkills(r, skills);
                int health = r.U16();
                int room = r.I8();
                if (!r.Ok()) return;
                ShipManager *own = G_->GetShipManager(0);
                if (!own || Crew::Guest(id)) continue;
                CrewMember *crew = own->AddCrewMemberFromString(name, species, true, room, false, male);
                if (!crew)
                {
                    Log("Boarding: the opponent's %s could not come aboard (room %d)", species.c_str(), room);
                    continue;
                }
                ApplySkills(crew, skills);
                crew->health.first = std::min((float)health, crew->health.second);
                crew->StartTeleportArrive();
                Crew::AddGuest(id, crew);
                ++g_board.received;
                Log("Boarding: the opponent's crew member %u (%s) came aboard (room %d)", (unsigned)id, species.c_str(), room);
            }
        }

        static void OnRecall(Reader &r)
        {
            std::vector<uint16_t> ids(r.U8());
            for (uint16_t &id : ids) id = r.U16();
            if (!r.Ok()) return;
            ShipManager *own = G_->GetShipManager(0);
            Writer w;
            w.U8((uint8_t)ids.size());
            for (uint16_t id : ids)
            {
                CrewMember *crew = Crew::Guest(id);
                bool alive = crew && !crew->bDead && crew->health.first > 0.f;
                w.U16(id);
                w.Bool(alive);
                w.U16((uint16_t)(alive ? std::max(0L, std::lround(crew->health.first)) : 0));
                WriteSkills(w, alive ? crew : nullptr);
                // Gone from our ship as FTL's teleport moves crew: onto the replica (their ship), where they are the
                // puppet for their id again. The dead are FTL's to clean up.
                ShipManager *replica = G_->GetShipManager(1);
                if (alive && own && replica)
                {
                    crew->EmptySlot();
                    own->vCrewList.erase(std::remove(own->vCrewList.begin(), own->vCrewList.end(), crew), own->vCrewList.end());
                    int room = replica->GetSystemRoom(SYS_TELEPORTER);
                    crew->SetCurrentShip(1);
                    replica->AddCrewMember(crew, room >= 0 ? room : 0);
                    crew->StartTeleportArrive();
                    Crew::AdoptPuppet(id, crew);
                }
                else
                {
                    Crew::RemoveGuest(id);
                }
                ++g_board.recallsReceived;
                Log("Boarding: the opponent's teleporter took crew member %u back (%s)", (unsigned)id, alive ? "alive" : "dead");
            }
            Net::Send(MSG_RETURNED, w, true);
        }

        void OnMessage(uint8_t type, Reader &r)
        {
            if (type == MSG_BOARD) OnBoard(r);
            else if (type == MSG_RECALL) OnRecall(r);
            else if (type == MSG_RETURNED) OnReturned(r);
        }

        void OnFrame()
        {
            if (!g_board.returns.empty()) ApplyReturns();
        }

        bool MayDamage(const ShipSystem *system)
        {
            return Crew::MayRepair(system);
        }

        static bool g_aiRunning = false;

        void SetAiRunning(bool running)
        {
            g_aiRunning = running;
        }

        bool RefusesAiOrder(const CrewMember *crew)
        {
            return g_aiRunning && crew && (Crew::IsGuest(crew) || Crew::IsPuppet(crew));
        }

        bool RunVerb(const std::string &what, std::string &message)
        {
            WorldManager *world = G_->GetWorld();
            CompleteShip *ours = world ? world->playerShip : nullptr;
            ShipManager *own = G_->GetShipManager(0);
            if (!ours || !own || !own->teleportSystem)
            {
                message = "our ship has no teleporter";
                return false;
            }
            ShipManager *enemy = G_->GetShipManager(1);
            if (enemy && what.compare(0, 6, "order ") == 0)
            {
                // Our orders to our crew aboard the enemy's ship (they go to its owner, DuelsMind.cpp).
                int room = std::atoi(what.c_str() + 6);
                std::vector<CrewMember*> aboard;
                for (CrewMember *crew : enemy->vCrewList)
                {
                    if (crew && crew->iShipId == 0 && !crew->bDead) aboard.push_back(crew);
                }
                int sent = 0;
                for (CrewMember *crew : aboard)
                {
                    if (enemy->CommandCrewMoveRoom(crew, room)) ++sent;
                }
                message = std::to_string(sent) + " of our crew aboard sent to the enemy's room " + std::to_string(room);
                return sent > 0;
            }
            bool send = what.compare(0, 5, "send ") == 0;
            bool recall = what.compare(0, 7, "recall ") == 0;
            if (!send && !recall)
            {
                message = "usage: teleport send <room> | teleport recall <room> | teleport order <room>";
                return false;
            }
            int room = std::atoi(what.c_str() + (send ? 5 : 7));
            if (!own->teleportSystem->Charged())
            {
                message = "the teleporter is not charged";
                return false;
            }
            // FTL's own teleport: send takes the crew standing in our teleporter room, recall ours in their room.
            ours->InitiateTeleport(room, send ? 1 : 2);
            message = std::string(send ? "teleporting to" : "teleporting back from") + " the enemy's room " + std::to_string(room);
            return true;
        }

        std::string Status()
        {
            const BoardingState &b = g_board;
            std::ostringstream out;
            out << "boarding: sent " << b.sent << " received " << b.received << ", recalled " << b.recalled
                << " (reports " << b.returned << "), taken back from us " << b.recallsReceived;
            return out.str();
        }
    }
}

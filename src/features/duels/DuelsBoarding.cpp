#include "Global.h"
#include "Duels.h"
#include "DuelsBoarding.h"
#include "DuelsCrew.h"
#include "DuelsDrones.h"
#include "DuelsNet.h"
#include "DuelsTrace.h"
#include "DuelsWire.h"
#include "Drones.h"

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
            uint32_t sent = 0, received = 0, recalled = 0, recallsReceived = 0, returned = 0, teleportsRefused = 0;
        };

        static BoardingState g_board;

        static bool InDuel()
        {
            return Net::IsConnected() && G_->GetShipManager(0) && G_->GetShipManager(1);
        }

        // ---------------------------------------------------------------------------------------------------------
        // Boarding drones: our pod flies as FTL flies it, but lands where the defender's copy landed
        // ---------------------------------------------------------------------------------------------------------

        enum : uint8_t
        {
            POD_LANDED = 1,      // MSG_POD_RESULT: the defender's copy landed (room, point); its robot is a guest there
            POD_DESTROYED = 2,   // MSG_POD_RESULT: shot down in the defender's space, or it could not launch
            POD_HEADING = 3      // MSG_POD_RESULT: the defender's copy picked where it lands (point), in the defender's space
        };

        // The replica's copy launches within a frame or two once the replica's drone system has the power; it keeps
        // trying for this long before the attacker hears that the pod is gone.
        static const double POD_LAUNCH_TIMEOUT_MS = 3000.0;
        // FTL moves a pod and lands it in the same frame, so ours waits once it is this close to where it would land
        // (a pod flies a few pixels a frame).
        static const float POD_HOLD_DISTANCE = 40.f;

        struct OurPod
        {
            BoarderPodDrone *pod = nullptr;
            uint16_t robot = 0;          // our id for its robot (the defender's guest entry uses it)
            bool deployed = false;       // as last seen
            bool answered = false;       // the defender said where its copy landed, or that it was shot down
            bool heading = false;        // the defender said where its copy goes (the point)
            bool landed = false;
            Pointf point = Pointf(0.f, 0.f);
            int room = -1;
            bool robotAway = false;      // our robot is on the replica, the puppet of the defender's guest entry
        };

        struct TheirPod
        {
            uint16_t robot = 0;
            int slot = -1;
            BoarderPodDrone *pod = nullptr;
            double since = 0.0;
            bool launched = false;
            bool headingSent = false;
            bool reported = false;
        };

        struct PodState
        {
            std::vector<OurPod> ours;              // by drone slot
            std::map<uint16_t, TheirPod> theirs;   // by the robot's id
            uint32_t launched = 0, launchesReceived = 0, landedReports = 0, destroyedReports = 0, held = 0;
        };

        static PodState g_pods;

        static BoarderPodDrone *PodInSlot(ShipManager *ship, int slot)
        {
            if (!ship || !ship->droneSystem || slot < 0 || slot >= (int)ship->droneSystem->drones.size()) return nullptr;
            Drone *drone = ship->droneSystem->drones[slot];
            return drone && drone->type == DRONE_BOARDER ? static_cast<BoarderPodDrone*>(drone) : nullptr;
        }

        static OurPod *OursFor(const BoarderPodDrone *pod)
        {
            for (OurPod &ours : g_pods.ours)
            {
                if (ours.pod == pod && ours.deployed) return &ours;
            }
            return nullptr;
        }

        static void SendPodResult(uint16_t robot, uint8_t result, int room, Pointf point)
        {
            Writer w;
            w.U16(robot);
            w.U8(result);
            w.I8((int8_t)room);
            w.F32(point.x);
            w.F32(point.y);
            Net::Send(MSG_POD_RESULT, w, true);
            if (result == POD_LANDED) ++g_pods.landedReports;
            else if (result == POD_DESTROYED) ++g_pods.destroyedReports;
        }

        // Our side: a pod leaving goes to the defender, with the id its robot will have there.
        static void WatchOurPods()
        {
            ShipManager *own = G_->GetShipManager(0);
            if (!own || !own->droneSystem) return;
            std::vector<Drone*> &drones = own->droneSystem->drones;
            if (g_pods.ours.size() != drones.size()) g_pods.ours.resize(drones.size());
            for (size_t slot = 0; slot < drones.size(); ++slot)
            {
                BoarderPodDrone *pod = PodInSlot(own, (int)slot);
                OurPod &ours = g_pods.ours[slot];
                bool deployed = pod && pod->deployed && !pod->bDead;
                if (deployed && !ours.deployed)
                {
                    ours = OurPod();
                    ours.pod = pod;
                    ours.deployed = true;
                    ours.robot = Crew::NewRobotId();
                    Writer w;
                    w.U8((uint8_t)slot);
                    w.U16(ours.robot);
                    Net::Send(MSG_POD, w, true);
                    ++g_pods.launched;
                    Log("Boarding: our boarding drone %u left (robot %u)", (unsigned)slot, (unsigned)ours.robot);
                }
                else if (!deployed && ours.deployed)
                {
                    ours.deployed = false;
                    if (ours.robotAway) Crew::CameHome(ours.robot);   // its puppet entry goes (the robot is gone)
                    Log("Boarding: our boarding drone %u is gone (robot %u)", (unsigned)slot, (unsigned)ours.robot);
                }
                if (ours.deployed && ours.landed && !ours.robotAway && pod->bDeliveredDrone && pod->boarderDrone)
                {
                    Crew::RobotAway(pod->boarderDrone, ours.robot);
                    ours.robotAway = true;
                    Log("Boarding: our boarding drone %u landed in the opponent's room %d, as its copy there did",
                        (unsigned)slot, pod->boarderDrone->iRoomId);
                }
            }
        }

        bool MayPodLoop(BoarderPodDrone *pod)
        {
            if (!InDuel() || !pod || pod->iShipId != 0) return true;
            OurPod *ours = OursFor(pod);
            if (!ours || pod->bDeliveredDrone) return true;
            bool inTargetSpace = pod->currentSpace == pod->destinationSpace && pod->currentSpace != pod->iShipId;
            // Where the defender's copy goes (and lands): both games' spaces share their coordinates. FTL picks a
            // point of its own as the pod comes into the target's space; this one replaces it.
            if ((ours->heading || ours->landed) && inTargetSpace) pod->destinationLocation = ours->point;
            if (ours->answered) return true;   // it lands there, or it was shot down (FTL plays the blast in its loop)
            float dx = pod->destinationLocation.x - pod->currentLocation.x;
            float dy = pod->destinationLocation.y - pod->currentLocation.y;
            bool close = inTargetSpace && dx * dx + dy * dy <= POD_HOLD_DISTANCE * POD_HOLD_DISTANCE;
            if (close) ++g_pods.held;
            return !close;   // it waits there until the defender's word
        }

        static void OnPodResult(Reader &r)
        {
            uint16_t robot = r.U16();
            uint8_t result = r.U8();
            int room = r.I8();
            Pointf point;
            point.x = r.F32();
            point.y = r.F32();
            if (!r.Ok()) return;
            for (OurPod &ours : g_pods.ours)
            {
                if (!ours.deployed || ours.robot != robot || ours.answered) continue;
                if (result == POD_HEADING)
                {
                    ours.heading = true;
                    ours.point = point;
                    return;
                }
                ours.answered = true;
                if (result == POD_LANDED)
                {
                    ours.landed = true;
                    ours.point = point;
                    ours.room = room;
                    Log("Boarding: the opponent's game says our boarding drone (robot %u) landed in its room %d",
                        (unsigned)robot, room);
                }
                else
                {
                    // As a hit ends a pod: it blows up and is lost at once (the drone system's rebuild starts), not
                    // only when its blast is over, so both drone systems show it gone at the same moment.
                    if (ours.pod && !ours.pod->bDead)
                    {
                        ours.pod->BlowUp(false);
                        ours.pod->SetDestroyed(true, true);
                    }
                    Log("Boarding: the opponent's game says our boarding drone (robot %u) was shot down", (unsigned)robot);
                }
                return;
            }
            Log("Boarding: a word on robot %u, which is not ours in flight", (unsigned)robot);
        }

        // Their side: the replica's pod in that slot flies in our space, as the opponent's flies in theirs.
        static void OnPod(Reader &r)
        {
            int slot = r.U8();
            uint16_t robot = r.U16();
            if (!r.Ok()) return;
            ++g_pods.launchesReceived;
            TheirPod &theirs = g_pods.theirs[robot];
            theirs = TheirPod();
            theirs.robot = robot;
            theirs.slot = slot;
            theirs.pod = PodInSlot(G_->GetShipManager(1), slot);
            theirs.since = WallMs();
            Log("Boarding: the opponent's boarding drone %d left (robot %u)", slot, (unsigned)robot);
        }

        static void WatchTheirPods()
        {
            ShipManager *replica = G_->GetShipManager(1);
            for (auto it = g_pods.theirs.begin(); it != g_pods.theirs.end();)
            {
                TheirPod &theirs = it->second;
                BoarderPodDrone *pod = theirs.pod;
                if (!replica || !pod || !replica->droneSystem)
                {
                    if (!theirs.reported) SendPodResult(theirs.robot, POD_DESTROYED, -1, Pointf(0.f, 0.f));
                    it = g_pods.theirs.erase(it);
                    continue;
                }
                if (!theirs.launched)
                {
                    if (pod->deployed && !pod->bDead)
                    {
                        theirs.launched = true;
                    }
                    else if (WallMs() - theirs.since > POD_LAUNCH_TIMEOUT_MS)
                    {
                        Log("Boarding: the replica's boarding drone %d did not launch (drone system power %d/%d)",
                            theirs.slot, replica->droneSystem->powerState.first, replica->droneSystem->powerState.second);
                        SendPodResult(theirs.robot, POD_DESTROYED, -1, Pointf(0.f, 0.f));
                        it = g_pods.theirs.erase(it);
                        continue;
                    }
                    else
                    {
                        // At our ship (a pod made before the ships met has no target), launched the way the drone
                        // button does it (the owner paid the drone part).
                        ShipManager *own = G_->GetShipManager(0);
                        if (own && pod->movementTarget != &own->_targetable) pod->SetMovementTarget(&own->_targetable);
                        if (replica->droneSystem->drone_count <= 0) replica->droneSystem->drone_count = 1;
                        replica->PowerDrone(pod, 1, false, false);
                        ++it;
                        continue;
                    }
                }
                if (!theirs.reported)
                {
                    // In our space now, with the point FTL picked for it: the owner's pod goes there too.
                    if (!theirs.headingSent && pod->currentSpace == 0 && pod->destinationSpace == 0 && !pod->bDeliveredDrone)
                    {
                        theirs.headingSent = true;
                        SendPodResult(theirs.robot, POD_HEADING, -1, pod->destinationLocation);
                    }
                    if (pod->bDeliveredDrone && pod->boarderDrone)
                    {
                        theirs.reported = true;
                        Crew::AddGuest(theirs.robot, pod->boarderDrone);
                        SendPodResult(theirs.robot, POD_LANDED, pod->boarderDrone->iRoomId, pod->currentLocation);
                        Log("Boarding: the opponent's boarding drone (robot %u) landed in our room %d",
                            (unsigned)theirs.robot, pod->boarderDrone->iRoomId);
                    }
                    else if (pod->bDead || !pod->deployed)
                    {
                        SendPodResult(theirs.robot, POD_DESTROYED, -1, Pointf(0.f, 0.f));
                        Log("Boarding: the opponent's boarding drone (robot %u) was shot down here", (unsigned)theirs.robot);
                        it = g_pods.theirs.erase(it);
                        continue;
                    }
                    ++it;
                    continue;
                }
                // Aboard: gone with its robot; powered as the owner's drone is (their drone power is theirs).
                if (pod->bDead || !pod->deployed)
                {
                    Log("Boarding: the opponent's boarding drone (robot %u) is gone", (unsigned)theirs.robot);
                    it = g_pods.theirs.erase(it);
                    continue;
                }
                bool ownerDeployed = false, ownerPowered = false;
                if (Drones::OwnerDrone(theirs.slot, ownerDeployed, ownerPowered) && ownerDeployed)
                {
                    if (ownerPowered && !pod->powered) replica->PowerDrone(pod, 1, false, false);
                    else if (!ownerPowered && pod->powered) replica->DePowerDrone(pod, false);
                }
                ++it;
            }
        }

        void Reset()
        {
            g_board = BoardingState();
            g_pods = PodState();
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
            // A boarding drone's robot comes aboard with its pod (below), not through here.
            if (!InDuel() || !crew || crew->iShipId != 0 || crew->IsDrone()) return;
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
                w.U16((uint16_t)Crew::WireHealth(crew));
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

        bool RefusesTeleport(const CompleteShip *ship)
        {
            if (!InDuel() || !ship || ship->shipManager != G_->GetShipManager(1)) return false;
            if (g_board.teleportsRefused++ == 0) Log("Boarding: the replica's own teleport is refused (its owner decides)");
            return true;
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
                w.U16((uint16_t)(alive ? Crew::WireHealth(crew) : 0));
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
            else if (type == MSG_POD) OnPod(r);
            else if (type == MSG_POD_RESULT) OnPodResult(r);
        }

        void OnFrame()
        {
            if (!g_board.returns.empty()) ApplyReturns();
            WatchOurPods();
            WatchTheirPods();
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
            if (!g_aiRunning || !crew) return false;
            if (Crew::IsGuest(crew)) return !const_cast<CrewMember*>(crew)->IsDrone();   // FTL's IsDrone isn't const
            return Crew::IsPuppet(crew);
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
            const PodState &p = g_pods;
            out << "boarding: sent " << b.sent << " received " << b.received << ", recalled " << b.recalled
                << " (reports " << b.returned << "), taken back from us " << b.recallsReceived
                << ", the replica's own teleports refused " << b.teleportsRefused
                << ", boarding drones: launched " << p.launched << " received " << p.launchesReceived << ", reported landed "
                << p.landedReports << " shot down " << p.destroyedReports << ", waited " << p.held << " frames";
            return out.str();
        }
    }
}

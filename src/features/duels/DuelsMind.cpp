#include "Global.h"
#include "Duels.h"
#include "DuelsCrew.h"
#include "DuelsMind.h"
#include "DuelsNet.h"
#include "DuelsWire.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <vector>

namespace Duels
{
    namespace Mind
    {
        // The replica's control timer runs this far behind its owner's, so the owner's release ends it (as the lock
        // timers, DuelsMatch.cpp).
        static const float TIMER_LAG_S = 0.25f;

        struct MindState
        {
            uint8_t control = 0;           // our latest control (its id)
            uint8_t theirControl = 0;      // the control the replica holds our crew for
            bool haveTimer = false;        // the owner's timer, from the latest state
            uint8_t stateControl = 0;
            float timer = 0.f, goal = 0.f;
            uint32_t controlsSent = 0, controlsReceived = 0, crewTaken = 0, ordersSent = 0, ordersReceived = 0;
        };

        static MindState g_mind;

        void Reset()
        {
            g_mind = MindState();
        }

        static bool InDuel()
        {
            return Net::IsConnected() && G_->GetShipManager(0) && G_->GetShipManager(1);
        }

        void AfterInitiate(MindSystem *system, size_t controlledBefore)
        {
            ShipManager *own = G_->GetShipManager(0);
            if (!InDuel() || !own || system != own->mindSystem) return;
            std::vector<uint16_t> ids;
            for (size_t i = controlledBefore; i < system->controlledCrew.size(); ++i)
            {
                int id = Crew::PuppetId(system->controlledCrew[i]);
                if (id >= 0) ids.push_back((uint16_t)id);
            }
            if (ids.empty()) return;   // our own crew freed, or nobody taken
            ++g_mind.control;
            ++g_mind.controlsSent;
            Writer w;
            w.U8(g_mind.control);
            w.U8((uint8_t)ids.size());
            for (uint16_t id : ids) w.U16(id);
            Net::Send(MSG_MIND, w, true);
            Log("Mind: our mind control took %u of the opponent's crew (control %u)", (unsigned)ids.size(),
                (unsigned)g_mind.control);
        }

        static void OnMind(Reader &r)
        {
            uint8_t control = r.U8();
            std::vector<uint16_t> ids(r.U8());
            for (uint16_t &id : ids) id = r.U16();
            if (!r.Ok()) return;
            ++g_mind.controlsReceived;
            ShipManager *replica = G_->GetShipManager(1);
            MindSystem *mind = replica ? replica->mindSystem : nullptr;
            if (!mind)
            {
                Log("Mind: the opponent's mind control took our crew, but the replica has no mind control system");
                return;
            }
            // The replica's mind control holds them, with FTL's (Hyperspace's) own timer, boosts and release.
            int taken = 0;
            for (uint16_t id : ids)
            {
                CrewMember *crew = Crew::OwnById(id);
                if (!crew || crew->bDead || crew->bMindControlled) continue;
                crew->SetMindControl(true);
                mind->controlledCrew.push_back(crew);
                ++taken;
            }
            g_mind.theirControl = control;
            if (taken > 0)
            {
                mind->controlTimer.first = 0.f;
                G_->GetSoundControl()->PlaySoundMix("mindControl", -1.f, false);
            }
            g_mind.crewTaken += taken;
            Log("Mind: the opponent's mind control took %d of our crew (control %u)", taken, (unsigned)control);
        }

        // Crew orders: to our crew under the opponent's mind control (their ids are ours), or to the opponent's crew
        // aboard our ship (guests, by their ids).
        enum : uint8_t { ORDER_YOURS = 1, ORDER_MINE = 2 };

        static void OnOrder(Reader &r)
        {
            uint8_t kind = r.U8();
            uint16_t id = r.U16();
            int room = r.I16();
            if (!r.Ok()) return;
            ++g_mind.ordersReceived;
            CrewMember *crew = kind == ORDER_YOURS ? Crew::OwnById(id) : Crew::Guest(id);
            if (!crew || crew->bDead) return;
            // Ours only while under their control; a guest not while under ours.
            if (kind == ORDER_YOURS ? !crew->bMindControlled : crew->bMindControlled) return;
            ShipManager *where = G_->GetShipManager(crew->currentShipId);
            bool ok = where && where->CommandCrewMoveRoom(crew, room);
            Log("Mind: the opponent sent %s crew member %u to room %d%s", kind == ORDER_YOURS ? "our" : "their (aboard)",
                (unsigned)id, room, ok ? "" : " (refused)");
        }

        void OnMessage(uint8_t type, Reader &r)
        {
            if (type == MSG_MIND) OnMind(r);
            else if (type == MSG_CREW_ORDER) OnOrder(r);
        }

        bool OrderToOwner(ShipManager *ship, CrewMember *crew, int room)
        {
            if (!InDuel() || !crew || ship != G_->GetShipManager(1)) return false;
            uint8_t kind;
            int id = Crew::AwayId(crew);
            if (id >= 0)
            {
                // Ours aboard their ship; under their mind control they don't take our orders.
                if (crew->bMindControlled) return true;
                kind = ORDER_MINE;
            }
            else
            {
                id = crew->bMindControlled ? Crew::PuppetId(crew) : -1;
                if (id < 0) return false;
                kind = ORDER_YOURS;
            }
            Writer w;
            w.U8(kind);
            w.U16((uint16_t)id);
            w.I16((int16_t)room);
            Net::Send(MSG_CREW_ORDER, w, true);
            ++g_mind.ordersSent;
            Log("Mind: our order for the opponent's crew member %d (room %d) goes to them", id, room);
            return true;
        }

        void WriteState(Writer &w)
        {
            ShipManager *own = G_->GetShipManager(0);
            MindSystem *mind = own ? own->mindSystem : nullptr;
            w.Bool(mind != nullptr);
            if (!mind) return;
            w.U8(g_mind.control);
            w.F32(mind->controlTimer.first);
            w.F32(mind->controlTimer.second);
        }

        bool ReadState(Reader &r)
        {
            g_mind.haveTimer = r.Bool();
            if (g_mind.haveTimer)
            {
                g_mind.stateControl = r.U8();
                g_mind.timer = r.F32();
                g_mind.goal = r.F32();
            }
            return r.Ok();
        }

        void ApplyState()
        {
            ShipManager *replica = G_->GetShipManager(1);
            MindSystem *mind = replica ? replica->mindSystem : nullptr;
            // Only for the control the replica holds our crew for: a state sent before it would end it at once.
            if (!mind || !g_mind.haveTimer || mind->controlledCrew.empty() || g_mind.stateControl != g_mind.theirControl) return;
            // The owner's control is over (its time ran out, or its system lost power): so is the replica's.
            if (g_mind.timer >= g_mind.goal)
            {
                mind->ReleaseCrew();
                Log("Mind: the opponent's mind control %u is over; our crew are free", (unsigned)g_mind.theirControl);
                return;
            }
            mind->controlTimer.first = std::min(mind->controlTimer.second, std::max(0.f, g_mind.timer - TIMER_LAG_S));
        }

        bool RunVerb(const std::string &what, std::string &message)
        {
            ShipManager *own = G_->GetShipManager(0);
            ShipManager *enemy = G_->GetShipManager(1);
            MindSystem *mind = own ? own->mindSystem : nullptr;
            if (!mind)
            {
                message = "our ship has no mind control system";
                return false;
            }
            if (enemy && what.compare(0, 6, "order ") == 0)
            {
                // Our orders to the crew our mind control holds (on the enemy's ship: they go to its owner).
                int room = std::atoi(what.c_str() + 6);
                int sent = 0;
                for (CrewMember *crew : mind->controlledCrew)
                {
                    ShipManager *where = G_->GetShipManager(crew->currentShipId);
                    if (where && where->CommandCrewMoveRoom(crew, room)) ++sent;
                }
                message = std::to_string(sent) + " crew under our mind control sent to room " + std::to_string(room);
                return sent > 0;
            }
            if (!enemy || what.compare(0, 5, "room ") != 0)
            {
                message = !enemy ? "no enemy ship" : "usage: mind room <room> | mind order <room>";
                return false;
            }
            int room = std::atoi(what.c_str() + 5);
            std::vector<CrewMember*> crew;
            for (CrewMember *member : enemy->vCrewList)
            {
                if (member && !member->bDead && member->currentShipId == 1 && member->iRoomId == room) crew.push_back(member);
            }
            if (crew.empty())
            {
                message = "no crew in the enemy's room " + std::to_string(room);
                return false;
            }
            if (!mind->CanUse())
            {
                message = "mind control not ready (power " + std::to_string(mind->GetEffectivePower()) + ", lock " +
                          std::to_string(mind->iLockCount) + ")";
                return false;
            }
            mind->QueueMindControl(&crew, room, 1);
            message = "mind control on the enemy's room " + std::to_string(room) + " (" + std::to_string(crew.size()) + " crew)";
            return true;
        }

        std::string Signature(ShipManager *ship)
        {
            MindSystem *mind = ship ? ship->mindSystem : nullptr;
            if (!mind) return "-";
            // Our control holds puppets (their owner's ids); the replica's holds our crew (our ids): the same ids.
            std::vector<int> ids;
            for (CrewMember *crew : mind->controlledCrew) ids.push_back(ship->iShipId == 0 ? Crew::PuppetId(crew) : Crew::OwnId(crew));
            if (ids.empty()) return "idle";
            std::sort(ids.begin(), ids.end());
            std::ostringstream out;
            out << 'c';
            for (size_t i = 0; i < ids.size(); ++i) out << (i ? "+" : "") << ids[i];
            return out.str();
        }

        std::string Status()
        {
            const MindState &m = g_mind;
            std::ostringstream out;
            out << "mind control: sent " << m.controlsSent << " received " << m.controlsReceived << " (crew taken "
                << m.crewTaken << "), orders sent " << m.ordersSent << " received " << m.ordersReceived;
            return out.str();
        }
    }
}

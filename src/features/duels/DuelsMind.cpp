#include "Global.h"
#include "CustomSystems.h"
#include "Duels.h"
#include "DuelsCrew.h"
#include "DuelsMind.h"
#include "DuelsNet.h"
#include "DuelsRounds.h"
#include "DuelsTrace.h"
#include "DuelsView.h"
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
        // A control aimed at their ship waits this long for their answer before another may go.
        static const double ANSWER_TIMEOUT_MS = 3000.0;

        struct MindState
        {
            uint8_t control = 0;           // our latest control that took their crew (its id; the state names it)
            uint8_t asked = 0;             // our latest control aimed at their ship (their game picks)
            double askedMs = -1.0;         // when it went (-1: its answer came)
            uint8_t theirControl = 0;      // the control the replica holds our crew for
            bool haveTimer = false;        // the owner's timer, from the latest state
            uint8_t stateControl = 0;
            float timer = 0.f, goal = 0.f;
            uint32_t controlsSent = 0, controlsReceived = 0, crewTaken = 0, answers = 0, crewHeld = 0, ordersSent = 0,
                     ordersReceived = 0;
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

        bool OrdersFromUs()
        {
            return (InDuel() || Rounds::IsLocal()) && !Net::Replaying();
        }

        bool QueueToOwner(MindSystem *system, int room, int shipId)
        {
            ShipManager *own = G_->GetShipManager(0);
            if (!InDuel() || !own || system != own->mindSystem || shipId != 1) return false;
            // The click is taken: the button disarms, as FTL's own control does once it is aimed (roadmap CS: armed still,
            // the next click on the crew it took aimed another control at them instead of selecting them).
            system->SetArmed(0);
            // One at a time: while their game picks, another click waits for its answer.
            double now = WallMs();
            if (g_mind.askedMs >= 0.0 && now - g_mind.askedMs < ANSWER_TIMEOUT_MS)
            {
                Log("Mind: our mind control on their room %d waits: control %u has no answer yet", room, (unsigned)g_mind.asked);
                return true;
            }
            if (system->GetEffectivePower() <= 0) return true;   // FTL's takes nobody without power
            int count = CustomMindSystem::GetLevel(system).count;
            ++g_mind.asked;
            g_mind.askedMs = now;
            ++g_mind.controlsSent;
            Writer w;
            w.U8(g_mind.asked);
            w.I8((int8_t)room);
            w.U8((uint8_t)std::max(0, std::min(count, 255)));
            Net::Send(MSG_MIND, w, true);
            Log("Mind: our mind control on their room %d goes to them (control %u, up to %d crew)", room, (unsigned)g_mind.asked, count);
            return true;
        }

        static void WriteIds(Writer &w, const std::vector<uint16_t> &ids)
        {
            w.U8((uint8_t)ids.size());
            for (uint16_t id : ids) w.U16(id);
        }

        static std::vector<uint16_t> ReadIds(Reader &r)
        {
            std::vector<uint16_t> ids(r.U8());
            for (uint16_t &id : ids) id = r.U16();
            return ids;
        }

        // Their mind control on our room: we pick as Hyperspace's MindSystem::InitiateMindControl does. Our crew there
        // not yet under their control, and theirs aboard under ours (it frees them); telepaths resist; drones and the
        // dying are no targets; our super shields stop it unless they have the Zoltan bypass. Up to their level's
        // number, at random. The replica's mind control holds ours, with FTL's own timer, boosts and release.
        static void OnMind(Reader &r)
        {
            uint8_t control = r.U8();
            int room = r.I8();
            int count = r.U8();
            if (!r.Ok()) return;
            ++g_mind.controlsReceived;
            ShipManager *own = G_->GetShipManager(0);
            ShipManager *replica = G_->GetShipManager(1);
            MindSystem *mind = replica ? replica->mindSystem : nullptr;
            std::vector<uint16_t> taken, freed, resisted;
            if (!own || !mind)
            {
                Log("Mind: the opponent's mind control on our room %d, but the replica has no mind control system", room);
            }
            else
            {
                bool shielded = own->shieldSystem && own->shieldSystem->shields.power.super.first > 0 &&
                                replica->HasEquipment("ZOLTAN_BYPASS") <= 0;
                std::vector<CrewMember*> queue;
                for (CrewMember *crew : own->vCrewList)
                {
                    if (!crew || crew->currentShipId != 0 || crew->iRoomId != room) continue;
                    if (crew->OutOfGame() || crew->IsDead() || !crew->crewAnim || crew->crewAnim->status == 3 || crew->IsDrone()) continue;
                    bool guest = Crew::IsGuest(crew);
                    if (crew->IsTelepathic())
                    {
                        if (!guest)
                        {
                            crew->SetResisted(true);
                            int id = Crew::OwnId(crew);
                            if (id >= 0) resisted.push_back((uint16_t)id);
                        }
                        continue;
                    }
                    if (guest != crew->bMindControlled || shielded) continue;
                    queue.push_back(crew);
                }
                while ((int)(taken.size() + freed.size()) < count && !queue.empty())
                {
                    size_t index = random32() % queue.size();
                    CrewMember *crew = queue[index];
                    queue.erase(queue.begin() + index);
                    if (Crew::IsGuest(crew))
                    {
                        // Ours lets them go (FTL's MindSystem::OnLoop drops them from our control).
                        crew->SetMindControl(false);
                        G_->GetSoundControl()->PlaySoundMix("mindControlEnd", -1.f, false);
                        freed.push_back((uint16_t)Crew::GuestIdOf(crew));
                        continue;
                    }
                    int id = Crew::OwnId(crew);
                    if (id < 0) continue;
                    crew->SetMindControl(true);
                    mind->controlledCrew.push_back(crew);
                    taken.push_back((uint16_t)id);
                }
                if (!taken.empty())
                {
                    mind->controlTimer.first = 0.f;
                    G_->GetSoundControl()->PlaySoundMix("mindControl", -1.f, false);
                }
            }
            g_mind.theirControl = control;
            g_mind.crewTaken += (uint32_t)taken.size();
            Writer w;
            w.U8(control);
            WriteIds(w, taken);
            WriteIds(w, freed);
            WriteIds(w, resisted);
            Net::Send(MSG_MIND_TAKEN, w, true);
            Log("Mind: the opponent's mind control %u on our room %d took %u of our crew, freed %u of theirs, %u resisted",
                (unsigned)control, room, (unsigned)taken.size(), (unsigned)freed.size(), (unsigned)resisted.size());
        }

        // Their answer: our control holds the puppets it took from now on, as FTL's InitiateMindControl starts a
        // control (its time runs, the system is locked while it holds them); one that only freed ours cools down.
        static void OnTaken(Reader &r)
        {
            uint8_t control = r.U8();
            std::vector<uint16_t> taken = ReadIds(r);
            std::vector<uint16_t> freed = ReadIds(r);
            std::vector<uint16_t> resisted = ReadIds(r);
            if (!r.Ok()) return;
            ++g_mind.answers;
            if (control == g_mind.asked) g_mind.askedMs = -1.0;
            ShipManager *own = G_->GetShipManager(0);
            MindSystem *mind = own ? own->mindSystem : nullptr;
            if (!mind) return;
            int held = 0;
            for (uint16_t id : taken)
            {
                CrewMember *crew = Crew::PuppetById(id);
                if (!crew || crew->bDead) continue;
                if (!crew->bMindControlled) crew->SetMindControl(true);
                if (std::find(mind->controlledCrew.begin(), mind->controlledCrew.end(), crew) == mind->controlledCrew.end())
                {
                    mind->controlledCrew.push_back(crew);
                }
                ++held;
            }
            for (uint16_t id : resisted)
            {
                CrewMember *crew = Crew::PuppetById(id);
                if (crew && !crew->bDead) crew->SetResisted(true);
            }
            if (held > 0)
            {
                g_mind.control = control;
                mind->controlTimer.first = 0.f;
                mind->LockSystem(-1);
                G_->GetSoundControl()->PlaySoundMix("mindControl", -1.f, false);
            }
            else if (!freed.empty())
            {
                mind->controlTimer.first = mind->controlTimer.second;
                mind->LockSystem(CustomMindSystem::GetLevel(mind).lock);
            }
            g_mind.crewHeld += held;
            Log("Mind: our mind control %u took %d of the opponent's crew (%u named), freed %u of ours, %u resisted",
                (unsigned)control, held, (unsigned)taken.size(), (unsigned)freed.size(), (unsigned)resisted.size());
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
            else if (type == MSG_MIND_TAKEN) OnTaken(r);
            else if (type == MSG_CREW_ORDER) OnOrder(r);
        }

        bool OrderToOwner(ShipManager *ship, CrewMember *crew, int room)
        {
            if (!InDuel() || !crew || ship != G_->GetShipManager(1)) return false;
            uint8_t kind;
            int id = Crew::AwayId(crew);
            if (id >= 0)
            {
                // Ours aboard their ship; under their mind control they don't take our orders (roadmap 3.8: their game
                // holds them, and told us so with their guest state).
                if (crew->bMindControlled)
                {
                    Log("Mind: our crew member %d aboard their ship is under their mind control: our order waits for its end", id);
                    return true;
                }
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
            if (what.compare(0, 4, "own ") == 0)
            {
                // Our mind control on our own ship's room (roadmap 3.8): the opponent's crew aboard there (guests) are
                // ours while it lasts.
                int room = std::atoi(what.c_str() + 4);
                std::vector<CrewMember*> crew;
                for (CrewMember *member : own->vCrewList)
                {
                    if (member && !member->bDead && member->iShipId != 0 && member->iRoomId == room) crew.push_back(member);
                }
                if (crew.empty())
                {
                    message = "no opponent's crew in our room " + std::to_string(room);
                    return false;
                }
                if (!mind->CanUse())
                {
                    message = "mind control not ready (power " + std::to_string(mind->GetEffectivePower()) + ", lock " +
                              std::to_string(mind->iLockCount) + ")";
                    return false;
                }
                mind->QueueMindControl(&crew, room, 0);
                message = "mind control on our room " + std::to_string(room) + " (" + std::to_string(crew.size()) + " of their crew)";
                return true;
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
            if (enemy && what.compare(0, 3, "ui ") == 0)
            {
                // As a player does it: armed as FTL's button arms it, then a click on the room's centre on the screen.
                int room = std::atoi(what.c_str() + 3);
                CApp *app = G_->GetCApp();
                CommandGui *gui = app ? app->gui : nullptr;
                float x, y;
                Pointf center = room >= 0 && room < (int)enemy->ship.vRoomList.size() ? enemy->GetRoomCenter(room) : Pointf(0.f, 0.f);
                if (!gui || room < 0 || room >= (int)enemy->ship.vRoomList.size() || !View::ShipToScreen(1, center.x, center.y, x, y))
                {
                    message = "no such room of the enemy's on the screen";
                    return false;
                }
                if (!mind->CanUse())
                {
                    message = "mind control not ready (power " + std::to_string(mind->GetEffectivePower()) + ", lock " +
                              std::to_string(mind->iLockCount) + ")";
                    return false;
                }
                gui->combatControl.ArmMindControl(1);
                int armed = gui->combatControl.MindControlArmed();
                gui->MouseMove((int)x, (int)y);
                gui->LButtonDown((int)x, (int)y, false);
                gui->LButtonUp((int)x, (int)y, false);
                message = "mind control armed (" + std::to_string(armed) + ") and clicked on the enemy's room " + std::to_string(room) +
                          " at " + std::to_string((int)x) + "," + std::to_string((int)y) + " (selected room " +
                          std::to_string(gui->combatControl.selectedRoom) + ", armed now " +
                          std::to_string(gui->combatControl.MindControlArmed()) + ")";
                return true;
            }
            if (enemy && what.compare(0, 9, "uirclick ") == 0)
            {
                // The order's right-click on the room's centre (after mind uiorder: FTL found the room under the mouse in
                // the frames between, as it does when a player moves the mouse there).
                int room = std::atoi(what.c_str() + 9);
                CApp *app = G_->GetCApp();
                CommandGui *gui = app ? app->gui : nullptr;
                Pointf center = room >= 0 && room < (int)enemy->ship.vRoomList.size() ? enemy->GetRoomCenter(room) : Pointf(0.f, 0.f);
                float rx, ry;
                if (!gui || !View::ShipToScreen(1, center.x, center.y, rx, ry))
                {
                    message = "no such room of the enemy's on the screen";
                    return false;
                }
                size_t before = gui->crewControl.selectedCrew.size();
                // As a player's mouse that rests there: FTL finds the enemy room under it once a frame (CombatControl::
                // UpdateTarget), and its crew control takes that room at the next move (the test's mouse isn't the
                // system's cursor, which FTL follows between frames).
                gui->MouseMove((int)rx, (int)ry);
                gui->combatControl.UpdateTarget();
                gui->MouseMove((int)rx, (int)ry);
                std::string crewSide = " (crew control: room " + std::to_string(gui->crewControl.selectedRoom) +
                                       (gui->crewControl.selectedPlayerShip ? ", our ship" : ", their ship") + ")";
                gui->RButtonDown((int)rx, (int)ry, false);
                gui->RButtonUp((int)rx, (int)ry, false);
                message = "right-clicked the enemy's room " + std::to_string(room) + " at " + std::to_string((int)rx) + "," +
                          std::to_string((int)ry) + " (the room under the mouse: " + std::to_string(gui->combatControl.selectedRoom) +
                          "; " + std::to_string(before) + " crew selected before, " + std::to_string(gui->crewControl.selectedCrew.size()) +
                          " after)" + crewSide;
                return true;
            }
            if (enemy && what.compare(0, 8, "uiorder ") == 0)
            {
                // As a player orders them (roadmap CS): a click on the first crew member our control holds, where the screen
                // shows it (FTL selects it), then the mouse over the room's centre (mind uirclick <room> gives the order).
                int room = std::atoi(what.c_str() + 8);
                CApp *app = G_->GetCApp();
                CommandGui *gui = app ? app->gui : nullptr;
                Pointf center = room >= 0 && room < (int)enemy->ship.vRoomList.size() ? enemy->GetRoomCenter(room) : Pointf(0.f, 0.f);
                float rx, ry, cx, cy;
                if (!gui || mind->controlledCrew.empty() || !View::ShipToScreen(1, center.x, center.y, rx, ry))
                {
                    message = mind->controlledCrew.empty() ? "our mind control holds nobody" : "no such room of the enemy's on the screen";
                    return false;
                }
                CrewMember *crew = mind->controlledCrew.front();
                if (!View::ShipToScreen(crew->currentShipId, crew->x, crew->y, cx, cy))
                {
                    message = "the crew member isn't on the screen";
                    return false;
                }
                gui->MouseMove((int)cx, (int)cy);
                gui->LButtonDown((int)cx, (int)cy, false);
                gui->LButtonUp((int)cx, (int)cy, false);
                const std::vector<CrewMember*> &selected = gui->crewControl.selectedCrew;
                bool picked = std::find(selected.begin(), selected.end(), crew) != selected.end();
                std::ostringstream out;
                out << "clicked our controlled crew member (room " << crew->iRoomId << ") at " << (int)cx << "," << (int)cy << ": "
                    << selected.size() << " selected" << (picked ? ", it among them" : ", not it") << "; the mouse over the enemy's room "
                    << room << " at " << (int)rx << "," << (int)ry;
                gui->MouseMove((int)rx, (int)ry);
                message = out.str();
                return picked;
            }
            if (!enemy || what.compare(0, 5, "room ") != 0)
            {
                message = !enemy ? "no enemy ship" : "usage: mind room <room> | mind ui <room> | mind uiorder <room> | mind own <room> | mind order <room>";
                return false;
            }
            int room = std::atoi(what.c_str() + 5);
            std::vector<CrewMember*> crew;
            for (CrewMember *member : enemy->vCrewList)
            {
                if (member && !member->bDead && member->currentShipId == 1 && member->iRoomId == room) crew.push_back(member);
            }
            // In a network duel their game picks whom it takes (our copy may not see who is there).
            if (crew.empty() && !InDuel())
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

        bool RepairsForUs(const CrewMember *crew)
        {
            ShipManager *own = G_->GetShipManager(0);
            MindSystem *mind = own ? own->mindSystem : nullptr;
            if (!crew || !crew->bMindControlled || crew->iShipId == 0 || crew->currentShipId != 0 || !mind) return false;
            return std::find(mind->controlledCrew.begin(), mind->controlledCrew.end(), crew) != mind->controlledCrew.end();
        }

        std::string Signature(ShipManager *ship)
        {
            MindSystem *mind = ship ? ship->mindSystem : nullptr;
            if (!mind) return "-";
            // Our control holds puppets and guests (their owner's ids); the replica's holds our crew (our ids), and its
            // owner's game holds ours aboard their ship (we see them as away crew under their control): the same ids.
            std::vector<int> ids;
            for (CrewMember *crew : mind->controlledCrew)
            {
                int puppet = Crew::PuppetId(crew);
                ids.push_back(ship->iShipId == 0 ? (puppet >= 0 ? puppet : Crew::GuestIdOf(crew)) : Crew::OwnId(crew));
            }
            if (ship->iShipId == 1)
            {
                for (const std::pair<uint16_t, CrewMember*> &entry : Crew::AwayCrew())
                {
                    if (entry.second->bMindControlled) ids.push_back(entry.first);
                }
            }
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
            out << "mind control: sent " << m.controlsSent << " (answers " << m.answers << ", crew held " << m.crewHeld
                << ") received " << m.controlsReceived << " (crew taken " << m.crewTaken << "), orders sent " << m.ordersSent
                << " received " << m.ordersReceived;
            return out.str();
        }
    }
}

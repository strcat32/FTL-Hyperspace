#include "Global.h"
#include "Duels.h"
#include "DuelsHacking.h"
#include "DuelsBays.h"
#include "DuelsNet.h"
#include "DuelsTrace.h"
#include "DuelsWire.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace Duels
{
    namespace Hacking
    {
        enum : uint8_t
        {
            EVENT_LAUNCH = 1,   // MSG_HACK: launch id, the system's type
            EVENT_STOP = 2,     // MSG_HACK: launch id
            RESULT_ATTACHED = 1,
            RESULT_DESTROYED = 2,
            RESULT_MOVED = 3     // MSG_HACK_RESULT: launch id, result, the system's type it moved to (roadmap DB)
        };

        // The replica's pulse timer runs this far behind its owner's, so the owner's lock ends the pulse, not the
        // replica's own count (as the lock timers, DuelsMatch.cpp).
        static const float PULSE_TIMER_LAG_S = 0.25f;
        // The replica's hacking system should launch within a frame or two; if it can't (no power), the attacker
        // hears that the drone is gone.
        static const double LAUNCH_TIMEOUT_MS = 3000.0;

        struct HackState
        {
            // Ours (the attacker's side).
            bool wasHacking = false;
            const ShipSystem *target = nullptr;   // the replica's system our drone went for
            uint8_t launch = 0;                   // our latest launch
            bool attached = false;                // the defender said it attached: pulses may start
            // Theirs (our game hacks our ship with the replica's hacking system).
            bool pending = false;                 // a launch whose outcome we still have to report
            uint8_t theirLaunch = 0;
            int theirType = -1;
            double since = 0.0;
            bool launched = false;                // the replica's drone has left
            bool havePulse = false;
            float pulseTime = 0.f;                // the owner's pulse progress (the state)
            // The weapon or drone the replica's drone attached to in one of our bays (roadmap DB): the hack goes where it
            // goes (a key, only compared).
            const void *stuckTo = nullptr;
            uint32_t moves = 0;
            bool theirLost = false;               // we blew their drone up (an empty bay, its weapon gone): not attached
            uint32_t launchesSent = 0, launchesReceived = 0, attachedReports = 0, destroyedReports = 0,
                     pulsesRefused = 0;
        };

        static HackState g_hack;

        void Reset()
        {
            g_hack = HackState();
        }

        static const char *ResultName(uint8_t result)
        {
            return result == RESULT_ATTACHED ? "attached" : result == RESULT_MOVED ? "moved" : "destroyed";
        }

        // A weapon's or drone's name for the log.
        static std::string ItemName(ShipManager *ship, const ShipSystem *bay)
        {
            if (!ship || !bay) return "?";
            int number = Bays::BayNumber(bay->iSystemType) - 1;
            const void *item = Bays::ItemInBay(ship, bay);
            if (!item || number < 0) return "nothing";
            if (ship->weaponSystem)
                for (ProjectileFactory *weapon : ship->GetWeaponList())
                    if (weapon == item) return weapon->blueprint ? weapon->blueprint->name : "a weapon";
            if (ship->droneSystem)
                for (Drone *drone : ship->GetDroneList())
                    if (drone == item) return drone->blueprint ? drone->blueprint->name : "a drone";
            return "?";
        }

        static std::string SystemLabel(int type)
        {
            return type < 0 ? std::string("-") : ShipSystem::SystemIdToName(type);
        }

        static void SendResult(uint8_t launch, uint8_t result)
        {
            Writer w;
            w.U8(launch);
            w.U8(result);
            Net::Send(MSG_HACK_RESULT, w, true);
            if (result == RESULT_ATTACHED) ++g_hack.attachedReports;
            else ++g_hack.destroyedReports;
            Log("Hacking: the opponent's drone (launch %u) %s here", (unsigned)launch, ResultName(result));
        }

        // Our side: a launch or a stop of our hacking goes to the opponent.
        static void WatchOurs()
        {
            ShipManager *own = G_->GetShipManager(0);
            HackingSystem *hacking = own ? own->hackingSystem : nullptr;
            if (!hacking) return;
            bool active = hacking->bHacking && hacking->currentSystem;
            if (active && (!g_hack.wasHacking || hacking->currentSystem != g_hack.target))
            {
                g_hack.target = hacking->currentSystem;
                g_hack.attached = false;
                ++g_hack.launch;
                ++g_hack.launchesSent;
                Writer w;
                w.U8(EVENT_LAUNCH);
                w.U8(g_hack.launch);
                w.U8((uint8_t)hacking->currentSystem->iSystemType);
                Net::Send(MSG_HACK, w, true);
                Log("Hacking: our drone left for the opponent's %s (launch %u)",
                    SystemLabel(hacking->currentSystem->iSystemType).c_str(), (unsigned)g_hack.launch);
            }
            else if (!active && g_hack.wasHacking)
            {
                Writer w;
                w.U8(EVENT_STOP);
                w.U8(g_hack.launch);
                Net::Send(MSG_HACK, w, true);
                Log("Hacking: ours stopped (launch %u)", (unsigned)g_hack.launch);
                g_hack.target = nullptr;
                g_hack.attached = false;
            }
            g_hack.wasHacking = active;
        }

        // Their side: what became of the replica's drone in our space.
        static void WatchTheirs()
        {
            if (!g_hack.pending) return;
            ShipManager *replica = G_->GetShipManager(1);
            HackingSystem *hacking = replica ? replica->hackingSystem : nullptr;
            if (!hacking)
            {
                g_hack.pending = false;
                return;
            }
            if (!g_hack.launched && hacking->bHacking && hacking->queuedSystem == nullptr) g_hack.launched = true;
            if (g_hack.launched)
            {
                if (hacking->bHacking && hacking->drone.arrived)
                {
                    g_hack.pending = false;
                    // At a bay it attaches to the weapon or drone there now, and sticks to it (roadmap DB); at an empty
                    // bay it is lost (the attacker may send the next one at once).
                    ShipManager *own = G_->GetShipManager(0);
                    if (Bays::IsBay(hacking->currentSystem))
                    {
                        const void *item = Bays::ItemInBay(own, hacking->currentSystem);
                        if (!item)
                        {
                            Log("Hacking: the opponent's drone reached our empty %s: it is lost",
                                SystemLabel(hacking->currentSystem->iSystemType).c_str());
                            hacking->BlowHackingDrone();
                            g_hack.theirLost = true;
                            SendResult(g_hack.theirLaunch, RESULT_DESTROYED);
                            return;
                        }
                        g_hack.stuckTo = item;
                        Log("Hacking: the opponent's drone sticks to our %s in %s", ItemName(own, hacking->currentSystem).c_str(),
                            SystemLabel(hacking->currentSystem->iSystemType).c_str());
                    }
                    SendResult(g_hack.theirLaunch, RESULT_ATTACHED);
                }
                else if (!hacking->bHacking || hacking->drone.bDead)
                {
                    g_hack.pending = false;
                    SendResult(g_hack.theirLaunch, RESULT_DESTROYED);
                }
            }
            else if (WallMs() - g_hack.since > LAUNCH_TIMEOUT_MS)
            {
                Log("Hacking: the replica's hacking system did not launch (power %d, locked %d)",
                    hacking->GetEffectivePower(), hacking->iLockCount);
                hacking->queuedSystem = nullptr;
                g_hack.pending = false;
                SendResult(g_hack.theirLaunch, RESULT_DESTROYED);
            }
        }

        // Their attached drone sticks to the weapon or drone it attached to (roadmap DB): when that moves to another bay
        // (dragged in the weapons bar), the hack goes with it; when it leaves the bays (to the cargo), the drone is lost.
        static void FollowStuck()
        {
            if (!g_hack.stuckTo) return;
            ShipManager *own = G_->GetShipManager(0);
            ShipManager *replica = G_->GetShipManager(1);
            HackingSystem *hacking = replica ? replica->hackingSystem : nullptr;
            if (!own || !hacking || !hacking->bHacking || !hacking->currentSystem || !hacking->drone.arrived || hacking->drone.bDead)
            {
                g_hack.stuckTo = nullptr;
                return;
            }
            ShipSystem *bay = Bays::BayOfItem(own, g_hack.stuckTo);
            if (bay == hacking->currentSystem) return;
            ShipSystem *old = hacking->currentSystem;
            old->bUnderAttack = false;
            old->iHackEffect = 0;
            if (!bay)
            {
                Log("Hacking: what the opponent's drone stuck to left our %s: the drone is lost", SystemLabel(old->iSystemType).c_str());
                g_hack.stuckTo = nullptr;
                hacking->BlowHackingDrone();
                g_hack.theirLost = true;
                SendResult(g_hack.theirLaunch, RESULT_DESTROYED);
                return;
            }
            hacking->currentSystem = bay;
            ++g_hack.moves;
            Log("Hacking: the opponent's hack moves with our %s from %s to %s", ItemName(own, bay).c_str(),
                SystemLabel(old->iSystemType).c_str(), SystemLabel(bay->iSystemType).c_str());
            Writer w;
            w.U8(g_hack.theirLaunch);
            w.U8(RESULT_MOVED);
            w.U8((uint8_t)bay->iSystemType);
            Net::Send(MSG_HACK_RESULT, w, true);
        }

        // A hacking drone flies only once it is in the space's drone list. Hyperspace puts it there when the system
        // is added with an enemy present, or at a new location; a duel's ships meet without either (our ship may
        // have had hacking before the opponent's arrived).
        static void EnsureInSpace(ShipManager *ship, ShipManager *enemy)
        {
            WorldManager *world = G_->GetWorld();
            if (!world || !ship || !enemy || !ship->hackingSystem) return;
            std::vector<SpaceDrone*> &drones = world->space.drones;
            SpaceDrone *drone = &ship->hackingSystem->drone;
            if (std::find(drones.begin(), drones.end(), drone) != drones.end()) return;
            if (!ship->hackingSystem->drone.movementTarget) ship->hackingSystem->drone.SetMovementTarget(&enemy->_targetable);
            drones.push_back(drone);
            Log("Hacking: ship %d's hacking drone joins the space", ship->iShipId);
        }

        void OnFrame()
        {
            EnsureInSpace(G_->GetShipManager(0), G_->GetShipManager(1));
            EnsureInSpace(G_->GetShipManager(1), G_->GetShipManager(0));
            WatchOurs();
            WatchTheirs();
            FollowStuck();
        }

        static void OnLaunch(uint8_t launch, int type)
        {
            ++g_hack.launchesReceived;
            ShipManager *own = G_->GetShipManager(0);
            ShipManager *replica = G_->GetShipManager(1);
            HackingSystem *hacking = replica ? replica->hackingSystem : nullptr;
            ShipSystem *target = own ? own->GetSystem(type) : nullptr;
            if (!hacking || !target)
            {
                Log("Hacking: the opponent's drone went for our %s, but %s", SystemLabel(type).c_str(),
                    !hacking ? "the replica has no hacking system" : "we have no such system");
                SendResult(launch, RESULT_DESTROYED);
                return;
            }
            // FTL's own launch (HackingSystem::OnLoop): a drone already out is replaced. It refuses while the system
            // is locked or blocked; the owner's lock is on the replica already, and the owner could launch.
            int lock = hacking->iLockCount;
            bool blocked = hacking->bBlocked;
            hacking->iLockCount = 0;
            hacking->bBlocked = false;
            hacking->StartHacking(target);
            hacking->iLockCount = lock;
            hacking->bBlocked = blocked;
            g_hack.pending = true;
            g_hack.stuckTo = nullptr;
            g_hack.theirLost = false;
            g_hack.theirLaunch = launch;
            g_hack.theirType = type;
            g_hack.since = WallMs();
            g_hack.launched = false;
            Log("Hacking: the opponent's drone left for our %s (launch %u)", SystemLabel(type).c_str(), (unsigned)launch);
        }

        static void OnStop(uint8_t launch)
        {
            ShipManager *replica = G_->GetShipManager(1);
            HackingSystem *hacking = replica ? replica->hackingSystem : nullptr;
            if (launch == g_hack.theirLaunch)
            {
                g_hack.pending = false;
                g_hack.stuckTo = nullptr;
                g_hack.theirLost = false;
            }
            if (!hacking) return;
            hacking->queuedSystem = nullptr;
            if (hacking->bHacking) hacking->StopHacking();
            Log("Hacking: the opponent's hacking stopped (launch %u)", (unsigned)launch);
        }

        static void OnResult(uint8_t launch, uint8_t result, int movedTo)
        {
            if (launch != g_hack.launch) return;   // about an earlier drone
            ShipManager *own = G_->GetShipManager(0);
            HackingSystem *hacking = own ? own->hackingSystem : nullptr;
            if (result == RESULT_MOVED)
            {
                // The weapon or drone our drone sticks to went to another bay there: our hack goes with it (roadmap DB).
                ShipManager *enemy = G_->GetShipManager(1);
                ShipSystem *bay = enemy && movedTo >= 0 ? enemy->GetSystem(movedTo) : nullptr;
                if (!hacking || !hacking->bHacking || !bay) return;
                if (hacking->currentSystem && hacking->currentSystem != bay)
                {
                    hacking->currentSystem->bUnderAttack = false;
                    hacking->currentSystem->iHackEffect = 0;
                }
                hacking->currentSystem = bay;
                g_hack.target = bay;   // the same drone: no new launch (WatchOurs)
                Log("Hacking: our hack moved with their weapon or drone to %s (launch %u)", SystemLabel(movedTo).c_str(), (unsigned)launch);
                return;
            }
            Log("Hacking: our drone (launch %u) %s there", (unsigned)launch, ResultName(result));
            if (result == RESULT_ATTACHED)
            {
                g_hack.attached = true;
            }
            else
            {
                g_hack.attached = false;
                if (hacking && hacking->bHacking) hacking->BlowHackingDrone();
            }
        }

        void OnMessage(uint8_t type, Reader &r)
        {
            if (type == MSG_HACK)
            {
                uint8_t event = r.U8();
                uint8_t launch = r.U8();
                int system = event == EVENT_LAUNCH ? r.U8() : -1;
                if (!r.Ok()) return;
                if (event == EVENT_LAUNCH) OnLaunch(launch, system);
                else if (event == EVENT_STOP) OnStop(launch);
            }
            else if (type == MSG_HACK_RESULT)
            {
                uint8_t launch = r.U8();
                uint8_t result = r.U8();
                int movedTo = result == RESULT_MOVED ? r.U8() : -1;
                if (r.Ok()) OnResult(launch, result, movedTo);
            }
        }

        void WriteState(Writer &w)
        {
            ShipManager *own = G_->GetShipManager(0);
            HackingSystem *hacking = own ? own->hackingSystem : nullptr;
            w.Bool(hacking != nullptr);
            if (hacking) w.F32(hacking->effectTimer.first);
        }

        bool ReadState(Reader &r)
        {
            g_hack.havePulse = r.Bool();
            g_hack.pulseTime = g_hack.havePulse ? r.F32() : 0.f;
            return r.Ok();
        }

        void ApplyState()
        {
            ShipManager *replica = G_->GetShipManager(1);
            HackingSystem *hacking = replica ? replica->hackingSystem : nullptr;
            // While the owner pulses (its lock, already on the replica), the replica's pulse keeps behind the owner's.
            if (!hacking || !g_hack.havePulse || hacking->iLockCount != -1) return;
            hacking->effectTimer.first = std::max(0.f, g_hack.pulseTime - PULSE_TIMER_LAG_S);
        }

        bool MayPulse(HackingSystem *system)
        {
            ShipManager *own = G_->GetShipManager(0);
            if (!Net::IsConnected() || !own || system != own->hackingSystem || !G_->GetShipManager(1)) return true;
            if (g_hack.attached) return true;
            ++g_hack.pulsesRefused;
            return false;
        }

        bool RunVerb(const std::string &what, std::string &message)
        {
            ShipManager *own = G_->GetShipManager(0);
            ShipManager *enemy = G_->GetShipManager(1);
            HackingSystem *hacking = own ? own->hackingSystem : nullptr;
            if (!hacking)
            {
                message = "our ship has no hacking system";
                return false;
            }
            if (what == "pulse")
            {
                bool before = hacking->iLockCount == -1;
                hacking->InitiatePulse();
                bool pulsing = hacking->iLockCount == -1;
                if (pulsing && !before)
                {
                    message = "pulse started";
                    return true;
                }
                // FTL's conditions (HackingSystem::InitiatePulse), and ours.
                std::ostringstream why;
                why << "no pulse: hacking " << hacking->bHacking << ", power " << hacking->GetEffectivePower()
                    << ", target " << (hacking->currentSystem ? SystemLabel(hacking->currentSystem->iSystemType) : "-")
                    << (hacking->currentSystem && hacking->currentSystem->CompletelyDestroyed() ? " (destroyed)" : "")
                    << ", drone deployed " << hacking->drone.deployed << " arrived " << hacking->drone.arrived
                    << " dead " << hacking->drone.bDead << ", lock " << hacking->iLockCount << ", confirmed "
                    << g_hack.attached << "; drone in space " << hacking->drone.currentSpace << " (to "
                    << hacking->drone.destinationSpace << ") at " << hacking->drone.currentLocation.x << ","
                    << hacking->drone.currentLocation.y << " going to " << hacking->drone.destinationLocation.x << ","
                    << hacking->drone.destinationLocation.y << ", final " << hacking->drone.finalDestination.x << ","
                    << hacking->drone.finalDestination.y << ", target " << (hacking->drone.movementTarget ? "set" : "none")
                    << ", room " << hacking->drone.prefRoom << ", powered " << hacking->drone.powered
                    << ", hack level " << hacking->drone.iHackLevel << ", ion " << hacking->drone.ionStun
                    << ", speed " << (hacking->drone.blueprint ? hacking->drone.blueprint->speed : -1)
                    << ", deployed last frame " << hacking->drone.deployedLastFrame << ", in space list "
                    << (G_->GetWorld() && std::find(G_->GetWorld()->space.drones.begin(), G_->GetWorld()->space.drones.end(),
                                                    (SpaceDrone*)&hacking->drone) != G_->GetWorld()->space.drones.end())
                    << ", target dying " << (hacking->drone.movementTarget ? hacking->drone.movementTarget->GetIsDying() : false)
                    << " valid " << (hacking->drone.movementTarget ? hacking->drone.movementTarget->ValidTarget() : false);
                message = why.str();
                return false;
            }
            if (what == "stop")
            {
                hacking->StopHacking();
                message = "hacking stopped";
                return true;
            }
            if (!enemy)
            {
                message = "no enemy ship";
                return false;
            }
            ShipSystem *target = nullptr;
            if (what.compare(0, 5, "room ") == 0)
            {
                // FTL's own lookup, as the player's click: W1 and D1 give bay 1 (DuelsBays).
                target = enemy->GetSystemInRoom(std::atoi(what.c_str() + 5));
            }
            else
            {
                int type = ShipSystem::NameToSystemId(what);
                target = type >= 0 ? enemy->GetSystem(type) : nullptr;
            }
            if (!target)
            {
                message = "the enemy has no system " + what;
                return false;
            }
            hacking->StartHacking(target);
            message = hacking->queuedSystem == target ? "hacking drone goes for the enemy's " + SystemLabel(target->iSystemType)
                                                      : "hacking refused (locked or blocked)";
            return hacking->queuedSystem == target;
        }

        std::string Signature(ShipManager *ship)
        {
            HackingSystem *hacking = ship ? ship->hackingSystem : nullptr;
            if (!hacking) return "-";
            if (!hacking->bHacking || !hacking->currentSystem) return "idle";
            std::ostringstream out;
            out << hacking->currentSystem->iSystemType;
            // Ours counts as attached when the defender said so; the replica's when its drone arrived here.
            bool attached = ship->iShipId == 0 ? g_hack.attached : hacking->drone.arrived && !g_hack.theirLost;
            if (attached) out << 'a';
            if (hacking->iLockCount == -1) out << 'p';
            return out.str();
        }

        std::string Status()
        {
            const HackState &h = g_hack;
            std::ostringstream out;
            out << "hacking: launches sent " << h.launchesSent << " received " << h.launchesReceived << ", reported attached "
                << h.attachedReports << " destroyed " << h.destroyedReports << ", moved " << h.moves << ", pulses held " << h.pulsesRefused;
            return out.str();
        }
    }
}

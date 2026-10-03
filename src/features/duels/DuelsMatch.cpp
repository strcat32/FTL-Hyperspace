#include "Global.h"
#include "CommandConsole.h"
#include "CustomDamage.h"
#include "Drones.h"
#include "HSVersion.h"
#include "Projectile_Extend.h"
#include "Systems.h"
#include "Duels.h"
#include "DuelsAccount.h"
#include "DuelsConsole.h"
#include "DuelsBays.h"
#include "DuelsBoarding.h"
#include "DuelsConfig.h"
#include "DuelsCrew.h"
#include "DuelsDemo.h"
#include "DuelsDrones.h"
#include "DuelsFair.h"
#include "DuelsLobby.h"
#include "DuelsVision.h"
#include "DuelsHacking.h"
#include "DuelsAi.h"
#include "DuelsMatch.h"
#include "DuelsMind.h"
#include "DuelsRooms.h"
#include "DuelsRounds.h"
#include "DuelsNet.h"
#include "DuelsRejoin.h"
#include "DuelsShipControl.h"
#include "DuelsShips.h"
#include "DuelsTrace.h"
#include "DuelsTune.h"
#include "DuelsView.h"
#include "DuelsWire.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <deque>
#include <map>
#include <set>
#include <sstream>

namespace Duels
{
    namespace Match
    {
        // Game messages (Net reserves types below 16 for the session).
        enum GameMessage : uint8_t
        {
            MSG_CHAT = 16,
            MSG_LOADOUT = 17,    // reliable: the ship to build as the replica
            MSG_READY = 18,      // reliable: the replica is built
            MSG_STATE = 19,      // unreliable, 10 Hz: hull, shields, systems, weapons
            MSG_SHOT = 20,       // reliable: a projectile left one of our weapons
            MSG_RESULT = 21,     // reliable: the defender's verdict on a shot
            MSG_SHOT_DOWNED = 23, // reliable: our shot ran into something in our own space before it left
            MSG_SETTINGS = 27,   // reliable, host to guest: the duel's settings (crew experience, the fine settings: BE)
            MSG_DEBUG = 40       // reliable, either way: this game's debug mode is on (roadmap T)
            // 22 (our hull reached 0) is gone: defeats go to the match flow (DuelsRounds.h, 38 and 39).
            // 24, 25: DuelsDrones.h
        };

        enum Outcome : uint8_t
        {
            PENDING = 0,
            OUTCOME_MISS = 1,     // dodged
            OUTCOME_SHIELD = 2,   // absorbed by the shields
            OUTCOME_HIT = 3,      // reached the hull
            OUTCOME_GONE = 4,     // the defender never saw it hit anything
            OUTCOME_DOWNED = 5    // exploded before the shields: shot down by a defense drone, or ran into a drone
        };

        static const char *OutcomeName(uint8_t outcome)
        {
            switch (outcome)
            {
            case OUTCOME_MISS: return "miss";
            case OUTCOME_SHIELD: return "shield";
            case OUTCOME_HIT: return "hit";
            case OUTCOME_GONE: return "gone";
            case OUTCOME_DOWNED: return "downed";
            default: return "pending";
            }
        }

        // Weapon blueprint types (CustomWeapons.cpp). Flak ("burst") fires laser blasts at scattered points.
        static const int WEAPON_LASER = 0;
        static const int WEAPON_MISSILES = 1;
        static const int WEAPON_BEAM = 2;
        static const int WEAPON_BOMB = 3;
        static const int WEAPON_BURST = 4;

        static bool Networked(int type)
        {
            return type >= WEAPON_LASER && type <= WEAPON_BURST;
        }

        // Where a shot comes from (MSG_SHOT): a weapon of the ship (slot), one of its drones (drone slot), one of its
        // artillery systems (index in ShipManager::artillerySystems), or a crystal shard (Crystal Vengeance: it breaks
        // off where the ship was hit, no slot).
        enum ShotSource : uint8_t
        {
            SOURCE_WEAPON = 0,
            SOURCE_DRONE = 1,
            SOURCE_ARTILLERY = 2,
            SOURCE_SHARD = 3
        };

        static const char *SourceName(uint8_t source)
        {
            switch (source)
            {
            case SOURCE_DRONE: return "drone";
            case SOURCE_ARTILLERY: return "artillery";
            case SOURCE_SHARD: return "shard";
            default: return "slot";
            }
        }

        static const double STATE_INTERVAL_MS = 100.0;
        static const double HOLD_TIMEOUT_MS = 3000.0;     // no verdict by then: show a miss, the state has the truth
        static const double BEAM_VERDICT_WAIT_MS = 3000.0;
        static const double NATURAL_OPPONENT_LEG_MS = 1100.0;   // step 1: a shot's flight out of the enemy window
        static const int MAX_SLOTS = 8;
        static const int MAX_ARTILLERY = 4;

        // A weapon slot or an artillery system: its own flight time estimate and volley timing (index into the
        // arrays below); -1 for drones and shards.
        static int TimingSlot(uint8_t source, int slot)
        {
            if (source == SOURCE_WEAPON && slot >= 0 && slot < MAX_SLOTS) return slot;
            if (source == SOURCE_ARTILLERY && slot >= 0 && slot < MAX_ARTILLERY) return MAX_SLOTS + slot;
            return -1;
        }

        // Our projectile flying at the opponent's replica.
        struct OutShot
        {
            Projectile *projectile = nullptr;
            unsigned int selfId = 0;
            uint32_t netId = 0;
            int slot = 0;
            int type = WEAPON_LASER;
            std::string weapon;
            double spawnMs = 0.0;
            double transferMs = -1.0;
            double holdStartMs = -1.0;
            double verdictMs = -1.0;
            uint8_t verdict = PENDING;
            int damage = 0;
            bool timedOut = false;
            double goneMs = -1.0;       // a beam whose sweep is over, still waiting for its verdict
            uint8_t source = SOURCE_WEAPON;
            bool drone = false;         // fired by one of our drones (slot = the drone's slot)
            Pointf downPoint;           // "downed": where it exploded on the defender's screen
            float downDistance = 1.0e9f;
            bool exploded = false;
            Fair::Value fair;           // our shot chain's value for it (roadmap 4.1)
        };

        // The opponent's projectile flying at our ship, created from their shot message.
        struct InShot
        {
            Projectile *projectile = nullptr;
            unsigned int selfId = 0;
            uint32_t netId = 0;
            int type = WEAPON_LASER;
            bool beamTouched = false;      // a beam reached our shields
            bool shieldTouched = false;    // it met our shields this frame: blocked if it stops there, else it goes on
            bool beamHit = false;          // a beam reached a room
            uint8_t source = SOURCE_WEAPON;
            bool drone = false;            // fired by one of the opponent's drones, in our space
            double catchUpMs = 0.0;        // a drone's shot makes up the message's delay this way, flying faster
            std::string weapon;
            double spawnMs = 0.0;          // the attacker's spawn time, in our clock
            double receivedMs = 0.0;
            double transferMs = -1.0;
            double releaseAt = 0.0;
            double releasedMs = -1.0;
            double decisionMs = -1.0;
            int speedUp = 1;
            bool released = false;
            uint8_t outcome = PENDING;
            int damage = 0;
            // Its dodge (roadmap 4.1): the attacker's chain value, and our roll with both players' values.
            Fair::Value fair;
            bool rolled = false, dodged = false;
            int evasion = 0;
            Fair::Value fairVerdict;
            // A replay (roadmap 5.1): the recorder's game decided it. Its verdict as recorded, and the wait for it at our
            // ship (as our shots wait at theirs).
            uint8_t recorded = PENDING;
            int recordedDamage = 0;
            Pointf recordedPoint;
            double recordedMs = -1.0, holdStartMs = -1.0;
            double goneMs = -1.0;          // gone before its verdict (a beam's comes when the recorder's sweep is over)
            float downDistance = 1.0e9f;
            bool exploded = false;
        };

        struct MatchState
        {
            bool initialised = false;
            std::string playerName = "Captain";
            std::string rankedName;   // a ranked room's: the Steam name (rules, section 6), while the session lasts

            bool loadoutSent = false;
            std::string sentArmament;      // our weapons and drones, in slot order, as the last loadout had them
            double driveChargingSince = -1.0;   // the fight our FTL drive charges in began (roadmap AD)
            bool driveReadyLogged = false;
            bool replicaReady = false;     // we built the opponent's ship
            bool peerReady = false;        // they built ours
            std::string opponentShip;
            std::string lastRefusal;       // a player our game refused while the room waited, and why (the Duels window)

            uint16_t stateSeq = 0;
            bool havePeerState = false;
            uint16_t peerStateSeq = 0;
            uint8_t peerVision = 0;          // what we see of their ship, as their last state says (roadmap 4.5)
            double lastStateSent = -1.0e9;
            double lastTrackMs = -1.0;       // TrackShots' last frame (the shots' waits stand still in a pause)
            bool stateDirty = false;
            uint32_t statesApplied = 0;

            uint32_t nextNetId = 1;
            std::vector<OutShot> out;
            std::vector<InShot> in;
            // Crystal shards of ours, from where they broke off until they cross into the replica's space (FTL loses
            // some that head away from it); only then do they go to the opponent.
            struct PendingShard
            {
                Projectile *projectile = nullptr;
                unsigned int selfId = 0;
                const WeaponBlueprint *blueprint = nullptr;
                Pointf origin;
                float heading = 0.f, entryAngle = 0.f;
            };
            std::vector<PendingShard> pendingShards;
            uint32_t shardsLost = 0;
            uint32_t artilleryHeld = 0;    // frames FTL's ReadyToFire said yes for the replica's artillery (held back)
            double legEstimate[MAX_SLOTS + MAX_ARTILLERY];   // by TimingSlot
            double lastFireMs[MAX_SLOTS + MAX_ARTILLERY];    // when a replica weapon last fired a received shot

            uint32_t shotsSent = 0, shotsReceived = 0, verdictsSent = 0, verdictsReceived = 0, holdTimeouts = 0;
            uint32_t hullKept = 0;         // hits whose copy would have taken the replica's last hull point
            std::set<std::string> unsupportedLogged;

            CsvFile shotsCsv;
            CsvFile syncCsv;
            std::string lastSignature[2];
        };

        static MatchState g_match;

        // A ship driven by its owner's states (the opponent's copy, ship 1; in a replay ship 0 too, roadmap 5.1): what
        // the states keep on it between them, by system id.
        struct DrivenFields
        {
            std::map<int, int> subsystemPower;   // the owner's (piloting, sensors, doors, battery)
            std::map<int, int> hackFlags;        // the owner's hacking of each system (HACK_* bits)
            std::map<int, int> bonusPower;       // the owner's bonus power (Zoltan crew) of each system
        };
        static DrivenFields g_driven[2];
        static bool g_replayOwnDriven = false;   // a replay's ship 0 follows the recorder's states (the first came)
        // The shot whose collision check runs with a verdict (no projectile: none): ours at the opponent's copy, by the
        // defender's verdict; in a replay (roadmap 5.1) theirs at our ship too, by the recorder's, and a hazard's (a rock,
        // the battery's shot) at our ship, for its look only: the recorder's states have what it did.
        struct Forced
        {
            Projectile *projectile = nullptr;
            int shipId = 1;
            uint8_t verdict = PENDING;
            int damage = 0;
            bool cosmetic = false;
        };
        static Forced g_forced;
        static double g_replayLastFireMs[MAX_SLOTS + MAX_ARTILLERY];   // a replay: when our weapon last fired a recorded shot

        static void ResetMatch()
        {
            MatchState &m = g_match;
            Vision::Reset();
            m.lastRefusal.clear();
            m.loadoutSent = false;
            m.replicaReady = false;
            m.peerReady = false;
            m.opponentShip.clear();
            m.havePeerState = false;
            m.stateDirty = false;
            m.statesApplied = 0;
            g_driven[0] = DrivenFields();
            g_driven[1] = DrivenFields();
            g_replayOwnDriven = false;
            m.out.clear();
            m.in.clear();
            m.pendingShards.clear();
            for (int i = 0; i < MAX_SLOTS + MAX_ARTILLERY; ++i)
            {
                m.legEstimate[i] = -1.0;
                m.lastFireMs[i] = -1.0e9;
            }
            m.lastSignature[0].clear();
            m.lastSignature[1].clear();
            g_forced = Forced();
            for (double &ms : g_replayLastFireMs) ms = -1.0e9;
            Drones::Reset();
            Crew::Reset();
            Rooms::Reset();
            Hacking::Reset();
            Mind::Reset();
            Boarding::Reset();
        }

        static void Announce(const std::string &text)
        {
            Log("Match: %s", text.c_str());
            Console::Print("DUEL: " + text);
        }

        // What matters goes to the feed at the bottom left too (roadmap L).
        static void Headline(const std::string &text)
        {
            Log("Match: %s", text.c_str());
            Console::Feed(text);
        }

        static bool InGame()
        {
            WorldManager *world = G_->GetWorld();
            CApp *app = G_->GetCApp();
            return world && world->playerShip && world->commandGui && app && !app->menu.bOpen;
        }

        static int WeaponSlot(ShipManager *ship, ProjectileFactory *weapon)
        {
            if (!ship || !ship->weaponSystem) return -1;
            std::vector<ProjectileFactory*> weapons = ship->GetWeaponList();
            for (size_t slot = 0; slot < weapons.size(); ++slot)
            {
                if (weapons[slot] == weapon) return (int)slot;
            }
            return -1;
        }

        static OutShot *FindOut(Projectile *projectile)
        {
            if (!projectile) return nullptr;
            for (OutShot &shot : g_match.out)
            {
                if (shot.projectile == projectile && shot.selfId == projectile->selfId) return &shot;
            }
            return nullptr;
        }

        static InShot *FindIn(Projectile *projectile)
        {
            if (!projectile) return nullptr;
            for (InShot &shot : g_match.in)
            {
                if (shot.projectile == projectile && shot.selfId == projectile->selfId) return &shot;
            }
            return nullptr;
        }

        static void LogShot(const char *side, uint32_t netId, const std::string &weapon, double spawn, double received,
                            double transfer, double release, double holdStart, double decision, double verdict,
                            uint8_t outcome, int damage, int speedUp)
        {
            if (!g_match.shotsCsv.IsOpen())
            {
                g_match.shotsCsv.Open("duels_shots.csv",
                                      "side,net_id,weapon,spawn_ms,received_ms,transfer_ms,release_ms,hold_start_ms,"
                                      "decision_ms,verdict_ms,held_ms,outcome,damage,speed_up,clock_offset_ms,rtt_ms");
            }
            double held = holdStart >= 0.0 && verdict >= 0.0 ? verdict - holdStart : 0.0;
            Row row;
            row << side << netId << weapon << spawn << received << transfer << release << holdStart << decision << verdict
                << held << OutcomeName(outcome) << damage << speedUp << (Net::HasClock() ? Net::LocalToPeerTime(0.0) : 0.0)
                << Net::RttMs();
            g_match.shotsCsv.WriteRow(row.str());
        }

        // --------------------------------------------------------------------------------------------------------
        // Loadout: what the opponent needs to build our ship
        // --------------------------------------------------------------------------------------------------------

        // Our weapons and drones in slot order, and our augments. When it changes (weapons dragged to other slots, a
        // refit), the opponent gets the loadout again: slots decide power, charge and the weapon bays.
        static std::string Armament(ShipManager *ship)
        {
            std::string text;
            if (!ship) return text;
            if (ship->weaponSystem)
            {
                for (ProjectileFactory *weapon : ship->GetWeaponList()) text += (weapon->blueprint ? weapon->blueprint->name : "?") + ",";
            }
            text += "|";
            if (ship->droneSystem)
            {
                for (Drone *drone : ship->GetDroneList()) text += (drone->blueprint ? drone->blueprint->name : "?") + ",";
            }
            text += "|";
            for (const std::string &augment : ship->GetAugmentationList()) text += augment + ",";
            return text;
        }

        // The replica's augments become the owner's: some act on our ship in our game (Defense Scrambler on our
        // defense drones, Hacking Stun with the replica's hacking), and our game builds the replica's shots.
        static void SetReplicaAugments(ShipManager *replica, const std::vector<std::string> &augments)
        {
            std::vector<std::string> current = replica->GetAugmentationList();
            if (current == augments) return;
            for (const std::string &augment : current) replica->RemoveAugmentation(augment);
            for (const std::string &augment : augments)
            {
                if (!replica->AddAugmentation(augment)) Log("Match: the replica cannot take the augment %s", augment.c_str());
            }
            std::string list;
            for (const std::string &augment : replica->GetAugmentationList()) list += (list.empty() ? "" : " ") + augment;
            Log("Match: replica augments: %s", list.empty() ? "none" : list.c_str());
        }

        void ShowOpponent(ShipManager *ship)
        {
            if (!ship) return;
            View::UsePlayerShieldPosition(ship);
            // Combat drones already bound to it took their waypoint from its shields before they moved; they take a
            // new one around the right ellipse (else the first shot can start inside the shields).
            if (ShipManager *own = G_->GetShipManager(0))
            {
                for (SpaceDrone *drone : own->spaceDrones)
                {
                    if (drone->type == 1 && drone->movementTarget == &ship->_targetable) drone->SetMovementTarget(&ship->_targetable);
                }
            }
        }

        Loadout TakeLoadout(ShipManager *ship)
        {
            Loadout loadout;
            if (!ship) return loadout;
            loadout.blueprint = ship->myBlueprint.blueprintName;
            loadout.hullMax = ship->ship.hullIntegrity.second;
            loadout.hull = ship->ship.hullIntegrity.first;
            PowerManager *power = PowerManager::GetPowerManager(ship->iShipId);
            loadout.reactor = power ? power->currentPower.second : 0;
            for (ShipSystem *system : ship->vSystemList) loadout.systems.push_back(std::make_pair(system->iSystemType, system->powerState.second));
            if (ship->weaponSystem)
            {
                for (ProjectileFactory *weapon : ship->GetWeaponList()) loadout.weapons.push_back(weapon->blueprint ? weapon->blueprint->name : "");
            }
            for (CrewMember *crew : ship->vCrewList) loadout.crew.push_back(crew->species);
            // Drones in slot order (drone messages refer to slots), and the drone parts.
            if (ship->droneSystem)
            {
                for (Drone *drone : ship->GetDroneList()) loadout.drones.push_back(drone->blueprint ? drone->blueprint->name : "");
            }
            loadout.droneParts = ship->GetDroneCount();
            loadout.augments = ship->GetAugmentationList();
            return loadout;
        }

        void WriteLoadout(Writer &w, const Loadout &loadout)
        {
            w.Str(loadout.blueprint);
            w.I16((int16_t)loadout.hullMax);
            w.I16((int16_t)loadout.hull);
            w.U8((uint8_t)loadout.reactor);
            w.U8((uint8_t)loadout.systems.size());
            for (const std::pair<int, int> &system : loadout.systems)
            {
                w.U8((uint8_t)system.first);
                w.U8((uint8_t)system.second);
            }
            w.U8((uint8_t)loadout.weapons.size());
            for (const std::string &weapon : loadout.weapons) w.Str(weapon);
            w.U8((uint8_t)loadout.crew.size());
            for (const std::string &species : loadout.crew) w.Str(species);
            w.U8((uint8_t)loadout.drones.size());
            for (const std::string &drone : loadout.drones) w.Str(drone);
            w.I16((int16_t)loadout.droneParts);
            w.U8((uint8_t)loadout.augments.size());
            for (const std::string &augment : loadout.augments) w.Str(augment);
        }

        static void SendLoadout()
        {
            ShipManager *ship = G_->GetShipManager(0);
            if (!ship) return;
            Loadout loadout = TakeLoadout(ship);
            Writer w;
            WriteLoadout(w, loadout);
            Net::Send(MSG_LOADOUT, w, true);
            g_match.loadoutSent = true;
            g_match.sentArmament = Armament(ship);
            Log("Match: loadout sent (%s, hull %d/%d, %u systems, %u weapons, %u drones, %u augments)", loadout.blueprint.c_str(),
                loadout.hull, loadout.hullMax, (unsigned)loadout.systems.size(), (unsigned)loadout.weapons.size(),
                (unsigned)loadout.drones.size(), (unsigned)loadout.augments.size());
        }

        void FitShip(ShipManager *ship, const Loadout &loadout)
        {
            // (The fitting was the replica's first: its names kept.)
            ShipManager *replica = ship;
            if (!replica) return;
            const int hullMax = loadout.hullMax, hull = loadout.hull, reactor = loadout.reactor, droneParts = loadout.droneParts;
            struct SystemLevel { int id; int level; };
            std::vector<SystemLevel> systems;
            for (const std::pair<int, int> &system : loadout.systems) systems.push_back(SystemLevel{system.first, system.second});
            const std::vector<std::string> &weapons = loadout.weapons, &drones = loadout.drones, &augments = loadout.augments;

            replica->ship.hullIntegrity.second = hullMax;
            replica->ship.hullIntegrity.first = hull;
            if (PowerManager *power = PowerManager::GetPowerManager(replica->iShipId)) power->currentPower.second = reactor;

            // Systems the owner sold in a preparation (roadmap V) leave the replica too.
            for (int id = 0; id < SYS_ALL; ++id)
            {
                if (!replica->HasSystem(id)) continue;
                bool kept = false;
                for (const SystemLevel &level : systems) kept = kept || level.id == id;
                if (kept) continue;
                replica->RemoveSystem(id);
                Log("Match: replica %s removed (the owner sold it)", SystemName(id));
            }

            for (const SystemLevel &level : systems)
            {
                if (!replica->HasSystem(level.id))
                {
                    replica->AddSystem(level.id);
                    if (!replica->HasSystem(level.id))
                    {
                        Log("Match: the replica cannot get %s", SystemName(level.id));
                        continue;
                    }
                }
                ShipSystem *system = replica->GetSystem(level.id);
                int difference = level.level - system->powerState.second;
                if (difference > 0) replica->UpgradeSystem(level.id, difference);
                else if (difference < 0 && level.level >= 1 && level.id < SYS_ALL)
                {
                    // A level taken back in the owner's preparation (roadmap V): the power first, then the level. (The
                    // bays follow their weapons and drones by themselves, DuelsBays.cpp.)
                    if (system->powerState.first > level.level) system->ForceDecreasePower(system->powerState.first - level.level);
                    system->UpgradeSystem(difference);
                    Log("Match: replica %s down to level %d", SystemName(level.id), level.level);
                }
                if (system->powerState.second != level.level)
                {
                    // (A bay's level follows its weapon or drone a frame later, DuelsBays.cpp.)
                    Log("Match: replica %s is level %d, the owner's is %d", ShipSystem::SystemIdToName(level.id).c_str(),
                        system->powerState.second, level.level);
                }
            }

            // Weapons in the owner's slot order; shots refer to slots.
            std::vector<ProjectileFactory*> current = replica->weaponSystem ? replica->GetWeaponList() : std::vector<ProjectileFactory*>();
            bool same = current.size() == weapons.size();
            for (size_t slot = 0; same && slot < weapons.size(); ++slot)
            {
                same = current[slot]->blueprint && current[slot]->blueprint->name == weapons[slot];
            }
            if (!same && replica->weaponSystem)
            {
                for (int slot = (int)current.size() - 1; slot >= 0; --slot) replica->RemoveWeapon(slot);
                for (size_t slot = 0; slot < weapons.size(); ++slot)
                {
                    WeaponBlueprint *weapon = G_->GetBlueprints()->GetWeaponBlueprint(weapons[slot]);
                    if (weapon) replica->AddWeapon(weapon, (int)slot);
                }
                Log("Match: replica weapons replaced (%u)", (unsigned)weapons.size());
            }

            // Drones likewise.
            std::vector<Drone*> currentDrones = replica->droneSystem ? replica->GetDroneList() : std::vector<Drone*>();
            bool sameDrones = currentDrones.size() == drones.size();
            for (size_t slot = 0; sameDrones && slot < drones.size(); ++slot)
            {
                sameDrones = currentDrones[slot]->blueprint && currentDrones[slot]->blueprint->name == drones[slot];
            }
            if (!sameDrones && replica->droneSystem)
            {
                for (int slot = (int)currentDrones.size() - 1; slot >= 0; --slot) replica->RemoveDrone(slot);
                for (size_t slot = 0; slot < drones.size(); ++slot)
                {
                    DroneBlueprint *drone = G_->GetBlueprints()->GetDroneBlueprint(drones[slot]);
                    if (drone) replica->AddDrone(drone, (int)slot);
                }
                Log("Match: replica drones replaced (%u)", (unsigned)drones.size());
            }
            if (replica->droneSystem) replica->ModifyDroneCount(droneParts - replica->GetDroneCount());
            SetReplicaAugments(replica, augments);
        }

        bool ReadLoadout(Reader &r, Loadout &loadout)
        {
            loadout.blueprint = r.Str();
            loadout.hullMax = r.I16();
            loadout.hull = r.I16();
            loadout.reactor = r.U8();
            loadout.systems.resize(r.U8());
            for (std::pair<int, int> &system : loadout.systems)
            {
                system.first = r.U8();
                system.second = r.U8();
            }
            loadout.weapons.resize(r.U8());
            for (std::string &weapon : loadout.weapons) weapon = r.Str();
            loadout.crew.resize(r.U8());
            for (std::string &species : loadout.crew) species = r.Str();
            loadout.drones.resize(r.U8());
            for (std::string &drone : loadout.drones) drone = r.Str();
            loadout.droneParts = r.I16();
            loadout.augments.resize(r.U8());
            for (std::string &augment : loadout.augments) augment = r.Str();
            return r.Ok();
        }

        void AfterShipSwitch()
        {
            WorldManager *world = G_->GetWorld();
            ShipManager *own = G_->GetShipManager(0);
            if (!world || !world->commandGui || !own) return;
            std::vector<ShipManager*> &ships = world->space.ships;
            bool inSpace = std::find(ships.begin(), ships.end(), own) != ships.end();
            if (!inSpace) world->space.AddShip(own);
            world->commandGui->combatControl.LinkShip(own);
            Log("Match: our ship %s is in space again (%s) and our weapon and drone controls follow it", own->myBlueprint.blueprintName.c_str(),
                inSpace ? "it was" : "the switch had taken it out");
        }

        bool RestoreOwnShip(const Loadout &loadout, std::string &message)
        {
            WorldManager *world = G_->GetWorld();
            ShipManager *own = G_->GetShipManager(0);
            if (!world || !world->playerShip || !own || loadout.blueprint.empty())
            {
                message = "no ship to restore";
                return false;
            }
            if (own->myBlueprint.blueprintName != loadout.blueprint)
            {
                // As the ship choice switches ships (DuelsRounds.cpp): the bays come with the blueprint first.
                Bays::PrepareBlueprint(G_->GetBlueprints()->GetShipBlueprint(loadout.blueprint, -1));
                bool switched = world->SwitchShip(loadout.blueprint);
                Log("Match: back after a crash: our ship becomes the %s again (%s)", loadout.blueprint.c_str(), switched ? "switched" : "the switch failed");
                if (!switched)
                {
                    message = "the switch to the " + loadout.blueprint + " failed";
                    return false;
                }
                AfterShipSwitch();
                own = G_->GetShipManager(0);
                if (!own) return false;
            }
            FitShip(own, loadout);
            message = loadout.blueprint + ", hull " + std::to_string(loadout.hull) + "/" + std::to_string(loadout.hullMax) + ", " +
                      std::to_string(loadout.weapons.size()) + " weapons, " + std::to_string(loadout.drones.size()) + " drones, " +
                      std::to_string(loadout.augments.size()) + " augments";
            return true;
        }

        void ReplayOwnLoadout(const uint8_t *data, size_t size)
        {
            Reader r(data, size);
            Loadout loadout;
            WorldManager *world = G_->GetWorld();
            if (!ReadLoadout(r, loadout) || !world || !world->playerShip) return;
            ShipManager *own = G_->GetShipManager(0);
            if (own && own->myBlueprint.blueprintName != loadout.blueprint)
            {
                // As the ship choice switches ships (DuelsRounds.cpp): the bays come with the blueprint first.
                Bays::PrepareBlueprint(G_->GetBlueprints()->GetShipBlueprint(loadout.blueprint, -1));
                bool switched = world->SwitchShip(loadout.blueprint);
                Log("Match: replay: our ship becomes the recorder's %s (%s)", loadout.blueprint.c_str(), switched ? "switched" : "the switch failed");
                if (switched) AfterShipSwitch();
                own = G_->GetShipManager(0);
            }
            if (!own) return;
            FitShip(own, loadout);
            Log("Match: replay: the recorder's ship fitted (%s)", loadout.blueprint.c_str());
        }

        static void ApplyLoadout(Reader &r)
        {
            Loadout loadout;
            if (!ReadLoadout(r, loadout))
            {
                Log("Match: malformed loadout");
                return;
            }
            const std::string &blueprint = loadout.blueprint;

            if (!InGame())
            {
                Log("Match: loadout received outside a game; ignored");
                return;
            }
            ShipManager *replica = G_->GetShipManager(1);
            if (replica && replica->myBlueprint.blueprintName != blueprint)
            {
                Log("Match: an enemy (%s) is already present; it stays", replica->myBlueprint.blueprintName.c_str());
            }
            // The replica's shields go where the player's own ship has them (DuelsView) from the moment it exists:
            // our combat drones pick their orbit from its shield ellipse as soon as it is our target.
            View::UsePlayerShieldPosition(nullptr);
            if (!replica)
            {
                std::string message;
                if (!SpawnEnemy(blueprint, message))
                {
                    Log("Match: cannot build the opponent's ship: %s", message.c_str());
                    return;
                }
                replica = G_->GetShipManager(1);
                if (!replica) return;
            }

            // The replica belongs to the network from now on: no AI, no pause, no local repower.
            State &state = GetState();
            state.aiOff[1] = true;
            state.noPause = true;

            FitShip(replica, loadout);
            ShowOpponent(replica);
            g_match.opponentShip = blueprint;
            g_match.replicaReady = true;
            Rounds::OnReplicaBuilt(replica);
            Net::Send(MSG_READY, Writer(), true);
            // (The log only: before the ship choice this is the opponent's hangar ship, and the choice names both ships.)
            Log("Match: the opponent's ship is built: the %s (%s)", Ships::Title(blueprint).c_str(), blueprint.c_str());
        }

        // --------------------------------------------------------------------------------------------------------
        // State: our ship ten times a second; the replica follows
        // --------------------------------------------------------------------------------------------------------

        // A system's power bars as its owner set them: from the reactor and from a running battery. (Bonus power,
        // from Zoltan crew for example, comes with the crew on each side.)
        static int PowerBars(const ShipSystem *system)
        {
            return system->powerState.first + system->iBatteryPower;
        }

        // Sets a replica system's power bars to the owner's, with the owner's lock already on it. Power goes down the
        // way ion and damage take it (which passes locks; shields keep a lone bar only while locked, as the owner's
        // did), and up the way the power bars add it.
        static void SetReplicaPower(ShipManager *replica, ShipSystem *system, int level)
        {
            if (!system->bNeedsPower)
            {
                // Subsystems (piloting, sensors, doors, battery) need no reactor power, and their power can't be
                // changed by hand; only the environment changes it (a nebula switches the sensors off). This game's
                // loop sets it again every frame from this game's environment, so HoldReplicaSubsystems holds it.
                system->powerState.first = std::max(0, std::min(level, system->powerState.second));
                return;
            }
            for (int guard = 0; guard < 32 && PowerBars(system) != level; ++guard)
            {
                bool changed;
                if (PowerBars(system) > level)
                {
                    changed = system->ForceDecreasePower(1);
                }
                else
                {
                    // The owner's power only rises under a lock as it ends; lift the replica's for the step.
                    int lock = system->iLockCount;
                    system->iLockCount = 0;
                    changed = replica->IncreaseSystemPower(system->iSystemType);
                    system->iLockCount = lock;
                }
                if (!changed) break;
            }
        }

        bool IsDriven(int shipId)
        {
            if (shipId == 1) return g_match.replicaReady && G_->GetShipManager(1) != nullptr;
            if (shipId == 0) return g_replayOwnDriven && Net::Replaying() && G_->GetShipManager(0) != nullptr;
            return false;
        }

        static bool DrivenShip(const ShipManager *ship)
        {
            return ship && (ship->iShipId == 0 || ship->iShipId == 1) && IsDriven(ship->iShipId) && ship == G_->GetShipManager(ship->iShipId);
        }

        void HoldReplicaSubsystems(ShipManager *ship)
        {
            if (!DrivenShip(ship)) return;
            HoldReplicaHacking(ship);
            Crew::AfterReplicaLoop(ship);
            Rooms::AfterReplicaLoop(ship);
            for (const std::pair<const int, int> &entry : g_driven[ship->iShipId].subsystemPower)
            {
                ShipSystem *system = ship->GetSystem(entry.first);
                if (system && !system->bNeedsPower)
                {
                    system->powerState.first = std::max(0, std::min(entry.second, system->powerState.second));
                }
            }
        }

        // The replica's lock timers run this far behind the owner's, so a lock ends when the owner's update says so,
        // not by the replica's own count (that would repower the system on its own).
        static const float LOCK_TIMER_LAG_S = 0.25f;

        struct SystemState { int id; int power; int health; int lock; float lockTime; float lockGoal; int hack; int bonus; };

        // A system's hacking in the state: FTL's hack level (1 = a drone attached, 2 = pulsing) and whether it is
        // hacked at all; and whether a crew member mans its console (protocol 20, DG: the other game reckoned its
        // player's sensors by its copy of their crew, hidden while it didn't see inside their ship).
        enum { HACK_LEVEL = 3, SYSTEM_MANNED = 0x40, HACK_UNDER_ATTACK = 0x80 };

        static int HackFlags(const ShipSystem *system)
        {
            return (std::max(0, std::min(system->iHackEffect, 2)) & HACK_LEVEL) | (system->bUnderAttack ? HACK_UNDER_ATTACK : 0) |
                   (system->iActiveManned > 0 ? SYSTEM_MANNED : 0);
        }

        uint8_t PeerVision()
        {
            return g_match.peerVision;
        }

        bool ReplicaManned(const ShipManager *ship, int systemType)
        {
            if (!DrivenShip(ship)) return false;
            const std::map<int, int> &flags = g_driven[ship->iShipId].hackFlags;
            auto found = flags.find(systemType);
            return found != flags.end() && (found->second & SYSTEM_MANNED) != 0;
        }

        void HoldReplicaHacking(ShipManager *ship)
        {
            if (!DrivenShip(ship)) return;
            const std::map<int, int> &hackFlags = g_driven[ship->iShipId].hackFlags;
            for (ShipSystem *system : ship->vSystemList)
            {
                auto found = hackFlags.find(system->iSystemType);
                int flags = found != hackFlags.end() ? found->second : 0;
                system->bUnderAttack = (flags & HACK_UNDER_ATTACK) != 0;
                system->iHackEffect = flags & HACK_LEVEL;
            }
        }

        static void ApplyLocks(ShipManager *replica, const std::vector<SystemState> &systems)
        {
            for (const SystemState &state : systems)
            {
                ShipSystem *system = replica->GetSystem(state.id);
                if (!system) continue;
                system->iLockCount = state.lock;
                if (state.lock > 0)
                {
                    system->lockTimer.running = true;
                    system->lockTimer.currTime = std::max(0.f, state.lockTime - LOCK_TIMER_LAG_S);
                    system->lockTimer.currGoal = state.lockGoal;
                }
            }
        }

        // Cloaking (roadmap 2.4). FTL's cloak: on for 5 s per power bar, each shot fired while cloaked takes a fifth
        // off (not with the Stealth Weapons augment), then locked for 4 lock periods; while it runs the ship has 60%
        // more evasion and the enemy's weapons don't charge. Each game's own ship decides all of that; the replica's
        // cloak only follows the owner's: the opponent's weapons stop charging while it is on (Hyperspace's
        // ProjectileFactory::Update asks the target's IsCloaked), and it shows cloaked.
        static bool g_cloakFromOwner = false;   // the state sync is switching the replica's cloak

        bool MaySwitchCloak(const CloakingSystem *cloak)
        {
            if (g_cloakFromOwner || !cloak) return true;
            for (int id = 0; id < 2; ++id)
            {
                ShipManager *ship = IsDriven(id) ? G_->GetShipManager(id) : nullptr;
                if (ship && ship->cloakSystem == cloak) return false;
            }
            return true;
        }

        int HullDamage(const Ship *ship, int amount)
        {
            ShipManager *driven = nullptr;
            for (int id = 0; id < 2 && !driven; ++id)
            {
                ShipManager *candidate = IsDriven(id) ? G_->GetShipManager(id) : nullptr;
                if (candidate && ship == &candidate->ship) driven = candidate;
            }
            if (!driven || amount <= 0) return amount;
            int keep = std::max(0, ship->hullIntegrity.first - 1);
            if (amount <= keep) return amount;
            ++g_match.hullKept;
            return keep;
        }

        static void SetReplicaCloak(CloakingSystem *cloak, bool on, float time, float goal)
        {
            if (cloak->bTurnedOn != on)
            {
                // FTL's own switch: its sound, and the locks it sets (the owner's replace them, ApplyLocks). It
                // refuses a locked cloak: the owner's lock while it runs is already on the replica.
                int lock = cloak->iLockCount;
                cloak->iLockCount = 0;
                g_cloakFromOwner = true;
                cloak->SetTurnedOn(on);
                g_cloakFromOwner = false;
                if (cloak->iLockCount == 0) cloak->iLockCount = lock;
                if (cloak->bTurnedOn != on)
                {
                    // Not working here (no power yet): switched without FTL's function.
                    Log("Match: the replica's cloak did not switch %s by itself (power %d, health %d)", on ? "on" : "off",
                        cloak->GetEffectivePower(), cloak->healthState.first);
                    cloak->bTurnedOn = on;
                }
            }
            if (on)
            {
                // Behind the owner's, like the lock timers.
                cloak->timer.currGoal = goal;
                cloak->timer.currTime = std::max(0.f, time - LOCK_TIMER_LAG_S);
                cloak->timer.running = true;
            }
        }

        // A replica system that could not take its owner's power (logged once per new combination).
        static void LogPowerMiss(const ShipSystem *system, int wanted)
        {
            static std::map<int, std::pair<int, int>> logged;
            std::pair<int, int> now(wanted, PowerBars(system));
            int key = system->_shipObj.iShipId * 1000 + system->iSystemType;
            auto found = logged.find(key);
            if (found != logged.end() && found->second == now) return;
            logged[key] = now;
            PowerManager *power = PowerManager::GetPowerManager(system->_shipObj.iShipId);
            if (!power) return;
            Log("Match: replica %s has %d power bars (reactor %d, battery %d, bonus %d, effective %d), the owner %d; "
                "reactor %d/%d, battery power %d/%d", SystemName(system->iSystemType), now.second,
                system->powerState.first, system->iBatteryPower, system->iBonusPower,
                const_cast<ShipSystem*>(system)->GetEffectivePower(), wanted, power->currentPower.first, power->currentPower.second,
                power->batteryPower.first, power->batteryPower.second);
        }

        // Our ship's state. record: for a demo (roadmap 5.1), written right after the one that goes, with
        // Vision::FullScope (all in sight) and without the crew ids' bookkeeping or the roster.
        static void WriteOwnState(Writer &w, ShipManager *ship, double now, uint16_t seq, bool record)
        {
            w.F64(now);
            w.U16(seq);
            // What the receiver sees of our ship: what follows holds back the rest (255 or -1: not seen).
            w.U8(Vision::Flags());
            w.I16((int16_t)Fair::CheatHull(ship->ship.hullIntegrity.first));

            Shields *shields = ship->shieldSystem;
            w.Bool(shields != nullptr);
            if (shields)
            {
                w.U8((uint8_t)std::max(0, Fair::CheatShieldLayers(shields->shields.power.first)));
                w.F32(shields->shields.charger);
                // The Zoltan super shield (Zoltan ships, the shield overcharger drone): its layers and their cap.
                w.U8((uint8_t)std::max(0, std::min(shields->shields.power.super.first, 255)));
                w.U8((uint8_t)std::max(0, std::min(shields->shields.power.super.second, 255)));
            }

            // Per system: power, health and the lock. Ion damage locks a system for one timer period per ion charge
            // (lock count 1-5, its timer counts each period); a running battery holds its own lock at -1, and its
            // cooldown afterwards is a lock like the ion one.
            w.U8((uint8_t)ship->vSystemList.size());
            for (ShipSystem *system : ship->vSystemList)
            {
                bool powerHidden = Vision::PowerHidden(system->iSystemType, system->bNeedsPower);
                w.U8((uint8_t)system->iSystemType);
                w.U8(powerHidden ? (uint8_t)255
                                 : (uint8_t)std::max(0, system->bNeedsPower && PowerBars(system) > 0 ? Fair::CheatPower(PowerBars(system)) : PowerBars(system)));
                w.U8((uint8_t)std::max(0, system->healthState.first));
                w.I8((int8_t)std::max(-1, std::min(system->iLockCount, 127)));
                if (system->iLockCount > 0)
                {
                    w.F32(system->lockTimer.currTime);
                    w.F32(system->lockTimer.currGoal);
                }
                w.U8((uint8_t)HackFlags(system));
                // Bonus power (Zoltan crew in the room): the replica's own crew are puppets that walk behind their
                // owners, so its bonus is the owner's, not what its puppets would give (SetBonusPower). Power too.
                w.U8(powerHidden ? (uint8_t)255 : (uint8_t)std::max(0, std::min(system->iBonusPower, 254)));
            }

            // The backup battery: on, and how far its 30 seconds have run.
            BatterySystem *battery = ship->batterySystem;
            w.Bool(battery != nullptr);
            if (battery)
            {
                w.Bool(battery->bTurnedOn);
                w.F32(battery->timer.currTime);
            }

            // Cloaking: on, and how far its time has run (its length is the power; each shot fired shortens it).
            CloakingSystem *cloak = ship->cloakSystem;
            w.Bool(cloak != nullptr);
            if (cloak)
            {
                w.Bool(cloak->bTurnedOn);
                w.F32(cloak->timer.currTime);
                w.F32(cloak->timer.currGoal);
            }

            std::vector<ProjectileFactory*> weapons = ship->weaponSystem ? ship->GetWeaponList() : std::vector<ProjectileFactory*>();
            w.U8((uint8_t)weapons.size());
            const Vision::Seen &seen = Vision::Current();
            for (ProjectileFactory *weapon : weapons)
            {
                // Powered: 1, 2 when its power isn't seen (roadmap 4.5).
                w.U8(seen.power ? (weapon->powered ? 1 : 0) : 2);
                w.F32(seen.charge ? Fair::CheatCharge(weapon->cooldown.first, weapon->cooldown.second) : -1.f);
                // Its full charge as it is now: a manned weapons system shortens it and FTL rescales the charge with it
                // (roadmap 4.1's charge check measures the share).
                w.F32(seen.charge ? weapon->cooldown.second : -1.f);
            }
            // Artillery: each system's charge (power comes with the systems). It fires by itself when charged; its
            // shots come as MSG_SHOT, so the replica's never fires on its own.
            w.U8((uint8_t)ship->artillerySystems.size());
            for (ArtillerySystem *artillery : ship->artillerySystems)
            {
                w.F32(!seen.charge ? -1.f : artillery && artillery->projectileFactory ? artillery->projectileFactory->cooldown.first : 0.f);
            }

            // Drones: power, launch, wreck and where they are (DuelsDrones.cpp).
            Drones::WriteState(w);
            // Crew: where each one is and how they are (DuelsCrew.cpp); who is on board goes first, when it changed.
            if (!record && Crew::RosterChanged())
            {
                Writer roster;
                Crew::WriteRoster(roster);
                Net::Send(Crew::MSG_CREW_ROSTER, roster, true);
            }
            Crew::WriteState(w, record);
            // Rooms: oxygen, fires, breaches, doors, lockdowns (DuelsRooms.cpp).
            Rooms::WriteState(w);
            // Hacking: how far our pulse has run (DuelsHacking.cpp); mind control: our control's timer (DuelsMind.cpp).
            Hacking::WriteState(w);
            Mind::WriteState(w);
            // The match: the damage our ship took this round (DuelsRounds.cpp).
            Rounds::WriteState(w);
        }

        static void SendState(double now)
        {
            ShipManager *ship = G_->GetShipManager(0);
            if (!ship) return;
            // What the opponent can see of our ship now (roadmap 4.5).
            Vision::Update();
            Writer w;
            uint16_t seq = ++g_match.stateSeq;
            WriteOwnState(w, ship, now, seq, false);
            Net::Send(MSG_STATE, w, false);
            // Our ship with all in sight: for our demo (roadmap 5.1) and the relay's (CN), when either records; every
            // second state (5 a second, DM: smaller demos; the replay's ship walks and charges between them).
            if (Demo::WantsFullStates() && seq % 2 == 0)
            {
                Vision::FullScope full;
                Writer record;
                WriteOwnState(record, ship, now, seq, true);
                Demo::FullState(record);
            }
            g_match.lastStateSent = now;
            g_match.stateDirty = false;
        }

        struct WeaponState { uint8_t powered; float charge, full; };   // powered 2, charge < 0: not seen

        // A ship's state as its owner sent it: the ship's own part (the modules' parts follow it in the message).
        struct ShipState
        {
            double sentAt = 0.0;
            uint16_t seq = 0;
            uint8_t vision = 0;
            int hull = 0;
            bool hasShields = false;
            int shieldLayers = 0;
            float shieldCharge = 0.f;
            int superShield = 0, superShieldMax = 0;
            std::vector<SystemState> systems;
            bool hasBattery = false, batteryOn = false;
            float batteryTime = 0.f;
            bool hasCloak = false, cloakOn = false;
            float cloakTime = 0.f, cloakGoal = 0.f;
            std::vector<WeaponState> weapons;
            std::vector<float> artilleryCharge;
        };

        static bool ReadShipState(Reader &r, ShipState &s)
        {
            s.sentAt = r.F64();
            s.seq = r.U16();
            s.vision = r.U8();   // what we see of their ship (roadmap 4.5)
            s.hull = r.I16();
            s.hasShields = r.Bool();
            if (s.hasShields)
            {
                s.shieldLayers = r.U8();
                s.shieldCharge = r.F32();
                s.superShield = r.U8();
                s.superShieldMax = r.U8();
            }
            s.systems.resize(r.U8());
            for (SystemState &system : s.systems)
            {
                system.id = r.U8();
                system.power = r.U8();
                system.health = r.U8();
                system.lock = r.I8();
                system.lockTime = system.lock > 0 ? r.F32() : 0.f;
                system.lockGoal = system.lock > 0 ? r.F32() : 0.f;
                system.hack = r.U8();
                system.bonus = r.U8();
            }
            s.hasBattery = r.Bool();
            s.batteryOn = s.hasBattery && r.Bool();
            s.batteryTime = s.hasBattery ? r.F32() : 0.f;
            s.hasCloak = r.Bool();
            s.cloakOn = s.hasCloak && r.Bool();
            s.cloakTime = s.hasCloak ? r.F32() : 0.f;
            s.cloakGoal = s.hasCloak ? r.F32() : 0.f;
            s.weapons.resize(r.U8());
            for (WeaponState &weapon : s.weapons)
            {
                weapon.powered = r.U8();
                weapon.charge = r.F32();
                weapon.full = r.F32();
            }
            s.artilleryCharge.resize(r.U8());
            for (float &charge : s.artilleryCharge) charge = r.F32();
            return r.Ok();
        }

        // The ship's part of a state on a ship driven by it (the opponent's copy; in a replay our ship too).
        static void ApplyShipState(ShipManager *replica, const ShipState &s)
        {
            const double sentAt = s.sentAt;
            const int hull = s.hull;
            const bool hasShields = s.hasShields;
            const int shieldLayers = s.shieldLayers;
            const float shieldCharge = s.shieldCharge;
            const int superShield = s.superShield, superShieldMax = s.superShieldMax;
            const std::vector<SystemState> &systems = s.systems;
            const bool hasBattery = s.hasBattery, batteryOn = s.batteryOn;
            const float batteryTime = s.batteryTime;
            const bool hasCloak = s.hasCloak, cloakOn = s.cloakOn;
            const float cloakTime = s.cloakTime, cloakGoal = s.cloakGoal;
            const std::vector<WeaponState> &weapons = s.weapons;
            const std::vector<float> &artilleryCharge = s.artilleryCharge;
            (void)sentAt; (void)hull; (void)hasShields; (void)shieldLayers; (void)shieldCharge; (void)superShield; (void)superShieldMax;
            (void)hasBattery; (void)batteryOn; (void)batteryTime; (void)hasCloak; (void)cloakOn; (void)cloakTime; (void)cloakGoal;
            (void)artilleryCharge;

            replica->ship.hullIntegrity.first = std::min(hull, replica->ship.hullIntegrity.second);

            // The owner's locks first (ion, and the battery's): power is then set the way the owner's was changed.
            ApplyLocks(replica, systems);
            for (const SystemState &state : systems)
            {
                g_driven[replica->iShipId].hackFlags[state.id] = state.hack;
                if (state.bonus != 255) g_driven[replica->iShipId].bonusPower[state.id] = state.bonus;   // 255: not seen (roadmap 4.5)
            }
            HoldReplicaHacking(replica);

            // The battery before the systems: the extra power it gives must be there for them to draw on. Its timer
            // runs behind the owner's like the lock timers, so it goes off when the owner's does.
            BatterySystem *battery = replica->batterySystem;
            if (hasBattery && battery)
            {
                if (batteryOn && !battery->bTurnedOn) battery->SetTurnedOn(true, true);
                else if (!batteryOn && battery->bTurnedOn) battery->SetTurnedOn(false, false);
                if (battery->bTurnedOn) battery->timer.currTime = std::max(0.f, batteryTime - LOCK_TIMER_LAG_S);
            }

            // Damage first, so power never exceeds what the system can hold.
            for (const SystemState &state : systems)
            {
                ShipSystem *system = replica->GetSystem(state.id);
                if (!system) continue;
                int health = std::max(0, std::min(state.health, system->healthState.second));
                if (system->healthState.first != health)
                {
                    system->healthState.first = health;
                    if (state.id != SYS_DRONES && PowerBars(system) > health) SetReplicaPower(replica, system, health);
                }
            }
            for (const SystemState &state : systems)
            {
                // Weapons and drones power follows the weapons and drones, one by one (below, and DuelsDrones.cpp):
                // raising drone power by itself would launch the replica's drones in slot order.
                ShipSystem *system = replica->GetSystem(state.id);
                if (state.id == SYS_WEAPONS || state.id == SYS_DRONES || !system || state.power == 255) continue;
                if (!system->bNeedsPower) g_driven[replica->iShipId].subsystemPower[state.id] = state.power;
                if (PowerBars(system) != state.power) SetReplicaPower(replica, system, state.power);
                if (PowerBars(system) != state.power) LogPowerMiss(system, state.power);
            }

            // The cloak after the systems, so its power is there.
            if (hasCloak && replica->cloakSystem) SetReplicaCloak(replica->cloakSystem, cloakOn, cloakTime, cloakGoal);

            if (hasShields && replica->shieldSystem)
            {
                replica->shieldSystem->shields.power.first = std::min(shieldLayers, 16);
                replica->shieldSystem->shields.charger = shieldCharge;
                replica->shieldSystem->shields.power.super.second = superShieldMax;
                replica->shieldSystem->shields.power.super.first = std::min(superShield, superShieldMax);
            }

            if (replica->weaponSystem)
            {
                std::vector<ProjectileFactory*> list = replica->GetWeaponList();
                for (size_t slot = 0; slot < list.size() && slot < weapons.size(); ++slot)
                {
                    // What isn't seen (roadmap 4.5) stays as last seen.
                    ProjectileFactory *weapon = list[slot];
                    if (weapons[slot].powered == 1 && !weapon->powered) replica->PowerWeapon(weapon, true, true);
                    else if (weapons[slot].powered == 0 && weapon->powered) replica->DePowerWeapon(weapon, true);
                    if (weapons[slot].charge >= 0.f) weapon->cooldown.first = std::min(weapons[slot].charge, weapon->cooldown.second);
                }
            }
            for (size_t i = 0; i < replica->artillerySystems.size() && i < artilleryCharge.size(); ++i)
            {
                ProjectileFactory *weapon = replica->artillerySystems[i] ? replica->artillerySystems[i]->projectileFactory : nullptr;
                if (weapon && artilleryCharge[i] >= 0.f) weapon->cooldown.first = std::min(artilleryCharge[i], weapon->cooldown.second);
            }
        }

        static void ApplyState(Reader &r)
        {
            ShipState s;
            if (!ReadShipState(r, s)) return;
            if (!Drones::ReadState(r) || !Crew::ReadState(r) || !Rooms::ReadState(r) || !Hacking::ReadState(r) || !Mind::ReadState(r) ||
                !Rounds::ReadState(r) || !r.Ok()) return;
            const double sentAt = s.sentAt;
            const uint16_t seq = s.seq;
            const uint8_t vision = s.vision;
            const int hull = s.hull;
            const bool hasShields = s.hasShields;
            const int shieldLayers = s.shieldLayers;
            const std::vector<SystemState> &systems = s.systems;
            const bool batteryOn = s.batteryOn;
            const std::vector<WeaponState> &weapons = s.weapons;

            // Snapshots may arrive out of order; only newer ones count.
            if (g_match.havePeerState && (uint16_t)(seq - g_match.peerStateSeq) >= 32768) return;
            g_match.havePeerState = true;
            g_match.peerStateSeq = seq;
            g_match.peerVision = vision;

            ShipManager *replica = G_->GetShipManager(1);
            if (!replica || !g_match.replicaReady) return;

            // Checked first, as it came (roadmap 4.1, layer 2): what the rules allow their ship.
            {
                Fair::StateCheck check;
                check.sentAt = sentAt;
                Rounds::Phase phase = Rounds::GetPhase();
                check.fight = Rounds::FightBegun() && (phase == Rounds::Phase::Fight || Rounds::Free());
                check.hull = hull;
                // (What we don't see of their ship isn't checked: it does nothing here.)
                if (hasShields)
                {
                    check.shieldLayers = shieldLayers;
                    for (const SystemState &state : systems)
                    {
                        if (state.id == SYS_SHIELDS) check.shieldPower = state.power;
                    }
                    if (check.shieldPower == 255) check.shieldLayers = -1;
                }
                // The systems that draw on the reactor (not the subsystems, not the bays: their bars are their weapons').
                bool powerSeen = true;
                std::string seenPower;
                for (const SystemState &state : systems)
                {
                    ShipSystem *system = state.id < SYS_CUSTOM_FIRST ? replica->GetSystem(state.id) : nullptr;
                    if (!system || !system->bNeedsPower) continue;
                    if (state.power == 255)
                    {
                        powerSeen = false;
                        continue;
                    }
                    check.powerUsed += state.power;
                    if (state.power > 0) seenPower += (seenPower.empty() ? "" : ", ") + ShipSystem::SystemIdToName(state.id) + " " + std::to_string(state.power);
                }
                PowerManager *power = PowerManager::GetPowerManager(1);
                if (power && powerSeen) check.powerAvailable = power->currentPower.second + (batteryOn ? 4 : 0);
                if (replica->droneSystem)
                {
                    for (Drone *drone : replica->droneSystem->drones)
                    {
                        if (drone && drone->type == DRONE_SHIP_REPAIR && drone->deployed && !drone->bDead) check.hullRepair = true;
                    }
                }
                for (const WeaponState &weapon : weapons)
                {
                    check.charges.push_back(weapon.charge < 0.f ? 0.f : weapon.charge);
                    check.cooldowns.push_back(weapon.charge < 0.f ? 0.f : weapon.full);
                }
                Fair::CheckState(check);
                // Not all of it seen (roadmap 4.5): what we see drawn, with their drones out, for the check by their shots
                // (any of their Zoltans may power a system for free).
                if (power && !powerSeen)
                {
                    int drones = 0, zoltans = 0;
                    if (replica->droneSystem)
                    {
                        for (Drone *drone : replica->droneSystem->drones)
                        {
                            if (drone && drone->powered && drone->blueprint) drones += drone->blueprint->power;
                        }
                    }
                    for (CrewMember *crew : replica->vCrewList)
                    {
                        if (crew && !crew->bDead && crew->iShipId == 1 && crew->species.compare(0, 6, "energy") == 0) ++zoltans;
                    }
                    if (drones > 0) seenPower += (seenPower.empty() ? "drones " : ", drones ") + std::to_string(drones);
                    Fair::PowerSeen(Net::HasClock() ? Net::PeerToLocalTime(sentAt) : WallMs(), check.powerUsed + drones,
                                    power->currentPower.second + (batteryOn ? 4 : 0) + zoltans,
                                    seenPower.empty() ? std::string("nothing else seen") : seenPower);
                }
            }

            ApplyShipState(replica, s);

            // Drones after the systems, so the reactor power they need is free.
            Drones::ApplyState(Net::HasClock() ? Net::PeerToLocalTime(sentAt) : WallMs());
            Crew::ApplyState(Net::HasClock() ? Net::PeerToLocalTime(sentAt) : WallMs());
            Rooms::ApplyState();
            Hacking::ApplyState();
            Mind::ApplyState();

            // Again, as switching the battery sets its own lock. Between updates the replica counts its lock timers on
            // (ShipSystem::OnLoop), so the lock display runs smoothly.
            ApplyLocks(replica, systems);
            ++g_match.statesApplied;
        }

        void ReplayOwnState(const uint8_t *data, size_t size)
        {
            ShipManager *own = G_->GetShipManager(0);
            if (!Net::Replaying() || !own) return;
            Reader r(data, size);
            ShipState s;
            if (!ReadShipState(r, s)) return;
            if (!g_replayOwnDriven) Log("Match: replay: our ship follows the recorder's states from now on");
            g_replayOwnDriven = true;
            ApplyShipState(own, s);
            // Its drones, crew and rooms.
            if (Drones::ReplayOwnState(r, WallMs()) && Crew::ReplayOwnState(r, WallMs())) Rooms::ReplayOwnState(r);
            ApplyLocks(own, s.systems);
        }

        // --------------------------------------------------------------------------------------------------------
        // Shots
        // --------------------------------------------------------------------------------------------------------

        // Tells the opponent about a shot of ours at their ship: where it goes and what the defender needs to build the
        // same projectile. A drone's shot starts in their space, at the drone; a crystal shard where our ship was hit.
        static void SendShot(Projectile *projectile, const WeaponBlueprint *blueprint, uint8_t source, int slot,
                             SpaceDrone *drone = nullptr, const MatchState::PendingShard *crystal = nullptr)
        {
            MatchState &m = g_match;
            int type = blueprint->type;
            double now = WallMs();
            OutShot shot;
            shot.projectile = projectile;
            shot.selfId = projectile->selfId;
            shot.netId = m.nextNetId++;
            shot.slot = slot;
            shot.type = type;
            shot.weapon = blueprint->name;
            shot.spawnMs = now;
            shot.source = source;
            shot.drone = source == SOURCE_DRONE;
            shot.fair = Fair::NextShot();
            m.out.push_back(shot);

            // The flight in our own space before the shot crosses into theirs (the defender's copy leaves the replica
            // then); a shard is sent as it crosses, so none is left.
            int timing = TimingSlot(source, slot);
            double leg = source == SOURCE_DRONE || source == SOURCE_SHARD ? 0.0
                       : timing >= 0 && m.legEstimate[timing] > 0.0 ? m.legEstimate[timing]
                       : (type == WEAPON_MISSILES ? 560.0 : 330.0);
            Writer w;
            w.U32(shot.netId);
            w.U8((uint8_t)slot);
            w.Str(shot.weapon);
            w.F32(projectile->target.x);
            w.F32(projectile->target.y);
            w.F64(now);
            w.F32((float)leg);
            // What the defender needs to build the same projectile: a beam's second point (it sweeps from the first),
            // a bomb's Zoltan-shield bypass, a flak shard's look (and whether it is one of the harmless ones).
            w.U8((uint8_t)type);
            if (type == WEAPON_BEAM)
            {
                BeamWeapon *beam = static_cast<BeamWeapon*>(projectile);
                w.F32(beam->target2.x);
                w.F32(beam->target2.y);
            }
            else if (type == WEAPON_BOMB)
            {
                w.U8(static_cast<BombProjectile*>(projectile)->superShieldBypass ? 1 : 0);
            }
            else if (type == WEAPON_BURST)
            {
                uint8_t shard = 0xFF;
                for (size_t i = 0; i < blueprint->miniProjectiles.size() && i < 0xFF; ++i)
                {
                    if (blueprint->miniProjectiles[i].image == projectile->flight_animation.animName)
                    {
                        shard = (uint8_t)i;
                        break;
                    }
                }
                w.U8(shard);
                w.U8(projectile->damage.iDamage == 0 && blueprint->damage.iDamage > 0 ? 1 : 0);
            }
            // Where it comes from: a weapon or an artillery system of our ship (its mount), one of our drones (position
            // in their space, and its aim), or a crystal shard (where it broke off in our space, heading, entry angle).
            w.U8(source);
            if (source == SOURCE_DRONE)
            {
                w.F32(projectile->position.x);
                w.F32(projectile->position.y);
                w.F32(drone ? drone->aimingAngle : 0.f);
            }
            else if (source == SOURCE_SHARD)
            {
                w.F32(crystal ? crystal->origin.x : projectile->position.x);
                w.F32(crystal ? crystal->origin.y : projectile->position.y);
                w.F32(crystal ? crystal->heading : projectile->heading);
                w.F32(crystal ? crystal->entryAngle : projectile->entryAngle);
            }
            // Its value for the dodge roll (roadmap 4.1).
            Fair::WriteValue(w, shot.fair);
            Net::Send(MSG_SHOT, w, true);
            ++m.shotsSent;
        }

        static bool CanNetwork(const WeaponBlueprint *blueprint)
        {
            if (Networked(blueprint->type)) return true;
            if (g_match.unsupportedLogged.insert(blueprint->name).second)
            {
                Announce(blueprint->name + " is not networked yet; the opponent won't see it");
            }
            return false;
        }

        // Our artillery system with this weapon, as its index in ShipManager::artillerySystems; -1 if none.
        static int ArtilleryIndex(ShipManager *ship, const ProjectileFactory *weapon)
        {
            if (!ship || !weapon) return -1;
            for (size_t i = 0; i < ship->artillerySystems.size(); ++i)
            {
                if (ship->artillerySystems[i] && ship->artillerySystems[i]->projectileFactory == weapon) return (int)i;
            }
            return -1;
        }

        void OnOwnProjectile(ProjectileFactory *weapon, Projectile *projectile)
        {
            MatchState &m = g_match;
            if (!Net::IsConnected() || !m.replicaReady || !projectile || projectile->destinationSpace != 1) return;
            ShipManager *ship = G_->GetShipManager(0);
            if (!weapon->blueprint) return;
            int slot = WeaponSlot(ship, weapon);
            uint8_t source = SOURCE_WEAPON;
            if (slot < 0)
            {
                slot = ArtilleryIndex(ship, weapon);
                source = SOURCE_ARTILLERY;
            }
            if (slot < 0 || !CanNetwork(weapon->blueprint)) return;
            SendShot(projectile, weapon->blueprint, source, slot);
        }

        bool AllowShards(const ShipManager *ship)
        {
            // No new shots once the round is decided (roadmap O), and no Crystal Vengeance shard either: a shot still in
            // the air that lands after the decision broke one off (step2-augments, 2026-10-01).
            Rounds::Phase phase = Rounds::GetPhase();
            if (phase == Rounds::Phase::Ending || phase == Rounds::Phase::RoundOver || phase == Rounds::Phase::MatchOver) return false;
            // The replica's shards come from its owner's game.
            return !ship || ship->iShipId != 1 || !Net::IsConnected() || !g_match.replicaReady;
        }

        bool AllowNewShots(const ProjectileFactory *weapon)
        {
            // A replay's ship 0 is the recorder's: its shots come from the demo (roadmap 5.1), never its own.
            if (weapon && weapon->iShipId == 0 && Net::Replaying()) return false;
            if (!weapon || !Rounds::InMatch()) return true;
            if (weapon->iShipId != 0 && !(weapon->iShipId == 1 && Ai::Active())) return true;
            Rounds::Phase phase = Rounds::GetPhase();
            return phase != Rounds::Phase::Ending && phase != Rounds::Phase::RoundOver && phase != Rounds::Phase::MatchOver;
        }

        bool ReplicaArtillery(const ArtillerySystem *artillery)
        {
            return artillery && artillery->_shipObj.iShipId == 1 && Net::IsConnected() && g_match.replicaReady;
        }

        void OnReplicaArtilleryHeld()
        {
            ++g_match.artilleryHeld;
        }

        bool ReplicaBonusPower(const ShipSystem *system, int &amount)
        {
            int id = system ? system->_shipObj.iShipId : -1;
            if ((id != 0 && id != 1) || !Net::IsConnected() || !IsDriven(id)) return false;
            const std::map<int, int> &bonusPower = g_driven[id].bonusPower;
            auto found = bonusPower.find(system->iSystemType);
            if (found == bonusPower.end()) return false;
            amount = found->second;
            return true;
        }

        void OnOwnShard(Projectile *projectile)
        {
            MatchState &m = g_match;
            if (!Net::IsConnected() || !m.replicaReady || !projectile || projectile->destinationSpace != 1) return;
            const std::string &name = PR_EX(projectile)->name;
            const WeaponBlueprint *blueprint = G_->GetBlueprints()->GetWeaponBlueprint(name);
            if (!blueprint || blueprint->name != name || !CanNetwork(blueprint)) return;
            MatchState::PendingShard shard;
            shard.projectile = projectile;
            shard.selfId = projectile->selfId;
            shard.blueprint = blueprint;
            shard.origin = projectile->position;
            shard.heading = projectile->heading;
            shard.entryAngle = projectile->entryAngle;
            m.pendingShards.push_back(shard);
        }

        // Each frame: a shard that crossed into the replica's space goes to the opponent now; one that is gone
        // without crossing (FTL lost it) never does.
        static void SendCrossedShards(const std::set<Projectile*> &live)
        {
            MatchState &m = g_match;
            for (size_t i = 0; i < m.pendingShards.size();)
            {
                MatchState::PendingShard &shard = m.pendingShards[i];
                bool alive = live.count(shard.projectile) && shard.projectile->selfId == shard.selfId;
                if (alive && shard.projectile->currentSpace != 1)
                {
                    ++i;
                    continue;
                }
                if (alive) SendShot(shard.projectile, shard.blueprint, SOURCE_SHARD, 0, nullptr, &shard);
                else ++m.shardsLost;
                m.pendingShards.erase(m.pendingShards.begin() + i);
            }
        }

        void OnOwnDroneProjectile(SpaceDrone *drone, Projectile *projectile)
        {
            if (!projectile || !drone || drone->iShipId != 0) return;
            if (projectile->currentSpace == drone->iShipId)
            {
                // A defense drone's shot in our space: it may hit their shots and drones here; they see a copy.
                Drones::OnOwnDroneShotInOwnSpace(drone, projectile);
                return;
            }
            MatchState &m = g_match;
            if (!Net::IsConnected() || !m.replicaReady || projectile->currentSpace != 1 || !drone->weaponBlueprint) return;
            int slot = Drones::SlotOf(G_->GetShipManager(0), drone);
            if (slot < 0 || !CanNetwork(drone->weaponBlueprint)) return;
            SendShot(projectile, drone->weaponBlueprint, SOURCE_DRONE, slot, drone);
        }

        bool HiddenFromDefense(const Targetable *target)
        {
            if (!target) return false;
            for (const InShot &shot : g_match.in)
            {
                const Projectile *projectile = shot.projectile;
                if (!projectile || &projectile->_targetable != target) continue;
                if (projectile->selfId != shot.selfId || projectile->currentSpace != 0) return false;
                return !shot.released || shot.catchUpMs > 0.0;
            }
            return false;
        }

        static void SendResult(InShot &shot, uint8_t outcome, int damage, Pointf point = Pointf(0.f, 0.f))
        {
            double now = WallMs();
            shot.outcome = outcome;
            shot.damage = damage;
            shot.decisionMs = now;
            Writer w;
            w.U32(shot.netId);
            w.U8(outcome);
            w.I8((int8_t)damage);
            if (outcome == OUTCOME_DOWNED)
            {
                w.F32(point.x);
                w.F32(point.y);
            }
            // The dodge as rolled (roadmap 4.1): the evasion and our verdict chain's value, for the attacker to check.
            w.Bool(shot.rolled);
            if (shot.rolled)
            {
                w.U8((uint8_t)std::max(0, std::min(255, shot.evasion)));
                Fair::WriteValue(w, shot.fairVerdict);
            }
            Net::Send(MSG_RESULT, w, true);
            ++g_match.verdictsSent;
            g_match.stateDirty = true;   // the damage should arrive together with the verdict
        }

        // A flak shard's own look; the harmless ones do no damage (as CustomWeapons.cpp builds them).
        static void MakeShard(Projectile *projectile, const WeaponBlueprint *blueprint, int shard, bool fakeShard)
        {
            const WeaponBlueprint::MiniProjectile &mini = blueprint->miniProjectiles[shard < (int)blueprint->miniProjectiles.size() ? shard : 0];
            projectile->flight_animation = G_->GetAnimationControl()->GetAnimation(mini.image);
            projectile->flight_animation.SetCurrentFrame(random32() % std::max(1, projectile->flight_animation.info.numFrames));
            projectile->flight_animation.Stop();
            if (!fakeShard && !mini.fake) return;
            Damage &damage = projectile->damage;
            damage.iDamage = 0;
            damage.iShieldPiercing = 0;
            damage.fireChance = 0;
            damage.breachChance = 0;
            damage.stunChance = 0;
            damage.iIonDamage = 0;
            damage.iSystemDamage = 0;
            damage.iPersDamage = 0;
            damage.bHullBuster = false;
            damage.ownerId = -1;
            damage.selfId = -1;
            damage.bLockdown = false;
            damage.crystalShard = false;
            damage.bFriendlyFire = true;
            damage.iStun = 0;
            projectile->death_animation.fScale = 0.25f;
        }

        static void PlayLaunchSound(const WeaponBlueprint *blueprint)
        {
            if (blueprint->effects.launchSounds.empty()) return;
            G_->GetSoundControl()->PlaySoundMix(blueprint->effects.launchSounds[random32() % blueprint->effects.launchSounds.size()], -1.f, false);
        }

        // A shot message (MSG_SHOT, as SendShot writes it).
        struct ShotMessage
        {
            uint32_t netId = 0;
            int slot = 0;
            std::string weapon;
            Pointf target, target2;
            double spawn = 0.0;
            double leg = 0.0;
            int type = WEAPON_LASER;
            bool bombBypass = false;
            int shard = 0xFF;
            bool fakeShard = false;
            uint8_t source = SOURCE_WEAPON;
            Pointf origin;
            float droneAim = 0.f, shardHeading = 0.f, shardEntry = 0.f;
            Fair::Value fair;
        };

        static bool ReadShot(Reader &r, ShotMessage &s)
        {
            s.netId = r.U32();
            s.slot = r.U8();
            s.weapon = r.Str();
            s.target.x = r.F32();
            s.target.y = r.F32();
            s.spawn = r.F64();
            s.leg = r.F32();
            s.type = r.U8();
            s.target2 = s.target;
            if (s.type == WEAPON_BEAM)
            {
                s.target2.x = r.F32();
                s.target2.y = r.F32();
            }
            else if (s.type == WEAPON_BOMB)
            {
                s.bombBypass = r.U8() != 0;
            }
            else if (s.type == WEAPON_BURST)
            {
                s.shard = r.U8();
                s.fakeShard = r.U8() != 0;
            }
            s.source = r.U8();
            if (s.source == SOURCE_DRONE)
            {
                s.origin.x = r.F32();
                s.origin.y = r.F32();
                s.droneAim = r.F32();
            }
            else if (s.source == SOURCE_SHARD)
            {
                s.origin.x = r.F32();
                s.origin.y = r.F32();
                s.shardHeading = r.F32();
                s.shardEntry = r.F32();
            }
            Fair::ReadValue(r, s.fair);
            return r.Ok();
        }

        static void CreateIncomingShot(Reader &r)
        {
            ShotMessage s;
            if (!ReadShot(r, s)) return;
            uint32_t netId = s.netId;
            int slot = s.slot;
            const std::string &weaponName = s.weapon;
            Pointf target = s.target, target2 = s.target2, origin = s.origin;
            double peerSpawn = s.spawn, ownLeg = s.leg;
            int type = s.type;
            bool bombBypass = s.bombBypass, fakeShard = s.fakeShard;
            int shard = s.shard;
            uint8_t source = s.source;
            bool fromDrone = source == SOURCE_DRONE;
            float droneAim = s.droneAim, shardHeading = s.shardHeading, shardEntry = s.shardEntry;
            const Fair::Value &fair = s.fair;
            MatchState &m = g_match;
            ++m.shotsReceived;
            double now = WallMs();

            InShot shot;
            shot.netId = netId;
            shot.type = type;
            shot.weapon = weaponName;
            shot.receivedMs = now;
            shot.spawnMs = Net::HasClock() ? Net::PeerToLocalTime(peerSpawn) : now;
            shot.source = source;
            shot.drone = fromDrone;
            shot.fair = fair;

            ShipManager *replica = G_->GetShipManager(1);
            ShipManager *own = G_->GetShipManager(0);
            ProjectileFactory *weapon = nullptr;
            const WeaponBlueprint *blueprint = nullptr;
            SpaceDrone *drone = nullptr;
            if (fromDrone || source == SOURCE_SHARD)
            {
                // A drone's weapon or a shard is a plain blueprint; a drone itself is the replica's (a puppet) in that
                // slot.
                blueprint = G_->GetBlueprints()->GetWeaponBlueprint(weaponName);
                if (blueprint && blueprint->name != weaponName) blueprint = nullptr;
                if (fromDrone && replica && replica->droneSystem && slot < (int)replica->droneSystem->drones.size())
                {
                    Drone *candidate = replica->droneSystem->drones[slot];
                    if (candidate->type == 1 || candidate->type == 5) drone = static_cast<SpaceDrone*>(candidate);
                }
            }
            else if (source == SOURCE_ARTILLERY)
            {
                // The replica's artillery system with the same index fires it.
                if (replica && slot < (int)replica->artillerySystems.size() && replica->artillerySystems[slot])
                {
                    ProjectileFactory *candidate = replica->artillerySystems[slot]->projectileFactory;
                    if (candidate && candidate->blueprint && candidate->blueprint->name == weaponName) weapon = candidate;
                }
                blueprint = weapon ? weapon->blueprint : nullptr;
            }
            else if (replica && replica->weaponSystem)
            {
                std::vector<ProjectileFactory*> list = replica->GetWeaponList();
                if (slot < (int)list.size() && list[slot]->blueprint && list[slot]->blueprint->name == weaponName) weapon = list[slot];
                blueprint = weapon ? weapon->blueprint : nullptr;
            }
            bool needsWeapon = source == SOURCE_WEAPON || source == SOURCE_ARTILLERY;
            if (!blueprint || !own || !replica || blueprint->type != type || !Networked(type) || (needsWeapon && !weapon))
            {
                Log("Match: shot %u from %s %s %d cannot be shown (not on the replica)", netId, weaponName.c_str(),
                    SourceName(source), slot);
                SendResult(shot, OUTCOME_GONE, 0);
                return;
            }
            // Its weapon's rate of fire, and that it was powered (roadmap 4.5: from what is always seen), by when it left
            // their weapon: their stamp, but no later than it came (a stamp ahead of its message is a lie).
            if (source == SOURCE_WEAPON) Fair::OnShot(slot, blueprint, replica, std::min(shot.spawnMs, now + 250.0 + Net::RttMs() * 0.5));

            Projectile *projectile = nullptr;
            if (fromDrone)
            {
                // A drone fires from its orbit, outside our shields; a shot from inside them would pass them unseen.
                // One claimed from inside (a bug, or a changed game) is moved out onto the combat drones' orbit,
                // 1.15 x the shield ellipse, along the line from the ellipse's centre.
                Globals::Ellipse shields = own->ship.GetBaseEllipse();
                float dx = origin.x - shields.center.x, dy = origin.y - shields.center.y;
                float radius = shields.a > 0.f && shields.b > 0.f
                    ? std::sqrt(dx * dx / (shields.a * shields.a) + dy * dy / (shields.b * shields.b)) : 2.f;
                if (radius < 1.f)
                {
                    if (radius < 0.001f)
                    {
                        dx = 0.f;
                        dy = -shields.b;
                        radius = 1.f;
                    }
                    Pointf moved(shields.center.x + dx * 1.15f / radius, shields.center.y + dy * 1.15f / radius);
                    Log("Match: drone shot %u starts inside our shields at (%.0f, %.0f); moved out to (%.0f, %.0f)", netId,
                        origin.x, origin.y, moved.x, moved.y);
                    origin = moved;
                }
                // As SpaceDrone::GetNextProjectile builds it: in our space from the start, at the drone, owned by the
                // replica (the constructor takes the space as the owner).
                switch (type)
                {
                case WEAPON_LASER:
                case WEAPON_BURST:
                {
                    LaserBlast *laser = new LaserBlast(origin, 0, 0, target);
                    laser->heading = -1.f;
                    laser->OnInit();
                    projectile = laser;
                    break;
                }
                case WEAPON_MISSILES:
                    projectile = new Missile(origin, 0, 0, target, droneAim);
                    break;
                case WEAPON_BEAM:
                    projectile = new BeamWeapon(origin, 0, 0, target, target2, blueprint->length, &own->_targetable, droneAim);
                    break;
                default:
                {
                    BombProjectile *bomb = new BombProjectile(origin, 0, 0, target);
                    bomb->superShieldBypass = bombBypass;
                    projectile = bomb;
                    break;
                }
                }
                projectile->Initialize(*blueprint);
                projectile->ownerId = 1;
                if (type == WEAPON_BURST && !blueprint->miniProjectiles.empty()) MakeShard(projectile, blueprint, shard, fakeShard);
                else if (drone) projectile->flight_animation = drone->weapon_animation;
                G_->GetWorld()->space.AddProjectile(projectile);
                PlayLaunchSound(blueprint);
                // No flight between the spaces to hide the delay in: it goes at once and makes up the one-way latency
                // over its first frames, so the attacker's copy waits at our shields for about that long, not the round
                // trip. A beam keeps its pace; a bomb has no flight.
                shot.releaseAt = 0.0;
                shot.speedUp = 1;
                if (type != WEAPON_BEAM && type != WEAPON_BOMB) shot.catchUpMs = Net::RttMs() * 0.5;
            }
            else
            {
                // Like ProjectileFactory::Update does it (CustomWeapons.cpp), at the replica's weapon (or artillery)
                // mount; a crystal shard where the replica was hit, as ShipManager::CheckCrystalAugment makes it.
                Pointf position = origin;
                float heading = shardHeading, entryAngle = shardEntry;
                if (weapon)
                {
                    Point fireLocation = weapon->weaponVisual.GetFireLocation() + weapon->localPosition;
                    if (type == WEAPON_MISSILES)
                    {
                        if (weapon->currentFiringAngle == 0.f) fireLocation.x += 16;
                        else if (weapon->currentFiringAngle == 270.f) fireLocation.y -= 16;
                    }
                    position = Pointf((float)fireLocation.x, (float)fireLocation.y);
                    heading = weapon->currentFiringAngle;
                    entryAngle = weapon->currentEntryAngle;
                }
                switch (type)
                {
                case WEAPON_LASER:
                case WEAPON_BURST:
                {
                    LaserBlast *laser = new LaserBlast(position, 1, 0, target);
                    if (!weapon) laser->heading = heading;   // as a shard is made
                    laser->OnInit();
                    projectile = laser;
                    break;
                }
                case WEAPON_MISSILES:
                    projectile = new Missile(position, 1, 0, target, heading);
                    break;
                case WEAPON_BEAM:
                {
                    BeamWeapon *beam = new BeamWeapon(position, 1, 0, target, target2, blueprint->length, &own->_targetable,
                                                      heading);
                    if (weapon) beam->SetWeaponAnimation(&weapon->weaponVisual);
                    projectile = beam;
                    break;
                }
                default:
                {
                    BombProjectile *bomb = new BombProjectile(position, 1, 0, target);
                    bomb->superShieldBypass = bombBypass;
                    projectile = bomb;
                    break;
                }
                }
                projectile->entryAngle = entryAngle;
                projectile->Initialize(*blueprint);
                projectile->heading = heading;
                if (type == WEAPON_BURST && !blueprint->miniProjectiles.empty()) MakeShard(projectile, blueprint, shard, fakeShard);
                else if (weapon) projectile->flight_animation = weapon->flight_animation;
                // A shard's hit breaks off no shard of its own (FTL marks them).
                if (source == SOURCE_SHARD) projectile->damage.crystalShard = true;
                G_->GetWorld()->space.AddProjectile(projectile);

                // The weapon fires once per volley: a flak volley's shards arrive together.
                int timing = TimingSlot(source, slot);
                bool sameVolley = type == WEAPON_BURST && timing >= 0 && now - m.lastFireMs[timing] < 100.0;
                if (timing >= 0) m.lastFireMs[timing] = now;
                if (!sameVolley)
                {
                    if (weapon) weapon->weaponVisual.StartFire();
                    PlayLaunchSound(blueprint);
                }

                // Timing: our copy should enter our space when theirs entered the replica's space on their screen, less
                // the one-way latency, so the verdict is back when their shot reaches our shields. The flight out of the
                // enemy window runs faster to make up for the time the message took. A beam has no flight: it reaches
                // across at once, and its sweep keeps the weapon's own pace.
                double oneWay = Net::RttMs() * 0.5;
                if (type == WEAPON_BEAM)
                {
                    shot.releaseAt = 0.0;
                    shot.speedUp = 1;
                }
                else
                {
                    shot.releaseAt = shot.spawnMs + ownLeg - oneWay;
                    double available = shot.releaseAt - now;
                    shot.speedUp = available >= NATURAL_OPPONENT_LEG_MS ? 1
                                   : std::min(8, (int)std::ceil(NATURAL_OPPONENT_LEG_MS / std::max(available, 120.0)));
                }
            }
            shot.projectile = projectile;
            shot.selfId = projectile->selfId;
            m.in.push_back(shot);
        }

        void ReplayOwnShot(const uint8_t *data, size_t size)
        {
            MatchState &m = g_match;
            ShipManager *own = G_->GetShipManager(0);
            ShipManager *replica = G_->GetShipManager(1);
            WorldManager *world = G_->GetWorld();
            if (!Net::Replaying() || !own || !replica || !world || !m.replicaReady) return;
            Reader r(data, size);
            ShotMessage s;
            if (!ReadShot(r, s)) return;
            const WeaponBlueprint *blueprint = G_->GetBlueprints()->GetWeaponBlueprint(s.weapon);
            if (blueprint && blueprint->name != s.weapon) blueprint = nullptr;
            ProjectileFactory *weapon = nullptr;
            if (s.source == SOURCE_WEAPON && own->weaponSystem)
            {
                std::vector<ProjectileFactory*> list = own->GetWeaponList();
                if (s.slot < (int)list.size() && list[s.slot]->blueprint && list[s.slot]->blueprint->name == s.weapon) weapon = list[s.slot];
            }
            else if (s.source == SOURCE_ARTILLERY && s.slot < (int)own->artillerySystems.size() && own->artillerySystems[s.slot])
            {
                ProjectileFactory *candidate = own->artillerySystems[s.slot]->projectileFactory;
                if (candidate && candidate->blueprint && candidate->blueprint->name == s.weapon) weapon = candidate;
            }
            bool needsWeapon = s.source == SOURCE_WEAPON || s.source == SOURCE_ARTILLERY;
            if (!blueprint || blueprint->type != s.type || !Networked(s.type) || (needsWeapon && !weapon))
            {
                Log("Match: replay: our shot %u from %s %s %d isn't on our ship", s.netId, s.weapon.c_str(), SourceName(s.source), s.slot);
                return;
            }

            // Where it starts, as the opponent's game built it from the same message (CreateIncomingShot), here from our
            // ship at theirs: our weapon's (or artillery's) mount, as ProjectileFactory::Update; our drone's place in
            // their space; or where our ship was hit, for a crystal shard.
            bool fromDrone = s.source == SOURCE_DRONE;
            Pointf position = s.origin;
            float heading = fromDrone ? s.droneAim : s.shardHeading, entryAngle = s.shardEntry;
            if (weapon)
            {
                Point fireLocation = weapon->weaponVisual.GetFireLocation() + weapon->localPosition;
                if (s.type == WEAPON_MISSILES)
                {
                    if (weapon->currentFiringAngle == 0.f) fireLocation.x += 16;
                    else if (weapon->currentFiringAngle == 270.f) fireLocation.y -= 16;
                }
                position = Pointf((float)fireLocation.x, (float)fireLocation.y);
                heading = weapon->currentFiringAngle;
                entryAngle = weapon->currentEntryAngle;
            }
            int space = fromDrone ? 1 : 0;
            Projectile *projectile = nullptr;
            switch (s.type)
            {
            case WEAPON_LASER:
            case WEAPON_BURST:
            {
                LaserBlast *laser = new LaserBlast(position, space, 1, s.target);
                if (fromDrone) laser->heading = -1.f;
                else if (!weapon) laser->heading = heading;   // as a shard is made
                laser->OnInit();
                projectile = laser;
                break;
            }
            case WEAPON_MISSILES:
                projectile = new Missile(position, space, 1, s.target, heading);
                break;
            case WEAPON_BEAM:
            {
                BeamWeapon *beam = new BeamWeapon(position, space, 1, s.target, s.target2, blueprint->length, &replica->_targetable, heading);
                if (weapon) beam->SetWeaponAnimation(&weapon->weaponVisual);
                projectile = beam;
                break;
            }
            default:
            {
                BombProjectile *bomb = new BombProjectile(position, space, 1, s.target);
                bomb->superShieldBypass = s.bombBypass;
                projectile = bomb;
                break;
            }
            }
            if (!fromDrone) projectile->entryAngle = entryAngle;
            projectile->Initialize(*blueprint);
            if (!fromDrone) projectile->heading = heading;
            projectile->ownerId = 0;
            SpaceDrone *drone = nullptr;
            if (fromDrone && own->droneSystem && s.slot < (int)own->droneSystem->drones.size())
            {
                Drone *candidate = own->droneSystem->drones[s.slot];
                if (candidate && (candidate->type == 1 || candidate->type == 5)) drone = static_cast<SpaceDrone*>(candidate);
            }
            if (s.type == WEAPON_BURST && !blueprint->miniProjectiles.empty()) MakeShard(projectile, blueprint, s.shard, s.fakeShard);
            else if (weapon) projectile->flight_animation = weapon->flight_animation;
            else if (drone) projectile->flight_animation = drone->weapon_animation;
            if (s.source == SOURCE_SHARD) projectile->damage.crystalShard = true;
            world->space.AddProjectile(projectile);

            // The weapon fires once per volley: a flak volley's shards leave together.
            double now = WallMs();
            int timing = TimingSlot(s.source, s.slot);
            bool sameVolley = s.type == WEAPON_BURST && timing >= 0 && now - g_replayLastFireMs[timing] < 100.0;
            if (timing >= 0) g_replayLastFireMs[timing] = now;
            if (!sameVolley)
            {
                if (weapon) weapon->weaponVisual.StartFire();
                PlayLaunchSound(blueprint);
            }

            // It waits at their ship for the opponent's verdict (MSG_RESULT, recorded as it came), as ours do in a duel.
            OutShot shot;
            shot.projectile = projectile;
            shot.selfId = projectile->selfId;
            shot.netId = s.netId;
            shot.slot = s.slot;
            shot.type = s.type;
            shot.weapon = s.weapon;
            shot.spawnMs = now;
            shot.source = s.source;
            shot.drone = fromDrone;
            shot.fair = s.fair;
            m.out.push_back(shot);
            ++m.shotsSent;
        }

        void ReplayOwnResult(const uint8_t *data, size_t size)
        {
            if (!Net::Replaying()) return;
            Reader r(data, size);
            uint32_t netId = r.U32();
            uint8_t outcome = r.U8();
            int damage = r.I8();
            Pointf point;
            if (outcome == OUTCOME_DOWNED)
            {
                point.x = r.F32();
                point.y = r.F32();
            }
            if (!r.Ok()) return;
            for (InShot &shot : g_match.in)
            {
                if (shot.netId != netId) continue;
                if (shot.recorded == PENDING)
                {
                    shot.recorded = outcome;
                    shot.recordedDamage = damage;
                    shot.recordedPoint = point;
                    shot.recordedMs = WallMs();
                }
                return;
            }
        }

        // A projectile shot down: its explosion, as FTL's (and its sound).
        static void Explode(Projectile *projectile)
        {
            if (!projectile || projectile->startedDeath) return;
            projectile->death_animation.Start(true);
            projectile->startedDeath = true;
            projectile->missed = true;
            if (!projectile->hitSolidSound.empty()) G_->GetSoundControl()->PlaySoundMix(projectile->hitSolidSound, -1.f, false);
        }

        static bool InSpace(const Projectile *projectile)
        {
            WorldManager *world = G_->GetWorld();
            return world && projectile &&
                   std::find(world->space.projectiles.begin(), world->space.projectiles.end(), projectile) != world->space.projectiles.end();
        }

        void ReplayOwnShotDowned(const uint8_t *data, size_t size)
        {
            if (!Net::Replaying()) return;
            Reader r(data, size);
            uint32_t netId = r.U32();
            r.F32();
            r.F32();
            if (!r.Ok()) return;
            for (OutShot &shot : g_match.out)
            {
                if (shot.netId != netId) continue;
                if (shot.verdict == PENDING)
                {
                    shot.verdict = OUTCOME_DOWNED;
                    shot.verdictMs = WallMs();
                }
                if (!shot.exploded && InSpace(shot.projectile) && shot.projectile->selfId == shot.selfId)
                {
                    shot.exploded = true;
                    Explode(shot.projectile);
                    Drones::EndVisualShotsNear(shot.projectile->position.x, shot.projectile->position.y, shot.projectile->currentSpace);
                }
                return;
            }
        }

        // The most evasion a ship can have with its engines' power (FTL's: 5 a bar, 3 from the sixth on), its crew
        // at their best (piloting and engines manned by masters: 10 each) and its cloak (60): for a verdict on a ship
        // whose crew we don't see.
        static int EvasionBound(ShipManager *ship)
        {
            static const int ENGINES[] = {0, 5, 10, 15, 20, 25, 28, 31, 35};
            int power = ship->GetSystemPower(SYS_ENGINES);
            int evasion = ENGINES[std::max(0, std::min(power, 8))] + 20;
            if (ship->cloakSystem && ship->cloakSystem->bTurnedOn) evasion += 60;
            return evasion;
        }

        static void ApplyResult(Reader &r)
        {
            uint32_t netId = r.U32();
            uint8_t outcome = r.U8();
            int damage = r.I8();
            Pointf point;
            if (outcome == OUTCOME_DOWNED)
            {
                point.x = r.F32();
                point.y = r.F32();
            }
            bool rolled = r.Bool();
            int evasion = 0;
            Fair::Value verdict;
            if (rolled)
            {
                evasion = r.U8();
                Fair::ReadValue(r, verdict);
            }
            if (!r.Ok()) return;
            ++g_match.verdictsReceived;
            for (OutShot &shot : g_match.out)
            {
                if (shot.netId != netId) continue;
                if (rolled)
                {
                    // Their roll, checked (roadmap 4.1); the evasion against their ship as our copy of it has it now,
                    // or, without their crew in sight (roadmap 4.5), the most its engines and cloak allow.
                    ShipManager *replica = G_->GetShipManager(1);
                    bool crewSeen = (g_match.peerVision & (Vision::SEES_INTERIOR | Vision::SEES_LIFEFORMS)) != 0;
                    Fair::CheckRoll(shot.fair, verdict, evasion, outcome == OUTCOME_MISS,
                                    !replica ? -1 : crewSeen ? replica->GetDodgeFactor() : EvasionBound(replica),
                                    "shot " + std::to_string(netId) + " (" + shot.weapon + ")");
                }
                if (shot.verdict == PENDING)
                {
                    shot.verdict = outcome;
                    shot.damage = damage;
                    shot.verdictMs = WallMs();
                    shot.downPoint = point;
                }
                return;
            }
            Log("Match: verdict %s for shot %u, which is gone already", OutcomeName(outcome), netId);
        }

        // The attacker's shot ran into something in the attacker's own space: its copy on our screen goes too.
        static void ApplyShotDowned(Reader &r)
        {
            uint32_t netId = r.U32();
            float x = r.F32();
            float y = r.F32();
            if (!r.Ok()) return;
            WorldManager *world = G_->GetWorld();
            for (InShot &shot : g_match.in)
            {
                if (shot.netId != netId) continue;
                if (shot.outcome == PENDING)
                {
                    shot.outcome = OUTCOME_DOWNED;
                    shot.decisionMs = WallMs();
                }
                Projectile *projectile = shot.projectile;
                bool alive = world && projectile &&
                             std::find(world->space.projectiles.begin(), world->space.projectiles.end(), projectile) != world->space.projectiles.end() &&
                             projectile->selfId == shot.selfId;
                if (alive && !projectile->startedDeath)
                {
                    projectile->death_animation.Start(true);
                    projectile->startedDeath = true;
                    projectile->missed = true;
                }
                Log("Match: shot %u ran into something on the attacker's side (%.0f, %.0f)", netId, x, y);
                return;
            }
        }

        int ProjectileUpdates(Projectile *projectile)
        {
            InShot *shot = FindIn(projectile);
            if (!shot) return 1;
            double now = WallMs();
            if (projectile->currentSpace == 1) return shot->speedUp;

            if (shot->transferMs < 0.0) shot->transferMs = now;
            if (!shot->released)
            {
                if (now < shot->releaseAt) return 0;   // waits at its entry point, just outside our view
                shot->released = true;
                shot->releasedMs = now;
            }
            if (shot->catchUpMs > 0.0)
            {
                // Three updates a frame gain two frames of flight each frame (SpeedFactor is 16 per second).
                double frameMs = std::max(1.0, (double)G_->GetCFPS()->GetSpeedFactor() * 62.5);
                shot->catchUpMs -= 2.0 * frameMs;
                return 3;
            }
            return 1;
        }

        // Inside the shields' ellipse as FTL's Shields::CollisionTest measures it (its half axes a and a times the
        // ellipse's ratio), whatever the layers (DI): the defender's state can take the copy's last layer away before
        // our shot gets there, and FTL's own test then lets the shot through to a room, though the defender's shields
        // stopped it.
        static bool InShieldEllipse(const Shields *shields, float x, float y)
        {
            const float a = shields->baseShield.a, b = shields->baseShield.a * shields->ellipseRatio;
            if (a <= 0.f || b <= 0.f) return false;
            const float dx = x - (float)shields->baseShield.center.x, dy = y - (float)shields->baseShield.center.y;
            return dx * dx / (a * a) + dy * dy / (b * b) < 1.f;
        }

        static bool CrossesShieldEllipse(const Shields *shields, Pointf start, Pointf finish)
        {
            return shields && !InShieldEllipse(shields, start.x, start.y) && InShieldEllipse(shields, finish.x, finish.y);
        }

        // Would this frame's movement decide the shot (cross the shields' ellipse, or reach the target point)?
        static bool WouldDecide(Projectile *projectile, ShipManager *replica)
        {
            if (projectile->AtTarget()) return true;
            return CrossesShieldEllipse(replica->shieldSystem, projectile->last_position, projectile->position);
        }

        // A shot of ours that the defender shot down (or that ran into a drone there) explodes on our screen too.
        static void ExplodeOutShot(OutShot &shot)
        {
            Projectile *projectile = shot.projectile;
            if (!projectile || shot.exploded) return;
            shot.exploded = true;
            Explode(projectile);
            Drones::EndVisualShotsNear(projectile->position.x, projectile->position.y, projectile->currentSpace);
        }

        static bool IsShipOrDrone(Collideable *other)
        {
            for (int shipId = 0; shipId < 2; ++shipId)
            {
                ShipManager *ship = G_->GetShipManager(shipId);
                if (ship && other == &ship->_collideable) return true;
            }
            WorldManager *world = G_->GetWorld();
            if (!world) return false;
            for (SpaceDrone *drone : world->space.drones)
            {
                if (other == &drone->_collideable) return true;
            }
            return false;
        }

        // A replay (roadmap 5.1): a shot or a hazard reaching our ship, the recorder's. Their shot plays out the recorder's
        // verdict, and waits for it just outside the decision as ours wait at their ship; ours never hit our own ship;
        // anything else (a rock, the battery's shot) shows its explosion and does nothing: the recorder's states have
        // what it did.
        static bool ReplayHitsOwnShip(Projectile *projectile, ShipManager *own)
        {
            if (FindOut(projectile)) return false;
            InShot *shot = FindIn(projectile);
            if (!shot)
            {
                g_forced.projectile = projectile;
                g_forced.shipId = 0;
                g_forced.verdict = OUTCOME_HIT;
                g_forced.cosmetic = true;
                return true;
            }
            if (shot->recorded == OUTCOME_DOWNED) return false;   // it explodes where that happened (TrackShots)
            if (shot->recorded == PENDING)
            {
                if (!WouldDecide(projectile, own)) return true;
                if (shot->holdStartMs < 0.0) shot->holdStartMs = WallMs();
                projectile->position = projectile->last_position;
                return false;
            }
            g_forced.projectile = projectile;
            g_forced.shipId = 0;
            g_forced.verdict = shot->recorded;
            g_forced.damage = shot->recordedDamage;
            return true;
        }

        bool BeginCollisionCheck(Projectile *projectile, Collideable *other)
        {
            g_forced = Forced();
            ShipManager *replica = G_->GetShipManager(1);
            if (!replica) return true;
            if (g_match.replicaReady && GetState().aiOff[1])
            {
                // Whose space decides (DuelsDrones.h). Copies of the opponent's drone shots never collide. In their
                // space only our shots at their ship count, by the defender's verdict. In ours, a shot hits another
                // shot only when one of them is our drones' (a defense drone's); everything else there is as in FTL.
                if (Drones::IsVisualShot(projectile)) return false;
                if (projectile->currentSpace == 1)
                {
                    if (other != &replica->_collideable) return false;
                }
                else if (!IsShipOrDrone(other))
                {
                    // Collideable is a Projectile's first base: anything but a ship or a drone is a projectile.
                    Projectile *hit = static_cast<Projectile*>(other);
                    return Drones::IsOwnDroneShot(projectile) || Drones::IsOwnDroneShot(hit);
                }
            }
            // A replay (roadmap 5.1): every shot's end is in the recording.
            if (Net::Replaying() && g_match.replicaReady)
            {
                ShipManager *own = G_->GetShipManager(0);
                if (own && other == &own->_collideable && IsDriven(0)) return ReplayHitsOwnShip(projectile, own);
                // Ours (the recorder's) meet nothing in our space: what they ran into there came as a message. Theirs
                // meet nothing but our ship.
                if (projectile->currentSpace == 0 && (FindOut(projectile) || FindIn(projectile))) return false;
            }
            if (other != &replica->_collideable) return true;
            OutShot *shot = FindOut(projectile);
            if (!shot) return true;
            if (shot->verdict == OUTCOME_DOWNED)
            {
                ExplodeOutShot(*shot);
                return false;
            }

            if (shot->verdict == PENDING)
            {
                if (!WouldDecide(projectile, replica)) return true;
                // Wait just outside the decision point until the defender's verdict arrives.
                if (shot->holdStartMs < 0.0) shot->holdStartMs = WallMs();
                projectile->position = projectile->last_position;
                return false;
            }
            g_forced.projectile = projectile;
            g_forced.shipId = 1;
            g_forced.verdict = shot->verdict;
            g_forced.damage = shot->damage;
            return true;
        }

        void EndCollisionCheck()
        {
            g_forced = Forced();
        }

        // A bomb appears in its target room and goes off after a short delay; the dodge is rolled when it appears
        // (a dodged bomb is set aside with a "MISS"). Our bomb in the replica never rolls: it waits at the moment it
        // would go off until the defender's verdict is here, then goes off or misses as the defender's did.
        bool BeginBombCheck(BombProjectile *bomb, Collideable *other)
        {
            g_forced = Forced();
            // A replay (roadmap 5.1): their bomb in our ship goes off, or misses, as it did in the recorder's game.
            ShipManager *own = G_->GetShipManager(0);
            if (Net::Replaying() && own && other == &own->_collideable && bomb->currentSpace == 0 && IsDriven(0))
            {
                InShot *in = FindIn(bomb);
                if (!in || bomb->explosiveDelay > 0.f || bomb->startedDeath || bomb->bMissed) return true;
                if (in->recorded == PENDING)
                {
                    if (in->holdStartMs < 0.0) in->holdStartMs = WallMs();
                    return false;
                }
                if (in->recorded == OUTCOME_MISS || in->recorded == OUTCOME_GONE)
                {
                    bomb->bMissed = true;
                    own->damMessages.push_back(new DamageMessage(1.f, bomb->position, DamageMessage::MISS));
                }
                g_forced.projectile = bomb;
                g_forced.shipId = 0;
                g_forced.verdict = in->recorded;
                g_forced.damage = in->recordedDamage;
                return true;
            }
            ShipManager *replica = G_->GetShipManager(1);
            if (!replica || other != &replica->_collideable || bomb->currentSpace != 1) return true;
            OutShot *shot = FindOut(bomb);
            if (!shot) return true;
            if (bomb->explosiveDelay > 0.f || bomb->startedDeath || bomb->bMissed) return true;

            if (shot->verdict == PENDING)
            {
                if (shot->holdStartMs < 0.0) shot->holdStartMs = WallMs();
                return false;
            }
            if (shot->verdict == OUTCOME_MISS || shot->verdict == OUTCOME_GONE)
            {
                bomb->bMissed = true;
                replica->damMessages.push_back(new DamageMessage(1.f, bomb->position, DamageMessage::MISS));
            }
            g_forced.projectile = bomb;
            g_forced.shipId = 1;
            g_forced.verdict = shot->verdict;
            g_forced.damage = shot->damage;
            return true;
        }

        bool ForcedDodge(ShipManager *ship, bool &dodged)
        {
            if (ship && ship->iShipId == 0 && Net::Replaying())
            {
                // A replay: their bomb in our ship is decided when it goes off, as the recorder's game had it.
                InShot *in = FindIn(CustomDamageManager::currentProjectile);
                if (!in || in->type != WEAPON_BOMB) return false;
                dodged = false;
                return true;
            }
            if (!ship || ship->iShipId != 1) return false;
            OutShot *shot = FindOut(CustomDamageManager::currentProjectile);
            if (!shot || shot->type != WEAPON_BOMB) return false;
            dodged = false;   // decided when it goes off (BeginBombCheck)
            return true;
        }

        bool SensorsAllow(const ShipManager *ship, int vision)
        {
            if (!ship || ship->iShipId != 0 || !Net::IsConnected() || !g_match.replicaReady || ship != G_->GetShipManager(0)) return true;
            switch (vision)
            {
                case 2: return (g_match.peerVision & Vision::SEES_INTERIOR) != 0;
                case 3: return (g_match.peerVision & Vision::SEES_POWER) != 0;
                case 4: return (g_match.peerVision & Vision::SEES_CHARGE) != 0;
                default: return true;
            }
        }

        void ClampVision(ShipManager *ship)
        {
            // A replay with full sensors (roadmap BA): every room of both ships lit (FTL draws the crew in lit rooms).
            if (ship && Demo::ReplayFullSensors() && (ship == G_->GetShipManager(0) || ship == G_->GetShipManager(1)))
            {
                for (Room *room : ship->ship.vRoomList)
                {
                    if (room) room->bBlackedOut = false;
                }
                return;
            }
            if (!ship || ship->iShipId != 1 || !Net::IsConnected() || !g_match.replicaReady || ship != G_->GetShipManager(1)) return;
            if (g_match.peerVision & Vision::SEES_INTERIOR) return;
            for (Room *room : ship->ship.vRoomList)
            {
                int id = room ? room->iRoomId : -1;
                if (id < 0 || (id < (int)ship->tempVision.size() && ship->tempVision[id])) continue;
                room->bBlackedOut = true;
            }
        }

        bool FullSensors(const ShipManager *ship)
        {
            return ship && ship->iShipId == 0 && Demo::ReplayFullSensors() && ship == G_->GetShipManager(0);
        }

        bool RolledDodge(ShipManager *ship, bool &dodged)
        {
            if (!ship || ship->iShipId != 0) return false;
            InShot *shot = FindIn(CustomDamageManager::currentProjectile);
            if (!shot) return false;
            if (Net::Replaying())
            {
                dodged = shot->recorded == OUTCOME_MISS;   // the recorder's game rolled it
                return true;
            }
            if (shot->rolled)
            {
                dodged = shot->dodged;   // asked again for the same shot
                return true;
            }
            int evasion = ship->GetDodgeFactor();
            Fair::Value verdict;
            bool rolledDodge = false;
            if (!Fair::Roll(shot->fair, evasion, verdict, rolledDodge)) return false;
            shot->rolled = true;
            shot->dodged = rolledDodge;
            shot->evasion = evasion;
            shot->fairVerdict = verdict;
            dodged = rolledDodge;
            return true;
        }

        void ObserveDodge(ShipManager *ship, bool dodged)
        {
            if (!ship || ship->iShipId != 0 || !dodged || Net::Replaying()) return;
            InShot *shot = FindIn(CustomDamageManager::currentProjectile);
            if (shot && shot->type == WEAPON_BOMB && shot->outcome == PENDING) SendResult(*shot, OUTCOME_MISS, 0);
        }

        // Our beam sweeping the replica: the defender's game does the damage (and the state sync shows it), ours only
        // draws it. It still runs with no damage, so the beam looks and sounds as it does.
        void MuteBeamDamage(ShipManager *ship, Damage &damage)
        {
            if (!ship) return;
            int type = -1;
            if (ship->iShipId == 1)
            {
                OutShot *shot = FindOut(CustomDamageManager::currentProjectile);
                if (shot) type = shot->type;
            }
            else if (ship->iShipId == 0 && Net::Replaying())
            {
                // A replay (roadmap 5.1): their beam sweeps our ship as the recorder's; its states have what it did.
                InShot *shot = FindIn(CustomDamageManager::currentProjectile);
                if (shot) type = shot->type;
            }
            if (type != WEAPON_BEAM) return;
            damage.iDamage = 0;
            damage.iShieldPiercing = 0;
            damage.fireChance = 0;
            damage.breachChance = 0;
            damage.stunChance = 0;
            damage.iIonDamage = 0;
            damage.iSystemDamage = 0;
            damage.iPersDamage = 0;
            damage.bHullBuster = false;
            damage.bLockdown = false;
            damage.crystalShard = false;
            damage.iStun = 0;
        }

        // The opponent's beam sweeping our ship: its verdict (for the trace) is sent when it is over.
        void ObserveBeam(ShipManager *ship, bool hit, int hullBefore)
        {
            if (!ship || ship->iShipId != 0 || Net::Replaying()) return;
            InShot *shot = FindIn(CustomDamageManager::currentProjectile);
            if (!shot || shot->type != WEAPON_BEAM) return;
            shot->beamHit = shot->beamHit || hit;
            shot->damage += std::max(0, hullBefore - ship->ship.hullIntegrity.first);
        }

        static bool ForcedApplies(ShipManager *ship)
        {
            return g_forced.projectile && ship && ship->iShipId == g_forced.shipId && CustomDamageManager::currentProjectile == g_forced.projectile;
        }

        bool ForcedShieldResponse(ShipManager *ship, Pointf start, Pointf finish, const Damage &damage,
                                  CollisionResponse &response)
        {
            // A hazard at our ship in a replay meets the shields as FTL has it (the next state has their layers).
            if (!ForcedApplies(ship) || g_forced.cosmetic) return false;
            response.collision_type = 0;
            response.point = Pointf(-2147483648.f, -2147483648.f);
            response.damage = 0;
            response.superDamage = 0;

            Shields *shields = ship->shieldSystem;
            if (!shields) return true;
            if (!CrossesShieldEllipse(shields, start, finish)) return true;

            switch (g_forced.verdict)
            {
            case OUTCOME_MISS:
            case OUTCOME_GONE:
                ship->damMessages.push_back(new DamageMessage(1.f, finish, DamageMessage::MISS));
                response.collision_type = 3;
                break;
            case OUTCOME_SHIELD:
                // Let the replica's shields show the hit; the state update brings the exact layers anyway. (With its last
                // layer already gone there, the shot still ends at the shields' edge, DI.)
                if (shields->shields.power.first <= 0 && shields->shields.power.super.first <= 0)
                {
                    static int logged = 0;
                    if (logged++ < 5) Log("Match: our shot ends at their shields' edge (their state took the layer away before it got there)");
                }
                response = shields->CollisionReal(finish.x, finish.y, damage, false);
                if (response.collision_type != 2)
                {
                    response.collision_type = 2;
                    response.point = finish;
                    response.damage = damage.iDamage;
                }
                break;
            default:
                break;   // a hit goes through the shields; the hull decides below
            }
            return true;
        }

        bool ForcedDamageArea(ShipManager *ship, Pointf location, bool &hit)
        {
            if (!ForcedApplies(ship)) return false;
            if (g_forced.cosmetic)
            {
                hit = true;   // a hazard at our ship in a replay: its explosion only
                return true;
            }
            switch (g_forced.verdict)
            {
            case OUTCOME_HIT:
                if (g_forced.damage > 0) ship->damMessages.push_back(new DamageMessage(1.f, g_forced.damage, location, false));
                hit = true;
                break;
            case OUTCOME_SHIELD:
                hit = true;   // their shields took it; ours were a moment out of date
                // (It ends at the shields' edge since DI; a shot that gets here still is logged.)
                Log("Match: a shot their shields stopped reached the hull of our copy of their ship (shown there)");
                break;
            default:
                if (!ship->bJumping) ship->damMessages.push_back(new DamageMessage(1.f, location, DamageMessage::MISS));
                hit = false;
                break;
            }
            return true;
        }

        void ObserveShield(ShipManager *ship, const CollisionResponse &response)
        {
            if (!ship || ship->iShipId != 0 || Net::Replaying()) return;
            InShot *shot = FindIn(CustomDamageManager::currentProjectile);
            if (!shot || shot->outcome != PENDING) return;
            if (shot->type == WEAPON_BEAM)
            {
                // A beam touches the shields every frame of its sweep; its verdict is sent when it is over.
                if (response.collision_type == 2) shot->beamTouched = true;
                return;
            }
            if (response.collision_type == 3) SendResult(*shot, OUTCOME_MISS, 0);
            // FTL answers "shield" for a shot that pierces the shields too (a missile goes through them, roadmap P: the
            // early verdict made our defense drone's later hit too late). Decided in TrackShots: blocked if it stops.
            else if (response.collision_type == 2) shot->shieldTouched = true;
        }

        void ObserveDamageArea(ShipManager *ship, bool hit, int hullBefore)
        {
            if (!ship || ship->iShipId != 0 || Net::Replaying()) return;
            InShot *shot = FindIn(CustomDamageManager::currentProjectile);
            if (!shot || shot->outcome != PENDING) return;
            SendResult(*shot, hit ? OUTCOME_HIT : OUTCOME_MISS, hit ? std::max(0, hullBefore - ship->ship.hullIntegrity.first) : 0);
        }

        // Shots whose projectile is gone are logged and forgotten; transfers feed the timing estimates.
        // The duel ends (leave, disconnect, quit) with shots still flying: they go into duels_shots.csv as they stand.
        static void FlushShotLog()
        {
            MatchState &m = g_match;
            for (const OutShot &shot : m.out)
            {
                LogShot("out", shot.netId, shot.weapon, shot.spawnMs, -1.0, shot.transferMs, -1.0, shot.holdStartMs, -1.0,
                        shot.verdictMs, shot.verdict, shot.damage, 1);
            }
            for (const InShot &shot : m.in)
            {
                LogShot("in", shot.netId, shot.weapon, shot.spawnMs, shot.receivedMs, shot.transferMs, shot.releasedMs, -1.0,
                        shot.decisionMs, -1.0, shot.outcome, shot.damage, shot.speedUp);
            }
            m.out.clear();
            m.in.clear();
        }

        // A replay (roadmap 5.1): their shot at our ship ends as it did in the recorder's game. Shot down there (by one of
        // the recorder's defense drones, or into a drone): it explodes about where that happened, as ours do.
        static void TrackReplayedInShot(InShot &shot, double now)
        {
            Projectile *projectile = shot.projectile;
            if (shot.recorded == OUTCOME_DOWNED && !shot.exploded)
            {
                float dx = projectile->position.x - shot.recordedPoint.x, dy = projectile->position.y - shot.recordedPoint.y;
                float distance = std::sqrt(dx * dx + dy * dy);
                bool passed = distance > shot.downDistance + 0.5f;
                shot.downDistance = std::min(shot.downDistance, distance);
                if (projectile->currentSpace == 0 &&
                    (distance < 15.f || passed || shot.holdStartMs >= 0.0 || now - shot.recordedMs > 500.0))
                {
                    shot.exploded = true;
                    Explode(projectile);
                }
            }
            if (shot.recorded == PENDING && shot.holdStartMs >= 0.0 && now - shot.holdStartMs > HOLD_TIMEOUT_MS)
            {
                shot.recorded = OUTCOME_MISS;
                shot.recordedMs = now;
                ++g_match.holdTimeouts;
                Log("Match: replay: no verdict for their shot %u after %.0f ms; shown as a miss", shot.netId, HOLD_TIMEOUT_MS);
            }
        }

        static void ShiftShotWaits(double ms)
        {
            if (ms <= 0.0) return;
            for (OutShot &shot : g_match.out)
            {
                if (shot.holdStartMs >= 0.0) shot.holdStartMs += ms;
                if (shot.goneMs >= 0.0) shot.goneMs += ms;
            }
            for (InShot &shot : g_match.in)
            {
                if (shot.holdStartMs >= 0.0) shot.holdStartMs += ms;
                if (shot.goneMs >= 0.0) shot.goneMs += ms;
                if (shot.releasedMs < 0.0) shot.releaseAt += ms;
            }
        }

        static void TrackShots(double now)
        {
            MatchState &m = g_match;
            WorldManager *world = G_->GetWorld();
            std::set<Projectile*> live;
            if (world)
            {
                for (Projectile *projectile : world->space.projectiles) live.insert(projectile);
            }
            // Crystal shards wait in our ship's barrage until FTL puts them into space.
            if (ShipManager *own = G_->GetShipManager(0))
            {
                for (Projectile *projectile : own->superBarrage) live.insert(projectile);
            }
            SendCrossedShards(live);

            for (size_t i = 0; i < m.out.size();)
            {
                OutShot &shot = m.out[i];
                bool alive = live.count(shot.projectile) && shot.projectile->selfId == shot.selfId;
                if (alive)
                {
                    Projectile *projectile = shot.projectile;
                    if (shot.transferMs < 0.0 && projectile->currentSpace == 1)
                    {
                        shot.transferMs = now;
                        int timing = TimingSlot(shot.source, shot.slot);
                        if (timing >= 0)
                        {
                            double leg = now - shot.spawnMs;
                            double &estimate = m.legEstimate[timing];
                            estimate = estimate > 0.0 ? estimate * 0.7 + leg * 0.3 : leg;
                        }
                    }
                    // It ran into something in our own space before it left (one of their drones): that is ours to
                    // decide, and the defender's copy goes too.
                    if (shot.verdict == PENDING && projectile->startedDeath && projectile->currentSpace == 0)
                    {
                        shot.verdict = OUTCOME_DOWNED;
                        shot.verdictMs = now;
                        shot.exploded = true;
                        Writer w;
                        w.U32(shot.netId);
                        w.F32(projectile->position.x);
                        w.F32(projectile->position.y);
                        Net::Send(MSG_SHOT_DOWNED, w, true);
                    }
                    // Shot down on the defender's screen: it explodes where that happened. It is about there now, as
                    // the defender's copy flies ahead of ours by the latency; at the latest where it stops coming closer.
                    if (shot.verdict == OUTCOME_DOWNED && !shot.exploded)
                    {
                        float dx = projectile->position.x - shot.downPoint.x, dy = projectile->position.y - shot.downPoint.y;
                        float distance = std::sqrt(dx * dx + dy * dy);
                        bool passed = distance > shot.downDistance + 0.5f;
                        shot.downDistance = std::min(shot.downDistance, distance);
                        if (projectile->currentSpace == 1 &&
                            (distance < 15.f || passed || shot.holdStartMs >= 0.0 || now - shot.verdictMs > 500.0))
                        {
                            ExplodeOutShot(shot);
                        }
                    }
                    if (shot.verdict == PENDING && shot.holdStartMs >= 0.0 && now - shot.holdStartMs > HOLD_TIMEOUT_MS)
                    {
                        shot.verdict = OUTCOME_MISS;
                        shot.timedOut = true;
                        shot.verdictMs = now;
                        ++m.holdTimeouts;
                        Log("Match: no verdict for shot %u after %.0f ms; shown as a miss", shot.netId, HOLD_TIMEOUT_MS);
                    }
                    ++i;
                    continue;
                }
                // A beam's verdict comes when the defender's sweep is over, usually after ours: wait a little for it.
                if (shot.type == WEAPON_BEAM && shot.verdict == PENDING)
                {
                    shot.projectile = nullptr;
                    if (shot.goneMs < 0.0) shot.goneMs = now;
                    if (now - shot.goneMs < BEAM_VERDICT_WAIT_MS)
                    {
                        ++i;
                        continue;
                    }
                }
                LogShot("out", shot.netId, shot.weapon, shot.spawnMs, -1.0, shot.transferMs, -1.0, shot.holdStartMs, -1.0,
                        shot.verdictMs, shot.verdict, shot.damage, 1);
                m.out.erase(m.out.begin() + i);
            }

            for (size_t i = 0; i < m.in.size();)
            {
                InShot &shot = m.in[i];
                bool alive = live.count(shot.projectile) && shot.projectile->selfId == shot.selfId;
                if (alive && Net::Replaying())
                {
                    TrackReplayedInShot(shot, now);
                    ++i;
                    continue;
                }
                if (alive)
                {
                    Projectile *projectile = shot.projectile;
                    // It met our shields: stopped there, the shields took it; going on, it pierced them (a missile).
                    if (shot.outcome == PENDING && shot.shieldTouched)
                    {
                        if (projectile->startedDeath) SendResult(shot, OUTCOME_SHIELD, 0);
                        else shot.shieldTouched = false;
                    }
                    // It exploded in our space before our shields: one of our defense drones shot it down, or it ran
                    // into a drone. (Hits on our ship decide it first; beams and bombs can't be shot down.)
                    if (shot.outcome == PENDING && projectile->startedDeath && projectile->currentSpace == 0 &&
                        shot.type != WEAPON_BEAM && shot.type != WEAPON_BOMB)
                    {
                        SendResult(shot, OUTCOME_DOWNED, 0, projectile->position);
                    }
                    ++i;
                    continue;
                }
                if (Net::Replaying())
                {
                    // Gone before the recorder's verdict came (a beam: its game sends it when its sweep is over): it
                    // waits a little for it, as ours wait for the defender's.
                    if (shot.recorded == PENDING)
                    {
                        shot.projectile = nullptr;
                        if (shot.goneMs < 0.0) shot.goneMs = now;
                        if (now - shot.goneMs < BEAM_VERDICT_WAIT_MS)
                        {
                            ++i;
                            continue;
                        }
                    }
                    shot.outcome = shot.recorded;
                    shot.damage = shot.recordedDamage;
                    shot.decisionMs = shot.recordedMs;
                }
                else if (shot.outcome == PENDING && shot.type == WEAPON_BEAM)
                {
                    // (DamageBeam returns false even when it did damage.)
                    bool hit = shot.beamHit || shot.damage > 0;
                    SendResult(shot, hit ? OUTCOME_HIT : shot.beamTouched ? OUTCOME_SHIELD : OUTCOME_MISS, shot.damage);
                }
                else if (shot.outcome == PENDING)
                {
                    SendResult(shot, shot.shieldTouched ? OUTCOME_SHIELD : OUTCOME_GONE, 0);
                }
                LogShot("in", shot.netId, shot.weapon, shot.spawnMs, shot.receivedMs, shot.transferMs, shot.releasedMs, -1.0,
                        shot.decisionMs, -1.0, shot.outcome, shot.damage, shot.speedUp);
                m.in.erase(m.in.begin() + i);
            }
        }

        // Everything the state sync carries, as one comparable line: hull, shields, systems (id:power/health, and
        // Ln while locked), weapons, drones.
        static std::string Signature(ShipManager *ship)
        {
            std::ostringstream out;
            out << ship->ship.hullIntegrity.first << ',' << (ship->shieldSystem ? ship->shieldSystem->shields.power.first : -1);
            if (ship->shieldSystem && ship->shieldSystem->shields.power.super.first > 0) out << '+' << ship->shieldSystem->shields.power.super.first;
            out << ',';
            for (ShipSystem *system : ship->vSystemList)
            {
                out << system->iSystemType << ':' << PowerBars(system) << '/' << system->healthState.first;
                if (system->iLockCount != 0) out << 'L' << system->iLockCount;   // ion lock; -1 = battery running
                if (system->bUnderAttack) out << 'H' << system->iHackEffect;       // hacked: 1 drone attached, 2 pulse
                out << ' ';
            }
            out << ',';
            if (ship->weaponSystem)
            {
                for (ProjectileFactory *weapon : ship->GetWeaponList()) out << (weapon->powered ? '1' : '0');
            }
            out << ',' << Drones::Signature(ship);
            out << ',' << Crew::Signature(ship) << ',' << Crew::RoomSignature(ship) << ',' << Rooms::Signature(ship)
                << ',' << Crew::AnimationSignature(ship) << ',' << Bays::Signature(ship);
            out << ',' << (!ship->cloakSystem ? "-" : ship->cloakSystem->bTurnedOn ? "on" : "off");
            out << ',' << Hacking::Signature(ship) << ',' << Mind::Signature(ship);
            return out.str();
        }

        // duels_sync.csv (with "trace on"): every change of our ship and of the replica, to check the replica
        // follows its owner within an update interval (tools/analyze-duel.py).
        static void TraceSync(double now)
        {
            MatchState &m = g_match;
            if (!GetState().trace || !m.replicaReady) return;
            for (int shipId = 0; shipId < 2; ++shipId)
            {
                ShipManager *ship = G_->GetShipManager(shipId);
                if (!ship) continue;
                // Our ship's row says what the opponent sees of it (roadmap 4.5): the analysis compares only that.
                std::string signature = Signature(ship) + "," + (shipId == 0 ? Vision::Signature() : std::string("-"));
                if (signature == m.lastSignature[shipId]) continue;
                m.lastSignature[shipId] = signature;
                if (!m.syncCsv.IsOpen()) m.syncCsv.Open("duels_sync.csv", "wall_ms,clock_offset_ms,ship,hull,shields,systems,weapons,drones,crew,crew_rooms,rooms,crew_anim,bays,cloak,hack,mind,vision");
                Row row;
                row << now << Net::LocalToPeerTime(0.0) << (shipId == 0 ? "own" : "replica") << signature;
                m.syncCsv.WriteRow(row.str());
            }
        }

        // --------------------------------------------------------------------------------------------------------
        // Session events
        // --------------------------------------------------------------------------------------------------------

        // ---------------------------------------------------------------------------------------------------------
        // Crew experience
        // ---------------------------------------------------------------------------------------------------------

        // Main skills mastered in about two of a match's five fights (docs/design/weapon-bays.md): FTL needs 26
        // dodges for piloting, 100 absorbed hits for shields, 116 volleys for weapons (humans).
        static const float XP_DEFAULT = 3.f, XP_MIN = 1.f, XP_MAX = 10.f;
        static float g_xpSetting = XP_DEFAULT;   // ours: counts when we host
        static float g_xpMatch = XP_DEFAULT;     // this duel's: the host's
        static float g_xpCarry = 0.f;            // what is left of a fraction
        static uint32_t g_xpGains = 0, g_xpCounted = 0;

        static std::string XpText(float factor)
        {
            char text[32];
            std::snprintf(text, sizeof(text), "x%g", factor);
            return text;
        }

        // The host's crew experience setting as last set, from duels.cfg (roadmap U): read once, before the first use;
        // a test scenario starts from the default.
        static void LoadXpSetting()
        {
            static bool loaded = false;
            if (loaded) return;
            loaded = true;
            if (!SettingsFromConfig()) return;
            float xp = (float)std::atof(Config::Value("xp").c_str());
            if (xp >= XP_MIN && xp <= XP_MAX) g_xpSetting = xp;
        }

        // The host's crew experience and its fine settings that differ from the defaults (roadmap BE): the match plays by
        // them in both games.
        static void SendSettings()
        {
            Writer w;
            w.F32(g_xpMatch);
            Tune::WriteMatch(w);
            Net::Send(MSG_SETTINGS, w, true);
        }

        static void ApplySettings(Reader &r)
        {
            float xp = r.F32();
            if (!r.Ok() || !(xp >= XP_MIN && xp <= XP_MAX)) return;
            if (xp != g_xpMatch) Announce("crew experience " + XpText(xp) + " (the host's setting)");
            g_xpMatch = xp;
            std::string note;
            if (!Tune::ReadMatch(r, note)) Log("Match: the host's fine settings don't read");
            if (!note.empty()) Log("Match: of the host's fine settings: %s", note.c_str());
        }

        float CrewXpSetting()
        {
            LoadXpSetting();
            return g_xpSetting;
        }

        float MatchXp()
        {
            return g_xpMatch;
        }

        bool SetCrewXp(float factor, std::string &message)
        {
            if (!(factor >= XP_MIN && factor <= XP_MAX))
            {
                message = "usage: xp <factor from 1 to 10> (1 is FTL's own pace)";
                return false;
            }
            if (Net::IsConnected() && !Net::IsHost())
            {
                message = "the host decides the crew experience: " + XpText(g_xpMatch);
                return false;
            }
            LoadXpSetting();
            g_xpSetting = factor;
            g_xpMatch = factor;
            if (SettingsFromConfig()) Config::SaveValue("xp", XpText(factor).substr(1));   // kept for the next start (roadmap U)
            if (Net::IsConnected()) SendSettings();
            message = "crew experience " + XpText(factor) + (Net::IsConnected() ? " for this duel" : " when you host");
            return true;
        }

        std::string CrewXpStatus()
        {
            LoadXpSetting();
            std::string text = "crew experience " + XpText(Net::IsConnected() ? g_xpMatch : g_xpSetting);
            if (Net::IsConnected() && !Net::IsHost()) text += " (the host's setting)";
            return text;
        }

        int SkillGains(const CrewMember *crew)
        {
            if (!crew || crew->iShipId != 0 || !Net::IsConnected()) return 1;
            float gain = g_xpMatch + g_xpCarry;
            int whole = std::max(0, (int)gain);
            g_xpCarry = gain - (float)whole;
            ++g_xpGains;
            g_xpCounted += (uint32_t)whole;
            return whole;
        }

        // ---------------------------------------------------------------------------------------------------------
        // Chat (rules, section 8): no flooding. A line is cleaned of control characters and cut to a length; a player
        // may send a few lines per 10 s. The receiving game applies the same limits itself (it never trusts the
        // sender): a flooding opponent's chat is muted for a while.
        // ---------------------------------------------------------------------------------------------------------

        static const size_t CHAT_MAX_CHARS = 120;
        static const size_t CHAT_LINES = 5;
        static const double CHAT_WINDOW_MS = 10000.0;
        static const double CHAT_MUTE_MS = 30000.0;

        struct ChatLimits
        {
            std::deque<double> sent, received;
            double mutedUntil = 0.0;
            uint32_t dropped = 0;
        };

        static ChatLimits g_chat;

        // Printable text only (UTF-8 kept), trimmed, at most CHAT_MAX_CHARS bytes (not cutting a character).
        static std::string CleanChat(const std::string &text)
        {
            std::string clean;
            for (unsigned char c : text)
            {
                if (c < 0x20 || c == 0x7f) c = ' ';
                clean += (char)c;
            }
            size_t start = clean.find_first_not_of(' ');
            if (start == std::string::npos) return "";
            clean = clean.substr(start, clean.find_last_not_of(' ') - start + 1);
            if (clean.size() > CHAT_MAX_CHARS)
            {
                size_t cut = CHAT_MAX_CHARS;
                while (cut > 0 && ((unsigned char)clean[cut] & 0xC0) == 0x80) --cut;   // not inside a character
                clean = clean.substr(0, cut);
            }
            return clean;
        }

        // Whether one more line fits into the last CHAT_WINDOW_MS (and counts it if it does).
        static bool ChatAllowed(std::deque<double> &times, double now)
        {
            while (!times.empty() && now - times.front() > CHAT_WINDOW_MS) times.pop_front();
            if (times.size() >= CHAT_LINES) return false;
            times.push_back(now);
            return true;
        }

        static void ReceiveChat(const std::string &raw)
        {
            double now = WallMs();
            std::string text = CleanChat(raw);
            if (text.empty()) return;
            if (now < g_chat.mutedUntil)
            {
                ++g_chat.dropped;
                return;
            }
            if (!ChatAllowed(g_chat.received, now))
            {
                g_chat.mutedUntil = now + CHAT_MUTE_MS;
                ++g_chat.dropped;
                Console::Feed(Net::PeerName() + "'s chat is muted for " + std::to_string((int)(CHAT_MUTE_MS / 1000.0)) +
                              " s (too many lines)");
                Log("Match: the opponent's chat is muted (more than %u lines in %.0f s)", (unsigned)CHAT_LINES, CHAT_WINDOW_MS / 1000.0);
                return;
            }
            Log("Match: chat from %s: %s", Net::PeerName().c_str(), text.c_str());
            Console::Chat(Net::PeerName(), text);
        }

        void ReplayChat(const std::string &name, const uint8_t *data, size_t size)
        {
            Reader r(data, size);
            std::string text = CleanChat(r.Str());
            if (!r.Ok() || text.empty()) return;
            Log("Match: replay: chat from %s: %s", name.c_str(), text.c_str());
            Console::Chat(name, text);
        }

        void ReplayRestate()
        {
            g_match.havePeerState = false;
        }

        // Back after this game crashed (roadmap BR, DuelsRejoin.cpp): our ship and crew are as the file has them
        // (restored before the way back was tried); the match goes on from the file (the host's game) or from the host's
        // state (a guest's). No demo here: the match's start isn't in this game any more (the other game's has it all).
        static void ColdConnected()
        {
            g_match.loadoutSent = false;
            g_match.peerReady = false;
            g_match.stateDirty = true;
            Fair::OnConnected(false);
            GetState().noPause = true;
            g_xpCarry = 0.f;
            if (Net::IsHost())
            {
                // The match's crew experience and fine settings as this game had them (the guest keeps the ones it got).
                g_xpMatch = Rejoin::MatchXp();
                Tune::UseMatch(Rejoin::MatchFine());
            }
            bool ours = GetState().debug, theirs = Net::PeerDebug();
            if (ours || theirs)
            {
                Headline(std::string("DEBUG DUEL: test commands are on (") + (ours ? "yours on" : "yours off") + ", " + Net::PeerName() + "'s " +
                         (theirs ? "on" : "off") + "); the match is unranked");
                if (theirs) EnableDebug(((Net::PeerName().empty() ? std::string("the other player") : Net::PeerName()) + "'s game has debug mode on").c_str());
            }
            Headline("Back in the match against " + Net::PeerName());
            Rounds::ColdConnected(Rejoin::RoundsPart(), Rejoin::RefitPart());
            Rejoin::OnColdConnected();
        }

        class Listener : public Net::Listener
        {
        public:
            void OnConnected() override
            {
                if (Net::Cold())
                {
                    ColdConnected();
                    return;
                }
                if (Net::Resumed())
                {
                    // Back after a lost connection, in the same match: both ships stay; the loadouts, the crew
                    // rosters and the match state go again, and the fight goes on.
                    g_match.loadoutSent = false;
                    g_match.peerReady = false;
                    g_match.stateDirty = true;
                    Crew::SendRosterAgain();
                    Fair::OnConnected(true);
                    // Their game started again after a crash (roadmap BR): it knows the match only from its file. A guest's
                    // gets the host's settings again; its states count from the start again.
                    if (Net::PeerCold())
                    {
                        g_match.havePeerState = false;
                        Demo::NotePeerCold();   // a replay of this match: the same
                        if (Net::IsHost()) SendSettings();
                    }
                    Headline(Net::PeerName() + (Net::PeerCold() ? " is back (their game started again): the match goes on" : " is back: the match goes on"));
                    Rounds::OnConnected();
                    return;
                }
                ResetMatch();
                // A demo of the match (roadmap 5.1), from its first message on.
                Demo::Begin(Net::IsHost(), Net::IsHost() ? Net::OwnName() : Net::PeerName(), Net::IsHost() ? Net::PeerName() : Net::OwnName());
                Fair::OnConnected(false);
                // No pause in a duel, from the first preparation on (rules, section 1): the store and the menus
                // would pause this game.
                GetState().noPause = true;
                if (Demo::ReplayRestarting()) Log("Match: the replay starts again");
                else Headline(Net::Replaying() ? "A replay: " + Net::PeerName() + " as the opponent"
                              : Net::IsHost() ? Net::PeerName() + " joined your duel" : "You joined " + Net::PeerName() + "'s duel");
                // The host's settings count for both; the guest has the default until they come.
                LoadXpSetting();
                g_xpMatch = Net::IsHost() ? g_xpSetting : XP_DEFAULT;
                g_xpCarry = 0.f;
                // A ranked room (roadmap BG): the season's crew experience and fine settings, not the host's own.
                bool season = Net::IsHost() && Net::RoomRanked() && Rounds::SeasonKnown();
                Tune::UseSeason(season ? &Rounds::SeasonFine() : nullptr);
                if (season) g_xpMatch = Rounds::SeasonXp();
                if (Net::IsHost())
                {
                    SendSettings();
                    Announce("crew experience " + XpText(g_xpMatch) + " (your setting, as the host)");
                }
                // Test commands can change ships, so both players see a debug duel for what it is. Either player's
                // debug mode gives both the test commands, and such a match is never ranked (roadmap T).
                bool ours = GetState().debug, theirs = Net::PeerDebug();
                if ((ours || theirs) && !Net::Replaying())   // a replay's match is the recorded one (its line says)
                {
                    Headline(std::string("DEBUG DUEL: test commands are on (") + (ours ? "yours on" : "yours off") + ", " +
                             Net::PeerName() + "'s " + (theirs ? "on" : "off") + "); the match is unranked");
                    if (theirs) EnableDebug(((Net::PeerName().empty() ? std::string("the other player") : Net::PeerName()) + "'s game has debug mode on").c_str());
                }
                // The match: the host's game starts it (round 1's preparation, or at once a free fight).
                Rounds::OnConnected();
            }

            void OnNotice(const std::string &text) override
            {
                Announce(text);
            }

            void OnRefused(const std::string &text) override
            {
                Headline(text);
                g_match.lastRefusal = text;
            }

            void OnConnectionLost(const std::string &reason, bool cutOff) override
            {
                Rounds::OnConnectionLost();
                Headline(reason + (cutOff ? ": trying to get back into the match" : ": the match waits for them to come back") +
                         " (" + std::to_string((int)(Net::REJOIN_GRACE_MS / 1000.0)) + " s)");
            }

            void OnDisconnected(const std::string &reason, bool opponentGone) override
            {
                // The player still here wins when the other is gone (rules, section 3); the match flow says so. The
                // opponent's ship leaves (it used to stay as an FTL enemy, and its artillery went on firing).
                if (Demo::ReplayRestarting()) Log("Match: disconnected: %s", reason.c_str());
                else Headline("Disconnected: " + reason);
                FlushShotLog();
                Demo::End("disconnected: " + reason);
                Rounds::OnDisconnected(opponentGone);
                ResetMatch();
                Tune::EndMatch();   // our own fine settings again (roadmap BE)
                Tune::UseSeason(nullptr);
                UseRankedName("");
                Rejoin::OnDisconnected(reason);
            }

            void OnOpponentGone() override
            {
                Rounds::OpponentGone();
            }

            void OnMessage(uint8_t type, Reader &reader) override
            {
                Demo::Received(type, reader.Position(), reader.Remaining());
                switch (type)
                {
                case MSG_CHAT:
                    ReceiveChat(reader.Str());
                    break;
                case MSG_SETTINGS:
                    if (!Net::IsHost()) ApplySettings(reader);
                    break;
                case MSG_DEBUG:
                    // The other player switched debug mode on: this game gets the test commands too (roadmap T).
                    if (!Net::PeerDebug())
                    {
                        Net::SetPeerDebug();
                        Headline("DEBUG DUEL: " + Net::PeerName() + " switched debug mode on; test commands work for both, and the match is unranked");
                    }
                    EnableDebug(((Net::PeerName().empty() ? std::string("the other player") : Net::PeerName()) + "'s game has debug mode on").c_str());
                    break;
                case MSG_LOADOUT:
                    ApplyLoadout(reader);
                    break;
                case MSG_READY:
                    g_match.peerReady = true;
                    Log("Match: the opponent has built our ship");
                    break;
                case MSG_STATE:
                    ApplyState(reader);
                    break;
                case MSG_SHOT:
                    CreateIncomingShot(reader);
                    break;
                case MSG_RESULT:
                    ApplyResult(reader);
                    break;
                case MSG_SHOT_DOWNED:
                    ApplyShotDowned(reader);
                    break;
                case Fair::MSG_CHAINS:
                    Fair::OnMessage(reader);
                    break;
                case Drones::MSG_DRONE_HIT:
                case Drones::MSG_DRONE_SHOT:
                    Drones::OnMessage(type, reader);
                    break;
                case Crew::MSG_CREW_ROSTER:
                    Crew::ApplyRoster(reader);
                    break;
                case Hacking::MSG_HACK:
                case Hacking::MSG_HACK_RESULT:
                    Hacking::OnMessage(type, reader);
                    break;
                case Mind::MSG_MIND:
                case Mind::MSG_MIND_TAKEN:
                case Mind::MSG_CREW_ORDER:
                    Mind::OnMessage(type, reader);
                    break;
                case Boarding::MSG_BOARD:
                case Boarding::MSG_RECALL:
                case Boarding::MSG_RETURNED:
                case Boarding::MSG_POD:
                case Boarding::MSG_POD_RESULT:
                case Boarding::MSG_CREW_POWER:
                    Boarding::OnMessage(type, reader);
                    break;
                case Rounds::MSG_MATCH:
                case Rounds::MSG_MATCH_EVENT:
                    Rounds::OnMessage(type, reader);
                    break;
                default:
                    Log("Match: unknown message %u", (unsigned)type);
                    break;
                }
            }
        };

        static Listener g_listener;

        void Init()
        {
            if (g_match.initialised) return;
            g_match.initialised = true;
            ResetMatch();
            // The player's name: the saved one, or for this start a crew member's name (the menu's name prompt offers
            // it on a first start and saves the player's choice; the "name" command changes it; later the Steam name).
            // Plain, without the "Captain_" of before: the screen shows 10 letters of a name (roadmap AG).
            std::string name = Config::PlayerName();
            if (name.empty() && G_->GetBlueprints())
            {
                bool male = true;
                name = G_->GetBlueprints()->GetCrewName(&male);
            }
            if (!name.empty()) g_match.playerName = name;
            Net::SetListener(&g_listener);
            Net::SetIdentity(g_match.playerName, VERSION, BUILD_IDENTIFIER_HASH);
            // Coming back into a ranked room needs a new ticket from the master (roadmap BG).
            Net::SetTicketSource([](const std::string &relay, Net::TicketDone done)
                                 {
                                     Account::Ticket(relay, [done](bool ok, const std::string &ticket, const std::string &key, const std::string &why)
                                                     {
                                                         if (!ok) Log("Match: no ticket for %s: %s", "the ranked room", why.c_str());
                                                         done(ok, ticket, key);
                                                     });
                                 });
        }

        void UseRankedName(const std::string &name)
        {
            Init();
            g_match.rankedName = name.substr(0, NAME_MAX);
            Net::SetIdentity(PlayerName(), VERSION, BUILD_IDENTIFIER_HASH);
        }

        // Before a duel connects: the game's data goes into the handshake (roadmap 4.1; FTL has loaded it by then).
        static void ShareGameData()
        {
            Net::SetGameData(Fair::GameDataHash());
        }

        void OnFrame(double now)
        {
            Init();
            Net::Update(now);
            // The match's file for coming back after a crash, and a way back being tried (roadmap BR).
            Rejoin::OnFrame(now);
            // A ranked match's result reached the relay (roadmap BG): the master rates it; the new rating comes.
            static int resultState = 0;
            int state = Net::ResultState();
            if (state != resultState)
            {
                if (state == 2) Account::AfterRankedMatch();
                resultState = state;
            }
            // The enemy window fits and mirrors the opponent's ship while it is a duel replica, or the AI's ship in a
            // match against the AI (roadmap AN).
            View::SetDuelOpponent((g_match.replicaReady || Ai::ShipStands()) && G_->GetShipManager(1) != nullptr);
            // A match against the AI runs in this game alone (roadmap 3.6): the frame without the network's part.
            if (!Net::IsConnected() && !Rounds::IsLocal()) return;
            // The relay's record of a ranked match (roadmap CN): its header and markers again.
            Demo::RelayFrame();

            MatchState &m = g_match;
            if (InGame())
            {
                // The loadout and the state go while the ships meet (the match flow); between rounds each player
                // refits. The loadout first: the other game builds our ship from it before our crew roster comes.
                // A replay sends nothing: our ship is the recorder's (roadmap 5.1).
                if (Rounds::ShipsMeet() && !Rounds::IsLocal() && !Net::Replaying())
                {
                    if (!m.loadoutSent) SendLoadout();
                    else if (Armament(G_->GetShipManager(0)) != m.sentArmament)
                    {
                        Log("Match: our weapons or drones changed their slots; the loadout goes again");
                        SendLoadout();
                    }
                    if (m.stateDirty || now - m.lastStateSent >= STATE_INTERVAL_MS) SendState(now);
                }
                if (m.replicaReady)
                {
                    Hacking::OnFrame();
                    Boarding::OnFrame();
                }
                ShipManager *ship = G_->GetShipManager(0);
                // The FTL drive charges in a match's fight only, from empty at its start (running away, roadmap AD); in a
                // replay never (the recorder's states don't carry its charge).
                if (ship && (!Rounds::EscapeAllowed() || Net::Replaying()))
                {
                    ship->jump_timer.first = 0.f;
                    m.driveChargingSince = -1.0;
                    m.driveReadyLogged = false;
                }
                else if (ship)
                {
                    if (m.driveChargingSince < 0.0) m.driveChargingSince = now;
                    bool ready = ship->jump_timer.first >= ship->jump_timer.second;
                    if (ready && !m.driveReadyLogged) Log("Match: the FTL drive is ready after %.1f s of the fight (%.1f)", (now - m.driveChargingSince) / 1000.0, ship->jump_timer.second);
                    m.driveReadyLogged = ready;
                }
                TraceSync(now);
            }
            Rounds::OnFrame(now);
            // While FTL's world stands still in a live duel (a timeout, roadmap BF; a lost connection) the shots stand
            // still, and so do their waits for verdicts and their releases: they move on by the frame.
            if (!Net::Replaying() && (Rounds::TimeoutPaused() || Rounds::NetPaused()) && m.lastTrackMs >= 0.0)
            {
                ShiftShotWaits(now - m.lastTrackMs);
            }
            else TrackShots(now);
            m.lastTrackMs = now;
            // The match's status as it changes, for the demo (roadmap BB) and the relay's (CN).
            if (Demo::Recording() || Net::RecordsAtRelay())
            {
                std::string why;
                bool ranked = Rounds::Ranked(why);
                Demo::NoteStatus(ranked, why);
            }
        }

        bool WaitingForMatch()
        {
            // (A crashed game going back into its match holds the ship with its own window, DuelsRejoin.cpp.)
            if (Net::Replaying() || Rounds::IsLocal() || Rounds::InMatch() || Rejoin::Trying() || !InGame()) return false;
            Net::Phase phase = Net::GetPhase();
            return phase != Net::Phase::Idle || Lobby::OpeningDuel();
        }

        std::string LastRefusal()
        {
            // (Only while the room waits: a match that began has a guest.)
            return Net::IsConnected() ? std::string() : g_match.lastRefusal;
        }

        std::string OpponentShip()
        {
            return g_match.opponentShip;
        }

        const std::string &PlayerName()
        {
            return g_match.rankedName.empty() ? g_match.playerName : g_match.rankedName;
        }

        std::string ScreenName(const std::string &name)
        {
            if (name.size() <= SCREEN_NAME_MAX) return name;
            return name.substr(0, SCREEN_NAME_MAX - 1) + "..";
        }

        uint32_t ShotsReceived()
        {
            return g_match.shotsReceived;
        }

        bool ShipsStand()
        {
            return g_match.replicaReady && g_match.peerReady;
        }

        void NewFight()
        {
            // Our crew aboard the opponent's ship came home first (DuelsRefit.cpp): only theirs go with it.
            FlushShotLog();
            ResetMatch();
            RemoveEnemy();
            View::UsePlayerShieldPosition(nullptr);
        }

        void SetPlayerName(const std::string &name)
        {
            Init();
            g_match.playerName = name.size() > NAME_MAX ? name.substr(0, NAME_MAX) : name;
            Net::SetIdentity(PlayerName(), VERSION, BUILD_IDENTIFIER_HASH);
        }

        bool Host(uint16_t port, bool loopbackOnly, std::string &message)
        {
            Init();
            ResetMatch();
            ShareGameData();
            return Net::Host(port, loopbackOnly, message);
        }

        bool Join(const std::string &host, uint16_t port, std::string &message)
        {
            Init();
            ResetMatch();
            ShareGameData();
            return Net::Join(host, port, message);
        }

        bool HostRelay(const std::string &server, uint16_t port, const std::string &roomName, const std::string &password,
                       bool listed, std::string &message)
        {
            Init();
            ResetMatch();
            ShareGameData();
            return Net::HostRelay(server, port, roomName, password, listed, message);
        }

        bool JoinRelay(const std::string &server, uint16_t port, const std::string &code, const std::string &password,
                       std::string &message)
        {
            Init();
            ResetMatch();
            ShareGameData();
            return Net::JoinRelay(server, port, code, password, message);
        }

        uint32_t NextShotId()
        {
            return g_match.nextNetId;
        }

        void ContinueShotIds(uint32_t next)
        {
            if (next > g_match.nextNetId) g_match.nextNetId = next;
        }

        void PrepareRejoin(const std::string &rankedName)
        {
            Init();
            UseRankedName(rankedName);
            ShareGameData();
        }

        void Leave()
        {
            FlushShotLog();
            Rejoin::Clear("left the duel");   // the other game is told: nothing waits for this one any more
            Demo::End("left the duel");
            Net::Leave("left the duel");
            ResetMatch();
            Tune::EndMatch();
            Ai::Stop();   // a match against the AI (roadmap 3.6)
        }

        void ForgetMatch()
        {
            // Back in the main menu, the duel or the replay over: nothing of it stays for the next run. A replay watched
            // after a match began with FTL's first box that wouldn't close (FTL takes its answer only while paused, and the
            // match's no-pause stayed) and MATCH OVER on the score panel (the user's third test, 2026-10-02).
            if (Net::GetPhase() != Net::Phase::Idle || Net::Replaying()) return;
            ResetMatch();
            GetState().noPause = false;
            GetState().aiOff[1] = false;
            Rounds::Reset();
        }

        void SetDebug(bool debug)
        {
            Net::SetDebugFlag(debug);
            // Switched on during a duel: the other game learns it (the handshake told it only at the start).
            if (debug && Net::IsConnected())
            {
                Writer w;
                Net::Send(MSG_DEBUG, w, true);
            }
        }

        int ChatFlood(int count)
        {
            // Test: lines without the sender's limits (the receiving game must hold them back itself).
            int sent = 0;
            for (int i = 0; i < count; ++i)
            {
                Writer w;
                w.Str("flood line " + std::to_string(i + 1));
                if (Net::Send(MSG_CHAT, w, true)) ++sent;
            }
            return sent;
        }

        bool Say(const std::string &raw, std::string &message)
        {
            std::string text = CleanChat(raw);
            if (text.empty())
            {
                message = "nothing to say";
                return false;
            }
            if (!Net::IsConnected())
            {
                message = "not connected";
                return false;
            }
            if (!ChatAllowed(g_chat.sent, WallMs()))
            {
                message = "wait a moment: at most " + std::to_string(CHAT_LINES) + " lines per " +
                          std::to_string((int)(CHAT_WINDOW_MS / 1000.0)) + " s";
                Console::Feed(message);
                return false;
            }
            Writer w;
            w.Str(text);
            if (!Net::Send(MSG_CHAT, w, true))
            {
                message = "not connected";
                return false;
            }
            // Our line under our name, as the score panel shows it (DJ: it said "You").
            const std::string name = PlayerName().empty() ? std::string("You") : PlayerName();
            Log("Match: our chat, as %s: %s", name.c_str(), text.c_str());
            Console::Chat(name, text);
            message = "said: " + text;
            return true;
        }

        std::string Status()
        {
            const MatchState &m = g_match;
            std::ostringstream out;
            out << Net::Status() << " | match: opponent " << (m.opponentShip.empty() ? "-" : m.opponentShip)
                << (m.replicaReady ? " built" : "") << (m.peerReady ? ", ours built there" : "")
                << ", states applied " << m.statesApplied << ", shots out " << m.shotsSent << " in " << m.shotsReceived
                << ", verdicts sent " << m.verdictsSent << " received " << m.verdictsReceived << ", hold timeouts "
                << m.holdTimeouts << ", replica's last hull point kept " << m.hullKept << ", crystal shards lost before crossing "
                << m.shardsLost << ", frames the replica's artillery was held back " << m.artilleryHeld << ", " << Drones::Status() << ", " << Crew::Status() << ", " << Rooms::Status()
                << ", " << Bays::Status() << ", " << Hacking::Status() << ", " << Mind::Status() << ", " << Boarding::Status() << ", " << CrewXpStatus() << " (skill gains " << g_xpGains << " counted "
                << g_xpCounted << "), " << Fair::Status() << ", " << Vision::Status();
            return out.str();
        }
    }
}

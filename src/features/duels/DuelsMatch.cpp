#include "Global.h"
#include "CommandConsole.h"
#include "CustomDamage.h"
#include "HSVersion.h"
#include "Systems.h"
#include "Duels.h"
#include "DuelsMatch.h"
#include "DuelsNet.h"
#include "DuelsShipControl.h"
#include "DuelsTrace.h"
#include "DuelsView.h"
#include "DuelsWire.h"

#include <algorithm>
#include <cmath>
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
            MSG_DEFEAT = 22      // reliable: our hull reached 0
        };

        enum Outcome : uint8_t
        {
            PENDING = 0,
            OUTCOME_MISS = 1,     // dodged
            OUTCOME_SHIELD = 2,   // absorbed by the shields
            OUTCOME_HIT = 3,      // reached the hull
            OUTCOME_GONE = 4      // the defender never saw it hit anything
        };

        static const char *OutcomeName(uint8_t outcome)
        {
            switch (outcome)
            {
            case OUTCOME_MISS: return "miss";
            case OUTCOME_SHIELD: return "shield";
            case OUTCOME_HIT: return "hit";
            case OUTCOME_GONE: return "gone";
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

        static const double STATE_INTERVAL_MS = 100.0;
        static const double HOLD_TIMEOUT_MS = 3000.0;     // no verdict by then: show a miss, the state has the truth
        static const double BEAM_VERDICT_WAIT_MS = 3000.0;
        static const double NATURAL_OPPONENT_LEG_MS = 1100.0;   // step 1: a shot's flight out of the enemy window
        static const int MAX_SLOTS = 8;

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
        };

        // The opponent's projectile flying at our ship, created from their shot message.
        struct InShot
        {
            Projectile *projectile = nullptr;
            unsigned int selfId = 0;
            uint32_t netId = 0;
            int type = WEAPON_LASER;
            bool beamTouched = false;      // a beam reached our shields
            bool beamHit = false;          // a beam reached a room
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
        };

        struct MatchState
        {
            bool initialised = false;
            std::string playerName = "Captain";

            bool loadoutSent = false;
            bool replicaReady = false;     // we built the opponent's ship
            bool peerReady = false;        // they built ours
            bool defeatSent = false;
            std::string opponentShip;

            uint16_t stateSeq = 0;
            bool havePeerState = false;
            uint16_t peerStateSeq = 0;
            double lastStateSent = -1.0e9;
            bool stateDirty = false;
            uint32_t statesApplied = 0;

            uint32_t nextNetId = 1;
            std::vector<OutShot> out;
            std::vector<InShot> in;
            double legEstimate[MAX_SLOTS];
            double lastFireMs[MAX_SLOTS];   // when a replica weapon last fired a received shot

            uint32_t shotsSent = 0, shotsReceived = 0, verdictsSent = 0, verdictsReceived = 0, holdTimeouts = 0;
            std::set<std::string> unsupportedLogged;

            CsvFile shotsCsv;
            CsvFile syncCsv;
            std::string lastSignature[2];
        };

        static MatchState g_match;
        static OutShot *g_forced = nullptr;   // our shot whose collision check is running with a verdict

        static void ResetMatch()
        {
            MatchState &m = g_match;
            m.loadoutSent = false;
            m.replicaReady = false;
            m.peerReady = false;
            m.defeatSent = false;
            m.opponentShip.clear();
            m.havePeerState = false;
            m.stateDirty = false;
            m.statesApplied = 0;
            m.out.clear();
            m.in.clear();
            for (int i = 0; i < MAX_SLOTS; ++i)
            {
                m.legEstimate[i] = -1.0;
                m.lastFireMs[i] = -1.0e9;
            }
            m.lastSignature[0].clear();
            m.lastSignature[1].clear();
            g_forced = nullptr;
        }

        static void Announce(const std::string &text)
        {
            Log("Match: %s", text.c_str());
            PrintHelper::GetInstance()->AddMessage("DUEL: " + text);
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

        static void SendLoadout()
        {
            ShipManager *ship = G_->GetShipManager(0);
            if (!ship) return;
            Writer w;
            w.Str(ship->myBlueprint.blueprintName);
            w.I16((int16_t)ship->ship.hullIntegrity.second);
            w.I16((int16_t)ship->ship.hullIntegrity.first);
            w.U8((uint8_t)PowerManager::GetPowerManager(0)->currentPower.second);

            w.U8((uint8_t)ship->vSystemList.size());
            for (ShipSystem *system : ship->vSystemList)
            {
                w.U8((uint8_t)system->iSystemType);
                w.U8((uint8_t)system->powerState.second);
            }
            std::vector<ProjectileFactory*> weapons = ship->weaponSystem ? ship->GetWeaponList() : std::vector<ProjectileFactory*>();
            w.U8((uint8_t)weapons.size());
            for (ProjectileFactory *weapon : weapons) w.Str(weapon->blueprint ? weapon->blueprint->name : "");
            w.U8((uint8_t)ship->vCrewList.size());
            for (CrewMember *crew : ship->vCrewList) w.Str(crew->species);

            Net::Send(MSG_LOADOUT, w, true);
            g_match.loadoutSent = true;
            Log("Match: loadout sent (%s, hull %d/%d, %u systems, %u weapons)", ship->myBlueprint.blueprintName.c_str(),
                ship->ship.hullIntegrity.first, ship->ship.hullIntegrity.second, (unsigned)ship->vSystemList.size(),
                (unsigned)weapons.size());
        }

        static void ApplyLoadout(Reader &r)
        {
            std::string blueprint = r.Str();
            int hullMax = r.I16();
            int hull = r.I16();
            int reactor = r.U8();
            struct SystemLevel { int id; int level; };
            std::vector<SystemLevel> systems(r.U8());
            for (SystemLevel &system : systems)
            {
                system.id = r.U8();
                system.level = r.U8();
            }
            std::vector<std::string> weapons(r.U8());
            for (std::string &weapon : weapons) weapon = r.Str();
            std::vector<std::string> crew(r.U8());
            for (std::string &species : crew) species = r.Str();
            if (!r.Ok())
            {
                Log("Match: malformed loadout");
                return;
            }

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

            replica->ship.hullIntegrity.second = hullMax;
            replica->ship.hullIntegrity.first = hull;
            PowerManager::GetPowerManager(1)->currentPower.second = reactor;

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
                if (system->powerState.second != level.level)
                {
                    Log("Match: replica %s is level %d, the owner's is %d", SystemName(level.id), system->powerState.second,
                        level.level);
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

            View::UsePlayerShieldPosition(replica);
            g_match.opponentShip = blueprint;
            g_match.replicaReady = true;
            Net::Send(MSG_READY, Writer(), true);
            Announce("opponent's ship " + blueprint + " is here");
        }

        // --------------------------------------------------------------------------------------------------------
        // State: our ship ten times a second; the replica follows
        // --------------------------------------------------------------------------------------------------------

        static void SendState(double now)
        {
            ShipManager *ship = G_->GetShipManager(0);
            if (!ship) return;
            Writer w;
            w.F64(now);
            w.U16(++g_match.stateSeq);
            w.I16((int16_t)ship->ship.hullIntegrity.first);

            Shields *shields = ship->shieldSystem;
            w.Bool(shields != nullptr);
            if (shields)
            {
                w.U8((uint8_t)std::max(0, shields->shields.power.first));
                w.F32(shields->shields.charger);
            }

            w.U8((uint8_t)ship->vSystemList.size());
            for (ShipSystem *system : ship->vSystemList)
            {
                w.U8((uint8_t)system->iSystemType);
                w.U8((uint8_t)std::max(0, system->powerState.first));
                w.U8((uint8_t)std::max(0, system->healthState.first));
            }

            std::vector<ProjectileFactory*> weapons = ship->weaponSystem ? ship->GetWeaponList() : std::vector<ProjectileFactory*>();
            w.U8((uint8_t)weapons.size());
            for (ProjectileFactory *weapon : weapons)
            {
                w.Bool(weapon->powered);
                w.F32(weapon->cooldown.first);
            }

            Net::Send(MSG_STATE, w, false);
            g_match.lastStateSent = now;
            g_match.stateDirty = false;
        }

        static void ApplyState(Reader &r)
        {
            r.F64();
            uint16_t seq = r.U16();
            int hull = r.I16();
            bool hasShields = r.Bool();
            int shieldLayers = 0;
            float shieldCharge = 0.f;
            if (hasShields)
            {
                shieldLayers = r.U8();
                shieldCharge = r.F32();
            }
            struct SystemState { int id; int power; int health; };
            std::vector<SystemState> systems(r.U8());
            for (SystemState &system : systems)
            {
                system.id = r.U8();
                system.power = r.U8();
                system.health = r.U8();
            }
            struct WeaponState { bool powered; float charge; };
            std::vector<WeaponState> weapons(r.U8());
            for (WeaponState &weapon : weapons)
            {
                weapon.powered = r.Bool();
                weapon.charge = r.F32();
            }
            if (!r.Ok()) return;

            // Snapshots may arrive out of order; only newer ones count.
            if (g_match.havePeerState && (uint16_t)(seq - g_match.peerStateSeq) >= 32768) return;
            g_match.havePeerState = true;
            g_match.peerStateSeq = seq;

            ShipManager *replica = G_->GetShipManager(1);
            if (!replica || !g_match.replicaReady) return;

            replica->ship.hullIntegrity.first = std::min(hull, replica->ship.hullIntegrity.second);

            // Damage first, so power never exceeds what the system can hold.
            for (const SystemState &state : systems)
            {
                ShipSystem *system = replica->GetSystem(state.id);
                if (!system) continue;
                int health = std::max(0, std::min(state.health, system->healthState.second));
                if (system->healthState.first != health)
                {
                    system->healthState.first = health;
                    if (system->powerState.first > health) SetSystemPower(replica, state.id, health);
                }
            }
            for (const SystemState &state : systems)
            {
                // Weapons power follows the weapons below, one by one.
                if (state.id == SYS_WEAPONS || !replica->GetSystem(state.id)) continue;
                if (replica->GetSystemPower(state.id) != state.power) SetSystemPower(replica, state.id, state.power);
            }

            if (hasShields && replica->shieldSystem)
            {
                replica->shieldSystem->shields.power.first = std::min(shieldLayers, 16);
                replica->shieldSystem->shields.charger = shieldCharge;
            }

            if (replica->weaponSystem)
            {
                std::vector<ProjectileFactory*> list = replica->GetWeaponList();
                for (size_t slot = 0; slot < list.size() && slot < weapons.size(); ++slot)
                {
                    ProjectileFactory *weapon = list[slot];
                    if (weapons[slot].powered && !weapon->powered) replica->PowerWeapon(weapon, true, true);
                    else if (!weapons[slot].powered && weapon->powered) replica->DePowerWeapon(weapon, true);
                    weapon->cooldown.first = std::min(weapons[slot].charge, weapon->cooldown.second);
                }
            }
            ++g_match.statesApplied;
        }

        // --------------------------------------------------------------------------------------------------------
        // Shots
        // --------------------------------------------------------------------------------------------------------

        void OnOwnProjectile(ProjectileFactory *weapon, Projectile *projectile)
        {
            MatchState &m = g_match;
            if (!Net::IsConnected() || !m.replicaReady || !projectile || projectile->destinationSpace != 1) return;
            ShipManager *ship = G_->GetShipManager(0);
            int slot = WeaponSlot(ship, weapon);
            if (slot < 0 || !weapon->blueprint) return;

            const WeaponBlueprint *blueprint = weapon->blueprint;
            int type = blueprint->type;
            if (!Networked(type))
            {
                if (m.unsupportedLogged.insert(blueprint->name).second)
                {
                    Announce(blueprint->name + " is not networked yet; the opponent won't see it");
                }
                return;
            }

            double now = WallMs();
            OutShot shot;
            shot.projectile = projectile;
            shot.selfId = projectile->selfId;
            shot.netId = m.nextNetId++;
            shot.slot = slot;
            shot.type = type;
            shot.weapon = blueprint->name;
            shot.spawnMs = now;
            m.out.push_back(shot);

            double leg = slot < MAX_SLOTS && m.legEstimate[slot] > 0.0 ? m.legEstimate[slot]
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
            Net::Send(MSG_SHOT, w, true);
            ++m.shotsSent;
        }

        static void SendResult(InShot &shot, uint8_t outcome, int damage)
        {
            double now = WallMs();
            shot.outcome = outcome;
            shot.damage = damage;
            shot.decisionMs = now;
            Writer w;
            w.U32(shot.netId);
            w.U8(outcome);
            w.I8((int8_t)damage);
            Net::Send(MSG_RESULT, w, true);
            ++g_match.verdictsSent;
            g_match.stateDirty = true;   // the damage should arrive together with the verdict
        }

        static void CreateIncomingShot(Reader &r)
        {
            uint32_t netId = r.U32();
            int slot = r.U8();
            std::string weaponName = r.Str();
            Pointf target;
            target.x = r.F32();
            target.y = r.F32();
            double peerSpawn = r.F64();
            double ownLeg = r.F32();
            int type = r.U8();
            Pointf target2 = target;
            bool bombBypass = false;
            int shard = 0xFF;
            bool fakeShard = false;
            if (type == WEAPON_BEAM)
            {
                target2.x = r.F32();
                target2.y = r.F32();
            }
            else if (type == WEAPON_BOMB)
            {
                bombBypass = r.U8() != 0;
            }
            else if (type == WEAPON_BURST)
            {
                shard = r.U8();
                fakeShard = r.U8() != 0;
            }
            if (!r.Ok()) return;
            MatchState &m = g_match;
            ++m.shotsReceived;
            double now = WallMs();

            InShot shot;
            shot.netId = netId;
            shot.type = type;
            shot.weapon = weaponName;
            shot.receivedMs = now;
            shot.spawnMs = Net::HasClock() ? Net::PeerToLocalTime(peerSpawn) : now;

            ShipManager *replica = G_->GetShipManager(1);
            ShipManager *own = G_->GetShipManager(0);
            ProjectileFactory *weapon = nullptr;
            if (replica && replica->weaponSystem)
            {
                std::vector<ProjectileFactory*> list = replica->GetWeaponList();
                if (slot < (int)list.size() && list[slot]->blueprint && list[slot]->blueprint->name == weaponName) weapon = list[slot];
            }
            const WeaponBlueprint *blueprint = weapon ? weapon->blueprint : nullptr;
            if (!weapon || !blueprint || !own || blueprint->type != type || !Networked(type))
            {
                Log("Match: shot %u from %s slot %d cannot be shown (no such weapon on the replica)", netId,
                    weaponName.c_str(), slot);
                SendResult(shot, OUTCOME_GONE, 0);
                return;
            }

            // Like ProjectileFactory::Update does it (CustomWeapons.cpp), at the replica's weapon mount.
            Point fireLocation = weapon->weaponVisual.GetFireLocation() + weapon->localPosition;
            if (type == WEAPON_MISSILES)
            {
                if (weapon->currentFiringAngle == 0.f) fireLocation.x += 16;
                else if (weapon->currentFiringAngle == 270.f) fireLocation.y -= 16;
            }
            Pointf position((float)fireLocation.x, (float)fireLocation.y);
            Projectile *projectile = nullptr;
            switch (type)
            {
            case WEAPON_LASER:
            case WEAPON_BURST:
            {
                LaserBlast *laser = new LaserBlast(position, 1, 0, target);
                laser->OnInit();
                projectile = laser;
                break;
            }
            case WEAPON_MISSILES:
                projectile = new Missile(position, 1, 0, target, weapon->currentFiringAngle);
                break;
            case WEAPON_BEAM:
            {
                BeamWeapon *beam = new BeamWeapon(position, 1, 0, target, target2, blueprint->length, &own->_targetable,
                                                  weapon->currentFiringAngle);
                beam->SetWeaponAnimation(&weapon->weaponVisual);
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
            projectile->entryAngle = weapon->currentEntryAngle;
            projectile->Initialize(*blueprint);
            projectile->heading = weapon->currentFiringAngle;
            if (type == WEAPON_BURST && !blueprint->miniProjectiles.empty())
            {
                // The shard's own look; the harmless ones do no damage (as CustomWeapons.cpp builds them).
                const WeaponBlueprint::MiniProjectile &mini = blueprint->miniProjectiles[shard < (int)blueprint->miniProjectiles.size() ? shard : 0];
                projectile->flight_animation = G_->GetAnimationControl()->GetAnimation(mini.image);
                projectile->flight_animation.SetCurrentFrame(random32() % std::max(1, projectile->flight_animation.info.numFrames));
                projectile->flight_animation.Stop();
                if (fakeShard || mini.fake)
                {
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
            }
            else
            {
                projectile->flight_animation = weapon->flight_animation;
            }
            G_->GetWorld()->space.AddProjectile(projectile);

            // The weapon fires once per volley: a flak volley's shards arrive together.
            bool sameVolley = type == WEAPON_BURST && slot < MAX_SLOTS && now - m.lastFireMs[slot] < 100.0;
            if (slot < MAX_SLOTS) m.lastFireMs[slot] = now;
            if (!sameVolley)
            {
                weapon->weaponVisual.StartFire();
                if (!blueprint->effects.launchSounds.empty())
                {
                    G_->GetSoundControl()->PlaySoundMix(blueprint->effects.launchSounds[random32() % blueprint->effects.launchSounds.size()], -1.f, false);
                }
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
            shot.projectile = projectile;
            shot.selfId = projectile->selfId;
            m.in.push_back(shot);
        }

        static void ApplyResult(Reader &r)
        {
            uint32_t netId = r.U32();
            uint8_t outcome = r.U8();
            int damage = r.I8();
            if (!r.Ok()) return;
            ++g_match.verdictsReceived;
            for (OutShot &shot : g_match.out)
            {
                if (shot.netId != netId) continue;
                if (shot.verdict == PENDING)
                {
                    shot.verdict = outcome;
                    shot.damage = damage;
                    shot.verdictMs = WallMs();
                }
                return;
            }
            Log("Match: verdict %s for shot %u, which is gone already", OutcomeName(outcome), netId);
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
            return 1;
        }

        // Would this frame's movement decide the shot (cross the shields, or reach the target point)?
        static bool WouldDecide(Projectile *projectile, ShipManager *replica)
        {
            if (projectile->AtTarget()) return true;
            Shields *shields = replica->shieldSystem;
            if (!shields) return false;
            Damage damage = projectile->damage;
            return shields->CollisionTest(projectile->last_position.x, projectile->last_position.y, damage).collision_type == 0 &&
                   shields->CollisionTest(projectile->position.x, projectile->position.y, damage).collision_type != 0;
        }

        bool BeginCollisionCheck(Projectile *projectile, Collideable *other)
        {
            g_forced = nullptr;
            ShipManager *replica = G_->GetShipManager(1);
            if (!replica || other != &replica->_collideable) return true;
            OutShot *shot = FindOut(projectile);
            if (!shot) return true;

            if (shot->verdict == PENDING)
            {
                if (!WouldDecide(projectile, replica)) return true;
                // Wait just outside the decision point until the defender's verdict arrives.
                if (shot->holdStartMs < 0.0) shot->holdStartMs = WallMs();
                projectile->position = projectile->last_position;
                return false;
            }
            g_forced = shot;
            return true;
        }

        void EndCollisionCheck()
        {
            g_forced = nullptr;
        }

        // A bomb appears in its target room and goes off after a short delay; the dodge is rolled when it appears
        // (a dodged bomb is set aside with a "MISS"). Our bomb in the replica never rolls: it waits at the moment it
        // would go off until the defender's verdict is here, then goes off or misses as the defender's did.
        bool BeginBombCheck(BombProjectile *bomb, Collideable *other)
        {
            g_forced = nullptr;
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
            g_forced = shot;
            return true;
        }

        bool ForcedDodge(ShipManager *ship, bool &dodged)
        {
            if (!ship || ship->iShipId != 1) return false;
            OutShot *shot = FindOut(CustomDamageManager::currentProjectile);
            if (!shot || shot->type != WEAPON_BOMB) return false;
            dodged = false;   // decided when it goes off (BeginBombCheck)
            return true;
        }

        void ObserveDodge(ShipManager *ship, bool dodged)
        {
            if (!ship || ship->iShipId != 0 || !dodged) return;
            InShot *shot = FindIn(CustomDamageManager::currentProjectile);
            if (shot && shot->type == WEAPON_BOMB && shot->outcome == PENDING) SendResult(*shot, OUTCOME_MISS, 0);
        }

        // Our beam sweeping the replica: the defender's game does the damage (and the state sync shows it), ours only
        // draws it. It still runs with no damage, so the beam looks and sounds as it does.
        void MuteBeamDamage(ShipManager *ship, Damage &damage)
        {
            if (!ship || ship->iShipId != 1) return;
            OutShot *shot = FindOut(CustomDamageManager::currentProjectile);
            if (!shot || shot->type != WEAPON_BEAM) return;
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
            if (!ship || ship->iShipId != 0) return;
            InShot *shot = FindIn(CustomDamageManager::currentProjectile);
            if (!shot || shot->type != WEAPON_BEAM) return;
            shot->beamHit = shot->beamHit || hit;
            shot->damage += std::max(0, hullBefore - ship->ship.hullIntegrity.first);
        }

        static bool ForcedApplies(ShipManager *ship)
        {
            return g_forced && ship && ship->iShipId == 1 && CustomDamageManager::currentProjectile == g_forced->projectile;
        }

        bool ForcedShieldResponse(ShipManager *ship, Pointf start, Pointf finish, const Damage &damage,
                                  CollisionResponse &response)
        {
            if (!ForcedApplies(ship)) return false;
            response.collision_type = 0;
            response.point = Pointf(-2147483648.f, -2147483648.f);
            response.damage = 0;
            response.superDamage = 0;

            Shields *shields = ship->shieldSystem;
            if (!shields) return true;
            bool crossing = shields->CollisionTest(start.x, start.y, damage).collision_type == 0 &&
                            shields->CollisionTest(finish.x, finish.y, damage).collision_type != 0;
            if (!crossing) return true;

            switch (g_forced->verdict)
            {
            case OUTCOME_MISS:
            case OUTCOME_GONE:
                ship->damMessages.push_back(new DamageMessage(1.f, finish, DamageMessage::MISS));
                response.collision_type = 3;
                break;
            case OUTCOME_SHIELD:
                // Let the replica's shields show the hit; the state update brings the exact layers anyway.
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
            switch (g_forced->verdict)
            {
            case OUTCOME_HIT:
                if (g_forced->damage > 0) ship->damMessages.push_back(new DamageMessage(1.f, g_forced->damage, location, false));
                hit = true;
                break;
            case OUTCOME_SHIELD:
                hit = true;   // their shields took it; ours were a moment out of date
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
            if (!ship || ship->iShipId != 0) return;
            InShot *shot = FindIn(CustomDamageManager::currentProjectile);
            if (!shot || shot->outcome != PENDING) return;
            if (shot->type == WEAPON_BEAM)
            {
                // A beam touches the shields every frame of its sweep; its verdict is sent when it is over.
                if (response.collision_type == 2) shot->beamTouched = true;
                return;
            }
            if (response.collision_type == 3) SendResult(*shot, OUTCOME_MISS, 0);
            else if (response.collision_type == 2) SendResult(*shot, OUTCOME_SHIELD, 0);
        }

        void ObserveDamageArea(ShipManager *ship, bool hit, int hullBefore)
        {
            if (!ship || ship->iShipId != 0) return;
            InShot *shot = FindIn(CustomDamageManager::currentProjectile);
            if (!shot || shot->outcome != PENDING) return;
            SendResult(*shot, hit ? OUTCOME_HIT : OUTCOME_MISS, hit ? std::max(0, hullBefore - ship->ship.hullIntegrity.first) : 0);
        }

        // Shots whose projectile is gone are logged and forgotten; transfers feed the timing estimates.
        static void TrackShots(double now)
        {
            MatchState &m = g_match;
            WorldManager *world = G_->GetWorld();
            std::set<Projectile*> live;
            if (world)
            {
                for (Projectile *projectile : world->space.projectiles) live.insert(projectile);
            }

            for (size_t i = 0; i < m.out.size();)
            {
                OutShot &shot = m.out[i];
                bool alive = live.count(shot.projectile) && shot.projectile->selfId == shot.selfId;
                if (alive)
                {
                    if (shot.transferMs < 0.0 && shot.projectile->currentSpace == 1)
                    {
                        shot.transferMs = now;
                        if (shot.slot < MAX_SLOTS)
                        {
                            double leg = now - shot.spawnMs;
                            double &estimate = m.legEstimate[shot.slot];
                            estimate = estimate > 0.0 ? estimate * 0.7 + leg * 0.3 : leg;
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
                if (alive)
                {
                    ++i;
                    continue;
                }
                if (shot.outcome == PENDING && shot.type == WEAPON_BEAM)
                {
                    // (DamageBeam returns false even when it did damage.)
                    bool hit = shot.beamHit || shot.damage > 0;
                    SendResult(shot, hit ? OUTCOME_HIT : shot.beamTouched ? OUTCOME_SHIELD : OUTCOME_MISS, shot.damage);
                }
                else if (shot.outcome == PENDING)
                {
                    SendResult(shot, OUTCOME_GONE, 0);
                }
                LogShot("in", shot.netId, shot.weapon, shot.spawnMs, shot.receivedMs, shot.transferMs, shot.releasedMs, -1.0,
                        shot.decisionMs, -1.0, shot.outcome, shot.damage, shot.speedUp);
                m.in.erase(m.in.begin() + i);
            }
        }

        // Everything the state sync carries, as one comparable line: hull, shields, systems (id:power/health), weapons.
        static std::string Signature(ShipManager *ship)
        {
            std::ostringstream out;
            out << ship->ship.hullIntegrity.first << ',' << (ship->shieldSystem ? ship->shieldSystem->shields.power.first : -1) << ',';
            for (ShipSystem *system : ship->vSystemList)
            {
                out << system->iSystemType << ':' << system->powerState.first << '/' << system->healthState.first << ' ';
            }
            out << ',';
            if (ship->weaponSystem)
            {
                for (ProjectileFactory *weapon : ship->GetWeaponList()) out << (weapon->powered ? '1' : '0');
            }
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
                std::string signature = Signature(ship);
                if (signature == m.lastSignature[shipId]) continue;
                m.lastSignature[shipId] = signature;
                if (!m.syncCsv.IsOpen()) m.syncCsv.Open("duels_sync.csv", "wall_ms,clock_offset_ms,ship,hull,shields,systems,weapons");
                Row row;
                row << now << Net::LocalToPeerTime(0.0) << (shipId == 0 ? "own" : "replica") << signature;
                m.syncCsv.WriteRow(row.str());
            }
        }

        // --------------------------------------------------------------------------------------------------------
        // Session events
        // --------------------------------------------------------------------------------------------------------

        class Listener : public Net::Listener
        {
        public:
            void OnConnected() override
            {
                ResetMatch();
                Announce("connected to " + Net::PeerName() + (Net::IsHost() ? " (you host)" : ""));
            }

            void OnDisconnected(const std::string &reason) override
            {
                Announce("disconnected: " + reason);
                ResetMatch();
            }

            void OnMessage(uint8_t type, Reader &reader) override
            {
                switch (type)
                {
                case MSG_CHAT:
                    Announce(Net::PeerName() + ": " + reader.Str());
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
                case MSG_DEFEAT:
                    Announce("the opponent's ship is destroyed - you win this round");
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
            Net::SetListener(&g_listener);
            Net::SetIdentity(g_match.playerName, VERSION, BUILD_IDENTIFIER_HASH);
        }

        void OnFrame(double now)
        {
            Init();
            Net::Update(now);
            // The enemy window fits and mirrors the opponent's ship while it is a duel replica.
            View::SetDuelOpponent(g_match.replicaReady && G_->GetShipManager(1) != nullptr);
            if (!Net::IsConnected()) return;

            MatchState &m = g_match;
            if (InGame())
            {
                if (!m.loadoutSent) SendLoadout();
                if (m.stateDirty || now - m.lastStateSent >= STATE_INTERVAL_MS) SendState(now);
                ShipManager *ship = G_->GetShipManager(0);
                // No escaping a duel: the FTL drive never finishes charging while the opponent is here.
                if (ship && m.replicaReady) ship->jump_timer.first = 0.f;
                if (ship && ship->ship.hullIntegrity.first <= 0 && !m.defeatSent)
                {
                    m.defeatSent = true;
                    Net::Send(MSG_DEFEAT, Writer(), true);
                    Announce("your ship is destroyed - the opponent wins this round");
                }
                TraceSync(now);
            }
            TrackShots(now);
        }

        void SetPlayerName(const std::string &name)
        {
            Init();
            g_match.playerName = name;
            Net::SetIdentity(name, VERSION, BUILD_IDENTIFIER_HASH);
        }

        bool Host(uint16_t port, bool loopbackOnly, std::string &message)
        {
            Init();
            ResetMatch();
            return Net::Host(port, loopbackOnly, message);
        }

        bool Join(const std::string &host, uint16_t port, std::string &message)
        {
            Init();
            ResetMatch();
            return Net::Join(host, port, message);
        }

        void Leave()
        {
            Net::Leave("left the duel");
            ResetMatch();
        }

        bool Say(const std::string &text)
        {
            Writer w;
            w.Str(text);
            return Net::Send(MSG_CHAT, w, true);
        }

        std::string Status()
        {
            const MatchState &m = g_match;
            std::ostringstream out;
            out << Net::Status() << " | match: opponent " << (m.opponentShip.empty() ? "-" : m.opponentShip)
                << (m.replicaReady ? " built" : "") << (m.peerReady ? ", ours built there" : "")
                << ", states applied " << m.statesApplied << ", shots out " << m.shotsSent << " in " << m.shotsReceived
                << ", verdicts sent " << m.verdictsSent << " received " << m.verdictsReceived << ", hold timeouts "
                << m.holdTimeouts;
            return out.str();
        }
    }
}

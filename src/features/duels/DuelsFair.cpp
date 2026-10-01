#include "Global.h"
#include "Duels.h"
#include "DuelsChain.h"
#include "DuelsCrypto.h"
#include "DuelsFair.h"
#include "DuelsNet.h"
#include "DuelsScript.h"
#include "DuelsTrace.h"
#include "DuelsWire.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <random>
#include <set>

namespace Duels
{
    namespace Fair
    {
        static const uint32_t CHAIN_LENGTH = 16384;   // values in a chain: more shots than a long match fires
        static const int EVASION_TOLERANCE = 15;      // a verdict's evasion may be this much over our copy's (it lags)
        // The state checks: a weapon charges 1 a second in FTL, re-loaders a little faster. A manned weapons system
        // shortens its full charge instead, and FTL rescales the charge with it (a gunner leaving put an honest weapon
        // a second ahead): so the check goes by the share of the full charge, over a window of 1 to 2 s, against the
        // shortest full charge in the window, with this much slack, from this long after the fight began (a
        // Pre-Igniter fills them at once). A breach of the shields' or the reactor's limit counts once it lasts this
        // many states (a moment's change is never caught half done).
        static const float CHARGE_RATE_MAX = 1.35f;
        static const float CHARGE_SLACK = 0.5f;
        static const double CHARGE_WINDOW_MIN_MS = 1000.0, CHARGE_WINDOW_MAX_MS = 2000.0;
        static const int PERSIST_STATES = 3;
        static const double CHARGE_AFTER_START_MS = 2500.0;
        // From their shots (roadmap 4.5): the best gunner shortens a full charge by a fifth; a weapon's shots this close
        // together are one volley; a volley may come this much of a charge early (timing); a weapon that fired was
        // powered from its fastest charge before until then, less this much at both ends; their states are kept this
        // long for it.
        static const float GUNNER_BEST = 0.8f;
        static const double VOLLEY_GAP_MS = 500.0;
        static const double RATE_SLACK = 0.25;
        static const double POWER_EDGE_MS = 500.0;
        static const double POWER_KEEP_MS = 30000.0;

        enum Cheat
        {
            CHEAT_OFF = 0,
            CHEAT_DODGE,     // our verdicts say "dodged", whatever the roll
            CHEAT_EVASION,   // we roll with more evasion than our ship has, and name it
            CHEAT_CHAIN,     // our verdict values are made up
            CHEAT_HULL,      // our state's hull rises a point a second
            CHEAT_SHIELDS,   // our state names two shield layers more
            CHEAT_POWER,     // our state names three bars (or as many as given) more in each powered system
            CHEAT_CHARGE,    // our state's weapons charge twice as fast
            CHEAT_RAPID      // our weapons charge twice as fast (really)
        };

        // A weapon's volleys, for their rate: FTL's charges as a bucket that fills at the fastest charge, holds one.
        struct Volleys
        {
            const WeaponBlueprint *weapon = nullptr;
            double lastShotMs = -1.0, lastVolleyMs = -1.0;
            double charges = 1.0, chargesAt = 0.0;
            bool reported = false;
        };

        // A state of theirs while their power isn't all seen: what we see drawn on their reactor, and the weapons we
        // know were powered then (from their shots after it).
        struct PowerSample
        {
            double atMs = 0.0;
            int seen = 0, available = 0, weaponPower = 0;
            uint32_t weapons = 0;   // slots
            bool breach = false, reported = false;
            std::string what;
        };

        // A weapon's charge since the start of its window: the state after a shot (or after it lost power), and every
        // 2 s.
        struct ChargeWindow
        {
            double since = 0.0;
            float from = 0.f, last = -1.f;   // shares of the full charge
            float shortest = 0.f;            // the shortest full charge in the window
            bool reported = false;
            std::vector<std::pair<double, float>> samples;   // the window's states: time, share (a dispute's evidence)
        };

        // The opponent's last state, for the checks: in this fight.
        struct SeenState
        {
            bool fight = false;
            double fightSince = 0.0;
            double sentAt = 0.0;
            int hull = -1;
            double hullRepairUntil = 0.0;   // their clock: a hull repair drone of theirs was out until then (and 3 s)
            std::vector<ChargeWindow> charges;
            int shieldBreaches = 0, powerBreaches = 0;   // states in a row
            bool shieldReported = false, powerReported = false;
            std::map<int, Volleys> volleys;              // by weapon slot
            std::deque<PowerSample> power;
        };

        struct FairState
        {
            Chain::Own shots, verdicts;
            Chain::Known theirShots, theirVerdicts;
            std::set<uint32_t> rolledShots;   // their shots we rolled for (a value can't be used twice)
            uint32_t nextShot = 1;
            uint32_t rolls = 0, fallbacks = 0, checked = 0, evasionChecked = 0, disputes = 0;
            int evasionLowest = 1000, evasionHighest = -1000;   // the verdicts' evasion less our copy's
            std::string lastDispute;
            bool usedUpLogged = false;
            int cheat = CHEAT_OFF, cheatAmount = 0;
            double cheatSince = 0.0;
            SeenState seen;
            uint32_t statesChecked = 0, volleysChecked = 0, powerSamples = 0;
        };

        static FairState g;

        static Chain::Bytes ToBytes(const uint8_t *bytes)
        {
            Chain::Bytes value;
            std::memcpy(value.data(), bytes, VALUE_SIZE);
            return value;
        }

        // A chain from a random start no one else can know: the system clock, a random device and this game's own
        // addresses and time, hashed.
        static void MakeChain(Chain::Own &chain)
        {
            struct
            {
                uint64_t ticks, steady;
                uint32_t noise[8];
                uintptr_t where;
                double wall;
            } seed;
            seed.ticks = (uint64_t)std::chrono::system_clock::now().time_since_epoch().count();
            seed.steady = (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
            std::random_device device;
            for (uint32_t &word : seed.noise) word = device();
            seed.where = (uintptr_t)&chain;
            seed.wall = WallMs();
            chain.Make(CHAIN_LENGTH, (const uint8_t*)&seed, sizeof(seed));
        }

        static std::string Hex(const uint8_t *bytes, size_t size)
        {
            static const char *digits = "0123456789abcdef";
            std::string text;
            for (size_t i = 0; i < size; ++i)
            {
                text += digits[bytes[i] >> 4];
                text += digits[bytes[i] & 15];
            }
            return text;
        }

        static void Dispute(const std::string &text)
        {
            ++g.disputes;
            g.lastDispute = text;
            Log("Fair: dispute: %s", text.c_str());
        }

        static void SendEnds()
        {
            Writer w;
            w.U32(CHAIN_LENGTH);
            w.Bytes(g.shots.values[0].data(), VALUE_SIZE);
            w.Bytes(g.verdicts.values[0].data(), VALUE_SIZE);
            Net::Send(MSG_CHAINS, w, true);
        }

        void OnConnected(bool resumed)
        {
            if (!resumed || !g.shots.Ready() || !g.verdicts.Ready())
            {
                int cheat = g.cheat, cheatAmount = g.cheatAmount;
                g = FairState();
                g.cheat = cheat;
                g.cheatAmount = cheatAmount;
                MakeChain(g.shots);
                MakeChain(g.verdicts);
                Log("Fair: our chains for this match (%u values each): shots end %s, verdicts end %s", CHAIN_LENGTH,
                    Hex(g.shots.values[0].data(), 8).c_str(), Hex(g.verdicts.values[0].data(), 8).c_str());
            }
            SendEnds();
        }

        // Their chain's end; the same end again (after a lost connection) keeps what was shown of it.
        static bool TakeEnd(Chain::Known &known, const Chain::Bytes &end, const char *which)
        {
            auto found = known.values.find(0);
            if (known.have && found != known.values.end() && found->second == end) return false;
            known.Start(end, CHAIN_LENGTH);
            Log("Fair: their %s chain ends %s", which, Hex(end.data(), 8).c_str());
            return true;
        }

        void OnMessage(Reader &r)
        {
            uint32_t length = r.U32();
            const uint8_t *shots = r.Position();
            if (!r.Skip(VALUE_SIZE)) return;
            const uint8_t *verdicts = r.Position();
            if (!r.Skip(VALUE_SIZE) || !r.Ok()) return;
            if (length != CHAIN_LENGTH)
            {
                Dispute("their chains have " + std::to_string(length) + " values, ours " + std::to_string(CHAIN_LENGTH));
                return;
            }
            if (TakeEnd(g.theirShots, ToBytes(shots), "shot")) g.rolledShots.clear();
            TakeEnd(g.theirVerdicts, ToBytes(verdicts), "verdict");
        }

        Value NextShot()
        {
            Value value;
            if (!g.shots.Ready()) return value;
            if (g.nextShot > g.shots.Length())
            {
                if (!g.usedUpLogged) Log("Fair: our shot chain is used up: FTL rolls our further shots' dodges");
                g.usedUpLogged = true;
                return value;
            }
            value.index = g.nextShot++;
            std::memcpy(value.bytes, g.shots.values[value.index].data(), VALUE_SIZE);
            return value;
        }

        void WriteValue(Writer &w, const Value &value)
        {
            w.U32(value.index);
            if (value.index) w.Bytes(value.bytes, VALUE_SIZE);
        }

        bool ReadValue(Reader &r, Value &value)
        {
            value = Value();
            value.index = r.U32();
            if (value.index)
            {
                const uint8_t *bytes = r.Position();
                if (!r.Skip(VALUE_SIZE)) return false;
                std::memcpy(value.bytes, bytes, VALUE_SIZE);
            }
            return r.Ok();
        }

        bool Roll(const Value &shot, int &evasion, Value &verdict, bool &dodged)
        {
            verdict = Value();
            if (!g.verdicts.Ready() || shot.index == 0 || shot.index > g.verdicts.Length())
            {
                ++g.fallbacks;
                return false;
            }
            if (!g.theirShots.Take(shot.index, ToBytes(shot.bytes)))
            {
                Dispute("their shot " + std::to_string(shot.index) + "'s value doesn't follow their shot chain");
                ++g.fallbacks;
                return false;
            }
            if (!g.rolledShots.insert(shot.index).second)
            {
                // Our value for it is known to them now: a second roll with it would be foreseen.
                Dispute("their shot value " + std::to_string(shot.index) + " came again");
                ++g.fallbacks;
                return false;
            }
            verdict.index = shot.index;
            std::memcpy(verdict.bytes, g.verdicts.values[shot.index].data(), VALUE_SIZE);
            if (g.cheat == CHEAT_CHAIN)
            {
                std::mt19937 random((uint32_t)std::chrono::steady_clock::now().time_since_epoch().count());
                for (uint8_t &byte : verdict.bytes) byte = (uint8_t)random();
            }
            if (g.cheat == CHEAT_EVASION) evasion += g.cheatAmount;
            dodged = Chain::Roll(ToBytes(shot.bytes), ToBytes(verdict.bytes)) < evasion;
            if (g.cheat == CHEAT_DODGE) dodged = true;
            ++g.rolls;
            return true;
        }

        void CheckRoll(const Value &shot, const Value &verdict, int evasion, bool dodged, int expectedEvasion, const std::string &what)
        {
            ++g.checked;
            if (shot.index == 0 || verdict.index != shot.index)
            {
                Dispute(what + ": its verdict's value is for shot " + std::to_string(verdict.index) + ", not " + std::to_string(shot.index));
                return;
            }
            if (!g.theirVerdicts.Take(verdict.index, ToBytes(verdict.bytes)))
            {
                Dispute(what + ": its verdict's value doesn't follow their verdict chain");
                return;
            }
            int roll = Chain::Roll(ToBytes(shot.bytes), ToBytes(verdict.bytes));
            bool expected = roll < evasion;
            if (expected != dodged)
            {
                Dispute(what + ": rolled " + std::to_string(roll) + " against evasion " + std::to_string(evasion) + ": " +
                        (expected ? "a dodge" : "a hit") + ", but their verdict says " + (dodged ? "dodged" : "hit"));
            }
            if (expectedEvasion >= 0)
            {
                ++g.evasionChecked;
                int over = evasion - expectedEvasion;
                g.evasionLowest = std::min(g.evasionLowest, over);
                g.evasionHighest = std::max(g.evasionHighest, over);
                if (over > EVASION_TOLERANCE)
                {
                    Dispute(what + ": their verdict names evasion " + std::to_string(evasion) + ", their ship on our screen has " +
                            std::to_string(expectedEvasion));
                }
            }
        }

        void CheckState(const StateCheck &state)
        {
            SeenState &seen = g.seen;
            ++g.statesChecked;
            if (!state.fight)
            {
                seen = SeenState();
                return;
            }
            if (!seen.fight)
            {
                seen = SeenState();
                seen.fight = true;
                seen.fightSince = state.sentAt;
            }
            bool follows = seen.sentAt > 0.0 && state.sentAt > seen.sentAt && state.sentAt - seen.sentAt < 2000.0;

            // The hull: it only goes down in a fight (FTL repairs a hull only at a store), but for a hull repair drone.
            if (state.hullRepair) seen.hullRepairUntil = state.sentAt + 3000.0;
            if (seen.hull >= 0 && state.hull > seen.hull && state.sentAt > seen.hullRepairUntil)
            {
                Dispute("their hull rose from " + std::to_string(seen.hull) + " to " + std::to_string(state.hull) + " in the fight");
            }
            seen.hull = state.hull;

            // Shield layers: at most half the shield system's power.
            bool shieldBreach = state.shieldLayers >= 0 && state.shieldLayers > state.shieldPower / 2;
            seen.shieldBreaches = shieldBreach ? seen.shieldBreaches + 1 : 0;
            if (seen.shieldBreaches == PERSIST_STATES && !seen.shieldReported)
            {
                seen.shieldReported = true;
                Dispute("their shields show " + std::to_string(state.shieldLayers) + " layers with " + std::to_string(state.shieldPower) +
                        " bars of power");
            }
            if (!shieldBreach) seen.shieldReported = false;

            // Power: the systems that draw on the reactor, at most the reactor and the battery.
            bool powerBreach = state.powerAvailable >= 0 && state.powerUsed > state.powerAvailable;
            seen.powerBreaches = powerBreach ? seen.powerBreaches + 1 : 0;
            if (seen.powerBreaches == PERSIST_STATES && !seen.powerReported)
            {
                seen.powerReported = true;
                Dispute("their systems draw " + std::to_string(state.powerUsed) + " bars, their reactor and battery give " +
                        std::to_string(state.powerAvailable));
            }
            if (!powerBreach) seen.powerReported = false;

            // The weapons' charge: no faster than FTL charges them, over a window of 1 to 2 s.
            if (seen.charges.size() != state.charges.size()) seen.charges.assign(state.charges.size(), ChargeWindow());
            bool charging = follows && state.sentAt - seen.fightSince > CHARGE_AFTER_START_MS;
            for (size_t slot = 0; slot < state.charges.size(); ++slot)
            {
                ChargeWindow &window = seen.charges[slot];
                float full = slot < state.cooldowns.size() ? state.cooldowns[slot] : 0.f;
                if (full < 0.1f)
                {
                    window = ChargeWindow();   // no full charge to measure by
                    continue;
                }
                float share = state.charges[slot] / full;
                if (!charging || window.last < 0.f || share < window.last - 0.002f || state.sentAt - window.since > CHARGE_WINDOW_MAX_MS)
                {
                    window.since = state.sentAt;
                    window.from = share;
                    window.shortest = full;
                    window.reported = false;
                    window.samples.clear();
                }
                else
                {
                    window.shortest = std::min(window.shortest, full);
                    if (state.sentAt - window.since >= CHARGE_WINDOW_MIN_MS && !window.reported)
                    {
                        float seconds = (float)((state.sentAt - window.since) / 1000.0);
                        float gained = (share - window.from) * window.shortest;   // seconds of charge, at the shortest
                        if (gained > CHARGE_RATE_MAX * seconds + CHARGE_SLACK)
                        {
                            window.reported = true;
                            char text[200];
                            snprintf(text, sizeof(text), "their weapon %u charged %.2f s in %.2f s (to %.2f of its full %.2f)", (unsigned)slot,
                                     gained, seconds, state.charges[slot], full);
                            Dispute(text);
                            std::string evidence;
                            for (const std::pair<double, float> &sample : window.samples)
                            {
                                snprintf(text, sizeof(text), " %.0f:%.3f", sample.first - window.since, sample.second);
                                evidence += text;
                            }
                            Log("Fair: the weapon's charge in that window (ms since its start: share of the full charge):%s", evidence.c_str());
                        }
                    }
                }
                if (window.samples.empty() || window.samples.back().first != state.sentAt) window.samples.push_back(std::make_pair(state.sentAt, share));
                window.last = share;
            }
            seen.sentAt = state.sentAt;
        }

        // Weapons known powered over their states: a run of breaching states, as many as a state's breach must last,
        // is one dispute.
        static void CheckPowerRuns()
        {
            std::deque<PowerSample> &samples = g.seen.power;
            size_t run = 0;
            for (size_t i = 0; i < samples.size(); ++i)
            {
                run = samples[i].breach ? run + 1 : 0;
                if (run < (size_t)PERSIST_STATES || samples[i].reported) continue;
                bool already = false;
                for (size_t j = i + 1 - run; j <= i; ++j) already = already || samples[j].reported;
                for (size_t j = i + 1 - run; j <= i; ++j) samples[j].reported = true;
                if (already) continue;
                const PowerSample &sample = samples[i];
                char text[300];
                snprintf(text, sizeof(text), "their systems draw at least %d bars (%s, weapons that fired after it %d), their reactor, battery "
                         "and Zoltans give %d", sample.seen + sample.weaponPower, sample.what.c_str(), sample.weaponPower, sample.available);
                Dispute(text);
            }
        }

        void PowerSeen(double sentMs, int seenPower, int available, const std::string &what)
        {
            if (!g.seen.fight) return;
            PowerSample sample;
            sample.atMs = sentMs;
            sample.seen = seenPower;
            sample.available = available;
            sample.what = what;
            sample.breach = seenPower > available;
            g.seen.power.push_back(sample);
            ++g.powerSamples;
            while (!g.seen.power.empty() && sentMs - g.seen.power.front().atMs > POWER_KEEP_MS) g.seen.power.pop_front();
            if (sample.breach) CheckPowerRuns();
        }

        void OnShot(int slot, const WeaponBlueprint *blueprint, ShipManager *replica, double firedMs)
        {
            const double localMs = firedMs;
            SeenState &seen = g.seen;
            if (!seen.fight || !blueprint || !replica || slot < 0 || slot >= 32) return;
            // A weapon that holds several charges fires them together (Hyperspace's charge levels): not measured.
            if (blueprint->chargeLevels > 1) return;
            float full = blueprint->cooldown;
            if (blueprint->boostPower.type == 1 && blueprint->boostPower.count > 0) full -= blueprint->boostPower.count * blueprint->boostPower.amount;
            float reloaders = std::max(0.f, replica->GetAugmentationValue("AUTO_COOLDOWN"));
            double fastest = std::max(0.2, (double)(full * GUNNER_BEST / (1.f + reloaders)));   // seconds between volleys

            Volleys &volleys = seen.volleys[slot];
            if (volleys.weapon != blueprint)
            {
                // Another weapon in that slot (moved there in the fight): its own charge, full for all we know.
                volleys = Volleys();
                volleys.weapon = blueprint;
                volleys.chargesAt = localMs;
            }
            bool sameVolley = volleys.lastShotMs >= 0.0 && localMs - volleys.lastShotMs < VOLLEY_GAP_MS;
            volleys.lastShotMs = localMs;
            if (sameVolley) return;
            volleys.charges = std::min(1.0, volleys.charges + (localMs - volleys.chargesAt) / 1000.0 / fastest) - 1.0;
            volleys.chargesAt = localMs;
            ++g.volleysChecked;
            if (volleys.charges < -RATE_SLACK && !volleys.reported)
            {
                volleys.reported = true;
                char text[240];
                snprintf(text, sizeof(text), "their weapon %d (%s) fired again %.2f s after its last volley; FTL charges it in %.2f s at the fastest",
                         slot, blueprint->name.c_str(), volleys.lastVolleyMs < 0.0 ? 0.0 : (localMs - volleys.lastVolleyMs) / 1000.0, fastest);
                Dispute(text);
            }
            if (volleys.charges >= 0.0) volleys.reported = false;
            volleys.lastVolleyMs = localMs;

            // It was powered while it charged.
            bool marked = false;
            for (PowerSample &sample : seen.power)
            {
                if (sample.atMs < localMs - fastest * 1000.0 + POWER_EDGE_MS || sample.atMs > localMs - POWER_EDGE_MS) continue;
                if (sample.weapons & (1u << slot)) continue;
                sample.weapons |= 1u << slot;
                sample.weaponPower += blueprint->power;
                sample.breach = sample.seen + sample.weaponPower > sample.available;
                marked = true;
            }
            if (marked) CheckPowerRuns();
        }

        void CheatCharging(ProjectileFactory *weapon)
        {
            // A second step before FTL's own (which then finds the charge full and readies the weapon).
            if (g.cheat != CHEAT_RAPID || !weapon || weapon->iShipId != 0 || !weapon->powered) return;
            if (weapon->cooldown.first >= weapon->cooldown.second - 0.01f) return;
            weapon->cooldown.first = std::min(weapon->cooldown.second - 0.01f,
                                              weapon->cooldown.first + G_->GetCFPS()->GetSpeedFactor() * 0.0625f);
        }

        int CheatHull(int hull)
        {
            if (g.cheat != CHEAT_HULL) return hull;
            return hull + (int)((WallMs() - g.cheatSince) / 1000.0);
        }

        int CheatShieldLayers(int layers)
        {
            return g.cheat == CHEAT_SHIELDS ? layers + 2 : layers;
        }

        int CheatPower(int power)
        {
            return g.cheat == CHEAT_POWER ? power + g.cheatAmount : power;
        }

        float CheatCharge(float charge, float cooldown)
        {
            // Twice as fast: the charge named is double the real one (up to the full charge).
            return g.cheat == CHEAT_CHARGE ? std::min(cooldown, charge * 2.f) : charge;
        }

        // Test verb "fair data <text>": this game names other data in its handshakes ("fair data real": its own again).
        static std::string g_dataOverride;

        std::string GameDataHash()
        {
            if (!g_dataOverride.empty()) return g_dataOverride;
            static std::string hash;
            if (!hash.empty()) return hash;
            BlueprintManager *blueprints = G_->GetBlueprints();
            if (!blueprints || blueprints->weaponBlueprints.empty()) return "";
            Crypto::Sha256 sha;
            auto text = [&](const std::string &value)
            {
                uint32_t size = (uint32_t)value.size();
                sha.Update((const uint8_t*)&size, sizeof(size));
                sha.Update((const uint8_t*)value.data(), value.size());
            };
            auto number = [&](int32_t value) { sha.Update((const uint8_t*)&value, sizeof(value)); };
            auto real = [&](float value) { sha.Update((const uint8_t*)&value, sizeof(value)); };
            for (const auto &entry : blueprints->weaponBlueprints)
            {
                const WeaponBlueprint &w = entry.second;
                const Damage &d = w.damage;
                text(entry.first);
                text(w.typeName);
                for (int value : {d.iDamage, d.iShieldPiercing, d.fireChance, d.breachChance, d.stunChance, d.iIonDamage, d.iSystemDamage,
                                  d.iPersDamage, (int)d.bHullBuster, (int)d.bLockdown, (int)d.crystalShard, d.iStun, w.shots, w.missiles,
                                  w.power, w.length, w.miniCount, w.radius, w.boostPower.type, w.boostPower.count, w.chargeLevels, w.desc.cost})
                {
                    number(value);
                }
                real(w.cooldown);
                real(w.speed);
                real(w.boostPower.amount);
            }
            for (const auto &entry : blueprints->droneBlueprints)
            {
                const DroneBlueprint &drone = entry.second;
                text(entry.first);
                text(drone.typeName);
                text(drone.weaponBlueprint);
                for (int value : {drone.level, drone.targetType, drone.power, drone.speed, drone.dodge, drone.desc.cost}) number(value);
                real(drone.cooldown);
            }
            for (const auto &entry : blueprints->augmentBlueprints)
            {
                text(entry.first);
                real(entry.second.value);
                number(entry.second.stacking ? 1 : 0);
                number(entry.second.desc.cost);
            }
            for (const auto &entry : blueprints->shipBlueprints)
            {
                // The player ships, without what the weapon bays add as a ship is built (only one game may have built it).
                const ShipBlueprint &ship = entry.second;
                if (entry.first.compare(0, 12, "PLAYER_SHIP_") != 0) continue;
                text(entry.first);
                for (int id : ship.systems)
                {
                    if (id >= SYS_CUSTOM_FIRST) continue;
                    auto info = ship.systemInfo.find(id);
                    number(id);
                    number(info != ship.systemInfo.end() ? info->second.powerLevel : -1);
                    number(info != ship.systemInfo.end() ? info->second.maxPower : -1);
                }
                for (const std::vector<std::string> *names : {&ship.weapons, &ship.drones, &ship.augments, &ship.defaultCrew})
                {
                    number((int32_t)names->size());
                    for (const std::string &name : *names) text(name);
                }
                for (int value : {ship.health, ship.maxPower, ship.missiles, ship.drone_count, ship.weaponSlots, ship.droneSlots}) number(value);
            }
            uint8_t digest[VALUE_SIZE];
            sha.Final(digest);
            hash = Hex(digest, 8);
            Log("Fair: the game's data hashes to %s (%u weapons, %u drones, %u augments)", hash.c_str(),
                (unsigned)blueprints->weaponBlueprints.size(), (unsigned)blueprints->droneBlueprints.size(),
                (unsigned)blueprints->augmentBlueprints.size());
            return hash;
        }

        int Disputes()
        {
            return (int)g.disputes;
        }

        std::string Status()
        {
            std::string text = "fair play: rolls " + std::to_string(g.rolls) + " (FTL's " + std::to_string(g.fallbacks) + "), our shots " +
                               std::to_string(g.nextShot - 1) + ", their rolls checked " + std::to_string(g.checked);
            if (g.evasionChecked)
            {
                text += " (evasion " + std::to_string(g.evasionChecked) + ", theirs less our copy's " + std::to_string(g.evasionLowest) +
                        " to " + std::to_string(g.evasionHighest) + ")";
            }
            text += ", their states checked " + std::to_string(g.statesChecked) + " (power by their shots " +
                    std::to_string(g.powerSamples) + "), their volleys checked " + std::to_string(g.volleysChecked) + ", disputes " +
                    std::to_string(g.disputes);
            if (!g.lastDispute.empty()) text += " (last: " + g.lastDispute + ")";
            if (g.cheat != CHEAT_OFF) text += ", CHEATING (test)";
            return text;
        }

        bool RunVerb(const Command &cmd, std::string &message)
        {
            if (ArgIs(cmd, 1, "cheat"))
            {
                int amount = 0;
                if (ArgIs(cmd, 2, "dodge")) g.cheat = CHEAT_DODGE;
                else if (ArgIs(cmd, 2, "chain")) g.cheat = CHEAT_CHAIN;
                else if (ArgIs(cmd, 2, "hull")) g.cheat = CHEAT_HULL;
                else if (ArgIs(cmd, 2, "shields")) g.cheat = CHEAT_SHIELDS;
                else if (ArgIs(cmd, 2, "power"))
                {
                    g.cheat = CHEAT_POWER;
                    g.cheatAmount = ArgInt(cmd, 3, amount) ? amount : 3;
                }
                else if (ArgIs(cmd, 2, "charge")) g.cheat = CHEAT_CHARGE;
                else if (ArgIs(cmd, 2, "rapid")) g.cheat = CHEAT_RAPID;
                else if (ArgIs(cmd, 2, "evasion") && ArgInt(cmd, 3, amount))
                {
                    g.cheat = CHEAT_EVASION;
                    g.cheatAmount = amount;
                }
                else if (ArgIs(cmd, 2, "off")) g.cheat = CHEAT_OFF;
                else
                {
                    message = "usage: fair cheat dodge|evasion <n>|chain|hull|shields|power [n]|charge|rapid|off";
                    return false;
                }
                g.cheatSince = WallMs();
                Log("Fair: test cheat %s", cmd.text.c_str());
                message = g.cheat == CHEAT_OFF ? "our verdicts are honest again" : "our verdicts lie now (a test)";
                return true;
            }
            if (ArgIs(cmd, 1, "data") && cmd.args.size() > 2)
            {
                g_dataOverride = ArgIs(cmd, 2, "real") ? std::string() : cmd.args[2];
                Log("Fair: test: our handshakes name the game's data as %s", GameDataHash().c_str());
                message = "our handshakes name the game's data as " + GameDataHash();
                return true;
            }
            if (cmd.args.size() > 1)
            {
                message = "usage: fair [cheat dodge|evasion <n>|chain|hull|shields|power|charge|rapid|off | data <text>|real]";
                return false;
            }
            message = Status();
            return true;
        }
    }
}

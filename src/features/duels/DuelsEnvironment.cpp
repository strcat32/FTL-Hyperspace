#include "Global.h"
#include "Duels.h"
#include "DuelsEnvironment.h"
#include "DuelsTrace.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace Duels
{
    namespace Environment
    {
        // FTL's periods between flares and pulses, in ms (SpaceManager::SetFireLevel, SetPulsarLevel).
        static const int SUN_MIN_MS = 28000, SUN_MAX_MS = 34000;
        static const int PULSAR_MIN_MS = 11000, PULSAR_MAX_MS = 18000;
        // The first rock comes this long after the fight's start.
        static const int ROCKS_LEAD_MS = 2000;
        // FTL's anti-ship battery (SpaceManager::SetPlanetaryDefense, UpdatePDS): a shot at a random room of the target
        // every 20-25 s (its flash timer), and misses for show every 2-5 s between.
        static const int BATTERY_MIN_MS = 20000, BATTERY_MAX_MS = 25000;
        static const int BATTERY_MISS_MIN_MS = 2000, BATTERY_MISS_MAX_MS = 5000;

        // FTL's asteroid generator (AsteroidGenerator::Initialize): the waves' lengths and the time between rocks, in
        // ms, for a ship with 2 shield layers and with 3. The generator sends every other rock to each of two ships,
        // so a ship gets one every second interval.
        enum Wave : uint8_t
        {
            WAVE_CALM = 0,
            WAVE_NORMAL = 1,
            WAVE_BARRAGE = 2
        };

        struct Range
        {
            int min, max;
        };

        struct Tier
        {
            Range length[3];     // calm, normal, barrage
            Range interval[3];   // the generator's, between two rocks (none in a calm)
        };

        static const Tier TIERS[2] = {
            {{{5000, 10000}, {10000, 20000}, {5000, 10000}}, {{0, 0}, {1000, 1800}, {900, 1300}}},     // 2 layers
            {{{12000, 15000}, {16000, 28000}, {8000, 13000}}, {{0, 0}, {900, 1400}, {900, 1300}}},     // 3 layers
        };

        // The same numbers on every computer (SplitMix64); the schedule must not use FTL's random32.
        struct Numbers
        {
            uint64_t state = 0;

            uint32_t Next()
            {
                uint64_t z = (state += 0x9E3779B97F4A7C15ull);
                z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
                z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
                return (uint32_t)((z ^ (z >> 31)) >> 32);
            }

            int Between(const Range &range)
            {
                if (range.max <= range.min) return range.min;
                return range.min + (int)(Next() % (uint32_t)(range.max - range.min + 1));
            }
        };

        struct Rock
        {
            double at = 0.0;       // ms from the fight's start
            int side = 0;          // FTL's Projectile::RandomSidePoint: 0 left, 1 right
            uint32_t room = 0;     // the room, modulo the ship's room count
        };

        struct State
        {
            bool active = false;
            Plan plan;
            int round = 0;
            double startMs = 0.0;          // the fight's start on our clock

            // Flares and pulses.
            Numbers flareNumbers;
            int flares = 0;                // done
            double nextFlareMs = 0.0;      // from the fight's start
            float timerBefore = 0.f;       // FTL's flash timer before its loop (a reset: the flare went off)

            // Rocks.
            Numbers rockNumbers;
            const Tier *tier = nullptr;
            uint8_t wave = WAVE_CALM;
            double waveEnd = 0.0;
            int rocks = 0;                 // scheduled so far (the same in both games)
            int rocksMade = 0;             // at our ship
            Rock nextRock;
            bool hasNextRock = false;

            // The battery's shots (the same in both games) and its misses (each game's own, for show).
            Numbers shotNumbers;
            int shots = 0;
            double nextShotMs = 0.0;
            uint32_t nextShotRoom = 0;
            double nextMissMs = 0.0;
        };

        static State g;

        static SpaceManager *Space()
        {
            WorldManager *world = G_->GetWorld();
            return world ? &world->space : nullptr;
        }

        // ---------------------------------------------------------------------------------------------------------
        // The host's roll and the names
        // ---------------------------------------------------------------------------------------------------------

        // Rules, section 5 (v0.16): the chance that a round's fight has a hazard, and the kinds' weights (none, sun,
        // pulsar, asteroids, nebula, storm, battery). Rounds 1 and 2 have none; round 3 eases in; the storm comes
        // from round 4, the battery only from round 5 (a kind switched off gives its share to the others).
        struct Row
        {
            int chance;
            int weights[KIND_COUNT];
        };

        static const Row ROWS[3] = {
            {20, {0, 30, 15, 30, 25, 0, 0}},     // round 3
            {35, {0, 25, 20, 25, 15, 15, 0}},    // round 4
            {50, {0, 20, 20, 20, 10, 15, 15}},   // round 5 and later
        };

        // The kinds this build can make.
        static const uint8_t BUILT = (1 << SUN) | (1 << PULSAR) | (1 << ASTEROIDS) | (1 << NEBULA) | (1 << STORM) | (1 << BATTERY);

        Plan Roll(uint8_t mode, uint8_t hazards, int round, std::mt19937 &random)
        {
            Plan plan;
            plan.seed = random();
            if (plan.seed == 0) plan.seed = 1;
            if (mode == MODE_OFF) return plan;
            if (mode > MODE_OFF && mode < MODE_COUNT)
            {
                plan.kind = (uint8_t)(mode - 1);   // one kind in every fight
                return plan;
            }
            if (round <= 2) return plan;
            const Row &row = ROWS[round == 3 ? 0 : round == 4 ? 1 : 2];
            if ((int)(random() % 100) >= row.chance) return plan;
            int total = 0;
            for (int kind = 1; kind < KIND_COUNT; ++kind)
                if ((hazards & BUILT) & (1 << kind)) total += row.weights[kind];
            if (total <= 0) return plan;
            int pick = (int)(random() % (uint32_t)total);
            for (int kind = 1; kind < KIND_COUNT; ++kind)
            {
                if (!((hazards & BUILT) & (1 << kind))) continue;
                pick -= row.weights[kind];
                if (pick < 0)
                {
                    plan.kind = (uint8_t)kind;
                    break;
                }
            }
            return plan;
        }

        static const char *const HAZARD_WORDS[KIND_COUNT] = {"", "sun", "pulsar", "asteroids", "nebula", "storm", "battery"};

        bool ParseHazards(const std::string &word, uint8_t &hazards)
        {
            std::string text = word;
            std::transform(text.begin(), text.end(), text.begin(), [](char c) { return (char)std::tolower((unsigned char)c); });
            if (text == "default") { hazards = DEFAULT_HAZARDS; return true; }
            if (text == "all") { hazards = DEFAULT_HAZARDS | (1 << BATTERY); return true; }
            if (text == "none") { hazards = 0; return true; }
            uint8_t bits = 0;
            std::istringstream parts(text);
            std::string part;
            while (std::getline(parts, part, ','))
            {
                if (part == "rocks" || part == "asteroid") part = "asteroids";
                if (part == "ionstorm") part = "storm";
                bool known = false;
                for (int kind = 1; kind < KIND_COUNT; ++kind)
                {
                    if (part != HAZARD_WORDS[kind]) continue;
                    bits |= (uint8_t)(1 << kind);
                    known = true;
                }
                if (!known) return false;
            }
            hazards = bits;
            return true;
        }

        std::string HazardsName(uint8_t hazards)
        {
            std::string text;
            for (int kind = 1; kind < KIND_COUNT; ++kind)
                if (hazards & (1 << kind)) text += std::string(text.empty() ? "" : ", ") + HAZARD_WORDS[kind];
            return text.empty() ? "none" : text;
        }

        bool ParseMode(const std::string &word, uint8_t &mode)
        {
            std::string text = word;
            std::transform(text.begin(), text.end(), text.begin(), [](char c) { return (char)std::tolower((unsigned char)c); });
            for (uint8_t m = 0; m < MODE_COUNT; ++m)
            {
                if (text == ModeName(m))
                {
                    mode = m;
                    return true;
                }
            }
            if (text == "rocks" || text == "asteroid")
            {
                mode = MODE_ASTEROIDS;
                return true;
            }
            if (text == "ionstorm")
            {
                mode = MODE_STORM;
                return true;
            }
            if (text == "asb")
            {
                mode = MODE_BATTERY;
                return true;
            }
            return false;
        }

        const char *ModeName(uint8_t mode)
        {
            switch (mode)
            {
            case MODE_AUTO: return "auto";
            case MODE_OFF: return "off";
            case MODE_SUN: return "sun";
            case MODE_PULSAR: return "pulsar";
            case MODE_ASTEROIDS: return "asteroids";
            case MODE_NEBULA: return "nebula";
            case MODE_STORM: return "storm";
            case MODE_BATTERY: return "battery";
            default: return "?";
            }
        }

        const char *KindName(uint8_t kind)
        {
            switch (kind)
            {
            case SUN: return "a sun";
            case PULSAR: return "a pulsar";
            case ASTEROIDS: return "an asteroid field";
            case NEBULA: return "a nebula";
            case STORM: return "an ion storm";
            case BATTERY: return "an anti-ship battery";
            default: return "open space";
            }
        }

        const char *KindShort(uint8_t kind)
        {
            switch (kind)
            {
            case SUN: return "solar flares every 28-34 s";
            case PULSAR: return "ion pulses every 11-18 s";
            case ASTEROIDS: return "rocks in waves";
            case NEBULA: return "no sensors";
            case STORM: return "the reactor halved, no sensors";
            case BATTERY: return "a shot through the shields every 20-25 s";
            default: return "no hazard";
            }
        }

        const char *KindEffect(uint8_t kind)
        {
            switch (kind)
            {
            case SUN: return "a solar flare every 28-34 s: fires and system damage, fewer with shields up";
            case PULSAR: return "an ion pulse every 11-18 s: ion damage to the shields and one more system";
            case ASTEROIDS: return "rocks in waves: 1 damage each; shields, evasion and defense drones stop them";
            case NEBULA: return "no sensors on either ship: neither player sees into the other's ship";
            case STORM: return "a nebula with an ion storm: no sensors, and the reactor at half its power (rounded up)";
            case BATTERY: return "a shot through the shields every 20-25 s: 3 damage and a breach";
            default: return "no hazard";
            }
        }

        // ---------------------------------------------------------------------------------------------------------
        // The schedule
        // ---------------------------------------------------------------------------------------------------------

        static void NextFlare()
        {
            Range period = g.plan.kind == SUN ? Range{SUN_MIN_MS, SUN_MAX_MS} : Range{PULSAR_MIN_MS, PULSAR_MAX_MS};
            g.nextFlareMs += g.flareNumbers.Between(period);
        }

        static void StartWave(uint8_t wave, double at)
        {
            g.wave = wave;
            g.waveEnd = at + g.rockNumbers.Between(g.tier->length[wave]);
        }

        static void SetRock(double at)
        {
            g.nextRock.at = at;
            g.nextRock.side = (int)(g.rockNumbers.Next() % 2);
            g.nextRock.room = g.rockNumbers.Next();
            g.hasNextRock = true;
        }

        // The next rock after the one at `after`: calms pass without rocks, and a wave's last interval that runs past
        // its end starts the calm there (as FTL's generator does).
        static void NextRock(double after)
        {
            double t = after;
            for (int guard = 0; guard < 1000; ++guard)
            {
                if (g.wave == WAVE_CALM)
                {
                    t = g.waveEnd;
                    StartWave(g.rockNumbers.Next() % 2 == 0 ? WAVE_NORMAL : WAVE_BARRAGE, t);
                    continue;
                }
                double interval = 2.0 * g.rockNumbers.Between(g.tier->interval[g.wave]);
                if (t + interval >= g.waveEnd)
                {
                    t = g.waveEnd;
                    StartWave(WAVE_CALM, t);
                    continue;
                }
                SetRock(t + interval);
                return;
            }
            g.hasNextRock = false;
        }

        // Where FTL's battery fires from: its planet, as seen from our ship (Hyperspace's UpdatePDS), or a side of the
        // screen when the beacon has none.
        static Point BatteryOrigin(SpaceManager *space)
        {
            if (space->currentPlanet.w <= 0) return Point((int)Projectile::RandomSidePoint(0).x, (int)Projectile::RandomSidePoint(0).y);
            Point origin(space->currentPlanet.x + space->currentPlanet.w / 2 - space->shipPosition.x,
                         space->currentPlanet.y + space->currentPlanet.h / 2 - space->shipPosition.y);
            origin.x = std::min(origin.x, 800);
            origin.y = std::min(origin.y, 800);
            return origin;
        }

        static void NextShot()
        {
            g.nextShotMs += g.shotNumbers.Between(Range{BATTERY_MIN_MS, BATTERY_MAX_MS});
            g.nextShotRoom = g.shotNumbers.Next();
        }

        // FTL's battery shot (PDS_SHOT: 3 damage, through 5 shield layers, a breach) at our own ship, as FTL's battery
        // fires it (Hyperspace's CreatePDSFire); a miss goes past to a side of the screen.
        static void FireBattery(bool miss)
        {
            ShipManager *own = G_->GetShipManager(0);
            SpaceManager *space = Space();
            ShipGraph *graph = ShipGraph::GetShipInfo(0);
            int rooms = graph ? (int)graph->rooms.size() : 0;
            WeaponBlueprint *shot = G_->GetBlueprints()->GetWeaponBlueprint("PDS_SHOT");
            if (!own || !space || own->bDestroyed || rooms <= 0 || !shot) return;
            Pointf target = miss ? Projectile::RandomSidePoint(0) : own->GetRoomCenter((int)(g.nextShotRoom % (uint32_t)rooms));
            space->CreatePDSFire(shot, BatteryOrigin(space), target, 0, true);
        }

        static void MakeRock(const Rock &rock)
        {
            ++g.rocks;
            ShipManager *own = G_->GetShipManager(0);
            SpaceManager *space = Space();
            ShipGraph *graph = ShipGraph::GetShipInfo(0);
            int rooms = graph ? (int)graph->rooms.size() : 0;
            if (!own || !space || own->bDestroyed || rooms <= 0) return;
            int room = (int)(rock.room % (uint32_t)rooms);
            Pointf start = Projectile::RandomSidePoint(rock.side);
            Pointf target = own->GetRoomCenter(room);
            space->CreateAsteroid(start, 0, -1, target, 0, -1.f);
            ++g.rocksMade;
            Log("Environment: rock %d at %.0f room %d", g.rocks, rock.at, room);
        }

        // ---------------------------------------------------------------------------------------------------------
        // FTL's hazards on and off
        // ---------------------------------------------------------------------------------------------------------

        // FTL's nebula and ion storm as the status effects of its events (StatusEffect::GetNebulaEffect: the sensors
        // limited to 0; GetStormEffect: the reactor divided by 2), on our own ship only: the opponent's game does its
        // ship's.
        static void OnOwnShip(int type, int system, int amount)
        {
            WorldManager *world = G_->GetWorld();
            ShipManager *own = G_->GetShipManager(0);
            if (!world || !own) return;
            world->ModifyStatusEffect(StatusEffect{type, system, amount, StatusEffect::TARGET_PLAYER}, own, StatusEffect::TARGET_PLAYER);
        }

        static void HazardsOff(SpaceManager *space)
        {
            // As Hyperspace's <removeHazards/> does, and the nebula and the storm with their effects on our ship.
            space->asteroidGenerator.bRunning = false;
            if (space->pulsarLevel) space->SetPulsarLevel(false);
            if (space->sunLevel) space->SetFireLevel(false);
            if (space->bPDS) space->SetPlanetaryDefense(false, 0);
            if (space->bStorm) space->SetStorm(false);
            if (space->bNebula) space->SetNebula(false);
            OnOwnShip(StatusEffect::TYPE_CLEAR, SYS_SENSORS, 0);
            OnOwnShip(StatusEffect::TYPE_CLEAR, SYS_REACTOR, 0);
        }

        void Begin(const Plan &plan, int round, double startMs)
        {
            End();
            SpaceManager *space = Space();
            if (plan.kind == NONE || plan.kind >= KIND_COUNT || !space) return;
            g = State();
            g.active = true;
            g.plan = plan;
            g.round = round;
            g.startMs = startMs;
            g.flareNumbers.state = ((uint64_t)plan.seed << 32) ^ 0x5F1A5EEDull;
            g.rockNumbers.state = ((uint64_t)plan.seed << 32) ^ 0xA57E401Dull;
            HazardsOff(space);
            switch (plan.kind)
            {
            case SUN:
                space->SetFireLevel(true);
                NextFlare();
                break;
            case PULSAR:
                space->SetPulsarLevel(true);
                NextFlare();
                break;
            case BATTERY:
                // FTL's battery for its look (the planet, its hazard sign), aimed at us; its own shots are held
                // (ReplacesBattery), ours come on the schedule.
                g.shotNumbers.state = ((uint64_t)plan.seed << 32) ^ 0xBA77E4EEull;
                space->SetPlanetaryDefense(true, 0);
                NextShot();
                g.nextMissMs = BATTERY_MISS_MIN_MS;
                break;
            case NEBULA:
                space->SetNebula(true);
                OnOwnShip(StatusEffect::TYPE_LIMIT, SYS_SENSORS, 0);
                break;
            case STORM:
                space->SetStorm(true);   // FTL's storm is a nebula too
                OnOwnShip(StatusEffect::TYPE_LIMIT, SYS_SENSORS, 0);
                OnOwnShip(StatusEffect::TYPE_DIVIDE, SYS_REACTOR, 2);
                break;
            case ASTEROIDS:
            {
                // FTL's field (its look and its hazard icon); its generator is kept quiet, our schedule makes the rocks.
                g.tier = &TIERS[round >= 5 ? 1 : 0];
                space->StartAsteroids(round >= 5 ? 6 : 4, false);
                // It starts with a normal wave or a barrage, its first rock a moment after the fight's start.
                StartWave(g.rockNumbers.Next() % 2 == 0 ? WAVE_NORMAL : WAVE_BARRAGE, 0.0);
                SetRock(ROCKS_LEAD_MS);
                break;
            }
            default:
                break;
            }
            Log("Environment: %s from the fight's start, round %d, seed %08x", KindName(plan.kind), round, plan.seed);
        }

        void End()
        {
            if (!g.active) return;
            g.active = false;
            if (SpaceManager *space = Space()) HazardsOff(space);
            Log("Environment: over (%d flares or pulses, %d rocks, %d of them at our ship, %d battery shots)", g.flares, g.rocks, g.rocksMade, g.shots);
        }

        void Shift(double ms)
        {
            if (!g.active || ms <= 0.0) return;
            g.startMs += ms;
            Log("Environment: the schedule moves on by %.0f ms (the match was paused)", ms);
        }

        void ClearBeacon()
        {
            End();
            SpaceManager *space = Space();
            if (!space) return;
            if (space->sunLevel || space->pulsarLevel || space->bPDS || space->asteroidGenerator.bRunning || space->bNebula || space->bStorm)
            {
                Log("Environment: the beacon's own hazards are off for the match (%s%s)", space->bStorm ? "an ion storm" : space->bNebula ? "a nebula" : "",
                    space->sunLevel || space->pulsarLevel || space->bPDS || space->asteroidGenerator.bRunning ? " a timed hazard or rocks" : "");
            }
            HazardsOff(space);
        }

        // ---------------------------------------------------------------------------------------------------------
        // Every frame
        // ---------------------------------------------------------------------------------------------------------

        void BeforeSpaceLoop()
        {
            if (!g.active) return;
            SpaceManager *space = Space();
            if (!space) return;
            double t = WallMs() - g.startMs;
            if (g.plan.kind == SUN || g.plan.kind == PULSAR)
            {
                // FTL's flash timer runs out at the scheduled moment: its warning (5 s before), flash and damage
                // follow from the time it has left.
                TimerHelper &timer = space->flashTimer;
                float left = (float)std::max(0.0, (g.nextFlareMs - t) / 1000.0);
                timer.currGoal = timer.currTime + left;
                timer.running = true;
                g.timerBefore = timer.currTime;
            }
            else if (g.plan.kind == ASTEROIDS)
            {
                while (g.hasNextRock && g.nextRock.at <= t)
                {
                    Rock rock = g.nextRock;
                    NextRock(rock.at);
                    MakeRock(rock);
                }
            }
            else if (g.plan.kind == BATTERY)
            {
                while (g.nextShotMs <= t)
                {
                    ++g.shots;
                    ShipGraph *graph = ShipGraph::GetShipInfo(0);
                    int rooms = graph ? (int)graph->rooms.size() : 1;
                    Log("Environment: shot %d at %.0f room %d", g.shots, g.nextShotMs, (int)(g.nextShotRoom % (uint32_t)std::max(1, rooms)));
                    FireBattery(false);
                    NextShot();
                }
                if (t >= g.nextMissMs)
                {
                    FireBattery(true);
                    g.nextMissMs = t + BATTERY_MISS_MIN_MS + std::rand() % (BATTERY_MISS_MAX_MS - BATTERY_MISS_MIN_MS + 1);
                }
            }
        }

        void AfterSpaceLoop()
        {
            if (!g.active || (g.plan.kind != SUN && g.plan.kind != PULSAR)) return;
            SpaceManager *space = Space();
            if (!space) return;
            // TimerHelper::Done set the timer back to 0 (or stopped it, if it doesn't loop): the flare or pulse went
            // off in this frame.
            if (space->flashTimer.currTime < g.timerBefore || !space->flashTimer.running)
            {
                ++g.flares;
                Log("Environment: %s %d at %.0f", g.plan.kind == SUN ? "flare" : "pulse", g.flares, g.nextFlareMs);
                Log("Environment: it went off %.0f ms after the fight's start", WallMs() - g.startMs);
                NextFlare();
            }
        }

        bool AllowsHazardDamage(ShipManager *ship)
        {
            if (!g.active || !ship || ship->iShipId == 0) return true;
            Log("Environment: FTL's %s skips the opponent's ship (its own game does it)", g.plan.kind == SUN ? "flare" : "pulse");
            return false;
        }

        bool ReplacesAsteroidGenerator()
        {
            return g.active && g.plan.kind == ASTEROIDS;
        }

        bool ReplacesBattery()
        {
            // FTL's own battery never fires then: ours does, on the schedule, at our own ship only.
            return g.active && g.plan.kind == BATTERY;
        }

        bool Active()
        {
            return g.active;
        }

        bool HidesWarning(const WarningMessage *message)
        {
            if (!g.active || !message) return false;
            WorldManager *world = G_->GetWorld();
            return world && world->commandGui && message == world->commandGui->spaceStatus.warningMessage;
        }

        std::string Status()
        {
            std::ostringstream out;
            if (!g.active) return "environment: none active";
            out << "environment: " << KindName(g.plan.kind) << " (round " << g.round << ", seed " << g.plan.seed << ")";
            double t = WallMs() - g.startMs;
            if (g.plan.kind == SUN || g.plan.kind == PULSAR)
            {
                out << ", " << g.flares << (g.plan.kind == SUN ? " flares" : " pulses") << ", the next in "
                    << (int)std::max(0.0, (g.nextFlareMs - t) / 1000.0) << " s";
            }
            else if (g.plan.kind == ASTEROIDS)
            {
                out << ", " << g.rocks << " rocks, wave " << (g.wave == WAVE_CALM ? "calm" : g.wave == WAVE_NORMAL ? "normal" : "barrage");
            }
            else if (g.plan.kind == BATTERY)
            {
                out << ", " << g.shots << " battery shots, the next in " << (int)std::max(0.0, (g.nextShotMs - t) / 1000.0) << " s";
            }
            return out.str();
        }
    }
}

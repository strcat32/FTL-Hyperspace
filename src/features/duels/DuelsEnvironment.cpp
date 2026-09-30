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

        Plan Roll(uint8_t mode, int round, std::mt19937 &random)
        {
            Plan plan;
            plan.seed = random();
            if (plan.seed == 0) plan.seed = 1;
            switch (mode)
            {
            case MODE_OFF: plan.kind = NONE; break;
            case MODE_SUN: plan.kind = SUN; break;
            case MODE_PULSAR: plan.kind = PULSAR; break;
            case MODE_ASTEROIDS: plan.kind = ASTEROIDS; break;
            default:
            {
                // Rules, section 5: none in rounds 1 and 2; from round 3 a chance of 20%, 35%, then 50%.
                int chance = round <= 2 ? 0 : std::min(50, 20 + 15 * (round - 3));
                if ((int)(random() % 100) < chance) plan.kind = (uint8_t)(1 + random() % 3);
                break;
            }
            }
            return plan;
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

        static void HazardsOff(SpaceManager *space)
        {
            // As Hyperspace's <removeHazards/> does; the nebula and the storm stay as they are.
            space->asteroidGenerator.bRunning = false;
            if (space->pulsarLevel) space->SetPulsarLevel(false);
            if (space->sunLevel) space->SetFireLevel(false);
            if (space->bPDS) space->SetPlanetaryDefense(false, 0);
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
            Log("Environment: over (%d flares or pulses, %d rocks, %d of them at our ship)", g.flares, g.rocks, g.rocksMade);
        }

        void ClearBeacon()
        {
            End();
            SpaceManager *space = Space();
            if (!space) return;
            if (space->sunLevel || space->pulsarLevel || space->bPDS || space->asteroidGenerator.bRunning)
            {
                Log("Environment: the beacon's own hazards are off for the match");
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
            return out.str();
        }
    }
}

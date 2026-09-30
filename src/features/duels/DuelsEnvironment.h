#pragma once

#include <cstdint>
#include <random>
#include <string>

struct ShipManager;
struct WarningMessage;

// The fight's environment (roadmap 3.4 and Y, rules section 5; docs/design/environment.md in the FTL: Duels
// repository): a sun, a pulsar, an asteroid field, a nebula or an ion storm, the same for both players. The host rolls
// a round's environment with a seed; both games play the same schedule from the fight's start, and each lets it act on
// its own ship only.
namespace Duels
{
    namespace Environment
    {
        enum Kind : uint8_t
        {
            NONE = 0,
            SUN = 1,         // solar flares: fires and system damage
            PULSAR = 2,      // ion pulses
            ASTEROIDS = 3,   // rocks
            NEBULA = 4,      // no sensors
            STORM = 5,       // a nebula with the reactor halved
            BATTERY = 6,     // an anti-ship battery (roadmap Y, second part: not built yet)
            KIND_COUNT = 7
        };

        // The kinds the host allows when the environment comes by chance (a bit for each kind), and the default:
        // everything but the battery (rules, section 5).
        const uint8_t DEFAULT_HAZARDS = (1 << SUN) | (1 << PULSAR) | (1 << ASTEROIDS) | (1 << NEBULA) | (1 << STORM);

        // The host's setting (match env): by the round (none in rounds 1 and 2), never, or one kind in every round.
        enum Mode : uint8_t
        {
            MODE_AUTO = 0,
            MODE_OFF = 1,
            MODE_SUN = 2,
            MODE_PULSAR = 3,
            MODE_ASTEROIDS = 4,
            MODE_NEBULA = 5,
            MODE_STORM = 6,
            MODE_COUNT = 7
        };

        struct Plan
        {
            uint8_t kind = NONE;
            uint32_t seed = 0;
        };

        // Host: a round's environment. In MODE_AUTO by the round's chance and the kinds' weights (rules, section 5),
        // among the kinds `hazards` allows.
        Plan Roll(uint8_t mode, uint8_t hazards, int round, std::mt19937 &random);

        bool ParseMode(const std::string &text, uint8_t &mode);
        const char *ModeName(uint8_t mode);
        // "sun,pulsar,nebula" (or default, all, none) as the bits of the allowed kinds, and back ("sun, pulsar, nebula").
        bool ParseHazards(const std::string &text, uint8_t &hazards);
        std::string HazardsName(uint8_t hazards);
        // "a sun", and what it does in a few words.
        const char *KindName(uint8_t kind);
        const char *KindShort(uint8_t kind);
        const char *KindEffect(uint8_t kind);

        // The fight begins (startMs: that moment on our clock), and ends. Begin with NONE only ends.
        void Begin(const Plan &plan, int round, double startMs);
        void End();
        // The match was paused (a lost connection) for this long: the schedule moves on with it.
        void Shift(double ms);
        // A new match: the beacon's own hazards (FTL's, from the location) are switched off, its nebula and storm too
        // (each game's map is its own: the host's beacon could be a nebula and the guest's not).
        void ClearBeacon();

        // Every frame, around FTL's SpaceManager::OnLoop: the flash timer set to the schedule, and our rocks made.
        void BeforeSpaceLoop();
        void AfterSpaceLoop();
        // FTL's flare or pulse on a ship: in a duel only on our own (the opponent's game does its ship's).
        bool AllowsHazardDamage(ShipManager *ship);
        // FTL's asteroid generator stays quiet while our schedule makes the rocks.
        bool ReplacesAsteroidGenerator();
        // FTL's "SOLAR FLARE / ION PULSE IMMINENT!" text (its sound still plays): not drawn in a duel (roadmap N).
        bool HidesWarning(const WarningMessage *message);

        bool Active();
        std::string Status();
    }
}

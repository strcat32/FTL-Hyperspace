#pragma once

#include <cstdint>
#include <random>
#include <string>

struct ShipManager;
struct WarningMessage;

// The fight's environment (roadmap 3.4, rules section 5; docs/design/environment.md in the FTL: Duels repository): a
// sun, a pulsar or an asteroid field, the same for both players. The host rolls a round's environment with a seed;
// both games play the same schedule from the fight's start, and each lets it act on its own ship only.
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
            KIND_COUNT = 4
        };

        // The host's setting (match env): by the round (none in rounds 1 and 2), never, or one kind in every round.
        enum Mode : uint8_t
        {
            MODE_AUTO = 0,
            MODE_OFF = 1,
            MODE_SUN = 2,
            MODE_PULSAR = 3,
            MODE_ASTEROIDS = 4,
            MODE_COUNT = 5
        };

        struct Plan
        {
            uint8_t kind = NONE;
            uint32_t seed = 0;
        };

        // Host: a round's environment.
        Plan Roll(uint8_t mode, int round, std::mt19937 &random);

        bool ParseMode(const std::string &text, uint8_t &mode);
        const char *ModeName(uint8_t mode);
        // "a sun", and what it does in a few words.
        const char *KindName(uint8_t kind);
        const char *KindShort(uint8_t kind);
        const char *KindEffect(uint8_t kind);

        // The fight begins (startMs: that moment on our clock), and ends. Begin with NONE only ends.
        void Begin(const Plan &plan, int round, double startMs);
        void End();
        // The match was paused (a lost connection) for this long: the schedule moves on with it.
        void Shift(double ms);
        // A new match: the beacon's own hazards (FTL's, from the location) are switched off.
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

#pragma once

#include "DuelsScript.h"

#include <string>

struct ShipManager;

namespace Duels
{
    // Ship-level verbs: power, weapon, fire, autofire, crew, door, cloak, spawn, export, import,
    // describe, pausetest, quit. Ship ids are 0 (player) and 1 (enemy); "the opponent" is 1 - id.
    bool ExecuteShipCommand(const Command &cmd, std::string &message);

    // Called from the ShipAI::OnLoop hook while a ship's AI is replaced by commands.
    // Clears whatever the AI had aimed, once per ship.
    void OnAiTakeover(ShipManager *ship);

    // Logs every power change of the replaced enemy ship's systems ("tracepower on").
    void TracePowerChanges(int frame);

    // Per-frame projectile tracing to duels_projectiles.csv (only while tracing and in a game).
    void TraceProjectiles();
    void CloseProjectileTrace();
}

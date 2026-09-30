#pragma once

#include "DuelsScript.h"

#include <string>

struct ShipManager;

namespace Duels
{
    // Ship-level verbs: power, weapon, fire, autofire, crew, door, cloak, spawn, export, import,
    // describe, pausetest, quit. Ship ids are 0 (player) and 1 (enemy); "the opponent" is 1 - id.
    bool ExecuteShipCommand(const Command &cmd, std::string &message);

    // Spawns a ship blueprint (a player ship, too) as the enemy, ship 1.
    bool SpawnEnemy(const std::string &blueprint, std::string &message);
    // The enemy ship (ship 1) leaves the location, with everything aboard; false if there is none.
    bool RemoveEnemy();

    // Sets a system's power the way the player's power bars do. Returns true if the level was reached.
    bool SetSystemPower(ShipManager *ship, int system, int level);

    // "shields", "engines", ... for system ids 0-15.
    const char *SystemName(int system);

    // Called from the ShipAI::OnLoop hook while a ship's AI is replaced by commands.
    // Clears whatever the AI had aimed, once per ship.
    void OnAiTakeover(ShipManager *ship);

    // Logs every power change of the replaced enemy ship's systems ("tracepower on").
    void TracePowerChanges(int frame);

    // "swap ... incoming": the swap, once the opponent's next shot is in the air.
    void SwapOnFrame();

    // Per-frame projectile tracing to duels_projectiles.csv (only while tracing and in a game).
    void TraceProjectiles();
    void CloseProjectileTrace();
}

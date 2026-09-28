#pragma once

#include "DuelsScript.h"

#include <string>
#include <vector>

struct CommandGui;

// FTL:Duels — 1v1 real-time PvP on top of Hyperspace.
// Step 1 (offline replica): drive the enemy ship from commands, keep the simulation from pausing,
// and trace frame timing and projectile flights to CSV files in the game directory.
// Step 2 (network duel): two games over UDP; each owns its ship, the opponent is a replica (DuelsMatch.h).
namespace Duels
{
    // Version of the Duels module (the Hyperspace version stays upstream's, so mods' version checks keep working).
    static const char *const VERSION = "0.3.0-dev";

    struct State
    {
        bool noPause = false;              // keep the simulation running whatever the UI does
        bool aiOff[2] = {false, false};    // ship AI replaced by commands, per ship id
        bool trace = false;                // write duels_frames.csv and duels_projectiles.csv
        bool quitRequested = false;        // set by the "quit" verb; the harness exits the game
        bool tracePower = false;           // log every power-up of a replaced ship's systems, with the caller
        double gameTime = 0.0;             // simulated seconds (SpeedFactor / 16 per WorldManager::OnLoop)

        // Timed script (from the console "duel script <file>" or the autotest harness).
        std::vector<Command> script;
        size_t nextScriptCommand = 0;
        double scriptStartMs = -1.0;       // < 0 when no script is running

        // Per-frame counters, reset after each CApp::OnLoop.
        int worldLoops = 0;                // WorldManager::OnLoop calls (simulation advanced)
        int pauseLoops = 0;                // WorldManager::PauseLoop calls (simulation paused)
        int inputPumps = 0;                // CApp::GenInputEvents calls
        int blockedPauses = 0;             // pause requests swallowed by no-pause
    };

    State &GetState();

    // printf-style logging to duels_log.txt (also mirrored into FTL_HS.log).
    void Log(const char *format, ...) __attribute__((format(printf, 1, 2)));

    // Executes one command immediately. Returns false and fills `message` on failure.
    bool Execute(const Command &cmd, std::string &message);

    // Starts a timed script; its times are seconds from now.
    bool StartScript(const std::string &path, std::string &message);

    // Called once per frame after CApp::OnLoop (runs in menus and while paused).
    void OnFrame();

    // Called when the game shuts down; flushes and closes trace files.
    void Shutdown();

    // Autotest harness (DuelsAutotest.cpp): runs a scenario from duels_autotest.txt without a human.
    void AutotestOnFrame();
    bool AutotestActive();

    // Opens the command console (F1), if nothing else has the focus (DuelsHooks.cpp).
    bool OpenConsole(CommandGui *gui);
}

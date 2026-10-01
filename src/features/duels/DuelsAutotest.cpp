#include "Global.h"
#include "Duels.h"
#include "DuelsTrace.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

// Autotest harness: when duels_autotest.txt exists in the game directory at startup, the game plays a scenario
// without a human. It starts a new game from the main menu, closes the intro dialogs, runs the scenario's timed
// commands (times count from the moment the game is ready), then quits. tools/run-autotest.ps1 drives it.
//
// Scenario files are ordinary duel scripts plus optional directives:
//   @timeout <seconds>     hard limit for the whole run (default 180)
//   @nodebug               don't switch debug mode on (it is on for every other scenario: test commands need it)
//   @config                the host's settings in duels.cfg are read and written as in a player's game (other
//                          scenarios start from the defaults and leave the file as it is)
//   @menu                  the scenario runs in the main menu (tests of the menu's windows): no game is started
//   @ship <BLUEPRINT>      the ship to start with (a player ship, e.g. PLAYER_SHIP_FED), instead of the hangar's
namespace Duels
{
    enum class Phase
    {
        Off,
        WaitMenu,
        OpenBuilder,
        WaitGame,
        CloseDialogs,
        Running,
        Quitting
    };

    static const char *AUTOTEST_FILE = "duels_autotest.txt";

    struct Autotest
    {
        bool checked = false;
        Phase phase = Phase::Off;
        int frames = 0;          // frames spent in the current phase
        int quietFrames = 0;     // consecutive frames without an open dialog
        int dialogsClosed = 0;
        double timeoutS = 180.0;
        double startMs = 0.0;
        std::string ship;        // @ship, empty for the hangar's own choice
        bool config = false;     // @config: the host's settings read from and written to duels.cfg
        bool menu = false;       // @menu: the scenario runs in the main menu
        std::vector<Command> script;
    };

    static Autotest g_auto;

    static void Enter(Phase phase)
    {
        g_auto.phase = phase;
        g_auto.frames = 0;
    }

    static void RequestQuit(const char *reason)
    {
        Log("Autotest: quitting (%s)", reason);
        Enter(Phase::Quitting);
    }

    static void LoadAutotest()
    {
        g_auto.checked = true;
        if (!std::ifstream(AUTOTEST_FILE)) return;

        g_auto.startMs = RealMs();
        std::vector<std::string> directives;
        std::string error;
        bool noDebug = false;
        if (!LoadScript(AUTOTEST_FILE, g_auto.script, directives, error))
        {
            Log("Autotest: %s", error.c_str());
            RequestQuit("bad scenario");
            return;
        }

        for (const std::string &directive : directives)
        {
            std::istringstream words(directive);
            std::string key, name;
            double value;
            words >> key;
            if (key == "@timeout" && (words >> value)) g_auto.timeoutS = value;
            else if (key == "@nodebug") noDebug = true;
            else if (key == "@ship" && (words >> name)) g_auto.ship = name;
            else if (key == "@config") g_auto.config = true;
            else if (key == "@menu") g_auto.menu = true;
            else Log("Autotest: unknown directive '%s'", directive.c_str());
        }

        Log("Autotest: scenario with %u commands, timeout %.0f s", (unsigned)g_auto.script.size(), g_auto.timeoutS);
        if (!noDebug) EnableDebug("test scenario");
        Enter(Phase::WaitMenu);
    }

    bool AutotestActive()
    {
        return g_auto.phase != Phase::Off;
    }

    bool SettingsFromConfig()
    {
        return !AutotestActive() || g_auto.config;
    }

    // @ship: the hangar switches to that ship (FTL keeps them as ships[type * 3 + variant]).
    static bool SelectShip(ShipBuilder &builder, const std::string &name)
    {
        const int count = (int)(sizeof(builder.ships) / sizeof(builder.ships[0]));
        for (int i = 0; i < count; ++i)
        {
            if (!builder.ships[i] || builder.ships[i]->blueprintName != name) continue;
            builder.SwitchShip(i / 3, i % 3);
            return builder.currentShip && builder.currentShip->myBlueprint.blueprintName == name;
        }
        return false;
    }

    void AutotestOnFrame()
    {
        if (!g_auto.checked) LoadAutotest();

        State &state = GetState();
        CApp *app = G_->GetCApp();
        WorldManager *world = G_->GetWorld();

        // "quit" also works from the console, outside the harness.
        if (state.quitRequested && g_auto.phase != Phase::Quitting)
        {
            state.quitRequested = false;
            RequestQuit("quit command");
        }
        if (g_auto.phase == Phase::Off || !app) return;

        ++g_auto.frames;
        if (g_auto.phase != Phase::Quitting && RealMs() - g_auto.startMs > g_auto.timeoutS * 1000.0)
        {
            RequestQuit("timeout");
        }

        switch (g_auto.phase)
        {
        case Phase::WaitMenu:
            // Let the main menu settle for a second before driving it.
            if (app->menu.bOpen && g_auto.frames >= 60 && g_auto.menu)
            {
                Log("Autotest: running the scenario in the main menu");
                state.script = g_auto.script;
                state.nextScriptCommand = 0;
                state.scriptStartMs = RealMs();
                Enter(Phase::Running);
            }
            else if (app->menu.bOpen && g_auto.frames >= 60)
            {
                app->menu.shipBuilder.Open();
                Log("Autotest: hangar opened");
                Enter(Phase::OpenBuilder);
            }
            break;

        case Phase::OpenBuilder:
            if (g_auto.frames >= 20)
            {
                if (!g_auto.ship.empty())
                {
                    bool selected = SelectShip(app->menu.shipBuilder, g_auto.ship);
                    Log("Autotest: ship %s %s", g_auto.ship.c_str(), selected ? "selected" : "not found in the hangar");
                }
                app->menu.shipBuilder.Finish();
                Log("Autotest: starting a new game");
                Enter(Phase::WaitGame);
            }
            break;

        case Phase::WaitGame:
            if (!app->menu.bOpen && world && world->bStartedGame && world->playerShip && world->commandGui)
            {
                Log("Autotest: in game after %d frames", g_auto.frames);
                g_auto.quietFrames = 0;
                Enter(Phase::CloseDialogs);
            }
            break;

        case Phase::CloseDialogs:
        {
            ChoiceBox &box = world->commandGui->choiceBox;
            if (box.bOpen && !box.choiceBoxes.empty())
            {
                g_auto.quietFrames = 0;
                if (g_auto.frames % 10 == 0)
                {
                    box.KeyDown(SDLK_1);
                    ++g_auto.dialogsClosed;
                }
            }
            else if (++g_auto.quietFrames >= 60)
            {
                Log("Autotest: %d dialog choices made, running the scenario", g_auto.dialogsClosed);
                state.script = g_auto.script;
                state.nextScriptCommand = 0;
                state.scriptStartMs = RealMs();
                Enter(Phase::Running);
            }
            break;
        }

        case Phase::Running:
            if (state.scriptStartMs < 0.0) RequestQuit("scenario finished without 'quit'");
            break;

        case Phase::Quitting:
            if (g_auto.frames == 1)
            {
                Shutdown();
                app->OnRequestExit();
            }
            else if (g_auto.frames > 180)
            {
                // OnRequestExit normally ends the main loop; this is the fallback.
                std::exit(0);
            }
            break;

        default:
            break;
        }
    }
}

#include "Global.h"
#include "Duels.h"
#include "DuelsAccount.h"
#include "DuelsConfig.h"
#include "DuelsConsole.h"
#include "DuelsMatch.h"
#include "DuelsNet.h"
#include "DuelsScreen.h"
#include "DuelsLobby.h"
#include "DuelsQueue.h"
#include "DuelsShipControl.h"
#include "DuelsTrace.h"
#include "DuelsView.h"
#include "DuelsWin32.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <map>

namespace Duels
{
    static State g_state;
    static FILE *g_logFile = nullptr;
    static CsvFile g_frames;
    static int g_frame = 0;
    static double g_lastFrameMs = 0.0;

    State &GetState()
    {
        return g_state;
    }

    void Log(const char *format, ...)
    {
        char buffer[1024];
        va_list args;
        va_start(args, format);
        vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);

        // Truncate once per run; reopen in append mode if Shutdown() closed it and something logs afterwards.
        static bool opened = false;
        if (!g_logFile)
        {
            if (!opened)
            {
                // The last two sessions' logs stay (roadmap BL: a crashed session's log was gone at the next start).
                std::remove("duels_log.2.txt");
                std::rename("duels_log.1.txt", "duels_log.2.txt");
                std::rename("duels_log.txt", "duels_log.1.txt");
            }
            g_logFile = fopen("duels_log.txt", opened ? "a" : "w");
            opened = true;
        }
        if (g_logFile)
        {
            fprintf(g_logFile, "[%10.1f ms] %s\n", WallMs(), buffer);
            fflush(g_logFile);
        }
        hs_log_file("[Duels] %s\n", buffer);
    }

    bool StartScript(const std::string &path, std::string &message)
    {
        std::vector<Command> script;
        std::vector<std::string> directives;
        if (!LoadScript(path, script, directives, message)) return false;
        if (!directives.empty()) Log("Script %s: ignoring %u harness directives", path.c_str(), (unsigned)directives.size());

        g_state.script = script;
        g_state.nextScriptCommand = 0;
        g_state.scriptStartMs = RealMs();
        message = "script " + path + " started";
        Log("Script %s started with %u commands", path.c_str(), (unsigned)script.size());
        return true;
    }

    static void RunDueScriptCommands()
    {
        if (g_state.scriptStartMs < 0.0) return;

        // Real time: a script goes on while a replay is paused (its clock stands still then).
        double elapsed = (RealMs() - g_state.scriptStartMs) / 1000.0;
        while (g_state.nextScriptCommand < g_state.script.size() &&
               g_state.script[g_state.nextScriptCommand].time <= elapsed)
        {
            const Command &cmd = g_state.script[g_state.nextScriptCommand++];
            std::string message;
            bool ok = Execute(cmd, message);
            Log("script t=%.2f line %d: %s -> %s%s", elapsed, cmd.line, cmd.text.c_str(),
                ok ? "ok" : "FAILED", message.empty() ? "" : (": " + message).c_str());
        }

        if (g_state.nextScriptCommand >= g_state.script.size())
        {
            Log("Script finished after %.2f s", elapsed);
            g_state.scriptStartMs = -1.0;
        }
    }

    static void TraceFrame()
    {
        if (!g_state.trace) return;

        if (!g_frames.IsOpen())
        {
            g_frames.Open("duels_frames.csv",
                          "frame,wall_ms,dt_ms,game_s,speed_factor,game_dt_s,speed_level,world_loops,pause_loops,input_pumps,"
                          "blocked_pauses,b_paused,auto_paused,menu_pause,event_pause,touch_pause,focus,minimized,"
                          "game_logic,rendering,in_game");
        }

        double now = WallMs();
        CApp *app = G_->GetCApp();
        CFPS *fps = G_->GetCFPS();
        CommandGui *gui = app ? app->gui : nullptr;
        bool inGame = app && app->world && gui && !app->menu.bOpen;

        Row row;
        row << g_frame << now << (g_lastFrameMs > 0.0 ? now - g_lastFrameMs : 0.0) << g_state.gameTime
            << fps->SpeedFactor << fps->SpeedFactor * 0.0625f << fps->speedLevel
            << g_state.worldLoops << g_state.pauseLoops << g_state.inputPumps << g_state.blockedPauses;
        if (gui)
        {
            row << gui->bPaused << gui->bAutoPaused << gui->menu_pause << gui->event_pause << gui->touch_pause;
        }
        else
        {
            row << "" << "" << "" << "" << "";
        }
        row << (app ? app->focus : false) << (app ? app->minimized : false)
            << (app ? app->gameLogic : false) << (app ? app->rendering : false) << inGame;
        g_frames.WriteRow(row.str());

        g_lastFrameMs = now;
    }

    // Test runs play silently (DUELS_MUTE=1, set by the test runners). Sound and music go to 0 on every frame, since
    // the options menu would set them back.
    void NoteSound(const std::string &name)
    {
        struct Count
        {
            double since = -1.0;
            int count = 0;
            double loggedAt = -1.0e12;
        };
        static std::map<std::string, Count> counts;
        double now = RealMs();
        Count &c = counts[name];
        if (c.since < 0.0 || now - c.since > 1000.0)
        {
            c.since = now;
            c.count = 0;
        }
        if (++c.count == 8 && now - c.loggedAt > 10000.0)
        {
            c.loggedAt = now;
            Log("Sounds: '%s' 8 times within a second", name.c_str());
        }
    }

    static void MuteForTests()
    {
        static int mute = -1;
        if (mute < 0)
        {
            const char *value = getenv("DUELS_MUTE");
            mute = value && value[0] == '1' ? 1 : 0;
            if (mute) Log("Sound and music off (DUELS_MUTE)");
        }
        SoundControl *sound = mute ? G_->GetSoundControl() : nullptr;
        if (!sound) return;
        if (sound->GetSoundVolume() != 0.f) sound->SetSoundVolume(0.f);
        if (sound->GetMusicVolume() != 0.f) sound->SetMusicVolume(0.f);
    }

    // FTL's world keeps real time only at 32 frames a second and more: its step stops growing at 31.25 ms (roadmap CY).
    // A duel's game below that charges its weapons, walks its crew and burns slower than the opponent's: it is told,
    // once a duel. (The frame limit and V-sync themselves change nothing else: each step is scaled by the frame's time.)
    static void CheckFrameRate()
    {
        static double since = -1.0;
        static int frames = 0;
        static bool told = false;
        if (!Net::IsConnected() || Net::Replaying())
        {
            since = -1.0;
            frames = 0;
            told = false;
            return;
        }
        double now = RealMs();
        if (since < 0.0)
        {
            since = now;
            frames = 0;
            return;
        }
        ++frames;
        if (now - since < 5000.0) return;
        double fps = frames * 1000.0 / (now - since);
        since = now;
        frames = 0;
        if (fps >= 30.0 || told) return;
        told = true;
        char text[200];
        std::snprintf(text, sizeof(text), "This game runs at %.0f frames a second: below 32 FTL slows its world down (your "
                                          "weapons charge slower). Close other programs or lower the screen's size.", fps);
        Console::Feed(text);
        Log("Frames: %.1f a second in a duel (FTL's world runs slower than real time below 32)", fps);
    }

    void OnFrame()
    {
        CheckFrameRate();
        if (++g_frame == 1)
        {
            Log("FTL:Duels module %s loaded", VERSION);
            const char *debug = getenv("DUELS_DEBUG");
            if (debug && debug[0] == '1') EnableDebug("DUELS_DEBUG");
            Console::MigrateKeys();
        }
        // The traces (duels_frames.csv, duels_sync.csv, duels_projectiles.csv) are on in the test builds unless duels.cfg
        // says "trace off" (the user's two-PC tests, 2026-10-02: "enable debug traces for both clients ... in the dist").
        // A test scenario sets its own (its first frame has read it by now).
        if (g_frame == 2 && SettingsFromConfig() && Config::Value("trace") != "off" && !g_state.trace)
        {
            g_state.trace = true;
            Log("Traces on (duels_frames.csv, duels_sync.csv, duels_projectiles.csv; \"trace off\" in duels.cfg switches them off)");
        }
        MuteForTests();
        PlaceOnTestDisplay();
        Match::OnFrame(WallMs());
        Account::Frame();   // the master's answers (roadmap BG)
        HeldMouseOnFrame();
        View::OnFrame();
        Screen::OnFrame();
        RunDueScriptCommands();
        SwapOnFrame();
        Lobby::OnFrame();
        Queue::OnFrame();   // the ranked queue (roadmap BW)
        AutotestOnFrame();
        TraceFrame();
        TraceProjectiles();
        TracePowerChanges(g_frame);

        g_state.worldLoops = 0;
        g_state.pauseLoops = 0;
        g_state.inputPumps = 0;
        g_state.blockedPauses = 0;
    }

    void Shutdown()
    {
        Match::Leave();
        g_state.trace = false;
        g_frames.Close();
        CloseProjectileTrace();
        if (g_logFile)
        {
            fclose(g_logFile);
            g_logFile = nullptr;
        }
    }
}

#include "Global.h"
#include "Duels.h"
#include "DuelsMatch.h"
#include "DuelsScreen.h"
#include "DuelsShipControl.h"
#include "DuelsTrace.h"
#include "DuelsView.h"

#include <cstdarg>
#include <cstdio>

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
        g_state.scriptStartMs = WallMs();
        message = "script " + path + " started";
        Log("Script %s started with %u commands", path.c_str(), (unsigned)script.size());
        return true;
    }

    static void RunDueScriptCommands()
    {
        if (g_state.scriptStartMs < 0.0) return;

        double elapsed = (WallMs() - g_state.scriptStartMs) / 1000.0;
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

    void OnFrame()
    {
        if (++g_frame == 1) Log("FTL:Duels module %s loaded", VERSION);
        Match::OnFrame(WallMs());
        View::OnFrame();
        Screen::OnFrame();
        RunDueScriptCommands();
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

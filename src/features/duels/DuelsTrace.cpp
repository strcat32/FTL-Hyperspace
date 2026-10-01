#include "DuelsTrace.h"

#include <algorithm>
#include <chrono>

namespace Duels
{
    static bool g_replayClock = false;
    static double g_replayMs = 0.0;   // WallMs while a replay's clock runs
    static double g_aheadMs = 0.0;    // WallMs ahead of real time after a replay ran faster than it

    double RealMs()
    {
        typedef std::chrono::steady_clock Clock;
        static const Clock::time_point start = Clock::now();
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }

    double WallMs()
    {
        return g_replayClock ? g_replayMs : RealMs() + g_aheadMs;
    }

    void ReplayClockOn()
    {
        if (g_replayClock) return;
        g_replayMs = WallMs();
        g_replayClock = true;
    }

    void ReplayClockAdvance(double ms)
    {
        if (g_replayClock && ms > 0.0) g_replayMs += ms;
    }

    void ReplayClockOff()
    {
        if (!g_replayClock) return;
        g_replayClock = false;
        g_aheadMs = std::max(g_aheadMs, g_replayMs - RealMs());
    }

    bool CsvFile::Open(const std::string &path, const std::string &header)
    {
        Close();
        file.open(path.c_str(), everOpened ? (std::ios::out | std::ios::app) : (std::ios::out | std::ios::trunc));
        if (!file) return false;
        if (!everOpened) file << header << '\n';
        file.flush();
        everOpened = true;
        unflushedRows = 0;
        return true;
    }

    void CsvFile::Close()
    {
        if (file.is_open())
        {
            file.flush();
            file.close();
        }
    }

    void CsvFile::WriteRow(const std::string &fields)
    {
        if (!file.is_open()) return;
        file << fields << '\n';
        if (++unflushedRows >= 120)
        {
            file.flush();
            unflushedRows = 0;
        }
    }
}

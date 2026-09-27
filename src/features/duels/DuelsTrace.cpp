#include "DuelsTrace.h"

#include <chrono>

namespace Duels
{
    double WallMs()
    {
        typedef std::chrono::steady_clock Clock;
        static const Clock::time_point start = Clock::now();
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
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

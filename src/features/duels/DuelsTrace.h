#pragma once

#include <fstream>
#include <sstream>
#include <string>

namespace Duels
{
    // Milliseconds since the game started, never going back. In a replay (roadmap 5.1) the replay's own time: FTL's world
    // time, which moves on with the world's steps (ReplayClockAdvance) and stands still while the replay is paused, so
    // everything timed by it follows the replay at any speed.
    double WallMs();
    // Real time since the game started, always (the test harness's script goes on while a replay is paused).
    double RealMs();
    // A replay's clock: from On, WallMs moves on only by Advance; Off goes back to real time (it never goes back).
    void ReplayClockOn();
    void ReplayClockAdvance(double ms);
    void ReplayClockOff();

    // Append-only CSV file in the game directory. Rows are flushed periodically so a crash loses little.
    class CsvFile
    {
    public:
        bool Open(const std::string &path, const std::string &header);
        void Close();
        bool IsOpen() const { return file.is_open(); }

        // Writes one row; `fields` must already be comma-separated.
        void WriteRow(const std::string &fields);

    private:
        std::ofstream file;
        int unflushedRows = 0;
        bool everOpened = false;  // reopening appends, so a late frame can't truncate a finished trace
    };

    // Small helper to build comma-separated rows: Row() << a << b << c; produces "a,b,c".
    class Row
    {
    public:
        template <typename T>
        Row &operator<<(const T &value)
        {
            if (!first) stream << ',';
            stream << value;
            first = false;
            return *this;
        }

        std::string str() const { return stream.str(); }

    private:
        std::ostringstream stream;
        bool first = true;
    };
}

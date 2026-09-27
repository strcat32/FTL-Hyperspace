#pragma once

#include <fstream>
#include <sstream>
#include <string>

namespace Duels
{
    // Monotonic wall-clock milliseconds since the first call.
    double WallMs();

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

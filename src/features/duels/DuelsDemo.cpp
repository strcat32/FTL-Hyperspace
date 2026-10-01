#include "Global.h"
#include "Duels.h"
#include "DuelsConfig.h"
#include "DuelsDemo.h"
#include "DuelsFair.h"
#include "DuelsNet.h"
#include "DuelsScript.h"
#include "DuelsTrace.h"
#include "DuelsWire.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <sstream>
#include <vector>
#include <zlib.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

namespace Duels
{
    namespace Demo
    {
        static const char MAGIC[8] = {'F', 'T', 'L', 'D', 'D', 'E', 'M', 'O'};
        static const uint8_t MSG_STATE = 19;   // DuelsMatch.cpp: each ship's state, ten times a second
        static const char *FOLDER = "demos";
        // The records go through the compressor in pieces this large, or this often (a demo stays readable up to
        // its last piece if the game stops).
        static const size_t PIECE_BYTES = 64 * 1024;
        static const double PIECE_MS = 2000.0;

        struct DemoState
        {
            bool configRead = false;
            bool enabled = true;
            FILE *file = nullptr;
            z_stream z;
            std::string path, lastPath;
            bool host = true;
            double startMs = 0.0, lastPieceMs = 0.0;
            std::vector<uint8_t> pending;
            uint64_t records = 0, rawBytes = 0, fileBytes = 0;
            uint32_t sent = 0, received = 0, fullStates = 0;
        };

        static DemoState g;

        static bool Enabled()
        {
            if (!g.configRead)
            {
                g.configRead = true;
                std::string text = Config::Value("record_demos");
                if (text == "off") g.enabled = false;
            }
            return g.enabled;
        }

        bool Recording()
        {
            return g.file != nullptr;
        }

        static void Put32(std::vector<uint8_t> &out, uint32_t value)
        {
            for (int i = 0; i < 4; ++i) out.push_back((uint8_t)(value >> (8 * i)));
        }

        // The pending records through the compressor into the file.
        static void Compress(int flush)
        {
            if (!g.file) return;
            uint8_t out[32768];
            g.z.next_in = g.pending.empty() ? nullptr : g.pending.data();
            g.z.avail_in = (uInt)g.pending.size();
            int result = Z_OK;
            do
            {
                g.z.next_out = out;
                g.z.avail_out = sizeof(out);
                result = deflate(&g.z, flush);
                size_t have = sizeof(out) - g.z.avail_out;
                if (have > 0)
                {
                    fwrite(out, 1, have, g.file);
                    g.fileBytes += have;
                }
            } while (g.z.avail_out == 0 || (flush == Z_FINISH && result == Z_OK));
            g.pending.clear();
            fflush(g.file);
            g.lastPieceMs = WallMs();
        }

        static void Record(uint8_t from, uint8_t kind, uint8_t type, const uint8_t *data, size_t size)
        {
            if (!g.file) return;
            uint32_t ms = (uint32_t)std::max(0.0, WallMs() - g.startMs);
            Put32(g.pending, ms);
            g.pending.push_back(from);
            g.pending.push_back(kind);
            g.pending.push_back(type);
            Put32(g.pending, (uint32_t)size);
            if (size > 0) g.pending.insert(g.pending.end(), data, data + size);
            ++g.records;
            g.rawBytes += 11 + size;
            if (g.pending.size() >= PIECE_BYTES || WallMs() - g.lastPieceMs >= PIECE_MS) Compress(Z_SYNC_FLUSH);
        }

        // A name for a file: letters, digits, - and _ (24 at most).
        static std::string FileWord(const std::string &name)
        {
            std::string word;
            for (char c : name)
            {
                if (std::isalnum((unsigned char)c) || c == '-' || c == '_') word += c;
                else if (c == ' ' && !word.empty() && word.back() != '_') word += '_';
                if (word.size() >= 24) break;
            }
            return word.empty() ? std::string("player") : word;
        }

        void Begin(bool host, const std::string &hostName, const std::string &guestName)
        {
            if (g.file) End("a new match");
            if (!Enabled()) return;
#ifdef _WIN32
            _mkdir(FOLDER);
#else
            mkdir(FOLDER, 0755);
#endif
            std::time_t now = std::time(nullptr);
            std::tm local = *std::localtime(&now);
            char stamp[32];
            std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
            std::string base = std::string(FOLDER) + "/" + stamp + "-" + FileWord(hostName) + "-vs-" + FileWord(guestName);
            std::string path = base + ".ftldemo";
            // Never over another file (two matches in one second).
            for (int i = 2; i < 100; ++i)
            {
                FILE *existing = std::fopen(path.c_str(), "rb");
                if (!existing) break;
                std::fclose(existing);
                path = base + "-" + std::to_string(i) + ".ftldemo";
            }
            FILE *file = std::fopen(path.c_str(), "wb");
            if (!file)
            {
                Log("Demo: can't write %s: not recorded", path.c_str());
                return;
            }
            g.z = z_stream();
            if (deflateInit(&g.z, Z_DEFAULT_COMPRESSION) != Z_OK)
            {
                std::fclose(file);
                Log("Demo: no compressor: not recorded");
                return;
            }
            g.file = file;
            g.path = path;
            g.host = host;
            g.startMs = WallMs();
            g.lastPieceMs = g.startMs;
            g.pending.clear();
            g.records = g.rawBytes = g.fileBytes = 0;
            g.sent = g.received = g.fullStates = 0;
            std::fwrite(MAGIC, 1, sizeof(MAGIC), file);
            uint8_t head[3] = {(uint8_t)(FORMAT & 0xff), (uint8_t)(FORMAT >> 8), COMPRESSION_DEFLATE};
            std::fwrite(head, 1, sizeof(head), file);
            g.fileBytes = sizeof(MAGIC) + sizeof(head);

            Writer w;
            w.U16(Net::PROTOCOL_VERSION);
            w.Str(Net::OwnVersion());
            w.Str(Net::OwnBuild());
            w.Str(Fair::GameDataHash());
            w.U8(host ? FROM_HOST : FROM_GUEST);
            w.Str(hostName);
            w.Str(guestName);
            w.U32((uint32_t)now);
            w.Bool(true);   // full states of the recorder's ship
            Record(host ? FROM_HOST : FROM_GUEST, KIND_MARKER, MARK_HEADER, w.data.data(), w.data.size());
            Log("Demo: recording %s", path.c_str());
        }

        void End(const std::string &why)
        {
            if (!g.file) return;
            Writer w;
            w.Str(why);
            Record(g.host ? FROM_HOST : FROM_GUEST, KIND_MARKER, MARK_END, w.data.data(), w.data.size());
            Compress(Z_FINISH);
            deflateEnd(&g.z);
            std::fclose(g.file);
            g.file = nullptr;
            g.lastPath = g.path;
            Log("Demo: saved %s (%s; %llu records, %u sent, %u received, %u full states; %.0f kB, %.0f kB in the file, %.0f s)",
                g.path.c_str(), why.c_str(), (unsigned long long)g.records, g.sent, g.received, g.fullStates, g.rawBytes / 1024.0,
                g.fileBytes / 1024.0, (WallMs() - g.startMs) / 1000.0);
        }

        void Sent(uint8_t type, const uint8_t *data, size_t size)
        {
            // Our states go as full states (FullState): the one that went shows less.
            if (!g.file || type == MSG_STATE) return;
            ++g.sent;
            Record(g.host ? FROM_HOST : FROM_GUEST, KIND_MESSAGE, type, data, size);
        }

        void Received(uint8_t type, const uint8_t *data, size_t size)
        {
            if (!g.file) return;
            ++g.received;
            Record(g.host ? FROM_GUEST : FROM_HOST, KIND_MESSAGE, type, data, size);
        }

        void FullState(const Writer &w)
        {
            if (!g.file) return;
            ++g.fullStates;
            Record(g.host ? FROM_HOST : FROM_GUEST, KIND_FULL_STATE, MSG_STATE, w.data.data(), w.data.size());
        }

        std::string Status()
        {
            std::ostringstream out;
            out << "demo: " << (Enabled() ? "on" : "off");
            if (g.file)
            {
                out << ", recording " << g.path << " (" << g.records << " records, " << (g.rawBytes / 1024) << " kB, "
                    << (g.fileBytes / 1024) << " kB in the file)";
            }
            else if (!g.lastPath.empty())
            {
                out << ", last " << g.lastPath;
            }
            return out.str();
        }

        bool RunVerb(const Command &cmd, std::string &message)
        {
            Enabled();
            if (ArgIs(cmd, 1, "on") || ArgIs(cmd, 1, "off"))
            {
                g.enabled = ArgIs(cmd, 1, "on");
                message = std::string("demos ") + (g.enabled ? "on: the next matches are recorded" : "off: the next matches aren't recorded");
                return true;
            }
            if (ArgIs(cmd, 1, "stop"))
            {
                if (!g.file)
                {
                    message = "no demo is being recorded";
                    return false;
                }
                End("stopped");
                message = "demo saved: " + g.lastPath;
                return true;
            }
            if (cmd.args.size() > 1)
            {
                message = "usage: demo [on|off|stop]";
                return false;
            }
            message = Status();
            return true;
        }
    }
}

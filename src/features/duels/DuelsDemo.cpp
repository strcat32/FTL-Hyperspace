#include "Global.h"
#include "Duels.h"
#include "DuelsBoarding.h"
#include "DuelsAccount.h"
#include "DuelsConfig.h"
#include "DuelsConsole.h"
#include "DuelsCrew.h"
#include "DuelsDemo.h"
#include "DuelsDrones.h"
#include "DuelsFair.h"
#include "DuelsMatch.h"
#include "DuelsNet.h"
#include "DuelsRounds.h"
#include "DuelsScript.h"
#include "DuelsTrace.h"
#include "DuelsWire.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>
#include <unordered_map>
#include <vector>
#include <boost/filesystem.hpp>
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
            int statusRanked = -1;          // the match's status as last recorded (BB)
            std::string statusWhy;
            std::string ticketNonce;        // a ranked room's match: its ticket's nonce (the demo goes to the master, BQ)
        };

        static DemoState g;

        // The swap after a match (BA, part 2).
        struct SwapState
        {
            std::vector<uint8_t> own;       // our full states this match: each one's length (u16) and bytes
            uint32_t ownCount = 0;
            bool started = false;           // the match is over: ours are going (or gone)
            std::vector<uint8_t> out;       // ours, deflated
            size_t outSent = 0, pieces = 0;
            bool lastSent = false;
            double nextPieceMs = 0.0;
            bool theirsHaveOurs = false;    // the other game said it has all of ours (MSG_DEMO_SAVED)
            std::vector<uint8_t> in;        // theirs, as the pieces come
            size_t piecesIn = 0;
            bool inDone = false;
            bool hold = false;              // tests: our pieces wait (demo hold on)
        };

        static SwapState g_swap;

        struct DemoRecord
        {
            uint32_t ms = 0;
            uint8_t from = 0, kind = 0, type = 0;
            std::vector<uint8_t> data;
        };

        struct ReplayState
        {
            bool active = false;
            std::string path, hostName, guestName;
            uint8_t recorder = FROM_HOST;
            std::vector<DemoRecord> records;
            std::string partnerPath;            // the other player's demo of the match, its full states joined (BA)
            bool hasFull[2] = {false, false};   // full states of the host's ship, of the guest's
            bool hasSeen[2] = {false, false};   // each one's states as they went to the other (sent, or received)
            uint8_t viewed = FROM_HOST;         // whose side the screen shows: our ship is theirs (BA)
            bool fullSensors = false;           // the other ship by its full states, all in sight (BA)
            size_t next = 0;
            double startMs = 0.0;            // the demo's start, on the replay's clock
            double firstMs = 0.0;            // its first state of our ship (ms in the demo): no seek goes further back
            bool paused = false;
            double speed = 1.0;              // 0.5, 1, 2, 4 or 8
            double seekTo = -1.0;            // running ahead to this time (ms in the demo); -1: not seeking
            bool pauseAfterSeek = false;
            bool ended = false;              // at its end: it stays on its last moment, paused
            double lastRealMs = -1.0;        // real time at the last frame (a recorded timeout passes in it, BF)
            int ranked = -1;                 // the recorded match's status (BB): 1, 0, -1 not known
            std::string unrankedWhy;
            uint32_t delivered = 0, ownLoadouts = 0, ownStates = 0, held = 0;
            // A long seek's cover (roadmap BO): from where, what it says; when this frame's steps began (real time).
            bool covering = false;
            double coverFrom = 0.0;
            std::string coverText;
            double stepsStartReal = 0.0;
        };

        static ReplayState g_replay;
        static bool g_restarting = false;   // StartReplay for a seek back: the feed stays quiet

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

        // A record at a time of the demo's (ms; the swap's are from the past: a demo's reader sorts them in).
        static void RecordAt(uint32_t ms, uint8_t from, uint8_t kind, uint8_t type, const uint8_t *data, size_t size)
        {
            if (!g.file) return;
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

        static void Record(uint8_t from, uint8_t kind, uint8_t type, const uint8_t *data, size_t size)
        {
            RecordAt((uint32_t)std::max(0.0, WallMs() - g.startMs), from, kind, type, data, size);
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
            g.ticketNonce.clear();
            g_swap = SwapState();   // a new connection, a new swap (BA)
            if (!Enabled() || Net::Replaying()) return;
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
            g.statusRanked = -1;
            g.statusWhy.clear();
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
            // A ranked room's demo goes to the master (roadmap BQ: signed-in players download it from the match's page):
            // it waits as <demo>.upload, its ticket's nonce in it, until the master has it (Account::UploadDemos; a game
            // closed before that sends it at its next start).
            if (g.ticketNonce.size() == 16)
            {
                static const char *const DIGITS = "0123456789abcdef";
                std::string hex;
                for (unsigned char c : g.ticketNonce)
                {
                    hex += DIGITS[c >> 4];
                    hex += DIGITS[c & 15];
                }
                FILE *marker = std::fopen((g.path + ".upload").c_str(), "w");
                if (marker)
                {
                    std::fprintf(marker, "%s\n", hex.c_str());
                    std::fclose(marker);
                }
                g.ticketNonce.clear();
                Account::UploadDemos();
            }
        }

        void SetTicket(const std::string &nonce)
        {
            if (g.file && nonce.size() == 16) g.ticketNonce = nonce;
        }

        void NoteStatus(bool ranked, const std::string &why)
        {
            if (!g.file || (g.statusRanked == (ranked ? 1 : 0) && g.statusWhy == why)) return;
            g.statusRanked = ranked ? 1 : 0;
            g.statusWhy = why;
            Writer w;
            w.Bool(ranked);
            w.Str(why);
            Record(g.host ? FROM_HOST : FROM_GUEST, KIND_MARKER, MARK_STATUS, w.data.data(), w.data.size());
            Log("Demo: the match is %s%s", ranked ? "ranked" : "unranked", why.empty() ? "" : (" (" + why + ")").c_str());
        }

        void Sent(uint8_t type, const uint8_t *data, size_t size)
        {
            // Our states too, as they went (what the opponent saw of our ship: their view in a replay, BA); in full they
            // come with FullState. The swap's own messages aren't the match's.
            if (!g.file || type == MSG_DEMO_STATES || type == MSG_DEMO_SAVED) return;
            ++g.sent;
            Record(g.host ? FROM_HOST : FROM_GUEST, KIND_MESSAGE, type, data, size);
        }

        void Received(uint8_t type, const uint8_t *data, size_t size)
        {
            if (!g.file || type == MSG_DEMO_STATES || type == MSG_DEMO_SAVED) return;
            ++g.received;
            Record(g.host ? FROM_GUEST : FROM_HOST, KIND_MESSAGE, type, data, size);
        }

        void FullState(const Writer &w)
        {
            // Kept for the swap after the match (BA), recorded or not: the other game may record.
            if (!g_swap.started && w.data.size() <= 0xffff)
            {
                g_swap.own.push_back((uint8_t)(w.data.size() & 0xff));
                g_swap.own.push_back((uint8_t)(w.data.size() >> 8));
                g_swap.own.insert(g_swap.own.end(), w.data.begin(), w.data.end());
                ++g_swap.ownCount;
            }
            if (!g.file) return;
            ++g.fullStates;
            Record(g.host ? FROM_HOST : FROM_GUEST, KIND_FULL_STATE, MSG_STATE, w.data.data(), w.data.size());
        }

        // ---------------------------------------------------------------------------------------------------------
        // The swap after a match (roadmap BA, part 2)
        // ---------------------------------------------------------------------------------------------------------

        static const size_t SWAP_PIECE = 1000;                 // deflated bytes a message (a packet holds 1158)
        static const double SWAP_PIECE_MS = 1000.0 / 60.0;     // a piece a frame at most (a relay takes 300 packets a second)
        static const size_t SWAP_PENDING = 24;                 // no new piece while this many reliable messages wait

        void StartSwap()
        {
            if (g_swap.started || !Net::IsConnected() || Net::Replaying()) return;
            g_swap.started = true;
            uLongf size = compressBound((uLong)g_swap.own.size());
            g_swap.out.resize(size);
            if (g_swap.own.empty() ||
                compress2(g_swap.out.data(), &size, g_swap.own.data(), (uLong)g_swap.own.size(), Z_DEFAULT_COMPRESSION) != Z_OK)
                size = 0;
            g_swap.out.resize(size);
            Log("Demo: the match is over: our %u full states go to the other game (%.0f kB, %.0f kB deflated)", g_swap.ownCount,
                g_swap.own.size() / 1024.0, size / 1024.0);
        }

        void SwapFrame(double now)
        {
            if (!g_swap.started || g_swap.lastSent || g_swap.hold || !Net::IsConnected()) return;
            if (now < g_swap.nextPieceMs || Net::PendingReliable() > SWAP_PENDING) return;
            const size_t n = std::min(SWAP_PIECE, g_swap.out.size() - g_swap.outSent);
            Writer w;
            w.U32((uint32_t)g_swap.out.size());
            w.U32((uint32_t)g_swap.own.size());
            w.U32((uint32_t)g_swap.outSent);
            w.F64(g.file ? g.startMs : -1.0);   // our demo's start (the other's demo knows how far apart the two began)
            if (n > 0) w.Bytes(g_swap.out.data() + g_swap.outSent, n);
            if (!Net::Send(MSG_DEMO_STATES, w, true)) return;
            g_swap.outSent += n;
            ++g_swap.pieces;
            g_swap.nextPieceMs = now + SWAP_PIECE_MS;
            if (g_swap.outSent >= g_swap.out.size())
            {
                g_swap.lastSent = true;
                Log("Demo: our full states went (%u pieces)", (unsigned)g_swap.pieces);
            }
        }

        void OnSwapMessage(uint8_t type, Reader &r)
        {
            if (type == MSG_DEMO_SAVED)
            {
                g_swap.theirsHaveOurs = true;
                Log("Demo: the other game has our full states");
                return;
            }
            if (type != MSG_DEMO_STATES || g_swap.inDone) return;
            const uint32_t total = r.U32(), raw = r.U32(), offset = r.U32();
            const double theirStart = r.F64();
            const size_t n = r.Remaining();
            if (!r.Ok() || offset != g_swap.in.size() || (size_t)offset + n > total || raw > 64u * 1024u * 1024u)
            {
                Log("Demo: a piece of the other game's full states doesn't fit (%u bytes at %u of %u)", (unsigned)n, offset, total);
                return;
            }
            if (n > 0) g_swap.in.insert(g_swap.in.end(), r.Position(), r.Position() + n);
            ++g_swap.piecesIn;
            if (g_swap.in.size() < total) return;
            g_swap.inDone = true;
            // All here: theirs into our demo, each at the time it went, on our clock.
            std::vector<uint8_t> states(raw);
            uLongf size = raw;
            const bool unpacked = total == 0 || uncompress(states.data(), &size, g_swap.in.data(), total) == Z_OK;
            uint32_t count = 0;
            double apart = theirStart >= 0.0 && g.file ? Net::PeerToLocalTime(theirStart) - g.startMs : 0.0;
            if (unpacked && total > 0 && g.file)
            {
                const uint8_t side = g.host ? FROM_GUEST : FROM_HOST;
                for (size_t pos = 0; pos + 2 <= size;)
                {
                    const size_t length = (size_t)states[pos] | ((size_t)states[pos + 1] << 8);
                    pos += 2;
                    if (pos + length > size) break;
                    if (length >= sizeof(double))
                    {
                        // (One from before our demo's start goes in at its start: their first state can go before
                        // our demo begins.)
                        double sentAt;
                        std::memcpy(&sentAt, &states[pos], sizeof(sentAt));
                        const double ms = std::max(0.0, Net::PeerToLocalTime(sentAt) - g.startMs);
                        RecordAt((uint32_t)ms, side, KIND_FULL_STATE, MSG_STATE, &states[pos], length);
                        ++count;
                    }
                    pos += length;
                }
                Writer marker;
                marker.F64(apart);
                marker.U32(count);
                Record(g.host ? FROM_HOST : FROM_GUEST, KIND_MARKER, MARK_SWAP, marker.data.data(), marker.data.size());
            }
            Writer w;
            Net::Send(MSG_DEMO_SAVED, w, true);
            Log("Demo: the other game's full states came (%u pieces, %.0f kB): %u into our demo (their demo began %+.0f ms after ours)%s",
                (unsigned)g_swap.piecesIn, total / 1024.0, count, apart, unpacked ? "" : "; they don't unpack");
        }

        bool SwapBusy()
        {
            return g_swap.started && Net::IsConnected() && !Net::Replaying() && (!g_swap.theirsHaveOurs || !g_swap.inDone);
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

        // ---------------------------------------------------------------------------------------------------------
        // Replay
        // ---------------------------------------------------------------------------------------------------------

        static uint32_t Get32(const std::vector<uint8_t> &data, size_t pos)
        {
            return (uint32_t)data[pos] | ((uint32_t)data[pos + 1] << 8) | ((uint32_t)data[pos + 2] << 16) | ((uint32_t)data[pos + 3] << 24);
        }

        // A demo file's records (any version of the game's), the first the header; with a limit, those in its first bytes
        // only (its header: 4096).
        static bool ReadRecords(const std::string &path, std::vector<DemoRecord> &records, std::string &message,
                                size_t limit = (size_t)-1)
        {
            FILE *file = std::fopen(path.c_str(), "rb");
            if (!file)
            {
                message = "no demo " + path;
                return false;
            }
            std::vector<uint8_t> raw;
            uint8_t buffer[65536];
            size_t got;
            while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) raw.insert(raw.end(), buffer, buffer + got);
            std::fclose(file);
            if (raw.size() < 11 || !std::equal(MAGIC, MAGIC + 8, raw.begin()))
            {
                message = path + " is no demo";
                return false;
            }
            uint16_t format = (uint16_t)(raw[8] | (raw[9] << 8));
            uint8_t compression = raw[10];
            if (format != FORMAT || (compression != COMPRESSION_DEFLATE && compression != 0))
            {
                message = "the demo's format (" + std::to_string(format) + ", compression " + std::to_string(compression) + ") isn't this game's";
                return false;
            }
            std::vector<uint8_t> data;
            if (compression == 0)
            {
                data.assign(raw.begin() + 11, raw.end());
            }
            else
            {
                z_stream z = z_stream();
                if (inflateInit(&z) != Z_OK)
                {
                    message = "no decompressor";
                    return false;
                }
                z.next_in = raw.data() + 11;
                z.avail_in = (uInt)(raw.size() - 11);
                int result = Z_OK;
                while (result == Z_OK)
                {
                    z.next_out = buffer;
                    z.avail_out = sizeof(buffer);
                    result = inflate(&z, Z_NO_FLUSH);
                    data.insert(data.end(), buffer, buffer + (sizeof(buffer) - z.avail_out));
                    if (z.avail_in == 0 && z.avail_out != 0) break;   // a demo cut short: what is there
                    if (data.size() >= limit) break;
                }
                inflateEnd(&z);
            }
            records.clear();
            size_t pos = 0;
            while (pos + 11 <= data.size())
            {
                DemoRecord record;
                record.ms = Get32(data, pos);
                record.from = data[pos + 4];
                record.kind = data[pos + 5];
                record.type = data[pos + 6];
                uint32_t size = Get32(data, pos + 7);
                pos += 11;
                if (pos + size > data.size()) break;
                record.data.assign(data.begin() + pos, data.begin() + pos + size);
                pos += size;
                records.push_back(std::move(record));
            }
            if (records.empty() || records[0].kind != KIND_MARKER || records[0].type != MARK_HEADER)
            {
                message = "the demo has no header";
                return false;
            }
            // In time order: the other player's full states come at the end, after the match (the swap, BA); records of
            // one time keep their order.
            std::stable_sort(records.begin() + 1, records.end(), [](const DemoRecord &a, const DemoRecord &b) { return a.ms < b.ms; });
            return true;
        }

        // A demo's header: who recorded it, the players, when.
        struct Header
        {
            uint16_t protocol = 0;
            std::string version;
            uint8_t recorder = FROM_HOST;
            std::string hostName, guestName;
            uint32_t startUtc = 0;
        };

        static bool ReadHeader(const DemoRecord &record, Header &h)
        {
            Reader r(record.data);
            h.protocol = r.U16();
            h.version = r.Str();
            r.Str();   // the build
            r.Str();   // the game data's hash
            h.recorder = r.U8();
            h.hostName = r.Str();
            h.guestName = r.Str();
            h.startUtc = r.U32();
            return r.Ok();
        }

        static bool ReadDemo(const std::string &path, ReplayState &out, std::string &message)
        {
            if (!ReadRecords(path, out.records, message)) return false;
            Header h;
            if (!ReadHeader(out.records[0], h))
            {
                message = "the demo's header is broken";
                return false;
            }
            if (h.protocol != Net::PROTOCOL_VERSION)
            {
                message = "the demo is from protocol " + std::to_string(h.protocol) + " (" + h.version + "), this game plays " +
                          std::to_string(Net::PROTOCOL_VERSION);
                return false;
            }
            out.recorder = h.recorder;
            out.hostName = h.hostName;
            out.guestName = h.guestName;
            out.path = path;
            return true;
        }

        // A folder: its newest demo (their names begin with the date and time).
        static std::string NewestDemo(const std::string &path)
        {
            namespace fs = boost::filesystem;
            boost::system::error_code error;
            if (!fs::is_directory(path, error)) return path;
            std::string newest;
            for (fs::directory_iterator it(path, error), end; !error && it != end; it.increment(error))
            {
                std::string name = it->path().filename().string();
                if (name.size() > 8 && name.compare(name.size() - 8, 8, ".ftldemo") == 0 && name > newest) newest = name;
            }
            return newest.empty() ? path : (fs::path(path) / newest).string();
        }

        static const uint8_t LIST_MSG_LOADOUT = 17;   // DuelsMatch.cpp: a ship's loadout, its blueprint first

        // What the browser shows of a demo: its header, the first loadout from each side, its last status, its length.
        static void ReadInfo(DemoInfo &info)
        {
            std::vector<DemoRecord> records;
            if (!ReadRecords(info.path, records, info.problem)) return;
            Header h;
            if (!ReadHeader(records[0], h))
            {
                info.problem = "the demo's header is broken";
                return;
            }
            info.hostName = h.hostName;
            info.guestName = h.guestName;
            info.startUtc = h.startUtc;
            if (h.protocol != Net::PROTOCOL_VERSION) info.problem = "from version " + h.version + ": this game plays only its own version's demos";
            for (const DemoRecord &record : records)
            {
                if (record.kind == KIND_MESSAGE && record.type == LIST_MSG_LOADOUT)
                {
                    std::string &ship = record.from == FROM_HOST ? info.hostShip : info.guestShip;
                    if (!ship.empty()) continue;
                    Reader r(record.data);
                    std::string blueprint = r.Str();
                    if (r.Ok()) ship = blueprint;
                }
                else if (record.kind == KIND_MARKER && record.type == MARK_STATUS && !record.data.empty())
                {
                    info.ranked = record.data[0] != 0 ? 1 : 0;
                }
            }
            info.lengthMs = records.back().ms;
        }

        std::vector<DemoInfo> ListDemos()
        {
            namespace fs = boost::filesystem;
            std::vector<DemoInfo> list;
            boost::system::error_code error;
            for (fs::directory_iterator it(FOLDER, error), end; !error && it != end; it.increment(error))
            {
                std::string name = it->path().filename().string();
                if (name.size() <= 8 || name.compare(name.size() - 8, 8, ".ftldemo") != 0) continue;
                DemoInfo info;
                info.file = name;
                info.path = (fs::path(FOLDER) / name).string();
                ReadInfo(info);
                list.push_back(info);
            }
            return list;
        }

        bool RecordingOn()
        {
            return Enabled();
        }

        void SetRecordingOn(bool on)
        {
            Enabled();
            g.enabled = on;
            if (SettingsFromConfig()) Config::SaveValue("record_demos", on ? "on" : "off");
            Log("Demo: %s", on ? "the next matches are recorded" : "the next matches aren't recorded");
        }

        static uint8_t Other(uint8_t side)
        {
            return side == FROM_HOST ? FROM_GUEST : FROM_HOST;
        }

        static const char *SideName(uint8_t side)
        {
            return side == FROM_HOST ? "host" : "guest";
        }

        // The other player's demo of the same match (roadmap BA): its full states join the replay's, so that it can show
        // that player's side too. It is the same match when its full states are the states the replay's demo got from
        // that player (each one's time of sending and number, its first 10 bytes, the same). Its times become the
        // replay's by the smallest difference between a state's writing there and its coming here (the demos' starts
        // apart, and the fastest delivery).
        static bool JoinPartner(ReplayState &replay, const std::string &path, std::string &why)
        {
            std::vector<DemoRecord> records;
            Header h;
            if (!ReadRecords(path, records, why)) return false;
            if (!ReadHeader(records[0], h) || h.protocol != Net::PROTOCOL_VERSION)
            {
                why = "not a demo of this version";
                return false;
            }
            if (h.recorder == replay.recorder || h.hostName != replay.hostName || h.guestName != replay.guestName)
            {
                why = "not the other player's demo of this match";
                return false;
            }
            const uint8_t side = h.recorder;
            std::unordered_map<std::string, uint32_t> received;
            for (const DemoRecord &record : replay.records)
            {
                if (record.kind == KIND_MESSAGE && record.type == MSG_STATE && record.from == side && record.data.size() >= 10)
                    received.emplace(std::string((const char *)record.data.data(), 10), record.ms);
            }
            int64_t offset = 0;
            size_t full = 0, matched = 0;
            for (const DemoRecord &record : records)
            {
                if (record.kind != KIND_FULL_STATE || record.from != side || record.data.size() < 10) continue;
                ++full;
                auto found = received.find(std::string((const char *)record.data.data(), 10));
                if (found == received.end()) continue;
                int64_t difference = (int64_t)found->second - (int64_t)record.ms;
                offset = matched == 0 ? difference : std::min(offset, difference);
                ++matched;
            }
            if (matched < 10 || matched * 2 < std::min(received.size(), full))
            {
                why = "not the same match (" + std::to_string(matched) + " of its " + std::to_string(full) + " states came to this one)";
                return false;
            }
            const int64_t end = replay.records.back().ms;
            size_t joined = 0;
            for (DemoRecord &record : records)
            {
                if (record.kind != KIND_FULL_STATE || record.from != side) continue;
                // One from before this demo's start goes in at its start (a full state stands for itself).
                int64_t ms = (int64_t)record.ms + offset;
                if (ms >= end) continue;
                record.ms = (uint32_t)std::max<int64_t>(0, ms);
                replay.records.push_back(std::move(record));
                ++joined;
            }
            // In time order: the header stays first; records of one time keep their order (the demo's own first).
            std::stable_sort(replay.records.begin() + 1, replay.records.end(),
                             [](const DemoRecord &a, const DemoRecord &b) { return a.ms < b.ms; });
            replay.partnerPath = path;
            Log("Demo: joined %s: %u full states of the %s's ship (%u of %u matched, %+d ms)", path.c_str(), (unsigned)joined,
                SideName(side), (unsigned)matched, (unsigned)full, (int)offset);
            return true;
        }

        // The other player's demo of the match in the same folder, if one is there: the players' names in its file name,
        // recorded by the other one, begun within a quarter of an hour (their clocks may be apart), then the same match
        // by its states (JoinPartner).
        static void FindPartner(ReplayState &replay)
        {
            namespace fs = boost::filesystem;
            boost::system::error_code error;
            const fs::path own(replay.path);
            fs::path folder = own.parent_path();
            if (folder.empty()) folder = ".";
            Header ours;
            if (!ReadHeader(replay.records[0], ours)) return;
            const std::string players = "-" + FileWord(replay.hostName) + "-vs-" + FileWord(replay.guestName);
            for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
            {
                const std::string name = it->path().filename().string();
                if (name.size() <= 8 || name.compare(name.size() - 8, 8, ".ftldemo") != 0) continue;
                if (name == own.filename().string() || name.find(players) == std::string::npos) continue;
                std::vector<DemoRecord> head;
                std::string why;
                Header h;
                if (!ReadRecords(it->path().string(), head, why, 4096) || !ReadHeader(head[0], h)) continue;
                if (h.recorder == replay.recorder || h.hostName != replay.hostName || h.guestName != replay.guestName) continue;
                if (std::llabs((long long)h.startUtc - (long long)ours.startUtc) > 900) continue;
                if (JoinPartner(replay, it->path().string(), why)) return;
                Log("Demo: %s isn't the other demo of this match (%s)", name.c_str(), why.c_str());
            }
        }

        // Which players' full states and states as they went the demo has.
        static void ScanSides(ReplayState &out)
        {
            for (const DemoRecord &record : out.records)
            {
                if (record.from > FROM_GUEST) continue;
                if (record.kind == KIND_FULL_STATE) out.hasFull[record.from] = true;
                else if (record.kind == KIND_MESSAGE && record.type == MSG_STATE) out.hasSeen[record.from] = true;
            }
        }

        // A demo for a replay, and the other player's demo of the match if there is one (`partner`: that one), unless the
        // demo has the other player's full states itself (the swap after the match, BA part 2).
        static bool Load(const std::string &path, const std::string &partner, ReplayState &out, std::string &message)
        {
            if (!ReadDemo(NewestDemo(path), out, message)) return false;
            ScanSides(out);
            const uint8_t other = Other(out.recorder);
            if (out.hasFull[other])
            {
                for (const DemoRecord &record : out.records)
                {
                    if (record.kind != KIND_MARKER || record.type != MARK_SWAP) continue;
                    Reader r(record.data);
                    const double apart = r.F64();
                    const uint32_t count = r.U32();
                    if (r.Ok())
                        Log("Demo: swapped: %u full states of the %s's ship came after the match (their demo began %+d ms after this one)",
                            count, SideName(other), (int)apart);
                }
                if (!partner.empty()) Log("Demo: %s isn't joined: the demo has both players' full states", partner.c_str());
                return true;
            }
            if (!partner.empty())
            {
                std::string why, partnerPath = NewestDemo(partner);
                if (!JoinPartner(out, partnerPath, why))
                {
                    message = partnerPath + " doesn't join: " + why;
                    return false;
                }
            }
            else
            {
                FindPartner(out);
            }
            ScanSides(out);
            return true;
        }

        static const uint8_t MSG_CHAT = 16, MSG_LOADOUT = 17, MSG_READY = 18, MSG_SHOT = 20, MSG_RESULT = 21, MSG_SHOT_DOWNED = 23,
                             MSG_DRONE_SHOT = 25, MSG_CREW_ROSTER = 26, MSG_SETTINGS = 27, MSG_BOARD = 32, MSG_RECALL = 33,
                             MSG_MATCH = 38, MSG_MATCH_EVENT = 39;

        // A loaded demo from its start, from one player's side (BA; the recorder's when the other's full states aren't
        // there). Each start says so in the log ("Demo: replaying": tools/compare-replay.py counts from the last).
        static void Run(ReplayState &&replay, uint8_t viewed, bool fullSensors)
        {
            if (viewed > FROM_GUEST || !replay.hasFull[viewed]) viewed = replay.recorder;
            const uint8_t other = Other(viewed);
            const std::string opponent = other == FROM_HOST ? replay.hostName : replay.guestName;
            Log("Demo: replaying %s (%u records, %.0f s; recorded by the %s, %s vs %s; the %s's side%s%s)", replay.path.c_str(),
                (unsigned)replay.records.size(), replay.records.back().ms / 1000.0, SideName(replay.recorder), replay.hostName.c_str(),
                replay.guestName.c_str(), SideName(viewed), fullSensors && replay.hasFull[other] ? ", full sensors" : "",
                replay.partnerPath.empty() ? "" : ", with the other's demo");
            // A replay that runs ends first (another one, or this one going back to its start).
            Net::BeginReplay(opponent);
            g_replay = std::move(replay);
            g_replay.active = true;
            g_replay.viewed = viewed;
            g_replay.fullSensors = fullSensors && g_replay.hasFull[other];
            // Its clock: FTL's world time from now on (DuelsTrace.h); the demo starts now.
            ReplayClockOn();
            g_replay.startMs = WallMs();
            // The recorded clocks against ours, from each player's first state: a recorder's own states were written as
            // they went, the other's came with their latency (as the recorder saw them), joined full states with the
            // fastest delivery's. The opponent's is the other player's; the match's times are the host's.
            double clock[2] = {0.0, 0.0};
            bool have[2] = {false, false};
            for (const DemoRecord &record : g_replay.records)
            {
                bool state = record.kind == KIND_FULL_STATE || (record.kind == KIND_MESSAGE && record.type == MSG_STATE);
                if (!state || record.from > FROM_GUEST || have[record.from] || record.data.size() < sizeof(double)) continue;
                double sentAt;
                std::memcpy(&sentAt, record.data.data(), sizeof(sentAt));
                clock[record.from] = sentAt - (g_replay.startMs + record.ms);
                have[record.from] = true;
                if (have[FROM_HOST] && have[FROM_GUEST]) break;
            }
            Net::SetReplayClock(clock[other]);
            Net::SetReplayHostClock(clock[FROM_HOST]);
            // Its start for the controls: before the shown player's first state the ships aren't fitted yet.
            for (const DemoRecord &record : g_replay.records)
            {
                if (record.kind != KIND_FULL_STATE || record.from != viewed) continue;
                g_replay.firstMs = record.ms;
                break;
            }
            // Our ship becomes the shown player's at once, from their first loadout, before the other ship is there
            // (roadmap BO): FTL's ship switch clears the location, and at the ships' meeting it took away the other ship
            // when that came first (from the guest's side: the enemy was gone after a view switch). Their ship is the
            // match's; the later loadouts fit it.
            for (const DemoRecord &record : g_replay.records)
            {
                if (record.kind != KIND_MESSAGE || record.type != MSG_LOADOUT || record.from != viewed) continue;
                Match::ReplayOwnLoadout(record.data.data(), record.data.size());
                break;
            }
        }

        // view: FROM_HOST or FROM_GUEST, or -1 for the recorder's side.
        static bool Start(const std::string &path, const std::string &partner, int view, std::string &message)
        {
            if (g.file) End("a replay begins");
            ReplayState replay;
            if (!Load(path, partner, replay, message)) return false;
            const uint8_t viewed = view == FROM_HOST || view == FROM_GUEST ? (uint8_t)view : replay.recorder;
            Run(std::move(replay), viewed, false);
            const std::string opponent = Other(g_replay.viewed) == FROM_HOST ? g_replay.hostName : g_replay.guestName;
            message = "replaying " + g_replay.path + " (" + std::to_string(g_replay.records.size()) + " records, " +
                      std::to_string((int)(g_replay.records.back().ms / 1000)) + " s), the " + SideName(g_replay.viewed) + "'s side, " +
                      opponent + " as the opponent" + (g_replay.partnerPath.empty() ? "" : ", with the other's demo");
            return true;
        }

        void StopReplay(const std::string &why)
        {
            if (!g_replay.active && !Net::Replaying()) return;
            g_replay.active = false;
            g_replay.covering = false;
            Net::EndReplay(why);
        }

        bool StartReplay(const std::string &path, std::string &message)
        {
            return Start(path, "", -1, message);
        }

        // The replay from its start again as it was loaded (a seek back, the other side): nothing is read again, and the
        // feed stays quiet (ReplayRestarting); its lines are from later in the demo, its own come again as it runs.
        static void Restart(uint8_t viewed, bool fullSensors)
        {
            ReplayState replay;
            replay.path = g_replay.path;
            replay.hostName = g_replay.hostName;
            replay.guestName = g_replay.guestName;
            replay.recorder = g_replay.recorder;
            replay.partnerPath = g_replay.partnerPath;
            for (int side = 0; side < 2; ++side)
            {
                replay.hasFull[side] = g_replay.hasFull[side];
                replay.hasSeen[side] = g_replay.hasSeen[side];
            }
            replay.records = std::move(g_replay.records);
            const double speed = g_replay.speed;
            g_replay.active = false;
            g_replay.records.clear();
            g_restarting = true;
            Run(std::move(replay), viewed, fullSensors);
            g_restarting = false;
            g_replay.speed = speed;
            Console::ClearFeed();
        }

        // The other ship follows the other player's full states (full sensors, or a demo without their states as they
        // went) or their states as they went.
        static bool OtherInFull()
        {
            return g_replay.fullSensors || !g_replay.hasSeen[Other(g_replay.viewed)];
        }

        // One record, its time come.
        static void Play(const DemoRecord &record)
        {
            const bool fromViewed = record.from == g_replay.viewed;
            if (record.kind == KIND_FULL_STATE)
            {
                // Our ship (the shown player's) follows its own states (stage 3: the ship, its crew and rooms); the other
                // ship its own with full sensors (BA), as if they came.
                if (fromViewed)
                {
                    Match::ReplayOwnState(record.data.data(), record.data.size());
                    ++g_replay.ownStates;
                }
                else if (OtherInFull())
                {
                    Net::Deliver(MSG_STATE, record.data.data(), record.data.size());
                    ++g_replay.delivered;
                }
                return;
            }
            if (record.kind == KIND_MARKER)
            {
                // The recorded match's status (roadmap BB), for the line at the top.
                if (record.type == MARK_STATUS)
                {
                    Reader r(record.data);
                    bool ranked = r.Bool();
                    std::string why = r.Str();
                    if (r.Ok())
                    {
                        g_replay.ranked = ranked ? 1 : 0;
                        g_replay.unrankedWhy = why;
                    }
                }
                return;
            }
            if (record.kind != KIND_MESSAGE) return;
            if (record.type == MSG_STATE)
            {
                // A state as it went: the other ship's as the shown player's game got it (ours follows its full states).
                if (fromViewed || OtherInFull())
                {
                    ++g_replay.held;
                    return;
                }
                Net::Deliver(record.type, record.data.data(), record.data.size());
                ++g_replay.delivered;
                return;
            }
            const bool fromHost = record.from == FROM_HOST;
            const bool matchFlow = record.type == MSG_SETTINGS || record.type == MSG_MATCH || record.type == MSG_MATCH_EVENT;
            if (fromViewed)
            {
                if (record.type == MSG_LOADOUT)
                {
                    Match::ReplayOwnLoadout(record.data.data(), record.data.size());
                    ++g_replay.ownLoadouts;
                }
                else if (record.type == MSG_CREW_ROSTER)
                {
                    Crew::ReplayOwnRoster(record.data.data(), record.data.size());
                    ++g_replay.delivered;
                }
                else if (record.type == MSG_BOARD || record.type == MSG_RECALL)
                {
                    // Stage 4b: its crew going aboard the opponent's ship and taken back.
                    Boarding::ReplayOwn(record.type, record.data.data(), record.data.size());
                    ++g_replay.delivered;
                }
                else if (record.type == MSG_DRONE_SHOT)
                {
                    // Stage 4b: its defense drones' shots, copies in our space.
                    Drones::ReplayOwnDroneShot(record.data.data(), record.data.size());
                    ++g_replay.delivered;
                }
                else if (record.type == MSG_SHOT || record.type == MSG_RESULT || record.type == MSG_SHOT_DOWNED)
                {
                    // Stage 4: its shots from our ship, its verdicts on the opponent's.
                    if (record.type == MSG_SHOT) Match::ReplayOwnShot(record.data.data(), record.data.size());
                    else if (record.type == MSG_RESULT) Match::ReplayOwnResult(record.data.data(), record.data.size());
                    else Match::ReplayOwnShotDowned(record.data.data(), record.data.size());
                    ++g_replay.delivered;
                }
                else if (record.type == MSG_CHAT)
                {
                    // Its own chat lines under its name (the opponent's come under theirs, as received).
                    Match::ReplayChat(g_replay.viewed == FROM_HOST ? g_replay.hostName : g_replay.guestName, record.data.data(),
                                      record.data.size());
                    ++g_replay.delivered;
                }
                else if (matchFlow && fromHost)
                {
                    Net::Deliver(record.type, record.data.data(), record.data.size());
                    ++g_replay.delivered;
                }
                else
                {
                    ++g_replay.held;
                }
                return;
            }
            bool shown = record.type == MSG_CHAT || record.type == MSG_LOADOUT || record.type == MSG_READY ||
                         record.type == MSG_CREW_ROSTER || record.type == MSG_SHOT || record.type == MSG_RESULT ||
                         record.type == MSG_SHOT_DOWNED || record.type == MSG_DRONE_SHOT || record.type == MSG_BOARD ||
                         record.type == MSG_RECALL || (matchFlow && fromHost);
            if (!shown)
            {
                ++g_replay.held;
                return;
            }
            Net::Deliver(record.type, record.data.data(), record.data.size());
            ++g_replay.delivered;
        }

        bool ReplayPaused()
        {
            return g_replay.active && g_replay.paused;
        }

        // A seek runs ahead as fast as a frame's budget of real time allows (roadmap BO; it was 32 steps a frame, about
        // half a minute a second, and the user watched it go): at most this many steps, until the budget is spent.
        static const int SEEK_STEPS = 4000;
        static const double SEEK_BUDGET_MS = 28.0;
        static const double COVER_FROM_MS = 3000.0;   // a seek this long or longer runs behind the cover

        void Pace(int &steps, float &share, double stepMs)
        {
            steps = 1;
            share = 1.f;
            g_replay.stepsStartReal = RealMs();
            if (!g_replay.active || g_replay.paused) return;
            // A seek runs ahead: no more steps than it takes to get there.
            double ahead = g_replay.seekTo - (WallMs() - g_replay.startMs);
            if (g_replay.seekTo >= 0.0) steps = stepMs > 0.0 ? std::max(1, std::min(SEEK_STEPS, (int)std::ceil(ahead / stepMs))) : SEEK_STEPS;
            else if (g_replay.speed >= 1.0) steps = (int)g_replay.speed;
            else share = (float)g_replay.speed;
        }

        bool SeekBudgetSpent()
        {
            return g_replay.active && g_replay.seekTo >= 0.0 && RealMs() - g_replay.stepsStartReal >= SEEK_BUDGET_MS;
        }

        // A seek to `target` from the replay's position now: behind the cover when it is long.
        static void CoverSeek(double target, const std::string &text)
        {
            const double position = WallMs() - g_replay.startMs;
            g_replay.covering = target - position >= COVER_FROM_MS;
            g_replay.coverFrom = position;
            g_replay.coverText = text;
        }

        // The records whose time has come.
        static void PlayDue()
        {
            double t = WallMs() - g_replay.startMs;
            while (g_replay.active && g_replay.next < g_replay.records.size() && g_replay.records[g_replay.next].ms <= t)
            {
                Play(g_replay.records[g_replay.next]);
                ++g_replay.next;
            }
        }

        void BeforeWorldStep(double stepMs)
        {
            if (!g_replay.active || g_replay.paused) return;
            ReplayClockAdvance(stepMs);
            PlayDue();
        }

        void ReplayFrame(double now)
        {
            if (!g_replay.active) return;
            // A recorded timeout (roadmap BF): FTL's world stands still, so the replay's clock (its world time) would too.
            // It passes in real time at the replay's speed instead (a seek jumps to its end), the records coming as it
            // goes (the players' orders, power and targets in it).
            const double real = RealMs();
            const double frameMs = g_replay.lastRealMs >= 0.0 ? std::min(250.0, real - g_replay.lastRealMs) : 0.0;
            g_replay.lastRealMs = real;
            if (!g_replay.paused && Rounds::TimeoutPaused())
            {
                const double left = Rounds::TimeoutLeftMs();
                const double step = g_replay.seekTo >= 0.0 ? std::min(left, std::max(0.0, g_replay.seekTo - (now - g_replay.startMs)))
                                                           : std::min(left, frameMs * g_replay.speed);
                if (step > 0.0)
                {
                    ReplayClockAdvance(step);
                    PlayDue();
                    now = WallMs();
                }
            }
            // A seek there: on at the speed before it, or paused again.
            if (g_replay.seekTo >= 0.0 && now - g_replay.startMs >= g_replay.seekTo)
            {
                Log("Demo: the replay is at %.1f s (seek)", (now - g_replay.startMs) / 1000.0);
                g_replay.seekTo = -1.0;
                g_replay.paused = g_replay.pauseAfterSeek;
                g_replay.covering = false;
            }
            if (g_replay.active && !g_replay.ended && g_replay.next >= g_replay.records.size())
            {
                // It stays on its last moment, paused, with its controls (roadmap AW): from the start, or a seek back.
                g_replay.ended = true;
                g_replay.paused = true;
                g_replay.seekTo = -1.0;
                g_replay.covering = false;
                Log("Demo: the replay is over (%u messages played, %u held, %u own loadouts, %u own states); it stays on its last moment",
                    g_replay.delivered, g_replay.held, g_replay.ownLoadouts, g_replay.ownStates);
            }
        }

        static std::string ReplayStatus()
        {
            if (!g_replay.active) return "replay: none";
            std::ostringstream out;
            double t = WallMs() - g_replay.startMs;
            out << "replay: " << g_replay.path << ", " << (int)(t / 1000.0) << " of " << (int)(g_replay.records.back().ms / 1000) << " s"
                << (g_replay.ended ? " (at its end)" : g_replay.paused ? " (paused)" : "") << (g_replay.seekTo >= 0.0 ? " (seeking)" : "")
                << ", speed " << g_replay.speed << ", the " << SideName(g_replay.viewed) << "'s side"
                << (g_replay.fullSensors ? ", full sensors" : "") << (g_replay.partnerPath.empty() ? "" : ", both demos")
                << ", record " << g_replay.next << " of " << g_replay.records.size() << ", " << g_replay.delivered << " played, "
                << g_replay.held << " held";
            return out.str();
        }

        // To a time of the demo (ms): ahead by running there, back by starting again and running there.
        static bool Seek(double target, std::string &message)
        {
            double length = g_replay.records.back().ms;
            target = std::max(std::min(g_replay.firstMs, length), std::min(target, length));
            double position = WallMs() - g_replay.startMs;
            bool paused = g_replay.paused && !g_replay.ended;   // from the end: on, playing
            if (target < position) Restart(g_replay.viewed, g_replay.fullSensors);
            g_replay.seekTo = target;
            g_replay.pauseAfterSeek = paused;
            g_replay.paused = false;
            char text[64];
            snprintf(text, sizeof(text), "To %d:%02d", (int)(target / 1000.0) / 60, (int)(target / 1000.0) % 60);
            CoverSeek(target, text);
            message = "replay seeking " + std::to_string((int)(target / 1000.0)) + " s" + (target < position ? " (from the start)" : "");
            return true;
        }

        ReplayView GetReplayView()
        {
            ReplayView v;
            if (!g_replay.active || g_replay.records.empty()) return v;
            v.active = true;
            v.paused = g_replay.paused;
            v.seeking = g_replay.seekTo >= 0.0;
            v.ended = g_replay.ended;
            v.lengthMs = g_replay.records.back().ms;
            v.positionMs = std::max(0.0, std::min(v.lengthMs, WallMs() - g_replay.startMs));
            v.speed = g_replay.speed;
            v.hostName = g_replay.hostName;
            v.guestName = g_replay.guestName;
            v.recorderHost = g_replay.recorder == FROM_HOST;
            v.viewedHost = g_replay.viewed == FROM_HOST;
            v.bothSides = g_replay.hasFull[FROM_HOST] && g_replay.hasFull[FROM_GUEST];
            v.fullSensors = g_replay.fullSensors;
            v.ranked = g_replay.ranked;
            v.unrankedWhy = g_replay.unrankedWhy;
            v.covering = g_replay.covering && g_replay.seekTo >= 0.0;
            if (v.covering)
            {
                v.coverText = g_replay.coverText;
                const double way = g_replay.seekTo - g_replay.coverFrom;
                v.coverProgress = way > 0.0 ? std::max(0.0, std::min(1.0, (v.positionMs - g_replay.coverFrom) / way)) : 1.0;
            }
            return v;
        }

        void ReplayPlayPause()
        {
            if (!g_replay.active) return;
            std::string message;
            if (g_replay.ended)
            {
                // From the start again, playing.
                if (Seek(0.0, message)) g_replay.pauseAfterSeek = false;
                return;
            }
            if (g_replay.seekTo >= 0.0) g_replay.pauseAfterSeek = !g_replay.pauseAfterSeek;
            else g_replay.paused = !g_replay.paused;
        }

        bool ReplayRestarting()
        {
            return g_restarting;
        }

        void ReplayStop()
        {
            if (!g_replay.active) return;
            std::string message;
            if (Seek(0.0, message)) g_replay.pauseAfterSeek = true;
        }

        void ReplaySwitchView()
        {
            if (!g_replay.active) return;
            const uint8_t side = Other(g_replay.viewed);
            if (!g_replay.hasFull[side])
            {
                Console::Feed("The other player's view needs their demo of this match in the same folder.");
                return;
            }
            // From the start again from that side, on to where it was (or was going), paused or playing as it was.
            const bool seeking = g_replay.seekTo >= 0.0;
            const double target = seeking ? g_replay.seekTo : std::min(WallMs() - g_replay.startMs, (double)g_replay.records.back().ms);
            const bool paused = seeking ? g_replay.pauseAfterSeek : g_replay.paused && !g_replay.ended;
            Restart(side, g_replay.fullSensors);
            g_replay.seekTo = std::max(g_replay.firstMs, target);
            g_replay.pauseAfterSeek = paused;
            g_replay.paused = false;
            CoverSeek(g_replay.seekTo, std::string(side == FROM_HOST ? "The host's view" : "The guest's view") + " (" +
                                           (side == FROM_HOST ? g_replay.hostName : g_replay.guestName) + ")");
            Log("Demo: the %s's side, on to %.1f s", SideName(side), g_replay.seekTo / 1000.0);
        }

        void ReplaySetFullSensors(bool on)
        {
            if (!g_replay.active || on == g_replay.fullSensors) return;
            const uint8_t other = Other(g_replay.viewed);
            if (on && !g_replay.hasFull[other])
            {
                Console::Feed("Full sensors need the other player's demo of this match in the same folder.");
                return;
            }
            g_replay.fullSensors = on;
            // The other ship by the chosen states at once, the last one before now (paused, the next would wait): it
            // counts even when older than the last one taken (Match::ReplayRestate).
            for (size_t i = std::min(g_replay.next, g_replay.records.size()); i-- > 1;)
            {
                const DemoRecord &record = g_replay.records[i];
                bool chosen = OtherInFull() ? record.kind == KIND_FULL_STATE : (record.kind == KIND_MESSAGE && record.type == MSG_STATE);
                if (record.from != other || !chosen) continue;
                Match::ReplayRestate();
                Net::Deliver(MSG_STATE, record.data.data(), record.data.size());
                break;
            }
            Log("Demo: full sensors %s", on ? "on (the other ship as it was, all in sight)" : "off (the other ship as the shown player saw it)");
        }

        bool ReplayFullSensors()
        {
            return g_replay.active && g_replay.fullSensors;
        }

        void ReplaySeekTo(double ms)
        {
            std::string message;
            if (g_replay.active) Seek(ms, message);
        }

        void ReplayStep(double ms)
        {
            if (g_replay.active) ReplaySeekTo(std::min(WallMs() - g_replay.startMs, (double)g_replay.records.back().ms) + ms);
        }

        void ReplaySpeedStep(int direction)
        {
            static const double SPEEDS[] = {0.5, 1.0, 2.0, 4.0, 8.0};
            if (!g_replay.active) return;
            int index = 1;
            for (int i = 0; i < 5; ++i)
            {
                if (SPEEDS[i] == g_replay.speed) index = i;
            }
            if (direction > 0) index = std::min(4, index + 1);
            else if (direction < 0) index = std::max(0, index - 1);
            else index = (index + 1) % 5;
            g_replay.speed = SPEEDS[index];
        }

        bool RunReplayVerb(const Command &cmd, std::string &message)
        {
            if (cmd.args.size() <= 1)
            {
                message = ReplayStatus();
                return true;
            }
            if (ArgIs(cmd, 1, "pause"))
            {
                if (!g_replay.active || g_replay.paused)
                {
                    message = "no replay runs";
                    return false;
                }
                // FTL's world stands still (FTL's pause), and the replay's clock with it.
                ReplayPlayPause();
                message = "replay paused";
                return true;
            }
            if (ArgIs(cmd, 1, "resume"))
            {
                if (!g_replay.active || !g_replay.paused)
                {
                    message = "no replay is paused";
                    return false;
                }
                message = g_replay.ended ? "replay from the start" : "replay resumed";
                ReplayPlayPause();
                return true;
            }
            if (ArgIs(cmd, 1, "speed"))
            {
                double speed = cmd.args.size() > 2 ? std::atof(cmd.args[2].c_str()) : 0.0;
                if (!g_replay.active || (speed != 0.5 && speed != 1.0 && speed != 2.0 && speed != 4.0 && speed != 8.0))
                {
                    message = "usage: replay speed 0.5|1|2|4|8 (while a replay runs)";
                    return false;
                }
                g_replay.speed = speed;
                std::ostringstream text;
                text << "replay speed " << speed;
                message = text.str();
                return true;
            }
            if (ArgIs(cmd, 1, "seek"))
            {
                // replay seek <s> (the demo's time), or +<s> / -<s> from where it is.
                std::string value = cmd.args.size() > 2 ? cmd.args[2] : "";
                if (!g_replay.active || value.empty())
                {
                    message = "usage: replay seek <s>|+<s>|-<s> (while a replay runs)";
                    return false;
                }
                double seconds = std::atof(value.c_str());
                bool relative = value[0] == '+' || value[0] == '-';
                double target = relative ? WallMs() - g_replay.startMs + seconds * 1000.0 : seconds * 1000.0;
                return Seek(target, message);
            }
            if (ArgIs(cmd, 1, "stop"))
            {
                if (!g_replay.active)
                {
                    message = "no replay runs";
                    return false;
                }
                StopReplay("stopped");
                message = "replay stopped";
                return true;
            }
            if (ArgIs(cmd, 1, "view"))
            {
                const bool host = ArgIs(cmd, 2, "host");
                if (!g_replay.active || (!host && !ArgIs(cmd, 2, "guest")))
                {
                    message = "usage: replay view host|guest (while a replay runs)";
                    return false;
                }
                const uint8_t side = host ? FROM_HOST : FROM_GUEST;
                if (side != g_replay.viewed)
                {
                    if (!g_replay.hasFull[side])
                    {
                        message = "only the recorder's side: the other player's demo of the match isn't joined";
                        return false;
                    }
                    ReplaySwitchView();
                }
                message = std::string("replay from the ") + SideName(side) + "'s side";
                return true;
            }
            if (ArgIs(cmd, 1, "sensors"))
            {
                const bool full = ArgIs(cmd, 2, "full");
                if (!g_replay.active || (!full && !ArgIs(cmd, 2, "seen")))
                {
                    message = "usage: replay sensors full|seen (while a replay runs)";
                    return false;
                }
                if (full && !g_replay.hasFull[Other(g_replay.viewed)])
                {
                    message = "full sensors need the other player's demo of the match";
                    return false;
                }
                ReplaySetFullSensors(full);
                message = full ? "replay with full sensors" : "replay as the shown player saw it";
                return true;
            }
            // replay <file> [with <file>] [view host|guest]: the words part it (a file's name may have spaces).
            std::string path, partner, view;
            std::string *part = &path;
            for (size_t i = 1; i < cmd.raw.size() && i < cmd.args.size(); ++i)
            {
                if (cmd.args[i] == "with")
                {
                    part = &partner;
                    continue;
                }
                if (cmd.args[i] == "view")
                {
                    part = &view;
                    continue;
                }
                if (!part->empty()) *part += " ";
                *part += part == &view ? cmd.args[i] : cmd.raw[i];
            }
            const int side = view == "host" ? FROM_HOST : view == "guest" ? FROM_GUEST : -1;
            if (path.empty() || (!view.empty() && side < 0))
            {
                message = "usage: replay <file> [with <file>] [view host|guest]";
                return false;
            }
            return Start(path, partner, side, message);
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
            if (ArgIs(cmd, 1, "hold"))
            {
                g_swap.hold = !ArgIs(cmd, 2, "off");
                Log("Demo: the swap's pieces %s (test)", g_swap.hold ? "wait" : "go");
                message = g_swap.hold ? "the swap's pieces wait (demo hold off lets them go)" : "the swap's pieces go";
                return true;
            }
            if (cmd.args.size() > 1)
            {
                message = "usage: demo [on|off|stop|hold on|off]";
                return false;
            }
            message = Status();
            return true;
        }
    }
}

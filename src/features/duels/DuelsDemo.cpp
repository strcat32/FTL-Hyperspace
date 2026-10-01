#include "Global.h"
#include "Duels.h"
#include "DuelsBoarding.h"
#include "DuelsConfig.h"
#include "DuelsConsole.h"
#include "DuelsCrew.h"
#include "DuelsDemo.h"
#include "DuelsDrones.h"
#include "DuelsFair.h"
#include "DuelsMatch.h"
#include "DuelsNet.h"
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
        };

        static DemoState g;

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
            size_t next = 0;
            double startMs = 0.0;            // the demo's start, on the replay's clock
            double firstMs = 0.0;            // its first state of our ship (ms in the demo): no seek goes further back
            bool paused = false;
            double speed = 1.0;              // 0.5, 1, 2, 4 or 8
            double seekTo = -1.0;            // running ahead to this time (ms in the demo); -1: not seeking
            bool pauseAfterSeek = false;
            bool ended = false;              // at its end: it stays on its last moment, paused
            int ranked = -1;                 // the recorded match's status (BB): 1, 0, -1 not known
            std::string unrankedWhy;
            uint32_t delivered = 0, ownLoadouts = 0, ownStates = 0, held = 0;
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

        // ---------------------------------------------------------------------------------------------------------
        // Replay
        // ---------------------------------------------------------------------------------------------------------

        static uint32_t Get32(const std::vector<uint8_t> &data, size_t pos)
        {
            return (uint32_t)data[pos] | ((uint32_t)data[pos + 1] << 8) | ((uint32_t)data[pos + 2] << 16) | ((uint32_t)data[pos + 3] << 24);
        }

        // A demo file's records (any version of the game's), the first the header.
        static bool ReadRecords(const std::string &path, std::vector<DemoRecord> &records, std::string &message)
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
            return true;
        }

        static bool ReadDemo(const std::string &path, ReplayState &out, std::string &message)
        {
            if (!ReadRecords(path, out.records, message)) return false;
            Reader header(out.records[0].data);
            uint16_t protocol = header.U16();
            std::string version = header.Str(), build = header.Str(), dataHash = header.Str();
            out.recorder = header.U8();
            out.hostName = header.Str();
            out.guestName = header.Str();
            if (!header.Ok())
            {
                message = "the demo's header is broken";
                return false;
            }
            if (protocol != Net::PROTOCOL_VERSION)
            {
                message = "the demo is from protocol " + std::to_string(protocol) + " (" + version + "), this game plays " +
                          std::to_string(Net::PROTOCOL_VERSION);
                return false;
            }
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
            Reader header(records[0].data);
            uint16_t protocol = header.U16();
            std::string version = header.Str();
            header.Str();
            header.Str();
            header.U8();
            info.hostName = header.Str();
            info.guestName = header.Str();
            info.startUtc = header.U32();
            if (!header.Ok())
            {
                info.problem = "the demo's header is broken";
                return;
            }
            if (protocol != Net::PROTOCOL_VERSION) info.problem = "from version " + version + ": this game plays only its own version's demos";
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

        bool StartReplay(const std::string &path, std::string &message)
        {
            if (g.file) End("a replay begins");
            ReplayState replay;
            if (!ReadDemo(NewestDemo(path), replay, message)) return false;
            const std::string opponent = replay.recorder == FROM_HOST ? replay.guestName : replay.hostName;
            Log("Demo: replaying %s (%u records, %.0f s; recorded by the %s, %s vs %s)", path.c_str(), (unsigned)replay.records.size(),
                replay.records.back().ms / 1000.0, replay.recorder == FROM_HOST ? "host" : "guest", replay.hostName.c_str(),
                replay.guestName.c_str());
            // A replay that runs ends first (another one, or this one going back to its start).
            Net::BeginReplay(opponent);
            g_replay = std::move(replay);
            g_replay.active = true;
            // Its clock: FTL's world time from now on (DuelsTrace.h); the demo starts now.
            ReplayClockOn();
            g_replay.startMs = WallMs();
            // The recorded clocks against ours, from the first state each side sent: the recorder's full states went as
            // they were written; the opponent's states as they came (their latency in it, as the recorder saw them).
            // The match's times are the host's: the recorder's own, if it hosted.
            double recorderClock = 0.0, peerClock = 0.0;
            bool haveRecorder = false, havePeer = false;
            for (const DemoRecord &record : g_replay.records)
            {
                if (record.data.size() < sizeof(double)) continue;
                bool ownState = record.kind == KIND_FULL_STATE && record.from == g_replay.recorder && !haveRecorder;
                bool peerState = record.kind == KIND_MESSAGE && record.type == MSG_STATE && record.from != g_replay.recorder && !havePeer;
                if (!ownState && !peerState) continue;
                double sentAt;
                std::memcpy(&sentAt, record.data.data(), sizeof(sentAt));
                double clock = sentAt - (g_replay.startMs + record.ms);
                if (ownState)
                {
                    recorderClock = clock;
                    haveRecorder = true;
                }
                else
                {
                    peerClock = clock;
                    havePeer = true;
                }
                if (haveRecorder && havePeer) break;
            }
            Net::SetReplayClock(peerClock);
            Net::SetReplayHostClock(g_replay.recorder == FROM_HOST ? recorderClock : peerClock);
            // Its start for the controls: before our ship's first state the ships aren't fitted yet.
            for (const DemoRecord &record : g_replay.records)
            {
                if (record.kind != KIND_FULL_STATE || record.from != g_replay.recorder) continue;
                g_replay.firstMs = record.ms;
                break;
            }
            message = "replaying " + path + " (" + std::to_string(g_replay.records.size()) + " records, " +
                      std::to_string((int)(g_replay.records.back().ms / 1000)) + " s), " + opponent + " as the opponent";
            return true;
        }

        static const uint8_t MSG_CHAT = 16, MSG_LOADOUT = 17, MSG_READY = 18, MSG_SHOT = 20, MSG_RESULT = 21, MSG_SHOT_DOWNED = 23,
                             MSG_DRONE_SHOT = 25, MSG_CREW_ROSTER = 26, MSG_SETTINGS = 27, MSG_BOARD = 32, MSG_RECALL = 33,
                             MSG_MATCH = 38, MSG_MATCH_EVENT = 39;

        // One record, its time come.
        static void Play(const DemoRecord &record)
        {
            if (record.kind == KIND_FULL_STATE)
            {
                // Our ship (the recorder's) follows its own states (stage 3: the ship, its crew and rooms).
                Match::ReplayOwnState(record.data.data(), record.data.size());
                ++g_replay.ownStates;
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
            bool fromRecorder = record.from == g_replay.recorder;
            bool fromHost = record.from == FROM_HOST;
            bool matchFlow = record.type == MSG_SETTINGS || record.type == MSG_MATCH || record.type == MSG_MATCH_EVENT;
            if (fromRecorder)
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
                    Match::ReplayChat(g_replay.recorder == FROM_HOST ? g_replay.hostName : g_replay.guestName, record.data.data(),
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
            bool shown = record.type == MSG_CHAT || record.type == MSG_LOADOUT || record.type == MSG_READY || record.type == MSG_STATE ||
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

        static const int SEEK_STEPS = 32;   // FTL's world steps a frame while a seek runs ahead

        void Pace(int &steps, float &share, double stepMs)
        {
            steps = 1;
            share = 1.f;
            if (!g_replay.active || g_replay.paused) return;
            // A seek runs ahead: no more steps than it takes to get there.
            double ahead = g_replay.seekTo - (WallMs() - g_replay.startMs);
            if (g_replay.seekTo >= 0.0) steps = stepMs > 0.0 ? std::max(1, std::min(SEEK_STEPS, (int)std::ceil(ahead / stepMs))) : SEEK_STEPS;
            else if (g_replay.speed >= 1.0) steps = (int)g_replay.speed;
            else share = (float)g_replay.speed;
        }

        void BeforeWorldStep(double stepMs)
        {
            if (!g_replay.active || g_replay.paused) return;
            ReplayClockAdvance(stepMs);
            double t = WallMs() - g_replay.startMs;
            while (g_replay.active && g_replay.next < g_replay.records.size() && g_replay.records[g_replay.next].ms <= t)
            {
                Play(g_replay.records[g_replay.next]);
                ++g_replay.next;
            }
        }

        void ReplayFrame(double now)
        {
            if (!g_replay.active) return;
            // A seek there: on at the speed before it, or paused again.
            if (g_replay.seekTo >= 0.0 && now - g_replay.startMs >= g_replay.seekTo)
            {
                Log("Demo: the replay is at %.1f s (seek)", (now - g_replay.startMs) / 1000.0);
                g_replay.seekTo = -1.0;
                g_replay.paused = g_replay.pauseAfterSeek;
            }
            if (g_replay.active && !g_replay.ended && g_replay.next >= g_replay.records.size())
            {
                // It stays on its last moment, paused, with its controls (roadmap AW): from the start, or a seek back.
                g_replay.ended = true;
                g_replay.paused = true;
                g_replay.seekTo = -1.0;
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
                << ", speed " << g_replay.speed
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
            if (target < position)
            {
                std::string path = g_replay.path;
                double speed = g_replay.speed;
                g_restarting = true;
                bool started = StartReplay(path, message);
                g_restarting = false;
                if (!started) return false;
                g_replay.speed = speed;
                // The feed's lines are from later in the demo; its own come again as it runs there.
                Console::ClearFeed();
            }
            g_replay.seekTo = target;
            g_replay.pauseAfterSeek = paused;
            g_replay.paused = false;
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
            v.ranked = g_replay.ranked;
            v.unrankedWhy = g_replay.unrankedWhy;
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
                g_replay.active = false;
                Net::EndReplay("stopped");
                message = "replay stopped";
                return true;
            }
            std::string path = cmd.raw.size() > 1 ? cmd.raw[1] : "";
            for (size_t i = 2; i < cmd.raw.size(); ++i) path += " " + cmd.raw[i];
            return StartReplay(path, message);
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

#include "Global.h"
#include "Duels.h"
#include "DuelsAccount.h"
#include "DuelsConsole.h"
#include "DuelsCrew.h"
#include "DuelsLobby.h"
#include "DuelsMatch.h"
#include "DuelsNet.h"
#include "DuelsRefit.h"
#include "DuelsRejoin.h"
#include "DuelsRounds.h"
#include "DuelsStyle.h"
#include "DuelsTrace.h"
#include "DuelsTune.h"
#include "DuelsWin32.h"
#include "DuelsWire.h"

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace Duels
{
    namespace Rejoin
    {
        static const char *const FILE_NAME = "duels-rejoin.dat";
        static const char *const TEMP_NAME = "duels-rejoin.tmp";
        static const char *const MAGIC = "FTL: Duels, a match to go back to";
        static const uint16_t FORMAT = 1;
        static const double SAVE_EVERY_MS = 1000.0;
        // The other game waits Net::REJOIN_GRACE_MS from when it noticed the loss: a relay drops a silent player after
        // 10 s, and a direct connection's silence is as long. The file's time counts that much longer.
        static const double NOTICE_MS = 10000.0;

        // The file as read.
        struct Saved
        {
            bool ok = false;
            std::string why;                // not ok: why
            std::string version;
            int64_t savedAt = 0;            // Unix time
            Net::Way way;
            std::string ownName, opponent;
            float xp = 3.f;
            std::vector<std::pair<std::string, std::string>> fine;
            int round = 0, rounds = 0;
            std::string points;             // "1 : 0.5", ours first
            std::string state;              // "round 2 of 5: fight"
            std::vector<uint8_t> roundsPart, refitPart, crewPart, loadoutPart;
            // Our ship beyond its loadout.
            int scrap = 0, missiles = 0, fuel = 0;
            float drive = 0.f;              // the FTL drive's charge (it charges in a fight: running away, roadmap AD)
            uint32_t nextShot = 1;          // our shots' next id
            struct System
            {
                int id = 0, health = 0, power = 0;
            };
            std::vector<System> systems;
            std::vector<bool> weaponsPowered;
        };

        struct RejoinState
        {
            bool written = false;           // this session wrote the file (it goes when the match ends)
            bool dirty = false;
            double lastSave = -1.0e12;
            uint32_t saves = 0;
            std::string opponent;           // the last name the session had for them (a lost connection forgets it)
            // CONTINUE: the way back, from Begin until the match is joined again or given up.
            bool trying = false;
            Saved saved;
            double triedSinceMs = 0.0;
            // The main menu's look at the file, at most once a second.
            double checkedMs = -1.0e12;
            bool available = false;
            Saved menu;
        };

        static RejoinState g;

        static void Blob(Writer &w, const Writer &part)
        {
            w.U32((uint32_t)part.data.size());
            w.Bytes(part.data.data(), part.data.size());
        }

        static std::vector<uint8_t> ReadBlob(Reader &r)
        {
            uint32_t size = r.U32();
            if (!r.Ok() || size > r.Remaining()) return std::vector<uint8_t>();
            const uint8_t *start = r.Position();
            r.Skip(size);
            return std::vector<uint8_t>(start, start + size);
        }

        // A match that can be gone back to runs: against another player, live, with a token, and a way to find it again
        // (a host without a relay can't be found: its guest takes no handshake).
        static bool Running()
        {
            if (Net::Replaying() || Rounds::IsLocal() || !Rounds::InMatch()) return false;
            Rounds::Phase phase = Rounds::GetPhase();
            if (phase == Rounds::Phase::None || phase == Rounds::Phase::MatchOver) return false;
            Net::Way way = Net::CurrentWay();
            return way.token != 0 && !way.server.empty() && (way.relay || !way.host);
        }

        static void Save()
        {
            ShipManager *own = G_->GetShipManager(0);
            if (!own) return;
            if (!Net::PeerName().empty()) g.opponent = Net::PeerName();
            Writer w;
            w.Str(MAGIC);
            w.U16(FORMAT);
            w.Str(Net::OwnVersion());
            uint64_t now = (uint64_t)std::time(nullptr);
            w.U32((uint32_t)(now & 0xffffffffu));
            w.U32((uint32_t)(now >> 32));
            Net::Way way = Net::CurrentWay();
            w.Bool(way.relay);
            w.Str(way.server);
            w.U16(way.port);
            w.Str(way.code);
            w.Str(way.password);
            w.Bool(way.host);
            w.Bool(way.ranked);
            w.U32((uint32_t)(way.token & 0xffffffffu));
            w.U32((uint32_t)(way.token >> 32));
            w.Str(Net::OwnName());
            w.Str(g.opponent);
            // The match's settings: a host's game plays by them again.
            w.F32(Match::MatchXp());
            std::vector<std::pair<std::string, std::string>> fine = Tune::MatchChanged();
            if (fine.size() > 255) fine.resize(255);
            w.U8((uint8_t)fine.size());
            for (const std::pair<std::string, std::string> &entry : fine)
            {
                w.Str(entry.first);
                w.Str(entry.second);
            }
            // The main menu's line about it.
            Rounds::Summary s = Rounds::GetSummary();
            w.U8((uint8_t)std::max(0, std::min(255, s.round)));
            w.U8((uint8_t)std::max(0, std::min(255, s.rounds)));
            w.Str(s.points[s.me ? 1 : 0] + " : " + s.points[s.me ? 0 : 1]);
            w.Str(s.state);
            // The parts the modules keep.
            Writer part;
            Rounds::WriteRejoin(part);
            Blob(w, part);
            part = Writer();
            Refit::WriteRejoin(part);
            Blob(w, part);
            part = Writer();
            Crew::WriteOwnCrew(part);
            Blob(w, part);
            part = Writer();
            Match::WriteLoadout(part, Match::TakeLoadout(own));
            Blob(w, part);
            // Our ship beyond its loadout.
            w.I32(own->currentScrap);
            w.I32(own->GetMissileCount());
            w.I32(own->fuel_count);
            w.F32(own->jump_timer.first);
            w.U32(Match::NextShotId());
            std::vector<ShipSystem*> systems;
            for (ShipSystem *system : own->vSystemList)
            {
                if (system && system->iSystemType >= 0 && system->iSystemType < SYS_ALL) systems.push_back(system);
            }
            w.U8((uint8_t)std::min<size_t>(systems.size(), 255));
            for (size_t i = 0; i < systems.size() && i < 255; ++i)
            {
                w.U8((uint8_t)systems[i]->iSystemType);
                w.U8((uint8_t)std::max(0, systems[i]->healthState.first));
                w.U8((uint8_t)std::max(0, systems[i]->powerState.first));
            }
            std::vector<ProjectileFactory*> weapons = own->weaponSystem ? own->GetWeaponList() : std::vector<ProjectileFactory*>();
            w.U8((uint8_t)std::min<size_t>(weapons.size(), 255));
            for (size_t i = 0; i < weapons.size() && i < 255; ++i) w.Bool(weapons[i]->powered);

            FILE *file = std::fopen(TEMP_NAME, "wb");
            if (!file)
            {
                Log("Rejoin: can't write %s", TEMP_NAME);
                return;
            }
            size_t written = std::fwrite(w.data.data(), 1, w.data.size(), file);
            bool closed = std::fclose(file) == 0;
            if (written != w.data.size() || !closed || !PutFileInPlace(TEMP_NAME, FILE_NAME))
            {
                Log("Rejoin: can't write %s", FILE_NAME);
                return;
            }
            if (!g.written || g.saves % 60 == 0)
            {
                Log("Rejoin: the match is in %s (%u bytes; %s, %s, %s)", FILE_NAME, (unsigned)w.data.size(), way.host ? "the host" : "the guest",
                    way.relay ? ("room " + way.code + " at the relay " + Net::RelayName(way.server, way.port)).c_str() : "a direct connection",
                    s.state.c_str());
            }
            g.written = true;
            ++g.saves;
        }

        static Saved Load()
        {
            Saved s;
            FILE *file = std::fopen(FILE_NAME, "rb");
            if (!file)
            {
                s.why = "isn't there";
                return s;
            }
            std::vector<uint8_t> bytes;
            uint8_t buffer[4096];
            size_t got;
            while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0 && bytes.size() < (1u << 20)) bytes.insert(bytes.end(), buffer, buffer + got);
            std::fclose(file);
            Reader r(bytes);
            if (r.Str() != MAGIC || r.U16() != FORMAT)
            {
                s.why = "is of another kind";
                return s;
            }
            s.version = r.Str();
            uint64_t low = r.U32(), high = r.U32();
            s.savedAt = (int64_t)(low | (high << 32));
            s.way.relay = r.Bool();
            s.way.server = r.Str();
            s.way.port = r.U16();
            s.way.code = r.Str();
            s.way.password = r.Str();
            s.way.host = r.Bool();
            s.way.ranked = r.Bool();
            uint64_t tokenLow = r.U32(), tokenHigh = r.U32();
            s.way.token = tokenLow | (tokenHigh << 32);
            s.ownName = r.Str();
            s.opponent = r.Str();
            s.xp = r.F32();
            s.fine.resize(r.U8());
            for (std::pair<std::string, std::string> &entry : s.fine)
            {
                entry.first = r.Str();
                entry.second = r.Str();
            }
            s.round = r.U8();
            s.rounds = r.U8();
            s.points = r.Str();
            s.state = r.Str();
            s.roundsPart = ReadBlob(r);
            s.refitPart = ReadBlob(r);
            s.crewPart = ReadBlob(r);
            s.loadoutPart = ReadBlob(r);
            s.scrap = r.I32();
            s.missiles = r.I32();
            s.fuel = r.I32();
            s.drive = r.F32();
            s.nextShot = r.U32();
            s.systems.resize(r.U8());
            for (Saved::System &system : s.systems)
            {
                system.id = r.U8();
                system.health = r.U8();
                system.power = r.U8();
            }
            s.weaponsPowered.resize(r.U8());
            for (size_t i = 0; i < s.weaponsPowered.size(); ++i) s.weaponsPowered[i] = r.Bool();
            s.ok = r.Ok() && s.way.token != 0 && !s.loadoutPart.empty();
            if (!s.ok) s.why = "doesn't read";
            return s;
        }

        // The time the other game still waits, as far as this game can tell.
        static double MsLeft(const Saved &s)
        {
            double since = ((double)std::time(nullptr) - (double)s.savedAt) * 1000.0;
            return Net::REJOIN_GRACE_MS + NOTICE_MS - std::max(0.0, since);
        }

        static void Remove(const char *why)
        {
            FILE *file = std::fopen(FILE_NAME, "rb");
            if (!file) return;
            std::fclose(file);
            std::remove(FILE_NAME);
            Log("Rejoin: %s goes (%s)", FILE_NAME, why);
        }

        void OnSaveNeeded()
        {
            g.dirty = true;
        }

        void OnFrame(double now)
        {
            if (g.trying)
            {
                if (Net::IsConnected())
                {
                    // (OnColdConnected came with the connection.)
                    g.trying = false;
                }
                return;
            }
            if (Running())
            {
                if (g.dirty || now - g.lastSave >= SAVE_EVERY_MS)
                {
                    g.dirty = false;
                    g.lastSave = now;
                    Save();
                }
            }
            else if (g.written)
            {
                g.written = false;
                Remove("the match is over");
            }
        }

        void Clear(const char *why)
        {
            // A crashed session's file stays until CONTINUE uses it (or it is too old): only this session's goes.
            if (!g.written && !g.trying) return;
            g.written = false;
            g.dirty = false;
            g.trying = false;
            Remove(why);
        }

        void OnDisconnected(const std::string &reason)
        {
            if (g.trying)
            {
                g.trying = false;
                Remove("the way back didn't work");
                Log("Rejoin: the way back into the match against %s didn't work: %s", g.saved.opponent.c_str(), reason.c_str());
                Console::Feed("Couldn't get back into the match: " + reason);
                Lobby::RequestMenu("Couldn't get back into the match against " + g.saved.opponent + ": " + reason + ".");
                return;
            }
            if (g.written) Clear("the connection ended");
        }

        bool Available()
        {
            double now = WallMs();
            if (now - g.checkedMs < 1000.0) return g.available;
            g.checkedMs = now;
            g.available = false;
            if (g.trying || Net::GetPhase() != Net::Phase::Idle || Net::Replaying()) return false;
            g.menu = Load();
            if (!g.menu.ok) return false;
            if (g.menu.version != Net::OwnVersion() || MsLeft(g.menu) <= 0.0)
            {
                Remove(g.menu.version != Net::OwnVersion() ? "of another version of the game" : "too old: the other game waits no longer");
                return false;
            }
            g.available = true;
            return true;
        }

        std::string Describe()
        {
            if (!Available()) return "";
            const Saved &s = g.menu;
            std::string text = s.rounds > 1 ? "Round " + std::to_string(s.round) + " of " + std::to_string(s.rounds) : std::string("The match");
            text += " against " + (s.opponent.empty() ? std::string("the other player") : s.opponent);
            if (!s.points.empty() && s.rounds > 1) text += ", " + s.points;
            text += " (about " + std::to_string((int)(MsLeft(s) / 1000.0)) + " s left)";
            return text;
        }

        // Our ship beyond its loadout: the systems' damage, then their power as it was; the weapons powered; scrap,
        // missiles and fuel.
        static void RestoreShipState(const Saved &s)
        {
            ShipManager *own = G_->GetShipManager(0);
            if (!own) return;
            own->currentScrap = s.scrap;
            own->ModifyMissileCount(s.missiles - own->GetMissileCount());
            own->fuel_count = s.fuel;
            // (A new run's drive is charged: the match's was charging from its fight's start, or empty between fights.)
            own->jump_timer.first = std::max(0.f, std::min(s.drive, own->jump_timer.second));
            std::string damaged;
            for (const Saved::System &saved : s.systems)
            {
                ShipSystem *system = own->GetSystem(saved.id);
                if (!system) continue;
                int lost = system->healthState.first - std::max(0, std::min(saved.health, system->healthState.second));
                if (lost > 0)
                {
                    system->AddDamage(lost);
                    damaged += (damaged.empty() ? "" : ", ") + ShipSystem::SystemIdToName(saved.id) + " -" + std::to_string(lost);
                }
            }
            for (const Saved::System &saved : s.systems)
            {
                ShipSystem *system = own->GetSystem(saved.id);
                if (saved.id == SYS_WEAPONS || saved.id == SYS_DRONES || !system) continue;
                while (own->GetSystemPower(saved.id) > saved.power && system->DecreasePower(false)) continue;
                while (own->GetSystemPower(saved.id) < saved.power && own->IncreaseSystemPower(saved.id)) continue;
            }
            if (own->weaponSystem)
            {
                std::vector<ProjectileFactory*> weapons = own->GetWeaponList();
                for (size_t slot = 0; slot < weapons.size() && slot < s.weaponsPowered.size(); ++slot)
                {
                    if (s.weaponsPowered[slot] && !weapons[slot]->powered) own->PowerWeapon(weapons[slot], true, false);
                }
            }
            // The shields up, as they were for the match (a new run's charge from nothing).
            own->InstantPowerShields();
            // Our shots' ids past those of the shots the other game knows (a second's shots after the last save too).
            Match::ContinueShotIds(s.nextShot + 1000);
            Log("Rejoin: our ship: hull %d/%d, scrap %d, missiles %d, fuel %d, the FTL drive %.1f of %.1f; damaged: %s", own->ship.hullIntegrity.first,
                own->ship.hullIntegrity.second, own->currentScrap, own->GetMissileCount(), own->fuel_count, own->jump_timer.first,
                own->jump_timer.second, damaged.empty() ? "nothing" : damaged.c_str());
        }

        bool Begin(std::string &message)
        {
            Saved s = Load();
            if (!s.ok)
            {
                message = "the match's file " + s.why;
                return false;
            }
            if (s.version != Net::OwnVersion())
            {
                message = "the match was played with another version of the game";
                return false;
            }
            double left = MsLeft(s);
            if (left <= 0.0)
            {
                Remove("too old: the other game waits no longer");
                message = "too late: the other game waits no longer";
                return false;
            }
            Match::PrepareRejoin(s.way.ranked ? s.ownName : std::string());
            std::string what;
            Match::Loadout loadout;
            Reader loadoutReader(s.loadoutPart);
            if (!Match::ReadLoadout(loadoutReader, loadout) || !Match::RestoreOwnShip(loadout, what))
            {
                message = "our ship can't be made again" + (what.empty() ? std::string() : ": " + what);
                return false;
            }
            Log("Rejoin: our ship again: %s", what.c_str());
            Reader crewReader(s.crewPart);
            if (!Crew::RestoreOwnCrew(crewReader, what)) Log("Rejoin: the crew: %s", what.c_str());
            RestoreShipState(s);
            // A ranked room's match: the season's settings for its result (the ticket for the room comes on its own).
            if (s.way.ranked && !Rounds::SeasonKnown()) Account::FetchSeason();
            if (!Net::StartColdRejoin(s.way, left, what))
            {
                message = what;
                return false;
            }
            g.saved = s;
            g.trying = true;
            GetState().noPause = true;   // our ship stands still as the file made it until the match is there (DuelsHooks.cpp)
            g.triedSinceMs = WallMs();
            g.opponent = s.opponent;
            g.checkedMs = -1.0e12;
            message = what;
            Console::Feed("Going back into the match against " + (s.opponent.empty() ? std::string("the other player") : s.opponent) + "...");
            return true;
        }

        bool Trying()
        {
            return g.trying;
        }

        float MatchXp()
        {
            return g.saved.xp;
        }

        std::vector<std::pair<std::string, std::string>> MatchFine()
        {
            return g.saved.fine;
        }

        const std::vector<uint8_t> &RoundsPart()
        {
            return g.saved.roundsPart;
        }

        const std::vector<uint8_t> &RefitPart()
        {
            return g.saved.refitPart;
        }

        void OnColdConnected()
        {
            g.trying = false;
            g.dirty = true;   // the file again at once, from the match as it goes on
            Log("Rejoin: back in the match against %s after %.1f s", Net::PeerName().c_str(), (WallMs() - g.triedSinceMs) / 1000.0);
        }

        void Render()
        {
            if (!g.trying) return;
            double msLeft = 0.0;
            bool cutOff = false;
            if (!Net::Reconnecting(msLeft, cutOff)) msLeft = 0.0;
            const float w = 520.f, h = 116.f, x = (1280.f - w) / 2.f, y = 250.f;
            Style::Dialog(x, y, w, h, "FTL: DUELS", false);
            CSurface::GL_SetColor(GL_Color(226.f / 255.f, 230.f / 255.f, 236.f / 255.f, 1.f));
            std::string opponent = g.saved.opponent.empty() ? std::string("the other player") : g.saved.opponent;
            freetype::easy_printCenter(12, x + w / 2.f, y + 26.f, "Going back into the match against " + opponent + "...");
            freetype::easy_printCenter(12, x + w / 2.f, y + 50.f, std::to_string((int)(msLeft / 1000.0)) + " s left while " + opponent + "'s game waits");
            CSurface::GL_SetColor(GL_Color(190.f / 255.f, 196.f / 255.f, 204.f / 255.f, 1.f));
            freetype::easy_printCenter(10, x + w / 2.f, y + 78.f, "To give up: ESC, then MAIN MENU");
            CSurface::GL_SetColor(COLOR_WHITE);
        }

        std::string Status()
        {
            std::string text = g.trying ? "going back into the match against " + g.saved.opponent : g.written ? "the match is kept in " + std::string(FILE_NAME)
                               : "no match kept";
            if (!g.trying && Available()) text += "; CONTINUE: " + Describe();
            return text;
        }
    }
}

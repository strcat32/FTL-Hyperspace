#include "Global.h"
#include "Duels.h"
#include "DuelsConfig.h"
#include "DuelsConsole.h"
#include "DuelsCrew.h"
#include "DuelsEnvironment.h"
#include "DuelsMatchUi.h"
#include "DuelsMatch.h"
#include "DuelsNet.h"
#include "DuelsAi.h"
#include "DuelsBays.h"
#include "DuelsRounds.h"
#include "DuelsScript.h"
#include "DuelsRefit.h"
#include "DuelsShips.h"
#include "DuelsTrace.h"
#include "DuelsWindow.h"
#include "DuelsWire.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <map>
#include <random>
#include <sstream>
#include <vector>

namespace Duels
{
    namespace Rounds
    {
        static const double STARTING_LEAD_MS = 1500.0;   // the fight begins this long after both ships stand
        static const double ENDING_MS = 3000.0;          // the first ship's explosion; shots in the air still count
        static const double RESULT_MS = 6000.0;          // the round's result on screen before the next preparation
        static const double DRAW_AGAIN_MS = 60000.0;     // after a declined offer, the same player waits this long
        static const float HULL_WEIGHT = 0.65f;          // the damage score: hull 0.65, crew 0.35 (rules, section 3)
        static const float SCORE_TIE = 0.05f;            // damage scores closer than this are equal
        // Anti-stall (rules, section 3): a new low of either player's hull or crew health (by more than this share of
        // the round's start) restarts the timer; when it runs out, the lows decide, and a lead under 10 is a draw.
        static const float STALL_STEP = 0.02f;
        static const float STALL_DRAW_LEAD = 10.f;
        // The ship choice (roadmap 3.9; rules, section 4): a ban's time and the pick's, then the server bans or picks
        // for the player; the bans leave this many types to pick from.
        static const double BAN_MS = 20000.0;
        static const double PICK_MS = 30000.0;
        static const double REVEAL_MS = 4000.0;          // both ships on screen before round 1's preparation
        static const int OFFER_SIZE = 3;

        // The third byte of an event is the round it belongs to; a ban's is its number in the choice instead.
        enum EventType : uint8_t
        {
            EV_READY = 1,        // done preparing
            EV_DEFEAT = 2,       // our ship is out (arg: REASON_DESTROYED or REASON_CREW)
            EV_FORFEIT = 3,      // we give up the match
            EV_CONCEDE = 4,      // we give up this round
            EV_DRAW_OFFER = 5,   // arg: DRAW_ROUND or DRAW_MATCH
            EV_DRAW_ANSWER = 6,  // arg: 1 accepted, 0 declined
            EV_UNREADY = 7,      // Ready taken back (the preparation goes on to its end)
            EV_ESCAPE = 8,       // we jumped away with a charged FTL drive (roadmap AD)
            EV_DRAW_BACK = 9,    // our draw offer taken back (roadmap AT)
            EV_BAN = 10,         // arg: a ship type (DuelsShips.h), banned in our turn (roadmap 3.9)
            EV_PICK = 11         // arg: the offered ship's index
        };

        // How the ships are chosen (roadmap 3.9).
        enum ShipsMode : uint8_t
        {
            SHIPS_OWN = 0,       // each player's own, from FTL's hangar
            SHIPS_BANS = 1,      // bans in turn from a pool of types, then each picks one of the three left
            SHIPS_LIST = 2,      // each picks one from the host's list (exact layouts)
            SHIPS_MODES = 3
        };

        enum PickState : uint8_t
        {
            PICK_NONE = 0,
            PICK_OWN = 1,        // the player picked
            PICK_SERVER = 2      // time ran out: the server picked for them
        };

        enum Reason : uint8_t
        {
            REASON_NONE = 0,
            REASON_DESTROYED = 1,   // the loser's ship was destroyed
            REASON_CREW = 2,        // the loser's crew died
            REASON_BOTH = 3,        // both ships went down: the round's damage score decided
            REASON_CONCEDED = 4,
            REASON_DRAW = 5,        // a draw offer was accepted
            REASON_STALL = 6,       // the anti-stall timer ran out
            REASON_LEFT = 7,        // the other player left
            REASON_FORFEIT = 8,
            REASON_ROUNDS = 9,      // the match: more rounds won
            REASON_SCORE = 10,      // the match: rounds won equal, the damage score decided
            REASON_ESCAPED = 11     // the loser jumped away: half a point for the winner (roadmap AD)
        };

        enum DrawScope : uint8_t
        {
            DRAW_NONE = 0,
            DRAW_ROUND = 1,
            DRAW_MATCH = 2
        };

        // Players by role: the host's game runs the match.
        static const uint8_t HOST = 0, GUEST = 1, NOBODY = 2;

        struct Settings
        {
            uint8_t rounds = 5;
            uint16_t prepSeconds = 60;
            bool permadeath = true;
            bool free = false;
            uint16_t stallSeconds = 300;    // anti-stall: a round without a new low for this long ends (0: never)
            uint8_t env = Environment::MODE_AUTO;   // the fights' environments (rules, section 5)
            uint8_t hazards = Environment::DEFAULT_HAZARDS;   // the kinds MODE_AUTO may roll
            // Public recording (roadmap 3.5): the server records the match (roadmap 4.2, 5.1); a match is ranked only
            // when it is recorded, so switching it off makes the match unranked.
            bool record = true;
            // The ships (roadmap 3.9): the way they are chosen, the types the bans start from, the host's list.
            uint8_t ships = SHIPS_OWN;
            uint16_t pool = Ships::ALL_TYPES;
            std::vector<std::string> list;
        };

        struct ShipBan
        {
            uint8_t type = 0;
            bool byServer = false;      // time ran out: the server banned it for the player
        };

        struct Result
        {
            uint8_t winner = NOBODY;
            uint8_t reason = REASON_NONE;
            float dealt[2] = {0.f, 0.f};   // the damage each player dealt in the round (host, guest)
        };

        // What MSG_MATCH carries: the host's match, the guest's copy of it.
        struct Data
        {
            Phase phase = Phase::None;
            uint8_t round = 0;              // 1-based
            uint64_t token = 0;             // this match, for coming back to it after a lost connection
            Settings settings;
            double phaseEnd = -1.0;         // on the host's clock; < 0: no time limit
            double fightStart = -1.0;       // on the host's clock: the moment the fight begins
            double stallEnd = -1.0;         // on the host's clock: the anti-stall timer runs out (Fight)
            uint8_t wins[2] = {0, 0};
            uint8_t draws = 0;              // rounds drawn: half a point for each player (rules, section 1)
            uint8_t halfWins[2] = {0, 0};   // rounds the other player ran away from: half a point each (roadmap AD)
            float score[2] = {0.f, 0.f};    // damage dealt over the rounds played
            std::vector<Result> results;
            bool ready[2] = {false, false};
            uint8_t drawBy = NOBODY;
            uint8_t drawScope = DRAW_NONE;
            double drawEnd = -1.0;          // on the host's clock
            uint16_t scrap = 0;             // this round's scrap
            std::vector<Refit::ShopItem> shop;   // this round's stock
            Environment::Plan env;          // this round's fight: its environment and the seed of its schedule
            uint8_t matchWinner = NOBODY;
            uint8_t matchReason = REASON_NONE;
            // The ship choice (Phase::Choice): who bans first (the bans go in turn), the bans so far, the ships offered
            // once they are done, whether each player has picked (what, the host's game keeps to itself until the
            // reveal), and each player's ship from the reveal on.
            uint8_t firstBanner = HOST;
            std::vector<ShipBan> bans;
            std::vector<std::string> offer;
            uint8_t picked[2] = {PICK_NONE, PICK_NONE};
            std::string ships[2];
        };

        // The damage our own ship takes in a round, counted by this game (the owner decides about its ship).
        struct Damage
        {
            bool counting = false;
            float hullPool = 0.f, crewPool = 0.f;   // at the fight's start
            float hullLost = 0.f, crewLost = 0.f;   // repairs and healing don't take anything back
            int lastHull = 0;
            std::map<CrewMember*, float> lastHealth;

            float Taken() const
            {
                float hull = hullPool > 0.f ? std::min(1.f, hullLost / hullPool) : 0.f;
                float crew = crewPool > 0.f ? std::min(1.f, crewLost / crewPool) : 0.f;
                return 100.f * (HULL_WEIGHT * hull + (1.f - HULL_WEIGHT) * crew);
            }
        };

        struct Local
        {
            Settings settings;              // the host's settings for the next match (verb "match")
            bool active = false;            // connected in a duel
            bool local = false;             // a match against FTL's AI in this game alone (roadmap 3.6)
            uint8_t me = HOST;
            Data data;
            bool dirty = false;             // host: the guest needs the match state

            // What this game has done for the phase it shows (both sides do the same things at the same moments).
            Phase appliedPhase = Phase::None;
            uint8_t appliedRound = 0;
            bool fightBegun = false;
            bool roundCleaned = false;      // crew home, mind control over (once per round)
            uint8_t scrapRound = 0;         // the round whose scrap we got

            Damage taken;                   // our ship's
            float peerTaken = 0.f;          // the other ship's, from their state
            float peerLevels[2] = {1.f, 1.f};   // the other ship's hull and crew health now, as shares of the start
            float lows[2][2] = {{1.f, 1.f}, {1.f, 1.f}};   // host: each player's lowest hull and crew health share

            // Host: each player's defeat in this round (host clock), and when each last offered a draw.
            double defeatAt[2] = {-1.0, -1.0};
            uint8_t defeatReason[2] = {REASON_NONE, REASON_NONE};
            double lastOffer[2] = {-1.0e12, -1.0e12};
            bool defeatSent = false;        // ours reported this round

            double pausedSince = -1.0;      // the connection was lost then (our clock): the match is paused
            uint32_t eventsSent = 0, eventsReceived = 0, statesSent = 0, statesReceived = 0;
            // Guest: we answered the host's draw offer (its end then is no taking back).
            bool drawAnswered = false;
            double lastSecondsShown = -1.0;
            std::mt19937 random;            // host: the shops' stock

            // The ship choice: the picks (host: both, by the offer's index; until the reveal only the host's game knows
            // them), ours, and what of the choice this game has announced so far.
            int picks[2] = {-1, -1};
            int ownPick = -1;
            size_t bansShown = 0, offerShown = 0;
            uint8_t pickedShown[2] = {PICK_NONE, PICK_NONE};
            uint8_t turnShown = NOBODY;
            bool shipsShown = false;
        };

        static Local g;

        // ---------------------------------------------------------------------------------------------------------
        // Helpers
        // ---------------------------------------------------------------------------------------------------------

        static double Now()
        {
            return WallMs();
        }

        // A time on the host's clock, on ours.
        static double FromHost(double hostTime)
        {
            if (g.me == HOST || hostTime < 0.0) return hostTime;
            return Net::HasClock() ? Net::HostToLocalTime(hostTime) : hostTime;
        }

        static uint8_t Other(uint8_t player)
        {
            return player == HOST ? GUEST : HOST;
        }

        static std::string Who(uint8_t player)
        {
            if (player == NOBODY) return "nobody";
            if (player == g.me) return "you";
            std::string name = g.local ? Ai::Name() : Net::PeerName();
            return name.empty() ? "the opponent" : name;
        }

        static const char *ReasonText(uint8_t reason)
        {
            switch (reason)
            {
            case REASON_DESTROYED: return "ship destroyed";
            case REASON_CREW: return "crew dead";
            case REASON_BOTH: return "both ships down, the round's damage score decided";
            case REASON_CONCEDED: return "conceded";
            case REASON_DRAW: return "draw agreed";
            case REASON_STALL: return "stalemate, the anti-stall score decided";
            case REASON_LEFT: return "the other player left";
            case REASON_FORFEIT: return "forfeit";
            case REASON_ROUNDS: return "more points";
            case REASON_SCORE: return "points equal, the damage score decided";
            case REASON_ESCAPED: return "ran away: half a point";
            default: return "-";
            }
        }

        static const char *PhaseName(Phase phase)
        {
            switch (phase)
            {
            case Phase::Prep: return "preparation";
            case Phase::Starting: return "the ships meet";
            case Phase::Fight: return "fight";
            case Phase::Ending: return "the round is ending";
            case Phase::RoundOver: return "round over";
            case Phase::MatchOver: return "match over";
            case Phase::Choice: return "ship choice";
            default: return "no match";
            }
        }

        // What matters goes to the feed at the bottom left, short (roadmap L); details only to the console's log.
        static void Announce(const std::string &text)
        {
            Log("Rounds: %s", text.c_str());
            Console::Feed(text);
        }

        static void Note(const std::string &text)
        {
            Log("Rounds: %s", text.c_str());
            Console::Print("DUEL: " + text);
        }

        static std::string Number(float value)
        {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%.1f", value);
            return buffer;
        }

        // The scrap a round brings (rules, section 7): 6 x (15 + 6 (k - 1)), and 10 more in round 1.
        static uint16_t ScrapFor(int round)
        {
            return (uint16_t)(6 * (15 + 6 * (round - 1)) + (round == 1 ? 10 : 0));
        }

        // The ship choice (roadmap 3.9): a match's, unless each player brings their own ship (or it is a free fight).
        static bool ChoosesShips(const Settings &s)
        {
            return !s.free && s.ships != SHIPS_OWN;
        }

        static int TypeCount(uint16_t types)
        {
            int count = 0;
            for (int type = 0; type < Ships::TYPE_COUNT; ++type) count += (types >> type) & 1;
            return count;
        }

        // The bans: as many as leave three types of the pool (rules, section 4: seven of ten); none for a list.
        static int BansTotal(const Data &d)
        {
            if (d.settings.ships != SHIPS_BANS) return 0;
            return std::max(0, TypeCount(d.settings.pool & Ships::ALL_TYPES) - OFFER_SIZE);
        }

        static bool BansDone(const Data &d)
        {
            return (int)d.bans.size() >= BansTotal(d);
        }

        // Whose ban the one at this index is: the first banner's, then in turn (with an odd number of bans the first
        // banner has one more).
        static uint8_t BannerOf(const Data &d, size_t index)
        {
            return index % 2 == 0 ? d.firstBanner : Other(d.firstBanner);
        }

        // The pool's types not banned yet.
        static uint16_t TypesLeft(const Data &d)
        {
            uint16_t left = d.settings.pool & Ships::ALL_TYPES;
            for (const ShipBan &ban : d.bans) left &= (uint16_t)~(1u << ban.type);
            return left;
        }

        static std::string ShipsModeName(const Settings &s)
        {
            switch (s.ships)
            {
            case SHIPS_BANS: return "bans from " + Ships::TypesText(s.pool) + ", then a pick";
            case SHIPS_LIST:
            {
                std::string list;
                for (const std::string &ship : s.list) list += (list.empty() ? "" : ", ") + Ships::Title(ship);
                return "a pick from " + (list.empty() ? std::string("an empty list") : list);
            }
            default: return "each player's own";
            }
        }

        // ---------------------------------------------------------------------------------------------------------
        // The match state on the wire
        // ---------------------------------------------------------------------------------------------------------

        static void WriteData(Writer &w, const Data &d)
        {
            w.U8((uint8_t)d.phase);
            w.U8(d.round);
            w.U32((uint32_t)(d.token & 0xffffffffu));
            w.U32((uint32_t)(d.token >> 32));
            w.U8(d.settings.rounds);
            w.U16(d.settings.prepSeconds);
            w.Bool(d.settings.permadeath);
            w.Bool(d.settings.free);
            w.U16(d.settings.stallSeconds);
            w.U8(d.settings.env);
            w.U8(d.settings.hazards);
            w.Bool(d.settings.record);
            w.F64(d.phaseEnd);
            w.F64(d.fightStart);
            w.F64(d.stallEnd);
            w.U8(d.wins[HOST]);
            w.U8(d.wins[GUEST]);
            w.U8(d.draws);
            w.U8(d.halfWins[HOST]);
            w.U8(d.halfWins[GUEST]);
            w.F32(d.score[HOST]);
            w.F32(d.score[GUEST]);
            w.U8((uint8_t)std::min<size_t>(d.results.size(), 255));
            for (size_t i = 0; i < d.results.size() && i < 255; ++i)
            {
                w.U8(d.results[i].winner);
                w.U8(d.results[i].reason);
                w.F32(d.results[i].dealt[HOST]);
                w.F32(d.results[i].dealt[GUEST]);
            }
            w.Bool(d.ready[HOST]);
            w.Bool(d.ready[GUEST]);
            w.U8(d.drawBy);
            w.U8(d.drawScope);
            w.F64(d.drawEnd);
            w.U16(d.scrap);
            w.U8((uint8_t)std::min<size_t>(d.shop.size(), 255));
            for (size_t i = 0; i < d.shop.size() && i < 255; ++i)
            {
                w.U8(d.shop[i].kind);
                w.Str(d.shop[i].blueprint);
                w.U16(d.shop[i].price);
                w.U8(d.shop[i].count);
            }
            w.U8(d.env.kind);
            w.U32(d.env.seed);
            w.U8(d.matchWinner);
            w.U8(d.matchReason);
            w.U8(d.settings.ships);
            w.U16(d.settings.pool);
            w.U8((uint8_t)std::min<size_t>(d.settings.list.size(), 255));
            for (size_t i = 0; i < d.settings.list.size() && i < 255; ++i) w.Str(d.settings.list[i]);
            w.U8(d.firstBanner);
            w.U8((uint8_t)std::min<size_t>(d.bans.size(), 255));
            for (size_t i = 0; i < d.bans.size() && i < 255; ++i)
            {
                w.U8(d.bans[i].type);
                w.Bool(d.bans[i].byServer);
            }
            w.U8((uint8_t)std::min<size_t>(d.offer.size(), 255));
            for (size_t i = 0; i < d.offer.size() && i < 255; ++i) w.Str(d.offer[i]);
            w.U8(d.picked[HOST]);
            w.U8(d.picked[GUEST]);
            w.Str(d.ships[HOST]);
            w.Str(d.ships[GUEST]);
        }

        static bool ReadData(Reader &r, Data &d)
        {
            d.phase = (Phase)r.U8();
            d.round = r.U8();
            uint64_t tokenLow = r.U32();
            uint64_t tokenHigh = r.U32();
            d.token = tokenLow | (tokenHigh << 32);
            d.settings.rounds = r.U8();
            d.settings.prepSeconds = r.U16();
            d.settings.permadeath = r.Bool();
            d.settings.free = r.Bool();
            d.settings.stallSeconds = r.U16();
            d.settings.env = r.U8();
            d.settings.hazards = r.U8();
            d.settings.record = r.Bool();
            d.phaseEnd = r.F64();
            d.fightStart = r.F64();
            d.stallEnd = r.F64();
            d.wins[HOST] = r.U8();
            d.wins[GUEST] = r.U8();
            d.draws = r.U8();
            d.halfWins[HOST] = r.U8();
            d.halfWins[GUEST] = r.U8();
            d.score[HOST] = r.F32();
            d.score[GUEST] = r.F32();
            d.results.resize(r.U8());
            for (Result &result : d.results)
            {
                result.winner = r.U8();
                result.reason = r.U8();
                result.dealt[HOST] = r.F32();
                result.dealt[GUEST] = r.F32();
            }
            d.ready[HOST] = r.Bool();
            d.ready[GUEST] = r.Bool();
            d.drawBy = r.U8();
            d.drawScope = r.U8();
            d.drawEnd = r.F64();
            d.scrap = r.U16();
            d.shop.resize(r.U8());
            for (Refit::ShopItem &item : d.shop)
            {
                item.kind = r.U8();
                item.blueprint = r.Str();
                item.price = r.U16();
                item.count = r.U8();
            }
            d.env.kind = r.U8();
            d.env.seed = r.U32();
            d.matchWinner = r.U8();
            d.matchReason = r.U8();
            d.settings.ships = r.U8();
            d.settings.pool = r.U16();
            d.settings.list.resize(r.U8());
            for (std::string &ship : d.settings.list) ship = r.Str();
            d.firstBanner = r.U8();
            d.bans.resize(r.U8());
            bool bansOk = true;
            for (ShipBan &ban : d.bans)
            {
                ban.type = r.U8();
                ban.byServer = r.Bool();
                if (ban.type >= Ships::TYPE_COUNT) bansOk = false;
            }
            d.offer.resize(r.U8());
            for (std::string &ship : d.offer) ship = r.Str();
            d.picked[HOST] = r.U8();
            d.picked[GUEST] = r.U8();
            d.ships[HOST] = r.Str();
            d.ships[GUEST] = r.Str();
            return r.Ok() && (uint8_t)d.phase <= (uint8_t)Phase::Choice && d.env.kind < Environment::KIND_COUNT &&
                   d.settings.env < Environment::MODE_COUNT && d.settings.ships < SHIPS_MODES && d.firstBanner <= GUEST &&
                   bansOk;
        }

        static void SendData()
        {
            if (g.local)
            {
                g.dirty = false;   // nobody to send it to
                return;
            }
            Writer w;
            WriteData(w, g.data);
            Net::Send(MSG_MATCH, w, true);
            g.dirty = false;
        }

        static void SendEvent(uint8_t type, uint8_t arg)
        {
            if (g.local) return;
            Writer w;
            w.U8(type);
            w.U8(arg);
            w.U8(type == EV_BAN ? (uint8_t)g.data.bans.size() : g.data.round);
            Net::Send(MSG_MATCH_EVENT, w, true);
            ++g.eventsSent;
        }

        // ---------------------------------------------------------------------------------------------------------
        // Our ship: damage and defeat
        // ---------------------------------------------------------------------------------------------------------

        // Our crew wherever they are: aboard our ship (not the opponent's boarders, not drones) and aboard theirs. (The
        // crew registry is the duel's; against the AI, FTL's own ship ids tell whose crew is whose.)
        static std::vector<CrewMember*> OurCrew()
        {
            std::vector<CrewMember*> crew;
            ShipManager *own = G_->GetShipManager(0);
            ShipManager *replica = G_->GetShipManager(1);
            if (own)
            {
                for (CrewMember *member : own->vCrewList)
                {
                    if (member && !member->IsDrone() && (g.local ? member->iShipId == 0 : !Crew::IsGuest(member))) crew.push_back(member);
                }
            }
            if (replica)
            {
                for (CrewMember *member : replica->vCrewList)
                {
                    if (member && !member->IsDrone() && (g.local ? member->iShipId == 0 : Crew::AwayId(member) >= 0)) crew.push_back(member);
                }
            }
            return crew;
        }

        static float Health(const CrewMember *crew)
        {
            return crew->bDead ? 0.f : std::max(0.f, crew->health.first);
        }

        static void StartCounting()
        {
            Damage &d = g.taken;
            d = Damage();
            ShipManager *own = G_->GetShipManager(0);
            if (!own) return;
            d.counting = true;
            d.hullPool = (float)own->ship.hullIntegrity.second;
            d.lastHull = own->ship.hullIntegrity.first;
            for (CrewMember *crew : OurCrew())
            {
                d.crewPool += crew->health.second;
                d.lastHealth[crew] = Health(crew);
            }
        }

        static void CountDamage()
        {
            Damage &d = g.taken;
            ShipManager *own = G_->GetShipManager(0);
            if (!d.counting || !own) return;
            int hull = own->ship.hullIntegrity.first;
            if (hull < d.lastHull) d.hullLost += (float)(d.lastHull - hull);
            d.lastHull = hull;

            std::vector<CrewMember*> crew = OurCrew();
            std::map<CrewMember*, float> seen;
            for (CrewMember *member : crew)
            {
                float health = Health(member);
                auto last = d.lastHealth.find(member);
                if (last != d.lastHealth.end() && health < last->second) d.crewLost += last->second - health;
                seen[member] = health;
            }
            // Gone from the lists: died (FTL removes the dead after their death).
            for (const std::pair<CrewMember* const, float> &last : d.lastHealth)
            {
                if (!seen.count(last.first)) d.crewLost += last.second;
            }
            d.lastHealth.swap(seen);
        }

        // Our ship's hull and crew health now, as shares of what it had when the fight began (anti-stall).
        static void OwnLevels(float levels[2])
        {
            const Damage &d = g.taken;
            ShipManager *own = G_->GetShipManager(0);
            levels[0] = levels[1] = 1.f;
            if (!own || !d.counting) return;
            if (d.hullPool > 0.f) levels[0] = std::max(0.f, (float)own->ship.hullIntegrity.first / d.hullPool);
            float health = 0.f;
            for (CrewMember *crew : OurCrew()) health += Health(crew);
            if (d.crewPool > 0.f) levels[1] = std::min(1.f, health / d.crewPool);
        }

        // Our ship is out of the round: its hull is gone, or its whole crew (with no clone on the way).
        static uint8_t OwnDefeat()
        {
            ShipManager *own = G_->GetShipManager(0);
            if (!own) return REASON_NONE;
            if (own->ship.hullIntegrity.first <= 0) return REASON_DESTROYED;
            if (Refit::CrewGone(own)) return REASON_CREW;
            return REASON_NONE;
        }

        // ---------------------------------------------------------------------------------------------------------
        // What each game does when the match moves on (both sides alike)
        // ---------------------------------------------------------------------------------------------------------

        // The round is decided (roadmap O): the opponent's ship is no target any more, so our weapons and drones fire
        // no new shots (FTL lets go of a target that isn't hostile); shots already in the air still land.
        static void HoldFire()
        {
            ShipManager *replica = G_->GetShipManager(1);
            if (!replica || !replica->_targetable.hostile) return;
            replica->_targetable.hostile = false;
            Log("Rounds: the round is decided: no new shots");
        }

        static void RoundCleanup()
        {
            if (g.roundCleaned) return;
            g.roundCleaned = true;
            g.taken.counting = false;
            Environment::End();
            HoldFire();
            bool ownDown = G_->GetShipManager(0) && G_->GetShipManager(0)->ship.hullIntegrity.first <= 0;
            bool theirsDown = G_->GetShipManager(1) && G_->GetShipManager(1)->ship.hullIntegrity.first <= 0;
            Refit::EndOfRound(ownDown, theirsDown);
            if (g.local) Ai::OnRoundEnd(ownDown, theirsDown);
        }

        // The ship choice's news since this game last showed it, in the feed: the bans, whose ban it is, the ships to
        // pick from, the picks (both games alike; the host's game changes the choice, the guest's learns it).
        static void ChoiceNews()
        {
            const Data &d = g.data;
            uint8_t them = Other(g.me);
            for (size_t i = g.bansShown; i < d.bans.size(); ++i)
            {
                uint8_t banner = BannerOf(d, i);
                std::string type = Ships::TypeName(d.bans[i].type);
                if (d.bans[i].byServer) Announce("Time is up: the " + type + " is banned for " + Who(banner));
                else Announce(banner == g.me ? "You ban the " + type : Who(banner) + " bans the " + type);
            }
            g.bansShown = d.bans.size();
            // (The choice's window shows whose ban it is; the console's player learns it here.)
            uint8_t turn = BansDone(d) ? NOBODY : BannerOf(d, d.bans.size());
            if (turn == g.me && g.turnShown != g.me) Log("Rounds: our ban ('ban <type>', %d s)", (int)(BAN_MS / 1000.0));
            g.turnShown = turn;
            if (!d.offer.empty() && g.offerShown != d.offer.size())
            {
                std::string list;
                for (size_t i = 0; i < d.offer.size(); ++i) list += (i ? ", " : "") + std::to_string(i + 1) + " " + Ships::Title(d.offer[i]);
                Announce("The ships to pick from: " + list);
            }
            g.offerShown = d.offer.size();
            if (d.picked[g.me] == PICK_OWN && g.pickedShown[g.me] == PICK_NONE && g.ownPick >= 0 && g.ownPick < (int)d.offer.size())
            {
                Announce("You pick the " + Ships::Title(d.offer[g.ownPick]));
            }
            if (d.picked[them] == PICK_OWN && g.pickedShown[them] == PICK_NONE) Announce(Who(them) + " has picked");
            g.pickedShown[HOST] = d.picked[HOST];
            g.pickedShown[GUEST] = d.picked[GUEST];
            // The reveal: both ships, before round 1's preparation.
            if (!d.ships[g.me].empty() && !g.shipsShown)
            {
                g.shipsShown = true;
                Log("Rounds: the ships: host %s, guest %s", d.ships[HOST].c_str(), d.ships[GUEST].c_str());
                Announce("Your ship: the " + Ships::Title(d.ships[g.me]) + (d.picked[g.me] == PICK_SERVER ? " (time was up: drawn for you)" : "") +
                         ". " + Who(them) + "'s: the " + Ships::Title(d.ships[them]));
            }
        }

        static void EnterChoice()
        {
            const Data &d = g.data;
            g.bansShown = g.offerShown = 0;
            g.pickedShown[HOST] = g.pickedShown[GUEST] = PICK_NONE;
            g.turnShown = NOBODY;
            g.shipsShown = false;
            g.ownPick = -1;
            // The match begins: the Duels window (the room's code, the waiting) makes way for the choice's (AM).
            if (::Duels::Window::IsOpen()) ::Duels::Window::Close();
            Log("Rounds: the ship choice: %s; first banner %s", ShipsModeName(d.settings).c_str(), d.firstBanner == HOST ? "host" : "guest");
            if (d.settings.ships == SHIPS_LIST) Announce("Ship choice: each picks one of the host's list");
            else
            {
                Announce("Ship choice: " + std::to_string(BansTotal(d)) + " bans in turn, " + (d.firstBanner == g.me ? std::string("you") : Who(d.firstBanner)) +
                         " first; then each picks one of the " + std::to_string(std::min(OFFER_SIZE, TypeCount(d.settings.pool))) + " ships left");
            }
            ChoiceNews();
        }

        // The reveal (roadmap 3.9): each game takes its player's ship by FTL's ship switch (Hyperspace's, as its
        // console's switch_ship), and the match starts from its levels and crew.
        static void TakeChosenShip()
        {
            const std::string &ours = g.data.ships[g.me];
            if (ours.empty()) return;
            ShipManager *own = G_->GetShipManager(0);
            WorldManager *world = G_->GetWorld();
            if (own && world && world->playerShip && own->myBlueprint.blueprintName != ours)
            {
                // The weapon and drone bays come with a ship's blueprint as FTL builds the ship (ShipManager::OnInit),
                // which the switch doesn't call: its blueprint gets them first, and its layout's cut is made.
                Bays::PrepareBlueprint(G_->GetBlueprints()->GetShipBlueprint(ours, -1));
                bool switched = world->SwitchShip(ours);
                Log("Rounds: our ship for the match: %s (%s)", ours.c_str(), switched ? "switched" : "the switch failed");
                Refit::OnMatchStart();
            }
        }

        static void EnterPrep()
        {
            const Data &d = g.data;
            Environment::End();
            Match::NewFight();
            if (d.round == 1) TakeChosenShip();
            if (d.round == 1 && g.local && !d.ships[GUEST].empty()) Ai::TakeShip(d.ships[GUEST]);   // its pick
            Refit::Restore(d.settings.permadeath);
            if (g.scrapRound != d.round)
            {
                g.scrapRound = d.round;
                Refit::GiveScrap(d.round == 1, d.scrap);
            }
            g.fightBegun = false;
            g.roundCleaned = false;
            g.defeatSent = false;
            g.taken = Damage();
            g.peerTaken = 0.f;
            g.peerLevels[0] = g.peerLevels[1] = 1.f;
            Refit::OpenShop(d.round, d.shop);
            if (g.local) Ai::OnPrep(d.round, d.scrap, d.shop, d.settings.permadeath);
            // The match begins: the Duels window (the room's code, the waiting) makes way; DUELS opens it again (AM).
            if (d.round == 1 && ::Duels::Window::IsOpen()) ::Duels::Window::Close();
            Announce("Round " + std::to_string(d.round) + " of " + std::to_string(d.settings.rounds) + ": preparation, " +
                     std::to_string(d.settings.prepSeconds) + " s (" + std::to_string(d.scrap) + " scrap, shop and upgrades)");
            // Revealed now, so that the players can prepare for it (rules, section 5).
            if (d.env.kind != Environment::NONE)
            {
                Announce(std::string("The fight is near ") + Environment::KindName(d.env.kind) + ": " +
                         Environment::KindShort(d.env.kind));
            }
            Log("Rounds: environment round %u kind %u seed %08x", (unsigned)d.round, (unsigned)d.env.kind, d.env.seed);
        }

        static void EnterStarting()
        {
            Refit::CloseShop();
            g.fightBegun = false;
            g.roundCleaned = false;
            g.defeatSent = false;
            if (!g.data.settings.free)
            {
                Note("round " + std::to_string(g.data.round) + ": the ships meet");
                MatchUi::Splash("ROUND " + std::to_string(g.data.round), MatchUi::WHITE, 1500.0, "environWarning", true);
            }
        }

        // The opponent's ship a target again. FTL lets go of a ship as a target while it isn't hostile (our weapons
        // then lose it, and with it its cloak: they would charge and fire at a cloaked ship), so it is targeted again
        // the way Hyperspace does for an enemy that turns hostile.
        static void MakeTarget()
        {
            ShipManager *replica = G_->GetShipManager(1);
            ShipManager *own = G_->GetShipManager(0);
            WorldManager *world = G_->GetWorld();
            if (!replica || !own || !world || !world->playerShip || !world->commandGui) return;
            replica->_targetable.hostile = true;
            CompleteShip *enemy = world->playerShip->enemyShip;
            if (enemy && enemy->shipManager == replica && own->current_target != replica)
            {
                world->commandGui->combatControl.Clear();
                world->commandGui->AddEnemyShip(enemy);
                Log("Rounds: the opponent's ship is our target again");
            }
        }

        static void BeginFight()
        {
            g.fightBegun = true;
            MakeTarget();
            if (!g.data.settings.free) Refit::ResetWeaponCharge();
            Refit::OnFightStart();
            StartCounting();
            if (!g.data.settings.free) Environment::Begin(g.data.env, g.data.round, FromHost(g.data.fightStart));
            if (!g.data.settings.free) Announce("Round " + std::to_string(g.data.round) + ": fight!");
            MatchUi::Splash("FIGHT!", MatchUi::GOLD, 1300.0, "surgeWarning");
        }

        // A player's points in halves: 2 for a round won, 1 for a round drawn (rules, section 1: as in chess), 1 for a
        // round the other player ran away from (roadmap AD).
        static int Halves(const Data &d, uint8_t player)
        {
            return 2 * d.wins[player] + d.draws + d.halfWins[player];
        }

        // "2", "1.5".
        static std::string Points(const Data &d, uint8_t player)
        {
            int halves = Halves(d, player);
            return std::to_string(halves / 2) + (halves % 2 ? ".5" : "");
        }

        static std::string PointsLine(const Data &d, uint8_t first, uint8_t second)
        {
            return Points(d, first) + " : " + Points(d, second);
        }

        static std::string ScoreLine()
        {
            const Data &d = g.data;
            uint8_t them = Other(g.me);
            return "points " + PointsLine(d, g.me, them) + ", damage score " + Number(d.score[g.me]) + " : " + Number(d.score[them]);
        }

        static void EnterRoundOver()
        {
            RoundCleanup();
            const Data &d = g.data;
            if (d.results.empty()) return;
            const Result &result = d.results.back();
            uint8_t them = Other(g.me);
            // The same words in both games (tools/analyze-duel.py compares them).
            Log("Rounds: result round %u winner %s reason %u dealt %.1f %.1f points %s %s", (unsigned)d.results.size(),
                result.winner == NOBODY ? "nobody" : result.winner == HOST ? "host" : "guest", (unsigned)result.reason,
                result.dealt[HOST], result.dealt[GUEST], Points(d, HOST).c_str(), Points(d, GUEST).c_str());
            std::string text = "Round " + std::to_string(d.results.size()) + ": ";
            text += result.winner == NOBODY ? "a draw" : result.winner == g.me ? "you win it" : Who(them) + " wins it";
            if (result.reason == REASON_ESCAPED)
            {
                // Running away (roadmap AD): the runner is gone, the other takes half a point.
                if (result.winner == g.me) MatchUi::Splash("ENEMY ESCAPED", g.me == HOST ? MatchUi::RED : MatchUi::BLUE, 2500.0, "jumpLeave");
                else MatchUi::Splash("ESCAPED", MatchUi::WHITE, 2500.0, "jumpLeave");
                text = "Round " + std::to_string(d.results.size()) + ": " +
                       (result.winner == g.me ? Who(them) + " ran away, half a point for you" : std::string("you ran away, half a point for ") + Who(them));
            }
            else if (result.winner == NOBODY) MatchUi::Splash("DRAW", MatchUi::WHITE, 2500.0, "jumpReady");
            else if (result.winner == g.me) MatchUi::Splash("YOU WIN", g.me == HOST ? MatchUi::RED : MatchUi::BLUE, 2500.0, "achievement");
            else MatchUi::Splash("YOU LOSE", g.me == HOST ? MatchUi::BLUE : MatchUi::RED, 2500.0, "powerUpFail");
            Announce(text + " (" + ReasonText(result.reason) + "). Points " + PointsLine(d, g.me, them));
            Note("round " + std::to_string(d.results.size()) + ": damage dealt " + Number(result.dealt[g.me]) + " : " +
                 Number(result.dealt[them]) + ", " + ScoreLine());
        }

        static void EnterMatchOver()
        {
            RoundCleanup();
            const Data &d = g.data;
            Log("Rounds: match winner %s reason %u points %s %s score %.1f %.1f rounds %u",
                d.matchWinner == NOBODY ? "nobody" : d.matchWinner == HOST ? "host" : "guest", (unsigned)d.matchReason,
                Points(d, HOST).c_str(), Points(d, GUEST).c_str(), d.score[HOST], d.score[GUEST], (unsigned)d.results.size());
            uint8_t them = Other(g.me);
            std::string text = "Match over: ";
            text += d.matchWinner == NOBODY ? "a draw" : d.matchWinner == g.me ? "you win" : Who(d.matchWinner) + " wins";
            if (d.matchWinner == NOBODY) MatchUi::Splash("MATCH DRAWN", MatchUi::WHITE, 4000.0, "jumpReady");
            else if (d.matchWinner == g.me) MatchUi::Splash("MATCH WON", g.me == HOST ? MatchUi::RED : MatchUi::BLUE, 4000.0, "victory");
            else MatchUi::Splash("MATCH LOST", g.me == HOST ? MatchUi::BLUE : MatchUi::RED, 4000.0, "powerUpFail");
            Announce(text + ", " + PointsLine(d, g.me, them) + " (" + ReasonText(d.matchReason) + ")");
            Note("match over: " + ScoreLine());
        }

        static void ApplyLocal()
        {
            const Data &d = g.data;
            if (d.phase == g.appliedPhase && d.round == g.appliedRound) return;
            g.appliedPhase = d.phase;
            g.appliedRound = d.round;
            Log("Rounds: phase %s, round %u", PhaseName(d.phase), (unsigned)d.round);
            switch (d.phase)
            {
            case Phase::Choice: EnterChoice(); break;
            case Phase::Prep: EnterPrep(); break;
            case Phase::Starting: EnterStarting(); break;
            case Phase::Ending:   // a ship is down: no more flares or rocks, and no new shots
                Environment::End();
                HoldFire();
                break;
            case Phase::RoundOver: EnterRoundOver(); break;
            case Phase::MatchOver: EnterMatchOver(); break;
            default: break;
            }
        }

        // ---------------------------------------------------------------------------------------------------------
        // The host runs the match
        // ---------------------------------------------------------------------------------------------------------

        static void SetPhase(Phase phase, double end)
        {
            g.data.phase = phase;
            g.data.phaseEnd = end;
            g.dirty = true;
            ApplyLocal();
        }

        static void ClearDraw()
        {
            g.data.drawBy = NOBODY;
            g.data.drawScope = DRAW_NONE;
            g.data.drawEnd = -1.0;
        }

        static void StartRound(uint8_t round)
        {
            Data &d = g.data;
            d.round = round;
            d.ready[HOST] = d.ready[GUEST] = false;
            d.fightStart = -1.0;
            d.scrap = d.settings.free ? 0 : ScrapFor(round);
            d.shop = d.settings.free ? std::vector<Refit::ShopItem>() : Refit::MakeStock(round, g.random);
            d.env = d.settings.free ? Environment::Plan() : Environment::Roll(d.settings.env, d.settings.hazards, round, g.random);
            g.defeatAt[HOST] = g.defeatAt[GUEST] = -1.0;
            g.defeatReason[HOST] = g.defeatReason[GUEST] = REASON_NONE;
            ClearDraw();
            if (d.settings.free) SetPhase(Phase::Starting, -1.0);
            else SetPhase(Phase::Prep, Now() + d.settings.prepSeconds * 1000.0);
        }

        // The ships to pick from: the host's list, or each type the bans left, with one of its layouts at random (the
        // server's, rules section 4).
        static void MakeOffer()
        {
            Data &d = g.data;
            d.offer.clear();
            if (d.settings.ships == SHIPS_LIST) d.offer = d.settings.list;
            else
            {
                uint16_t left = TypesLeft(d);
                for (int type = 0; type < Ships::TYPE_COUNT; ++type)
                {
                    std::vector<std::string> variants = Ships::Variants(type);
                    if (!(left & (1 << type)) || variants.empty()) continue;
                    d.offer.push_back(variants[std::uniform_int_distribution<size_t>(0, variants.size() - 1)(g.random)]);
                }
            }
            d.phaseEnd = Now() + PICK_MS;
            g.dirty = true;
        }

        // Test verb "match firstban host|guest|random": who bans first in the next matches (else the server's draw).
        static uint8_t g_firstBanner = NOBODY;

        // Before round 1 (roadmap 3.9): the server draws who bans first.
        static void StartChoice()
        {
            Data &d = g.data;
            d.round = 0;
            d.firstBanner = g_firstBanner != NOBODY ? g_firstBanner : std::uniform_int_distribution<int>(0, 1)(g.random) ? GUEST : HOST;
            d.bans.clear();
            d.offer.clear();
            d.picked[HOST] = d.picked[GUEST] = PICK_NONE;
            d.ships[HOST].clear();
            d.ships[GUEST].clear();
            g.picks[HOST] = g.picks[GUEST] = -1;
            if (BansDone(d)) MakeOffer();   // a list, or a pool of three types or fewer
            if (BansDone(d) && d.offer.empty())
            {
                Note("no ships to choose from: each player keeps their own");
                StartRound(1);
                return;
            }
            SetPhase(Phase::Choice, Now() + (d.offer.empty() ? BAN_MS : PICK_MS));
        }

        // Both have picked: both games learn both ships and show them; the first preparation begins with them.
        static void Reveal()
        {
            Data &d = g.data;
            for (uint8_t player : {HOST, GUEST})
            {
                int pick = g.picks[player] >= 0 && g.picks[player] < (int)d.offer.size() ? g.picks[player] : 0;
                d.ships[player] = d.offer.empty() ? std::string() : d.offer[pick];
            }
            d.phaseEnd = Now() + REVEAL_MS;
            g.dirty = true;
        }

        static void AddBan(uint8_t type, bool byServer)
        {
            Data &d = g.data;
            ShipBan ban;
            ban.type = type;
            ban.byServer = byServer;
            Log("Rounds: ban %u: %s by %s%s", (unsigned)d.bans.size() + 1, Ships::TypeWord(type),
                BannerOf(d, d.bans.size()) == HOST ? "host" : "guest", byServer ? " (time was up)" : "");
            d.bans.push_back(ban);
            g.dirty = true;
            if (BansDone(d)) MakeOffer();
            else d.phaseEnd = Now() + BAN_MS;
        }

        // Time is up (rules, section 4): the server bans a type at random for the player whose ban it is, or picks a
        // ship at random for each player who hasn't.
        static void ChoiceTimeout()
        {
            Data &d = g.data;
            if (!BansDone(d))
            {
                std::vector<uint8_t> types;
                uint16_t left = TypesLeft(d);
                for (int type = 0; type < Ships::TYPE_COUNT; ++type)
                {
                    if (left & (1 << type)) types.push_back((uint8_t)type);
                }
                if (types.empty()) MakeOffer();
                else AddBan(types[std::uniform_int_distribution<size_t>(0, types.size() - 1)(g.random)], true);
                return;
            }
            for (uint8_t player : {HOST, GUEST})
            {
                if (d.picked[player] != PICK_NONE || d.offer.empty()) continue;
                g.picks[player] = std::uniform_int_distribution<int>(0, (int)d.offer.size() - 1)(g.random);
                d.picked[player] = PICK_SERVER;
                Log("Rounds: time was up: the %s's ship is picked: %s", player == HOST ? "host" : "guest", d.offer[g.picks[player]].c_str());
            }
            Reveal();
        }

        static void EndMatch(uint8_t winner, uint8_t reason)
        {
            g.data.matchWinner = winner;
            g.data.matchReason = reason;
            ClearDraw();
            Net::SetMatchToken(0);
            SetPhase(Phase::MatchOver, -1.0);
        }

        // The damage each player dealt this round: what the other's ship took.
        static void DealtThisRound(float dealt[2])
        {
            float hostTaken = g.me == HOST ? g.taken.Taken() : g.peerTaken;
            float guestTaken = g.me == HOST ? g.peerTaken : g.taken.Taken();
            dealt[HOST] = guestTaken;
            dealt[GUEST] = hostTaken;
        }

        static void FinishRound(uint8_t winner, uint8_t reason)
        {
            Data &d = g.data;
            Result result;
            result.winner = winner;
            result.reason = reason;
            DealtThisRound(result.dealt);
            d.results.push_back(result);
            if (winner != NOBODY && reason == REASON_ESCAPED) ++d.halfWins[winner];
            else if (winner != NOBODY) ++d.wins[winner];
            else ++d.draws;
            d.score[HOST] += result.dealt[HOST];
            d.score[GUEST] += result.dealt[GUEST];
            ClearDraw();
            SetPhase(Phase::RoundOver, Now() + RESULT_MS);
        }

        // One player has more points than the other can still reach (each round left is worth a point), or the rounds
        // are used up.
        static bool MatchDecided(uint8_t &winner, uint8_t &reason)
        {
            const Data &d = g.data;
            int left = std::max(0, (int)d.settings.rounds - (int)d.results.size());
            int host = Halves(d, HOST), guest = Halves(d, GUEST);
            if (host > guest + 2 * left || guest > host + 2 * left || (left == 0 && host != guest))
            {
                winner = host > guest ? HOST : GUEST;
                reason = REASON_ROUNDS;
                return true;
            }
            if (left > 0) return false;
            reason = REASON_SCORE;
            winner = std::fabs(d.score[HOST] - d.score[GUEST]) < SCORE_TIE ? NOBODY : d.score[HOST] > d.score[GUEST] ? HOST : GUEST;
            return true;
        }

        // The first ship's explosion is over: who won the round.
        static void DecideRound()
        {
            bool hostDown = g.defeatAt[HOST] >= 0.0, guestDown = g.defeatAt[GUEST] >= 0.0;
            if (hostDown && guestDown)
            {
                float dealt[2];
                DealtThisRound(dealt);
                uint8_t winner = std::fabs(dealt[HOST] - dealt[GUEST]) < SCORE_TIE ? NOBODY : dealt[HOST] > dealt[GUEST] ? HOST : GUEST;
                FinishRound(winner, REASON_BOTH);
            }
            else if (hostDown) FinishRound(GUEST, g.defeatReason[HOST]);
            else FinishRound(HOST, g.defeatReason[GUEST]);
        }

        static void HostEvent(uint8_t player, uint8_t type, uint8_t arg)
        {
            Data &d = g.data;
            double now = Now();
            switch (type)
            {
            case EV_READY:
                if (d.phase == Phase::Prep && !d.ready[player])
                {
                    d.ready[player] = true;
                    g.dirty = true;
                    Announce(player == g.me ? std::string("You are ready") : Who(player) + " is ready");
                }
                break;
            case EV_UNREADY:
                if (d.phase == Phase::Prep && d.ready[player])
                {
                    d.ready[player] = false;
                    g.dirty = true;
                    Announce(player == g.me ? std::string("You are not ready any more") : Who(player) + " is not ready any more");
                }
                break;
            case EV_DEFEAT:
                if ((d.phase == Phase::Fight || d.phase == Phase::Ending) && g.defeatAt[player] < 0.0)
                {
                    g.defeatAt[player] = now;
                    g.defeatReason[player] = arg == REASON_CREW ? REASON_CREW : REASON_DESTROYED;
                    Log("Rounds: %s is out (%s)", player == HOST ? "the host" : "the guest", ReasonText(g.defeatReason[player]));
                    if (d.phase == Phase::Fight) SetPhase(Phase::Ending, now + ENDING_MS);
                }
                break;
            case EV_FORFEIT:
                if (d.phase != Phase::None && d.phase != Phase::MatchOver)
                {
                    // The opponent wins the rounds still to play, this one too (rules, section 3).
                    uint8_t other = Other(player);
                    int left = std::max(0, (int)d.settings.rounds - (int)d.results.size());
                    d.wins[other] = (uint8_t)std::min(255, d.wins[other] + left);
                    EndMatch(other, REASON_FORFEIT);
                }
                break;
            case EV_CONCEDE:
                if (d.phase == Phase::Starting || d.phase == Phase::Fight) FinishRound(Other(player), REASON_CONCEDED);
                break;
            case EV_ESCAPE:
                // Only in the fight itself: once a ship is down (Ending) the round is decided already.
                if (d.phase == Phase::Fight && g.defeatAt[player] < 0.0) FinishRound(Other(player), REASON_ESCAPED);
                break;
            case EV_DRAW_OFFER:
            {
                bool roundOk = arg == DRAW_ROUND && (d.phase == Phase::Starting || d.phase == Phase::Fight);
                bool matchOk = arg == DRAW_MATCH && (d.phase == Phase::Prep || d.phase == Phase::Starting || d.phase == Phase::Fight);
                if (d.drawBy == NOBODY && (roundOk || matchOk) && now - g.lastOffer[player] >= DRAW_AGAIN_MS)
                {
                    // It stands until it is answered or taken back (AT); the round's end ends it.
                    d.drawBy = player;
                    d.drawScope = arg;
                    d.drawEnd = -1.0;
                    g.lastOffer[player] = now;
                    g.dirty = true;
                    Announce(player == g.me ? std::string("You offer a draw for the ") + (arg == DRAW_MATCH ? "match" : "round")
                                            : Who(player) + " offers a draw for the " + (arg == DRAW_MATCH ? "match" : "round") +
                                                  (arg == DRAW_MATCH ? ": accept in the Duels window" : ": the DRAW button accepts"));
                }
                break;
            }
            case EV_DRAW_BACK:
                if (d.drawBy == player)
                {
                    ClearDraw();
                    g.dirty = true;
                    Announce(player == g.me ? std::string("You take back your draw offer") : Who(player) + " takes back the draw offer");
                }
                break;
            case EV_BAN:
                // In the player's turn, a type still in the choice (roadmap 3.9).
                if (d.phase == Phase::Choice && !BansDone(d) && BannerOf(d, d.bans.size()) == player && arg < Ships::TYPE_COUNT &&
                    (TypesLeft(d) & (1 << arg)))
                {
                    AddBan(arg, false);
                }
                break;
            case EV_PICK:
                if (d.phase == Phase::Choice && BansDone(d) && d.picked[player] == PICK_NONE && arg < d.offer.size())
                {
                    g.picks[player] = arg;
                    d.picked[player] = PICK_OWN;
                    g.dirty = true;
                    Log("Rounds: the %s has picked", player == HOST ? "host" : "guest");
                    if (d.picked[HOST] != PICK_NONE && d.picked[GUEST] != PICK_NONE) Reveal();
                }
                break;
            case EV_DRAW_ANSWER:
                if (d.drawBy == Other(player))
                {
                    uint8_t scope = d.drawScope;
                    if (arg && scope == DRAW_MATCH) EndMatch(NOBODY, REASON_DRAW);
                    else if (arg && scope == DRAW_ROUND && (d.phase == Phase::Starting || d.phase == Phase::Fight)) FinishRound(NOBODY, REASON_DRAW);
                    else
                    {
                        ClearDraw();
                        g.dirty = true;
                        Announce(std::string("The draw offer was ") + (arg ? "too late" : "declined"));
                    }
                }
                break;
            default:
                break;
            }
        }

        // Anti-stall (rules, section 3): every new low of a player's hull or crew health restarts the timer. When it
        // runs out, each player's score is 0.65 x their lowest hull + 0.35 x their lowest crew health (shares of the
        // round's start); the higher one wins the round, a lead under 10 (of 100) is a draw.
        static void CheckStall(double now)
        {
            Data &d = g.data;
            float levels[2][2];
            OwnLevels(levels[HOST]);
            levels[GUEST][0] = g.peerLevels[0];
            levels[GUEST][1] = g.peerLevels[1];
            bool newLow = false;
            for (int player = 0; player < 2; ++player)
            {
                for (int pool = 0; pool < 2; ++pool)
                {
                    if (levels[player][pool] < g.lows[player][pool] - STALL_STEP)
                    {
                        g.lows[player][pool] = levels[player][pool];
                        newLow = true;
                    }
                }
            }
            if (newLow)
            {
                d.stallEnd = now + d.settings.stallSeconds * 1000.0;
                g.dirty = true;
                return;
            }
            if (now < d.stallEnd) return;
            float score[2];
            for (int player = 0; player < 2; ++player)
            {
                score[player] = 100.f * (HULL_WEIGHT * g.lows[player][0] + (1.f - HULL_WEIGHT) * g.lows[player][1]);
            }
            Log("Rounds: no new low for %u s: stall scores %.1f (host) %.1f (guest)", (unsigned)d.settings.stallSeconds, score[HOST],
                score[GUEST]);
            uint8_t winner = std::fabs(score[HOST] - score[GUEST]) < STALL_DRAW_LEAD ? NOBODY : score[HOST] > score[GUEST] ? HOST : GUEST;
            FinishRound(winner, REASON_STALL);
        }

        static void HostFrame(double now)
        {
            Data &d = g.data;
            switch (d.phase)
            {
            case Phase::Choice:
                if (now >= d.phaseEnd && !d.ships[HOST].empty()) StartRound(1);   // the reveal is over
                else if (now >= d.phaseEnd) ChoiceTimeout();
                break;
            case Phase::Prep:
                if (now >= d.phaseEnd || (d.ready[HOST] && d.ready[GUEST])) SetPhase(Phase::Starting, -1.0);
                break;
            case Phase::Starting:
                if (g.local ? Ai::ShipStands() : Match::ShipsStand())
                {
                    d.fightStart = d.settings.free ? now : now + STARTING_LEAD_MS;
                    d.stallEnd = d.settings.stallSeconds > 0 ? d.fightStart + d.settings.stallSeconds * 1000.0 : -1.0;
                    for (float (&low)[2] : g.lows) low[0] = low[1] = 1.f;
                    SetPhase(Phase::Fight, -1.0);
                }
                break;
            case Phase::Fight:
                if (g.fightBegun && d.stallEnd >= 0.0) CheckStall(now);
                break;
            case Phase::Ending:
                if (now >= d.phaseEnd) DecideRound();
                break;
            case Phase::RoundOver:
                if (now >= d.phaseEnd)
                {
                    uint8_t winner, reason;
                    if (MatchDecided(winner, reason)) EndMatch(winner, reason);
                    else StartRound((uint8_t)(d.round + 1));
                }
                break;
            default:
                break;
            }
            if (g.dirty) SendData();
        }

        // ---------------------------------------------------------------------------------------------------------
        // Entry points
        // ---------------------------------------------------------------------------------------------------------

        // The host's settings as last set, from duels.cfg (roadmap U): read once, before the first use.
        static bool g_settingsLoaded = false;

        static void LoadSettings()
        {
            if (g_settingsLoaded) return;
            g_settingsLoaded = true;
            if (!SettingsFromConfig()) return;   // a test scenario starts from the defaults
            Settings &s = g.settings;
            int value = std::atoi(Config::Value("match_rounds").c_str());
            if (value >= 1 && value <= 99) s.rounds = (uint8_t)value;
            std::string text = Config::Value("match_prep");
            value = std::atoi(text.c_str());
            if (!text.empty() && value >= 0 && value <= 3600) s.prepSeconds = (uint16_t)value;
            text = Config::Value("match_stall");
            value = std::atoi(text.c_str());
            if (!text.empty() && value >= 0 && value <= 3600) s.stallSeconds = (uint16_t)value;
            text = Config::Value("match_permadeath");
            if (text == "on" || text == "off") s.permadeath = text == "on";
            text = Config::Value("match_free");
            if (text == "on" || text == "off") s.free = text == "on";
            uint8_t mode;
            if (Environment::ParseMode(Config::Value("match_env"), mode)) s.env = mode;
            uint8_t hazards;
            text = Config::Value("match_hazards");
            if (!text.empty() && Environment::ParseHazards(text, hazards)) s.hazards = hazards;
            text = Config::Value("match_record");
            if (text == "on" || text == "off") s.record = text == "on";
            // A player's match chooses its ships by bans unless the host set it otherwise (a test scenario starts from
            // each player's own ship, the hangar's).
            text = Config::Value("match_ships");
            s.ships = text == "own" ? SHIPS_OWN : text == "list" ? SHIPS_LIST : SHIPS_BANS;
            uint16_t pool;
            if (Ships::ParseTypes(Config::Value("match_pool"), pool)) s.pool = pool;
            std::vector<std::string> list;
            std::stringstream ships(Config::Value("match_list"));
            std::string ship, blueprint;
            while (std::getline(ships, ship, ','))
            {
                if (Ships::ParseShip(ship, blueprint)) list.push_back(blueprint);
            }
            if (!list.empty()) s.list = list;
        }

        // Kept for the next start; a test scenario leaves duels.cfg as it is (the next test starts from its own).
        static void SaveSettings()
        {
            if (!SettingsFromConfig()) return;
            const Settings &s = g.settings;
            Config::SaveValue("match_rounds", std::to_string(s.rounds));
            Config::SaveValue("match_prep", std::to_string(s.prepSeconds));
            Config::SaveValue("match_stall", std::to_string(s.stallSeconds));
            Config::SaveValue("match_permadeath", s.permadeath ? "on" : "off");
            Config::SaveValue("match_free", s.free ? "on" : "off");
            Config::SaveValue("match_env", Environment::ModeName(s.env));
            std::string hazards = Environment::HazardsName(s.hazards);   // "sun, pulsar" or "none"
            hazards.erase(std::remove(hazards.begin(), hazards.end(), ' '), hazards.end());
            Config::SaveValue("match_hazards", hazards);
            Config::SaveValue("match_record", s.record ? "on" : "off");
            Config::SaveValue("match_ships", s.ships == SHIPS_BANS ? "bans" : s.ships == SHIPS_LIST ? "list" : "own");
            Config::SaveValue("match_pool", Ships::TypesText(s.pool));
            std::string list;
            for (const std::string &ship : s.list) list += (list.empty() ? "" : ",") + ship;
            Config::SaveValue("match_list", list);
        }

        void Reset()
        {
            LoadSettings();
            Settings settings = g.settings;
            g = Local();
            g.settings = settings;
        }

        // The host's draws (the shops, the environments, the ship choice): from the system clock's nanoseconds (the game's
        // own milliseconds since its start came out nearly the same at every test's connection, and so did the draws).
        static uint32_t NewSeed()
        {
            uint64_t ticks = (uint64_t)std::chrono::system_clock::now().time_since_epoch().count();
            uint32_t seed = (uint32_t)(ticks ^ (ticks >> 32)) ^ (uint32_t)(WallMs() * 1000.0);
            Log("Rounds: the match's draws from seed %08x", seed);
            return seed;
        }

        void OnConnected()
        {
            if (Net::Resumed() && g.data.phase != Phase::None && g.data.phase != Phase::MatchOver)
            {
                // Back after a lost connection: the same match goes on (the host sends its state again). Its timers
                // waited while it was paused: the host moves them on by the pause (the guest follows the host's, and
                // moves its environment's schedule when the new fight start arrives).
                g.active = true;
                double paused = g.pausedSince >= 0.0 ? Now() - g.pausedSince : 0.0;
                g.pausedSince = -1.0;
                if (g.me == HOST)
                {
                    Data &d = g.data;
                    for (double *time : {&d.phaseEnd, &d.fightStart, &d.stallEnd, &d.drawEnd})
                    {
                        if (*time >= 0.0) *time += paused;
                    }
                    Environment::Shift(paused);
                    g.dirty = true;
                }
                Log("Rounds: the match goes on after the lost connection (%s, round %u, paused %.1f s)", PhaseName(g.data.phase),
                    (unsigned)g.data.round, paused / 1000.0);
                return;
            }
            Reset();
            Environment::ClearBeacon();
            g.active = true;
            g.me = Net::IsHost() ? HOST : GUEST;
            g.random.seed(NewSeed() ^ (uint32_t)(Net::MatchSeed() & 0xffffffffu));
            Refit::OnMatchStart();
            if (g.me != HOST) return;   // the host's match state comes
            g.data = Data();
            g.data.settings = g.settings;
            g.data.token = ((uint64_t)g.random() << 32) | g.random();
            if (g.data.token == 0) g.data.token = 1;
            Net::SetMatchToken(g.data.token);
            if (g.data.settings.free) g.data.settings.rounds = 1;
            Announce(g.data.settings.free ? std::string("A free fight (no rounds)")
                                          : "A match of best of " + std::to_string(g.data.settings.rounds) + " rounds, " +
                                                std::to_string(g.data.settings.prepSeconds) + " s preparation, permanent death " +
                                                (g.data.settings.permadeath ? "on" : "off"));
            if (ChoosesShips(g.data.settings)) StartChoice();
            else StartRound(1);
        }

        void StartLocal()
        {
            Reset();
            Environment::ClearBeacon();
            g.active = true;
            g.local = true;
            g.me = HOST;
            g.random.seed(NewSeed());
            Refit::OnMatchStart();
            g.data = Data();
            g.data.settings = g.settings;
            g.data.token = 1;
            if (g.data.settings.free) g.data.settings.rounds = 1;
            Announce(g.data.settings.free ? std::string("A free fight against the AI (no rounds)")
                                          : "A match against the AI: best of " + std::to_string(g.data.settings.rounds) + " rounds, " +
                                                std::to_string(g.data.settings.prepSeconds) + " s preparation, permanent death " +
                                                (g.data.settings.permadeath ? "on" : "off"));
            // The ships are chosen as in a duel, the AI banning and picking in its turns (DuelsAi.cpp); with each
            // player's own, ours is the hangar's and the AI's the one HOST DUEL's window set.
            if (ChoosesShips(g.data.settings)) StartChoice();
            else StartRound(1);
        }

        bool NextChoosesShips()
        {
            LoadSettings();
            return ChoosesShips(g.settings);
        }

        void OpponentBan(int type)
        {
            if (g.local && g.active && type >= 0 && type < Ships::TYPE_COUNT) HostEvent(GUEST, EV_BAN, (uint8_t)type);
        }

        void OpponentPick(int index)
        {
            if (g.local && g.active && index >= 0 && index < 256) HostEvent(GUEST, EV_PICK, (uint8_t)index);
        }

        bool IsLocal()
        {
            return g.local;
        }

        void OpponentReady()
        {
            if (g.local && g.active) HostEvent(GUEST, EV_READY, 0);
        }

        void OpponentDefeated(bool crewDead)
        {
            if (g.local && g.active) HostEvent(GUEST, EV_DEFEAT, crewDead ? REASON_CREW : REASON_DESTROYED);
        }

        void OpponentState(float hullLost, float crewLost, float hullShare, float crewShare)
        {
            if (!g.local || !g.active || (g.data.phase != Phase::Fight && g.data.phase != Phase::Ending)) return;
            g.peerTaken = 100.f * (HULL_WEIGHT * hullLost + (1.f - HULL_WEIGHT) * crewLost);
            g.peerLevels[0] = hullShare;
            g.peerLevels[1] = crewShare;
        }

        void OnConnectionLost()
        {
            if (!g.active || g.pausedSince >= 0.0) return;
            Phase phase = g.data.phase;
            if (phase == Phase::None || phase == Phase::MatchOver) return;
            g.pausedSince = Now();
            Log("Rounds: the match is paused while the connection is lost");
        }

        bool NetPaused()
        {
            return g.active && g.pausedSince >= 0.0;
        }

        void OnDisconnected(bool opponentGone)
        {
            Net::SetMatchToken(0);
            Environment::End();
            g.pausedSince = -1.0;
            if (!g.active) return;
            g.active = false;
            Data &d = g.data;
            bool running = d.phase != Phase::None && d.phase != Phase::MatchOver;
            if (running)
            {
                // Until rejoining is built: the player still here wins, if the other left (rules, section 3).
                if (opponentGone)
                {
                    d.matchWinner = g.me;
                    d.matchReason = REASON_LEFT;
                    d.phase = Phase::MatchOver;
                    g.appliedPhase = Phase::MatchOver;
                    RoundCleanup();
                    Announce("Match over: you win (the other player left)");
                }
                else
                {
                    RoundCleanup();
                    Announce("The match ended: the connection is lost");
                }
            }
            Match::NewFight();
        }

        void OnMessage(uint8_t type, Reader &r)
        {
            if (!g.active) return;
            if (type == MSG_MATCH)
            {
                if (g.me == HOST) return;
                Data data;
                if (!ReadData(r, data))
                {
                    Log("Rounds: malformed match state");
                    return;
                }
                uint8_t drawBefore = g.data.drawBy;
                Phase phaseBefore = g.data.phase;
                bool readyBefore = g.data.ready[HOST];
                // The host moved the fight's start on by a pause (a lost connection): the environment's schedule too.
                if (g.fightBegun && data.round == g.data.round && data.fightStart >= 0.0 && g.data.fightStart >= 0.0 &&
                    data.fightStart != g.data.fightStart)
                {
                    Environment::Shift(data.fightStart - g.data.fightStart);
                }
                g.data = data;
                Net::SetMatchToken(data.phase == Phase::MatchOver ? 0 : data.token);
                if (data.drawBy == HOST && drawBefore != HOST)
                {
                    g.drawAnswered = false;
                    Announce(Who(HOST) + " offers a draw for the " + (data.drawScope == DRAW_MATCH ? "match" : "round") +
                             (data.drawScope == DRAW_MATCH ? ": accept in the Duels window" : ": the DRAW button accepts"));
                }
                // The host's offer gone in the same phase, unanswered: taken back (AT).
                if (drawBefore == HOST && data.drawBy == NOBODY && data.phase == phaseBefore && !g.drawAnswered)
                {
                    Announce(Who(HOST) + " takes back the draw offer");
                }
                if (data.ready[HOST] && !readyBefore && data.phase == Phase::Prep) Announce(Who(HOST) + " is ready");
                ApplyLocal();
            }
            else if (type == MSG_MATCH_EVENT)
            {
                uint8_t event = r.U8();
                uint8_t arg = r.U8();
                uint8_t round = r.U8();
                if (!r.Ok()) return;
                ++g.eventsReceived;
                if (g.me != HOST) return;   // events go to the host
                // An event of an earlier round (a defeat reported as the round ended) doesn't count in this one, nor a
                // ban made for an earlier turn (the server banned for the player as it came).
                if (event == EV_BAN ? round != g.data.bans.size() : round != g.data.round && event != EV_FORFEIT) return;
                HostEvent(GUEST, event, arg);
            }
        }

        // Our own actions: the host takes them at once, the guest tells the host.
        static void OwnEvent(uint8_t type, uint8_t arg)
        {
            if (g.me == HOST) HostEvent(HOST, type, arg);
            else SendEvent(type, arg);
        }

        void OnFrame(double now)
        {
            if (!g.active || !(g.local || Net::IsConnected())) return;
            Data &d = g.data;
            if (g.local) Ai::OnFrame();
            if (d.phase == Phase::Choice) ChoiceNews();

            if (d.phase == Phase::Fight && !g.fightBegun && d.fightStart >= 0.0 && now >= FromHost(d.fightStart)) BeginFight();
            if (g.fightBegun && (d.phase == Phase::Fight || d.phase == Phase::Ending)) CountDamage();

            // Our defeat, once per round.
            if (g.fightBegun && !g.defeatSent && (d.phase == Phase::Fight || d.phase == Phase::Ending))
            {
                uint8_t reason = OwnDefeat();
                if (reason != REASON_NONE)
                {
                    g.defeatSent = true;
                    Announce(reason == REASON_CREW ? "Your crew is dead" : "Your ship is destroyed");
                    OwnEvent(EV_DEFEAT, reason);
                }
            }

            if (g.me == HOST) HostFrame(now);

            // The last seconds of the preparation, counted down.
            if (d.phase == Phase::Prep && d.phaseEnd >= 0.0)
            {
                // Only marks passed while the preparation runs (a short one starts below some of them).
                double left = (FromHost(d.phaseEnd) - now) / 1000.0;
                for (double mark : {30.0, 10.0, 5.0})
                {
                    if (left <= mark && g.lastSecondsShown > mark) Announce(std::to_string((int)mark) + " s to the fight");
                }
                // The last five seconds, big in the middle of the screen with a beep each (roadmap AC).
                for (int mark = 5; mark >= 1; --mark)
                {
                    if (left <= mark && g.lastSecondsShown > mark) MatchUi::Splash(std::to_string(mark), MatchUi::WHITE, 900.0, "powerUpSystem");
                }
                g.lastSecondsShown = left;
            }
            else g.lastSecondsShown = -1.0;
        }

        Phase GetPhase()
        {
            return g.active ? g.data.phase : Phase::None;
        }

        bool Free()
        {
            return g.data.settings.free;
        }

        bool ShipsMeet()
        {
            // Until the opponent's ship leaves at the next preparation (or the connection ends).
            if (!g.active) return false;
            Phase phase = g.data.phase;
            return phase == Phase::Starting || phase == Phase::Fight || phase == Phase::Ending || phase == Phase::RoundOver ||
                   phase == Phase::MatchOver;
        }

        bool FightBegun()
        {
            return g.active && g.fightBegun;
        }

        void OnReplicaBuilt(ShipManager *replica)
        {
            // Not a target before a round's fight begins (a free fight begins at once).
            if (replica && g.active && !g.fightBegun && !g.data.settings.free) replica->_targetable.hostile = false;
        }

        void WriteState(Writer &w)
        {
            float levels[2];
            OwnLevels(levels);
            w.F32(g.taken.Taken());
            w.F32(levels[0]);
            w.F32(levels[1]);
            ++g.statesSent;
        }

        bool ReadState(Reader &r)
        {
            float taken = r.F32();
            float hull = r.F32();
            float crew = r.F32();
            if (!r.Ok()) return false;
            if (g.active && (g.data.phase == Phase::Fight || g.data.phase == Phase::Ending))
            {
                g.peerTaken = taken;
                g.peerLevels[0] = hull;
                g.peerLevels[1] = crew;
            }
            ++g.statesReceived;
            return true;
        }

        bool GameOverAllowed()
        {
            return !g.active;
        }

        bool ShoppingAllowed()
        {
            return !g.active || g.data.phase == Phase::Prep;
        }

        bool InPreparation()
        {
            return g.active && g.data.phase == Phase::Prep;
        }

        bool InMatch()
        {
            return g.active;
        }

        bool EscapeAllowed()
        {
            return g.active && g.data.phase == Phase::Fight && g.defeatAt[g.me] < 0.0;
        }

        bool DriveReady()
        {
            ShipManager *own = G_->GetShipManager(0);
            return own && own->jump_timer.first >= own->jump_timer.second && own->SystemFunctions(SYS_ENGINES) &&
                   own->SystemFunctions(SYS_PILOT);
        }

        bool Escape(std::string &message)
        {
            if (!EscapeAllowed())
            {
                message = "running away is for a match's fight";
                return false;
            }
            OwnEvent(EV_ESCAPE, 0);
            message = "you jump away";
            ShipManager *own = G_->GetShipManager(0);
            Log("Rounds: we jump away (the FTL drive at %.1f of %.1f)", own ? own->jump_timer.first : -1.f, own ? own->jump_timer.second : -1.f);
            return true;
        }

        // ---------------------------------------------------------------------------------------------------------
        // Verbs
        // ---------------------------------------------------------------------------------------------------------

        bool IsVerb(const std::string &verb)
        {
            return verb == "match" || verb == "ready" || verb == "forfeit" || verb == "concede" || verb == "draw" || verb == "escape" ||
                   verb == "ban" || verb == "pick";
        }

        static bool SettingsVerb(const Command &cmd, std::string &message)
        {
            LoadSettings();
            Settings &s = g.settings;
            // Test verb: a hazard from now on in this fight, in a match against the AI only (in a duel both games build
            // the same environment from the host's plan).
            if (ArgIs(cmd, 1, "env") && ArgIs(cmd, 2, "now"))
            {
                uint8_t mode = Environment::MODE_OFF;
                if (!g.local || g.data.phase != Phase::Fight || cmd.args.size() < 4 || !Environment::ParseMode(cmd.args[3], mode) ||
                    mode < Environment::MODE_SUN)
                {
                    message = "usage: match env now sun|pulsar|asteroids|nebula|storm|battery (in a fight against the AI)";
                    return false;
                }
                Environment::Plan plan;
                plan.kind = (uint8_t)(mode - 1);
                plan.seed = (uint32_t)g.random();
                Environment::Begin(plan, g.data.round, WallMs());
                message = std::string("this fight is near ") + Environment::KindName(plan.kind) + " from now on";
                return true;
            }
            if (cmd.args.size() >= 2 && g.active)
            {
                message = "the match is on: its settings are the host's from the start (change them before 'host')";
                return false;
            }
            // Test verb: who bans first in the ship choice (else the server draws it).
            if (ArgIs(cmd, 1, "firstban"))
            {
                if (ArgIs(cmd, 2, "host")) g_firstBanner = HOST;
                else if (ArgIs(cmd, 2, "guest")) g_firstBanner = GUEST;
                else if (ArgIs(cmd, 2, "random")) g_firstBanner = NOBODY;
                else { message = "usage: match firstban host|guest|random"; return false; }
                message = std::string("the first ban: ") + (g_firstBanner == HOST ? "the host's" : g_firstBanner == GUEST ? "the guest's" : "drawn");
                return true;
            }
            if (ArgIs(cmd, 1, "free")) s.free = true;
            else if (ArgIs(cmd, 1, "rounds") && cmd.args.size() == 2) s.free = false;
            else if (ArgIs(cmd, 1, "rounds"))
            {
                int rounds;
                if (!ArgInt(cmd, 2, rounds) || rounds < 1 || rounds > 99) { message = "usage: match rounds <1-99>"; return false; }
                s.rounds = (uint8_t)rounds;
                s.free = false;
            }
            else if (ArgIs(cmd, 1, "prep"))
            {
                int seconds;
                if (!ArgInt(cmd, 2, seconds) || seconds < 0 || seconds > 3600) { message = "usage: match prep <seconds>"; return false; }
                s.prepSeconds = (uint16_t)seconds;
            }
            else if (ArgIs(cmd, 1, "stall"))
            {
                int seconds;
                if (!ArgInt(cmd, 2, seconds) || seconds < 0 || seconds > 3600) { message = "usage: match stall <seconds> (0: off)"; return false; }
                s.stallSeconds = (uint16_t)seconds;
            }
            else if (ArgIs(cmd, 1, "record"))
            {
                if (ArgIs(cmd, 2, "on")) s.record = true;
                else if (ArgIs(cmd, 2, "off")) s.record = false;
                else { message = "usage: match record on|off (public recording; off: the match is unranked)"; return false; }
            }
            else if (ArgIs(cmd, 1, "permadeath"))
            {
                if (ArgIs(cmd, 2, "on")) s.permadeath = true;
                else if (ArgIs(cmd, 2, "off")) s.permadeath = false;
                else { message = "usage: match permadeath on|off"; return false; }
            }
            else if (ArgIs(cmd, 1, "env"))
            {
                uint8_t mode;
                if (cmd.args.size() < 3 || !Environment::ParseMode(cmd.args[2], mode))
                {
                    message = "usage: match env auto|off|sun|pulsar|asteroids|nebula|storm|battery";
                    return false;
                }
                s.env = mode;
            }
            else if (ArgIs(cmd, 1, "hazards"))
            {
                uint8_t hazards;
                if (cmd.args.size() < 3 || !Environment::ParseHazards(cmd.args[2], hazards))
                {
                    message = "usage: match hazards default|all|none|<kind>,<kind>... (sun, pulsar, asteroids, nebula, storm, battery)";
                    return false;
                }
                s.hazards = hazards;
            }
            else if (ArgIs(cmd, 1, "ships"))
            {
                // The ship choice (roadmap 3.9): each player's own (FTL's hangar), bans from the pool, or the host's list.
                std::vector<std::string> list;
                bool listOk = ArgIs(cmd, 2, "list") && cmd.raw.size() >= 4;
                if (listOk)
                {
                    std::stringstream ships(cmd.raw[3]);
                    std::string ship, blueprint;
                    while (std::getline(ships, ship, ','))
                    {
                        if (!Ships::ParseShip(ship, blueprint) || list.size() >= 30) listOk = false;
                        else if (std::find(list.begin(), list.end(), blueprint) == list.end()) list.push_back(blueprint);
                    }
                }
                if (ArgIs(cmd, 2, "own")) s.ships = SHIPS_OWN;
                else if (ArgIs(cmd, 2, "bans")) s.ships = SHIPS_BANS;
                else if (listOk && !list.empty())
                {
                    s.ships = SHIPS_LIST;
                    s.list = list;
                }
                else
                {
                    message = "usage: match ships own|bans|list <ship>,<ship>... (a ship: kestrel-b, or its blueprint, PLAYER_SHIP_HARD_2)";
                    return false;
                }
            }
            else if (ArgIs(cmd, 1, "pool"))
            {
                uint16_t pool;
                if (cmd.args.size() < 3 || !Ships::ParseTypes(cmd.args[2], pool))
                {
                    message = "usage: match pool all|<type>,<type>... (kestrel, stealth, mantis, engi, federation, slug, rock, zoltan, crystal, lanius)";
                    return false;
                }
                s.pool = pool;
            }
            else if (cmd.args.size() >= 2)
            {
                message = "usage: match [rounds [<n>] | prep <seconds> | stall <seconds> | permadeath on|off | env auto|off|sun|pulsar|asteroids|nebula|storm|battery | hazards <kinds> | record on|off | ships own|bans|list <ships> | pool <types> | free]";
                return false;
            }
            if (cmd.args.size() >= 2)
            {
                SaveSettings();
                message = s.free ? std::string("next duel: a free fight (no rounds)")
                                 : "next duel: best of " + std::to_string(s.rounds) + " rounds, " + std::to_string(s.prepSeconds) +
                                       " s preparation, permanent death " + (s.permadeath ? "on" : "off") + ", environment " +
                                       Environment::ModeName(s.env) + (s.env == Environment::MODE_AUTO ? " (" + Environment::HazardsName(s.hazards) + ")" : "") +
                                       (s.record ? ", recorded" : ", not recorded (unranked)") + ", ships: " + ShipsModeName(s);
                return true;
            }
            message = Status();
            return true;
        }

        NextDuel GetNextDuel()
        {
            LoadSettings();
            const Settings &s = g.settings;
            NextDuel next;
            next.rounds = s.rounds;
            next.prepSeconds = s.prepSeconds;
            next.stallSeconds = s.stallSeconds;
            next.permadeath = s.permadeath;
            next.env = s.env;
            next.hazards = s.hazards;
            next.record = s.record;
            next.ships = s.ships;
            next.pool = s.pool;
            next.list = s.list;
            return next;
        }

        bool SetNextDuel(const NextDuel &next, std::string &message)
        {
            LoadSettings();
            if (g.active)
            {
                message = "the match is on: its settings are the host's from the start";
                return false;
            }
            // The ships: a pool to ban from needs a type, a list a ship (roadmap 3.9).
            if (next.ships == SHIPS_BANS && !(next.pool & Ships::ALL_TYPES))
            {
                message = "Tick at least one ship type for the bans.";
                return false;
            }
            std::vector<std::string> list;
            for (const std::string &ship : next.list)
            {
                std::string blueprint;
                if (Ships::ParseShip(ship, blueprint) && std::find(list.begin(), list.end(), blueprint) == list.end()) list.push_back(blueprint);
            }
            if (next.ships == SHIPS_LIST && list.empty())
            {
                message = "Tick at least one ship for the list.";
                return false;
            }
            Settings &s = g.settings;
            s.rounds = (uint8_t)std::max(1, std::min(99, next.rounds));
            s.prepSeconds = (uint16_t)std::max(0, std::min(3600, next.prepSeconds));
            s.stallSeconds = (uint16_t)std::max(0, std::min(3600, next.stallSeconds));
            s.permadeath = next.permadeath;
            s.env = next.env < Environment::MODE_COUNT ? next.env : (uint8_t)Environment::MODE_AUTO;
            s.hazards = next.hazards;
            s.record = next.record;
            s.ships = next.ships < SHIPS_MODES ? next.ships : (uint8_t)SHIPS_BANS;
            s.pool = next.pool & Ships::ALL_TYPES ? next.pool & Ships::ALL_TYPES : Ships::ALL_TYPES;
            if (!list.empty()) s.list = list;
            s.free = false;
            SaveSettings();
            message = "best of " + std::to_string(s.rounds) + " rounds, " + std::to_string(s.prepSeconds) + " s preparation, anti-stall " +
                      (s.stallSeconds ? std::to_string(s.stallSeconds) + " s" : std::string("off")) + ", permanent death " +
                      (s.permadeath ? "on" : "off") + ", environment " + Environment::ModeName(s.env) +
                      (s.env == Environment::MODE_AUTO ? " (" + Environment::HazardsName(s.hazards) + ")" : "") +
                      (s.record ? ", recorded" : ", not recorded (unranked)") + ", ships: " + ShipsModeName(s);
            return true;
        }

        bool RunVerb(const Command &cmd, std::string &message)
        {
            const std::string &verb = cmd.args[0];
            if (verb == "match") return SettingsVerb(cmd, message);
            const Data &d = g.data;
            if (!g.active || d.phase == Phase::None || d.phase == Phase::MatchOver)
            {
                message = "no match is running";
                return false;
            }
            if (verb == "ban")
            {
                // The ship choice (roadmap 3.9): a type out of the choice, in our turn.
                int type;
                if (d.phase != Phase::Choice || BansDone(d)) { message = "no bans now"; return false; }
                uint8_t banner = BannerOf(d, d.bans.size());
                if (banner != g.me) { message = "it is " + Who(banner) + "'s ban"; return false; }
                if (ArgIs(cmd, 1, "any"))
                {
                    // Any type still in the choice, at random.
                    std::vector<int> open;
                    for (int t = 0; t < Ships::TYPE_COUNT; ++t)
                    {
                        if (TypesLeft(d) & (1 << t)) open.push_back(t);
                    }
                    if (open.empty()) { message = "no type left to ban"; return false; }
                    std::mt19937 random((uint32_t)std::chrono::system_clock::now().time_since_epoch().count());
                    type = open[std::uniform_int_distribution<size_t>(0, open.size() - 1)(random)];
                }
                else if (cmd.args.size() < 2 || !Ships::ParseType(cmd.args[1], type))
                {
                    message = "usage: ban <type>|any (kestrel, stealth, mantis, engi, federation, slug, rock, zoltan, crystal, lanius; or 1-10)";
                    return false;
                }
                if (!(TypesLeft(d) & (1 << type))) { message = std::string("the ") + Ships::TypeName(type) + " is out of the choice already"; return false; }
                OwnEvent(EV_BAN, (uint8_t)type);
                message = std::string("you ban the ") + Ships::TypeName(type);
                return true;
            }
            if (verb == "pick")
            {
                int pick;
                if (d.phase != Phase::Choice || d.offer.empty()) { message = "no ships to pick from now"; return false; }
                if (d.picked[g.me] != PICK_NONE) { message = "you have picked your ship"; return false; }
                if (!ArgInt(cmd, 1, pick) || pick < 1 || pick > (int)d.offer.size())
                {
                    message = "usage: pick <1-" + std::to_string(d.offer.size()) + ">";
                    return false;
                }
                g.ownPick = pick - 1;
                OwnEvent(EV_PICK, (uint8_t)(pick - 1));
                message = "you pick the " + Ships::Title(d.offer[pick - 1]);
                return true;
            }
            if (verb == "ready")
            {
                if (d.phase != Phase::Prep) { message = "'ready' ends the preparation early"; return false; }
                bool off = ArgIs(cmd, 1, "off");
                OwnEvent(off ? EV_UNREADY : EV_READY, 0);
                message = off ? "not ready" : "ready";
                return true;
            }
            if (verb == "forfeit")
            {
                OwnEvent(EV_FORFEIT, 0);
                message = "you forfeit the match";
                return true;
            }
            if (verb == "escape")
            {
                // As FTL's JUMP button in a fight (roadmap AD): the drive charged, the engines and piloting working.
                if (!EscapeAllowed()) { message = "running away is for a match's fight"; return false; }
                if (!DriveReady()) { message = "the FTL drive isn't ready (charged, with engines and piloting working)"; return false; }
                return Escape(message);
            }
            if (verb == "concede")
            {
                if (d.phase != Phase::Starting && d.phase != Phase::Fight) { message = "a round can be conceded in its fight"; return false; }
                OwnEvent(EV_CONCEDE, 0);
                message = "you concede this round";
                return true;
            }
            if (verb == "draw")
            {
                if (ArgIs(cmd, 1, "back"))
                {
                    if (d.drawBy != g.me) { message = "no draw offer of yours to take back"; return false; }
                    OwnEvent(EV_DRAW_BACK, 0);
                    message = "draw offer taken back";
                    return true;
                }
                if (ArgIs(cmd, 1, "yes") || ArgIs(cmd, 1, "no"))
                {
                    if (d.drawBy != Other(g.me)) { message = "no draw offer to answer"; return false; }
                    g.drawAnswered = true;
                    OwnEvent(EV_DRAW_ANSWER, ArgIs(cmd, 1, "yes") ? 1 : 0);
                    message = ArgIs(cmd, 1, "yes") ? "draw accepted" : "draw declined";
                    return true;
                }
                uint8_t scope = ArgIs(cmd, 1, "match") ? DRAW_MATCH : ArgIs(cmd, 1, "round") || cmd.args.size() == 1 ? DRAW_ROUND : DRAW_NONE;
                if (scope == DRAW_NONE) { message = "usage: draw [round|match] | draw yes|no | draw back"; return false; }
                if (d.drawBy != NOBODY) { message = "a draw offer is already open"; return false; }
                OwnEvent(EV_DRAW_OFFER, scope);
                message = std::string("draw offered for the ") + (scope == DRAW_MATCH ? "match" : "round");
                return true;
            }
            message = "unknown match command";
            return false;
        }

        // ---------------------------------------------------------------------------------------------------------
        // On screen and in the status
        // ---------------------------------------------------------------------------------------------------------

        std::string Status()
        {
            LoadSettings();
            const Data &d = g.data;
            std::ostringstream out;
            if (!g.active && d.phase == Phase::None)
            {
                const Settings &s = g.settings;
                out << "no match; the next duel you host: "
                    << (s.free ? std::string("a free fight") : "best of " + std::to_string(s.rounds) + " rounds, " +
                                                                   std::to_string(s.prepSeconds) + " s preparation, permanent death " +
                                                                   (s.permadeath ? "on" : "off") + ", environment " +
                                                                   Environment::ModeName(s.env))
                    << (s.record ? ", recorded" : ", not recorded (unranked)") << ", ships: " << ShipsModeName(s);
                return out.str();
            }
            if (d.phase == Phase::Choice)
            {
                // The ship choice: the bans so far, whose turn it is, the offer and who has picked (not what).
                out << "match: ship choice (" << ShipsModeName(d.settings) << "), bans";
                for (size_t i = 0; i < d.bans.size(); ++i)
                {
                    out << " [" << (BannerOf(d, i) == HOST ? "host " : "guest ") << Ships::TypeWord(d.bans[i].type)
                        << (d.bans[i].byServer ? " (time was up)" : "") << "]";
                }
                double left = std::max(0.0, (FromHost(d.phaseEnd) - Now()) / 1000.0);
                if (!BansDone(d)) out << ", now the " << (BannerOf(d, d.bans.size()) == HOST ? "host" : "guest") << "'s ban";
                else
                {
                    out << ", offer";
                    for (size_t i = 0; i < d.offer.size(); ++i) out << " [" << i + 1 << " " << d.offer[i] << "]";
                    out << ", picked: host " << (d.picked[HOST] ? "yes" : "no") << ", guest " << (d.picked[GUEST] ? "yes" : "no");
                    if (!d.ships[HOST].empty()) out << ", the ships: host " << d.ships[HOST] << ", guest " << d.ships[GUEST];
                }
                out << ", " << (int)std::ceil(left) << " s left, types left " << Ships::TypesText(TypesLeft(d));
                return out.str();
            }
            out << "match: round " << (int)d.round << "/" << (int)d.settings.rounds << (d.settings.free ? " (free fight)" : "")
                << ", " << PhaseName(d.phase) << ", " << ScoreLine() << ", results";
            for (const Result &result : d.results)
            {
                out << " [" << (result.winner == NOBODY ? "draw" : result.winner == HOST ? "host" : "guest") << " "
                    << ReasonText(result.reason) << " " << Number(result.dealt[HOST]) << ":" << Number(result.dealt[GUEST]) << "]";
            }
            out << ", environment " << Environment::KindName(d.env.kind) << " (setting " << Environment::ModeName(d.settings.env) << ")";
            out << (d.settings.record ? ", recorded" : ", not recorded (unranked)");
            if (!d.ships[HOST].empty()) out << ", ships " << d.ships[HOST] << " : " << d.ships[GUEST];
            if (Environment::Active()) out << ", " << Environment::Status();
            out << ", our damage taken " << Number(g.taken.Taken()) << " (hull " << g.taken.hullLost << "/" << g.taken.hullPool
                << ", crew " << Number(g.taken.crewLost) << "/" << Number(g.taken.crewPool) << "), theirs " << Number(g.peerTaken)
                << ", events sent " << g.eventsSent << " received " << g.eventsReceived;
            if (d.phase == Phase::MatchOver)
            {
                out << ", winner " << (d.matchWinner == NOBODY ? "nobody" : d.matchWinner == HOST ? "host" : "guest") << " ("
                    << ReasonText(d.matchReason) << ")";
            }
            return out.str();
        }

        // ---------------------------------------------------------------------------------------------------------
        // The Duels window (DuelsWindow.cpp)
        // ---------------------------------------------------------------------------------------------------------

        // A time in the settings: "5 min", or "45 s".
        static std::string Duration(int seconds)
        {
            return seconds % 60 == 0 ? std::to_string(seconds / 60) + " min" : std::to_string(seconds) + " s";
        }

        // A result's reason in a few words, for the Duels window's row.
        static const char *ShortReason(uint8_t reason)
        {
            switch (reason)
            {
            case REASON_BOTH: return "both ships down";
            case REASON_STALL: return "stalemate";
            case REASON_LEFT: return "player left";
            case REASON_SCORE: return "damage score";
            case REASON_ESCAPED: return "ran away";
            default: return ReasonText(reason);
            }
        }

        // The settings as the Duels window's rows (AS): always five short lines.
        static std::vector<std::string> SettingsRows(const Settings &s)
        {
            std::vector<std::string> rows;
            std::string reasons = GetState().debug ? "debug mode" : "";
            if (g.local) reasons += std::string(reasons.empty() ? "" : ", ") + "against the AI";
            if (!s.record) reasons += std::string(reasons.empty() ? "" : ", ") + "not recorded";
            if (s.free)
            {
                rows = {"A free fight (no rounds)", "", ""};
            }
            else
            {
                rows.push_back("Best of " + std::to_string(s.rounds) + " rounds, " + std::to_string(s.prepSeconds) + " s preparation");
                rows.push_back(std::string("Permanent death ") + (s.permadeath ? "on" : "off"));
                rows.push_back(s.stallSeconds > 0 ? "No progress for " + Duration(s.stallSeconds) + " ends a round" : "No time limit without progress");
            }
            if (s.env == Environment::MODE_OFF || (s.env == Environment::MODE_AUTO && s.hazards == 0)) rows.push_back("No hazards");
            else if (s.env != Environment::MODE_AUTO) rows.push_back(std::string("Every fight near ") + Environment::KindName(s.env - 1));
            else if (s.hazards == Environment::DEFAULT_HAZARDS) rows.push_back("Hazards by chance from round 3");
            else rows.push_back("Hazards from round 3: " + Environment::HazardsName(s.hazards));
            rows.push_back(reasons.empty() ? "Ranked" : "Unranked: " + reasons);
            return rows;
        }

        Summary GetSummary()
        {
            LoadSettings();
            Summary s;
            const Data &d = g.data;
            s.inMatch = d.phase != Phase::None && (g.active || d.phase == Phase::MatchOver);
            if (!s.inMatch)
            {
                s.rules = SettingsRows(g.settings);
                return s;
            }
            uint8_t them = Other(g.me);
            s.rules = SettingsRows(d.settings);
            s.state = (d.settings.free ? std::string("free fight") : "round " + std::to_string(d.round) + " of " +
                                                                        std::to_string(d.settings.rounds)) +
                      ": " + PhaseName(d.phase);
            if (d.phase == Phase::MatchOver)
            {
                s.state = d.matchWinner == NOBODY ? "match over: a draw" : d.matchWinner == g.me ? "match over: you win" : "match over: you lose";
                s.state += std::string(" (") + ReasonText(d.matchReason) + ")";
            }
            // The ship choice (roadmap 3.9).
            Summary::Choice &c = s.choice;
            c.bans = d.settings.ships == SHIPS_BANS;
            c.pool = d.settings.pool & Ships::ALL_TYPES;
            for (size_t i = 0; i < d.bans.size(); ++i)
            {
                c.banned.push_back(d.bans[i].type);
                c.bannedBy.push_back(BannerOf(d, i));
                c.byServer.push_back(d.bans[i].byServer);
            }
            c.bansTotal = BansTotal(d);
            c.firstBanner = d.firstBanner;
            c.banner = d.phase == Phase::Choice && !BansDone(d) ? BannerOf(d, d.bans.size()) : NOBODY;
            c.offer = d.offer;
            c.picked[HOST] = d.picked[HOST] != PICK_NONE;
            c.picked[GUEST] = d.picked[GUEST] != PICK_NONE;
            c.ourPick = g.ownPick;
            c.ships[HOST] = d.ships[HOST];
            c.ships[GUEST] = d.ships[GUEST];
            if (d.phase == Phase::Choice)
            {
                s.state = !c.ships[HOST].empty() ? std::string("ship choice: the ships are known")
                          : c.banner == NOBODY  ? std::string("ship choice: the pick")
                                                : "ship choice: ban " + std::to_string(d.bans.size() + 1) + " of " + std::to_string(c.bansTotal) +
                                                      (c.banner == g.me ? ", yours" : ", " + Who(c.banner) + "'s");
            }
            s.score = ScoreLine();
            if (!d.settings.free && d.phase != Phase::MatchOver)
            {
                s.fight = d.env.kind == Environment::NONE ? std::string("This round: open space")
                                                          : std::string("This round: near ") + Environment::KindName(d.env.kind);
            }
            int won = 0, lost = 0, drawn = 0;
            for (size_t i = 0; i < d.results.size(); ++i)
            {
                const Result &result = d.results[i];
                if (result.winner == NOBODY) ++drawn;
                else if (result.winner == g.me) ++won;
                else ++lost;
                std::string line = "Round " + std::to_string(i + 1) + ": ";
                line += result.winner == NOBODY ? "a draw" : result.winner == g.me ? "you won" : "you lost";
                line += std::string(" (") + ReasonText(result.reason) + "; damage dealt " + Number(result.dealt[g.me]) + " : " +
                        Number(result.dealt[them]) + ")";
                s.results.push_back(line);
            }
            s.tally = d.results.empty() ? std::string("Rounds: none decided yet")
                                        : "Rounds: " + std::to_string(won) + " won, " + std::to_string(lost) + " lost, " +
                                              std::to_string(drawn) + " drawn";
            if (!d.results.empty())
            {
                const Result &last = d.results.back();
                s.lastRound = std::string("Last round: ") + (last.winner == NOBODY ? "a draw" : last.winner == g.me ? "you won" : "you lost") +
                              " (" + ShortReason(last.reason) + ")";
            }
            bool running = g.active && d.phase != Phase::MatchOver;
            s.ready = d.ready[g.me];
            s.canReady = running && d.phase == Phase::Prep && !s.ready;
            s.canConcede = running && (d.phase == Phase::Starting || d.phase == Phase::Fight);
            s.canForfeit = running;
            bool noOffer = d.drawBy == NOBODY;
            s.canOfferRoundDraw = running && noOffer && (d.phase == Phase::Starting || d.phase == Phase::Fight);
            s.canOfferMatchDraw = running && noOffer && (d.phase == Phase::Prep || d.phase == Phase::Starting || d.phase == Phase::Fight);
            s.drawToAnswer = running && d.drawBy == them;
            s.weOfferDraw = running && d.drawBy == g.me;
            s.drawIsMatch = d.drawScope == DRAW_MATCH;
            s.phase = d.phase;
            s.round = d.round;
            s.rounds = d.settings.rounds;
            s.free = d.settings.free;
            s.me = g.me;
            s.names[g.me] = Match::ScreenName(Match::PlayerName());
            s.names[them] = g.local ? Match::ScreenName(Ai::Name())
                                    : Net::PeerName().empty() ? std::string("Opponent") : Match::ScreenName(Net::PeerName());
            s.points[HOST] = Points(d, HOST);
            s.points[GUEST] = Points(d, GUEST);
            s.opponentReady = d.ready[them];
            s.canUnready = running && d.phase == Phase::Prep && s.ready;
            if (!d.settings.free && d.env.kind != Environment::NONE && (d.phase == Phase::Prep || d.phase == Phase::Starting))
            {
                s.envName = Environment::KindName(d.env.kind);
            }
            // The timer that counts now: a lost connection's wait first, then the preparation, and the anti-stall timer
            // in its last minute (a draw offer has no time limit, AT).
            double now = Now(), waitMs;
            bool cutOff;
            if (Net::Reconnecting(waitMs, cutOff))
            {
                s.paused = true;
                s.pausedText = cutOff ? std::string("Getting back into the match") : "Waiting for " + s.names[them];
                s.countdownLabel = "Paused";
                s.countdownMs = waitMs;
            }
            else if (d.phase == Phase::Choice && d.phaseEnd >= 0.0)
            {
                s.countdownLabel = !c.ships[HOST].empty() ? "Round 1 in" : c.banner == g.me ? "Your ban" : c.banner != NOBODY ? "Their ban"
                                   : c.picked[g.me] ? "Their pick" : "Your pick";
                s.countdownMs = FromHost(d.phaseEnd) - now;
            }
            else if (d.phase == Phase::Prep && d.phaseEnd >= 0.0)
            {
                s.countdownLabel = "Fight in";
                s.countdownMs = FromHost(d.phaseEnd) - now;
            }
            else if (d.phase == Phase::Fight && g.fightBegun && d.stallEnd >= 0.0 && FromHost(d.stallEnd) - now < 60000.0)
            {
                s.countdownLabel = "No progress";
                s.countdownMs = FromHost(d.stallEnd) - now;
            }
            if (d.drawBy != NOBODY)
            {
                s.drawText = (d.drawBy == g.me ? std::string("You offer") : Who(them) + " offers") + " a draw for the " +
                             (d.drawScope == DRAW_MATCH ? "match" : "round");
            }
            return s;
        }

        bool Act(const std::string &command, std::string &message)
        {
            Command cmd;
            if (!ParseCommand(command, cmd) || !IsVerb(cmd.args[0]))
            {
                message = "unknown action";
                return false;
            }
            return RunVerb(cmd, message);
        }
    }
}

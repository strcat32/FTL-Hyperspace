#include "Global.h"
#include "Duels.h"
#include "DuelsConfig.h"
#include "DuelsConsole.h"
#include "DuelsCrew.h"
#include "DuelsEnvironment.h"
#include "DuelsMatchUi.h"
#include "DuelsMatch.h"
#include "DuelsNet.h"
#include "DuelsRounds.h"
#include "DuelsScript.h"
#include "DuelsRefit.h"
#include "DuelsTrace.h"
#include "DuelsWire.h"

#include <algorithm>
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
        static const double DRAW_OFFER_MS = 15000.0;     // time to accept a draw offer (rules, section 3)
        static const double DRAW_AGAIN_MS = 60000.0;     // after a declined offer, the same player waits this long
        static const float HULL_WEIGHT = 0.65f;          // the damage score: hull 0.65, crew 0.35 (rules, section 3)
        static const float SCORE_TIE = 0.05f;            // damage scores closer than this are equal
        // Anti-stall (rules, section 3): a new low of either player's hull or crew health (by more than this share of
        // the round's start) restarts the timer; when it runs out, the lows decide, and a lead under 10 is a draw.
        static const float STALL_STEP = 0.02f;
        static const float STALL_DRAW_LEAD = 10.f;

        enum EventType : uint8_t
        {
            EV_READY = 1,        // done preparing
            EV_DEFEAT = 2,       // our ship is out (arg: REASON_DESTROYED or REASON_CREW)
            EV_FORFEIT = 3,      // we give up the match
            EV_CONCEDE = 4,      // we give up this round
            EV_DRAW_OFFER = 5,   // arg: DRAW_ROUND or DRAW_MATCH
            EV_DRAW_ANSWER = 6,  // arg: 1 accepted, 0 declined
            EV_UNREADY = 7,      // Ready taken back (the preparation goes on to its end)
            EV_ESCAPE = 8        // we jumped away with a charged FTL drive (roadmap AD)
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
            double lastSecondsShown = -1.0;
            std::mt19937 random;            // host: the shops' stock
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
            return Net::HasClock() ? Net::PeerToLocalTime(hostTime) : hostTime;
        }

        static uint8_t Other(uint8_t player)
        {
            return player == HOST ? GUEST : HOST;
        }

        static std::string Who(uint8_t player)
        {
            if (player == NOBODY) return "nobody";
            if (player == g.me) return "you";
            std::string name = Net::PeerName();
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
            return r.Ok() && (uint8_t)d.phase <= (uint8_t)Phase::MatchOver && d.env.kind < Environment::KIND_COUNT &&
                   d.settings.env < Environment::MODE_COUNT;
        }

        static void SendData()
        {
            Writer w;
            WriteData(w, g.data);
            Net::Send(MSG_MATCH, w, true);
            g.dirty = false;
        }

        static void SendEvent(uint8_t type, uint8_t arg)
        {
            Writer w;
            w.U8(type);
            w.U8(arg);
            w.U8(g.data.round);
            Net::Send(MSG_MATCH_EVENT, w, true);
            ++g.eventsSent;
        }

        // ---------------------------------------------------------------------------------------------------------
        // Our ship: damage and defeat
        // ---------------------------------------------------------------------------------------------------------

        // Our crew wherever they are: aboard our ship (not the opponent's boarders, not drones) and aboard theirs.
        static std::vector<CrewMember*> OurCrew()
        {
            std::vector<CrewMember*> crew;
            ShipManager *own = G_->GetShipManager(0);
            ShipManager *replica = G_->GetShipManager(1);
            if (own)
            {
                for (CrewMember *member : own->vCrewList)
                {
                    if (member && !member->IsDrone() && !Crew::IsGuest(member)) crew.push_back(member);
                }
            }
            if (replica)
            {
                for (CrewMember *member : replica->vCrewList)
                {
                    if (member && !member->IsDrone() && Crew::AwayId(member) >= 0) crew.push_back(member);
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
        }

        static void EnterPrep()
        {
            const Data &d = g.data;
            Environment::End();
            Match::NewFight();
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
                    d.drawBy = player;
                    d.drawScope = arg;
                    d.drawEnd = now + DRAW_OFFER_MS;
                    g.lastOffer[player] = now;
                    g.dirty = true;
                    Announce(player == g.me ? std::string("You offer a draw for the ") + (arg == DRAW_MATCH ? "match" : "round")
                                            : Who(player) + " offers a draw for the " + (arg == DRAW_MATCH ? "match" : "round") +
                                                  (arg == DRAW_MATCH ? ": accept in the Duels window (15 s)" : ": the DRAW button accepts (15 s)"));
                }
                break;
            }
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
            if (d.drawBy != NOBODY && d.drawEnd >= 0.0 && now >= d.drawEnd)
            {
                ClearDraw();
                g.dirty = true;
                Announce("The draw offer ran out");
            }
            switch (d.phase)
            {
            case Phase::Prep:
                if (now >= d.phaseEnd || (d.ready[HOST] && d.ready[GUEST])) SetPhase(Phase::Starting, -1.0);
                break;
            case Phase::Starting:
                if (Match::ShipsStand())
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
        }

        void Reset()
        {
            LoadSettings();
            Settings settings = g.settings;
            g = Local();
            g.settings = settings;
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
            g.random.seed((uint32_t)WallMs() ^ (uint32_t)(Net::MatchSeed() & 0xffffffffu));
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
            StartRound(1);
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
                    Announce(Who(HOST) + " offers a draw for the " + (data.drawScope == DRAW_MATCH ? "match" : "round") +
                             (data.drawScope == DRAW_MATCH ? ": accept in the Duels window (15 s)" : ": the DRAW button accepts (15 s)"));
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
                // An event of an earlier round (a defeat reported as the round ended) doesn't count in this one.
                if (round != g.data.round && event != EV_FORFEIT) return;
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
            if (!g.active || !Net::IsConnected()) return;
            Data &d = g.data;


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
            return verb == "match" || verb == "ready" || verb == "forfeit" || verb == "concede" || verb == "draw" || verb == "escape";
        }

        static bool SettingsVerb(const Command &cmd, std::string &message)
        {
            LoadSettings();
            Settings &s = g.settings;
            if (cmd.args.size() >= 2 && g.active)
            {
                message = "the match is on: its settings are the host's from the start (change them before 'host')";
                return false;
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
            else if (cmd.args.size() >= 2)
            {
                message = "usage: match [rounds [<n>] | prep <seconds> | stall <seconds> | permadeath on|off | env auto|off|sun|pulsar|asteroids|nebula|storm|battery | hazards <kinds> | free]";
                return false;
            }
            if (cmd.args.size() >= 2)
            {
                SaveSettings();
                message = s.free ? std::string("next duel: a free fight (no rounds)")
                                 : "next duel: best of " + std::to_string(s.rounds) + " rounds, " + std::to_string(s.prepSeconds) +
                                       " s preparation, permanent death " + (s.permadeath ? "on" : "off") + ", environment " +
                                       Environment::ModeName(s.env) + (s.env == Environment::MODE_AUTO ? " (" + Environment::HazardsName(s.hazards) + ")" : "");
                return true;
            }
            message = Status();
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
                if (ArgIs(cmd, 1, "yes") || ArgIs(cmd, 1, "no"))
                {
                    if (d.drawBy != Other(g.me)) { message = "no draw offer to answer"; return false; }
                    OwnEvent(EV_DRAW_ANSWER, ArgIs(cmd, 1, "yes") ? 1 : 0);
                    message = ArgIs(cmd, 1, "yes") ? "draw accepted" : "draw declined";
                    return true;
                }
                uint8_t scope = ArgIs(cmd, 1, "match") ? DRAW_MATCH : ArgIs(cmd, 1, "round") || cmd.args.size() == 1 ? DRAW_ROUND : DRAW_NONE;
                if (scope == DRAW_NONE) { message = "usage: draw [round|match] | draw yes|no"; return false; }
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

        static std::string Clock(double ms)
        {
            int seconds = (int)std::ceil(std::max(0.0, ms) / 1000.0);
            char buffer[16];
            snprintf(buffer, sizeof(buffer), "%d:%02d", seconds / 60, seconds % 60);
            return buffer;
        }

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
                                                                   Environment::ModeName(s.env));
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

        static std::string SettingsText(const Settings &s)
        {
            const char *unranked = GetState().debug ? "; unranked: debug mode is on" : "";
            if (s.free) return std::string("a free fight (no rounds)") + unranked;
            std::string text = "best of " + std::to_string(s.rounds) + " rounds, " + std::to_string(s.prepSeconds) +
                               " s preparation, permanent death " + (s.permadeath ? "on" : "off");
            if (s.stallSeconds > 0) text += ", no progress for " + std::to_string(s.stallSeconds / 60) + " min ends a round";
            if (s.env == Environment::MODE_OFF || (s.env == Environment::MODE_AUTO && s.hazards == 0)) text += ", no hazards";
            else if (s.env != Environment::MODE_AUTO) text += std::string(", every fight in ") + Environment::KindName(s.env - 1);
            else if (s.hazards != Environment::DEFAULT_HAZARDS) text += ", hazards: " + Environment::HazardsName(s.hazards);
            return text + unranked;
        }

        Summary GetSummary()
        {
            LoadSettings();
            Summary s;
            const Data &d = g.data;
            s.inMatch = d.phase != Phase::None && (g.active || d.phase == Phase::MatchOver);
            if (!s.inMatch)
            {
                s.settings = "The next duel you host: " + SettingsText(g.settings) + ".";
                return s;
            }
            uint8_t them = Other(g.me);
            s.settings = SettingsText(d.settings);
            s.state = (d.settings.free ? std::string("free fight") : "round " + std::to_string(d.round) + " of " +
                                                                        std::to_string(d.settings.rounds)) +
                      ": " + PhaseName(d.phase);
            if (d.phase == Phase::MatchOver)
            {
                s.state = d.matchWinner == NOBODY ? "match over: a draw" : d.matchWinner == g.me ? "match over: you win" : "match over: you lose";
                s.state += std::string(" (") + ReasonText(d.matchReason) + ")";
            }
            s.score = ScoreLine();
            if (!d.settings.free && d.phase != Phase::MatchOver)
            {
                s.environment = d.env.kind == Environment::NONE
                                    ? std::string("This round's fight: open space, no hazard")
                                    : std::string("This round's fight: near ") + Environment::KindName(d.env.kind) + " (" +
                                          Environment::KindShort(d.env.kind) + ")";
            }
            for (size_t i = 0; i < d.results.size(); ++i)
            {
                const Result &result = d.results[i];
                std::string line = "Round " + std::to_string(i + 1) + ": ";
                line += result.winner == NOBODY ? "a draw" : result.winner == g.me ? "you won" : "you lost";
                line += std::string(" (") + ReasonText(result.reason) + "; damage dealt " + Number(result.dealt[g.me]) + " : " +
                        Number(result.dealt[them]) + ")";
                s.results.push_back(line);
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
            s.phase = d.phase;
            s.round = d.round;
            s.rounds = d.settings.rounds;
            s.free = d.settings.free;
            s.me = g.me;
            s.names[g.me] = Match::PlayerName();
            s.names[them] = Net::PeerName().empty() ? std::string("Opponent") : Net::PeerName();
            s.points[HOST] = Points(d, HOST);
            s.points[GUEST] = Points(d, GUEST);
            s.opponentReady = d.ready[them];
            s.canUnready = running && d.phase == Phase::Prep && s.ready;
            if (!d.settings.free && d.env.kind != Environment::NONE && (d.phase == Phase::Prep || d.phase == Phase::Starting))
            {
                s.envName = Environment::KindName(d.env.kind);
            }
            // The timer that counts now: a lost connection's wait first, then the preparation, a draw offer, and the
            // anti-stall timer in its last minute.
            double now = Now(), waitMs;
            bool cutOff;
            if (Net::Reconnecting(waitMs, cutOff))
            {
                s.paused = true;
                s.pausedText = cutOff ? std::string("Getting back into the match") : "Waiting for " + Who(them);
                s.countdownLabel = "Paused";
                s.countdownMs = waitMs;
            }
            else if (d.phase == Phase::Prep && d.phaseEnd >= 0.0)
            {
                s.countdownLabel = "Fight in";
                s.countdownMs = FromHost(d.phaseEnd) - now;
            }
            else if (d.phase == Phase::Fight && d.drawBy != NOBODY && d.drawEnd >= 0.0)
            {
                s.countdownLabel = "Draw offer";
                s.countdownMs = FromHost(d.drawEnd) - now;
            }
            else if (d.phase == Phase::Fight && g.fightBegun && d.stallEnd >= 0.0 && FromHost(d.stallEnd) - now < 60000.0)
            {
                s.countdownLabel = "No progress";
                s.countdownMs = FromHost(d.stallEnd) - now;
            }
            if (d.drawBy != NOBODY)
            {
                s.drawText = (d.drawBy == g.me ? std::string("You offer") : Who(them) + " offers") + " a draw for the " +
                             (d.drawScope == DRAW_MATCH ? "match" : "round") + " (" + Clock(FromHost(d.drawEnd) - Now()) + ")";
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

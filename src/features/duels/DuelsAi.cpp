#include "Global.h"
#include "Duels.h"
#include "DuelsAi.h"
#include "DuelsRounds.h"
#include "DuelsShipControl.h"
#include "DuelsTrace.h"

#include <algorithm>
#include <map>
#include <random>

namespace Duels
{
    namespace Ai
    {
        struct AiState
        {
            bool active = false;
            std::string blueprint;       // the AI's ship
            int round = 0;
            bool spawned = false;        // its ship came this round
            bool defeated = false;       // its defeat is reported this round

            // The damage its ship takes this round, as DuelsRounds.cpp counts ours: hull and crew lost.
            bool counting = false;
            float hullPool = 0.f, crewPool = 0.f, hullLost = 0.f, crewLost = 0.f;
            int lastHull = 0;
            std::map<CrewMember*, float> lastHealth;
        };

        static AiState g;

        // FTL's player ships, their layouts A, B and C (the Crystal and Lanius ships have two). The hangar's own list is
        // empty until it has been opened once.
        static const char *const TYPES[] = {"PLAYER_SHIP_HARD", "PLAYER_SHIP_STEALTH", "PLAYER_SHIP_MANTIS", "PLAYER_SHIP_CIRCLE",
                                            "PLAYER_SHIP_FED", "PLAYER_SHIP_JELLY", "PLAYER_SHIP_ROCK", "PLAYER_SHIP_ENERGY",
                                            "PLAYER_SHIP_CRYSTAL", "PLAYER_SHIP_ANAEROBIC"};
        static const char *const LAYOUTS[] = {"", "_2", "_3"};

        std::vector<std::string> PlayerShips()
        {
            std::vector<std::string> ships;
            BlueprintManager *blueprints = G_->GetBlueprints();
            for (const char *type : TYPES)
            {
                for (const char *layout : LAYOUTS)
                {
                    std::string name = std::string(type) + layout;
                    ShipBlueprint *bp = blueprints ? blueprints->GetShipBlueprint(name, -1) : nullptr;
                    if (bp && bp->blueprintName == name) ships.push_back(name);
                }
            }
            return ships;
        }

        std::string ShipTitle(const std::string &blueprint)
        {
            BlueprintManager *blueprints = G_->GetBlueprints();
            ShipBlueprint *bp = blueprints ? blueprints->GetShipBlueprint(blueprint, -1) : nullptr;
            if (!bp || bp->blueprintName != blueprint) return blueprint;
            TextString shipClass = bp->shipClass;
            std::string title = shipClass.GetText();
            char layout = blueprint.size() > 2 && blueprint[blueprint.size() - 2] == '_' ? (char)('A' + (blueprint.back() - '1')) : 'A';
            return (title.empty() ? blueprint : title) + " " + std::string(1, layout);
        }

        void Start(const std::string &blueprint)
        {
            g = AiState();
            g.active = true;
            g.blueprint = blueprint;
            if (g.blueprint.empty())
            {
                std::vector<std::string> ships = PlayerShips();
                std::mt19937 random((uint32_t)WallMs());
                g.blueprint = ships.empty() ? std::string("PLAYER_SHIP_HARD") : ships[random() % ships.size()];
            }
            // FTL's own ship AI flies it (a duel before it had the opponent's replaced by its owner's game).
            GetState().aiOff[1] = false;
            Log("Ai: a match against FTL's AI in %s (%s)", g.blueprint.c_str(), ShipTitle(g.blueprint).c_str());
            Rounds::StartLocal();
        }

        bool Active()
        {
            return g.active;
        }

        void Stop()
        {
            if (!g.active) return;
            g.active = false;
            RemoveEnemy();
            Rounds::Reset();
            Log("Ai: the match against the AI ends");
        }

        std::string Name()
        {
            std::string title = ShipTitle(g.blueprint);
            // "Kestrel Cruiser A": the ship's first word is enough on the screen.
            size_t space = title.find(' ');
            return "AI " + (space == std::string::npos ? title : title.substr(0, space));
        }

        void OnPrep(int round)
        {
            g.round = round;
            g.spawned = false;
            g.defeated = false;
            g.counting = false;
            // Part 1: the AI takes the round as its ship comes; its shopping comes with part 2.
            Rounds::OpponentReady();
        }

        bool ShipStands()
        {
            return g.spawned && G_->GetShipManager(1) != nullptr;
        }

        // The AI's crew: its ship's own, wherever they are (boarders on ours too), drones not.
        static std::vector<CrewMember*> AiCrew()
        {
            std::vector<CrewMember*> crew;
            for (int id = 0; id < 2; ++id)
            {
                ShipManager *ship = G_->GetShipManager(id);
                if (!ship) continue;
                for (CrewMember *member : ship->vCrewList)
                {
                    if (member && !member->IsDrone() && member->iShipId == 1) crew.push_back(member);
                }
            }
            return crew;
        }

        static float Health(const CrewMember *crew)
        {
            return crew->bDead ? 0.f : std::max(0.f, crew->health.first);
        }

        static void StartCounting(ShipManager *ship)
        {
            g.counting = true;
            g.hullPool = (float)ship->ship.hullIntegrity.second;
            g.lastHull = ship->ship.hullIntegrity.first;
            g.hullLost = g.crewLost = g.crewPool = 0.f;
            g.lastHealth.clear();
            for (CrewMember *crew : AiCrew())
            {
                g.crewPool += crew->health.second;
                g.lastHealth[crew] = Health(crew);
            }
        }

        // Its damage (the damage score) and its hull and crew now (the anti-stall rule), to the match.
        static void Count(ShipManager *ship)
        {
            int hull = ship ? ship->ship.hullIntegrity.first : 0;
            if (hull < g.lastHull) g.hullLost += (float)(g.lastHull - hull);
            g.lastHull = hull;
            std::map<CrewMember*, float> seen;
            float health = 0.f;
            for (CrewMember *member : AiCrew())
            {
                float now = Health(member);
                auto last = g.lastHealth.find(member);
                if (last != g.lastHealth.end() && now < last->second) g.crewLost += last->second - now;
                seen[member] = now;
                health += now;
            }
            for (const std::pair<CrewMember* const, float> &last : g.lastHealth)
            {
                if (!seen.count(last.first)) g.crewLost += last.second;   // gone from the lists: died
            }
            g.lastHealth.swap(seen);
            float hullShare = g.hullPool > 0.f ? std::max(0.f, (float)hull / g.hullPool) : 1.f;
            float crewShare = g.crewPool > 0.f ? std::min(1.f, health / g.crewPool) : 1.f;
            Rounds::OpponentState(g.hullPool > 0.f ? std::min(1.f, g.hullLost / g.hullPool) : 0.f,
                                  g.crewPool > 0.f ? std::min(1.f, g.crewLost / g.crewPool) : 0.f, hullShare, crewShare);
        }

        // Its crew is dead: none of them alive, and no clone on the way.
        static bool CrewGone()
        {
            for (CrewMember *crew : AiCrew())
            {
                if (!crew->bDead && crew->health.first > 0.f) return false;
            }
            CrewMemberFactory *factory = G_->GetCrewFactory();
            return !factory || factory->CountCloneReadyCrew(false) == 0;
        }

        void OnFrame()
        {
            if (!g.active) return;
            Rounds::Phase phase = Rounds::GetPhase();
            ShipManager *ship = G_->GetShipManager(1);

            // The ships meet: the AI's ship comes (not a target yet: the fight hasn't begun).
            if (phase == Rounds::Phase::Starting && !g.spawned)
            {
                std::string message;
                if (!ship && SpawnEnemy(g.blueprint, message)) ship = G_->GetShipManager(1);
                if (ship)
                {
                    g.spawned = true;
                    Rounds::OnReplicaBuilt(ship);
                    Log("Ai: round %d: its %s is here (hull %d, %u crew)", g.round, g.blueprint.c_str(), ship->ship.hullIntegrity.first,
                        (unsigned)AiCrew().size());
                }
                else Log("Ai: its ship didn't come: %s", message.c_str());
            }

            bool fighting = Rounds::FightBegun() && (phase == Rounds::Phase::Fight || phase == Rounds::Phase::Ending);
            if (fighting && !g.counting && ship) StartCounting(ship);
            if (g.counting && fighting) Count(ship);

            // Its defeat, once a round: its ship destroyed (FTL takes a destroyed enemy away), or its crew dead.
            if (fighting && g.counting && !g.defeated)
            {
                bool destroyed = !ship || ship->bDestroyed || ship->ship.hullIntegrity.first <= 0;
                bool crewDead = !destroyed && CrewGone();
                if (destroyed || crewDead)
                {
                    g.defeated = true;
                    Log("Ai: round %d: its %s", g.round, destroyed ? "ship is destroyed" : "crew is dead");
                    Rounds::OpponentDefeated(crewDead);
                }
            }
        }
    }
}

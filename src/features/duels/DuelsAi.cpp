#include "Global.h"
#include "Duels.h"
#include "DuelsAi.h"
#include "DuelsLobby.h"
#include "DuelsMatch.h"
#include "DuelsRounds.h"
#include "DuelsShipControl.h"
#include "DuelsTrace.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <random>

namespace Duels
{
    namespace Ai
    {
        // A crew member the AI keeps between rounds: its ship comes anew each round, and its crew get their names back.
        struct CrewEntry
        {
            std::string species, name;
        };

        struct AiState
        {
            bool active = false;
            std::string blueprint;       // the AI's ship
            double startMs = 0.0;        // when the match began
            bool boxSeen = false;        // FTL's first message box came (no pause once it is gone)
            int round = 0;
            bool spawned = false;        // its ship came this round
            bool defeated = false;       // its defeat is reported this round

            // The damage its ship takes this round, as DuelsRounds.cpp counts ours: hull and crew lost.
            bool counting = false;
            float hullPool = 0.f, crewPool = 0.f, hullLost = 0.f, crewLost = 0.f;
            int lastHull = 0;
            std::map<CrewMember*, float> lastHealth;

            // Kept between rounds: its fitting (its first ship's, and what it bought since), its missiles, its scrap, and
            // its crew as the last round left them (with Permanent Death the dead stay dead, but the captain, the first
            // of its crew, always comes back: rules, section 2).
            bool fitted = false;
            Match::Loadout loadout;
            int missiles = 0;
            int scrap = 0;
            bool permadeath = true;
            std::vector<Refit::ShopItem> stock;   // this round's, less what it bought
            CrewEntry captain;
            std::vector<CrewEntry> crew;          // for the next round
            std::vector<CrewEntry> aboard;        // alive aboard its ship when it last stood
            int crewHere = 0;                     // its crew this round (FTL takes those who leave a frame later)
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
            g.startMs = WallMs();
            g.blueprint = blueprint;
            if (g.blueprint.empty())
            {
                std::vector<std::string> ships = PlayerShips();
                // (The system clock: the game's own milliseconds since its start repeat from run to run.)
                uint64_t ticks = (uint64_t)std::chrono::system_clock::now().time_since_epoch().count();
                std::mt19937 random((uint32_t)(ticks ^ (ticks >> 32)));
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
            GetState().noPause = false;
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

        void OnPrep(int round, int scrap, const std::vector<Refit::ShopItem> &stock, bool permadeath)
        {
            if (round != g.round) g.scrap += scrap;   // once a round, as ours (Refit::GiveScrap)
            g.round = round;
            g.spawned = false;
            g.defeated = false;
            g.counting = false;
            g.stock = stock;
            g.permadeath = permadeath;
            // It shops when its ship comes (its first ship gives its fitting), and is ready at once.
            Rounds::OpponentReady();
        }

        bool ShipStands()
        {
            return g.active && g.spawned && G_->GetShipManager(1) != nullptr;
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

        static bool Alive(const CrewMember *crew)
        {
            return crew && !crew->bDead && !crew->bOutOfGame && crew->health.first > 0.f;
        }

        static CrewEntry Entry(CrewMember *crew)
        {
            return CrewEntry{crew->species, crew->GetName()};
        }

        // ---------------------------------------------------------------------------------------------------------
        // The shopping rule, when its ship comes (docs/design/ai-opponent.md). Round after round of purchases while it
        // can pay: its shields to their next layer; the weapon of the stock with the most damage in a volley, for a free
        // slot, with the weapons system's levels it needs to be powered; a weapons level while its weapons need more
        // power than the system has (a ship can start so); one level of engines; missiles when a weapon fires them, and
        // drone parts for its drones. A new level comes with the reactor bar it takes. The stock is the match's (the same
        // as ours), and the rule has no chance in it: a test repeats.
        // ---------------------------------------------------------------------------------------------------------

        static const int MISSILES_WANTED = 8, DRONE_PARTS_WANTED = 6;
        // FTL's boarding AI of its enemies with a teleporter (the Mantis and Rebel ships: 1).
        static const int BOARDING_AI = 1;

        static int LevelOf(int id)
        {
            for (const std::pair<int, int> &system : g.loadout.systems)
            {
                if (system.first == id) return system.second;
            }
            return 0;
        }

        // The price of `levels` more of a system it has (shields, weapons or engines: each level takes a reactor bar
        // too); -1 past the system's top level or the reactor's.
        static int LevelsPrice(int id, int levels)
        {
            SystemBlueprint *bp = G_->GetBlueprints()->GetSystemBlueprint(ShipSystem::SystemIdToName(id));
            int level = LevelOf(id);
            if (!bp || level <= 0 || levels < 0 || level + levels > bp->maxPower) return -1;
            if (g.loadout.reactor + levels > Refit::ReactorMax(g.blueprint)) return -1;
            int price = 0;
            for (int i = 1; i <= levels; ++i)
            {
                int to = level + i;
                if (to - 2 < 0 || to - 2 >= (int)bp->upgradeCosts.size()) return -1;
                price += bp->upgradeCosts[to - 2] + Refit::ReactorPrice(g.blueprint, g.loadout.reactor + i);
            }
            return price;
        }

        static bool BuyLevels(int id, int levels)
        {
            int price = LevelsPrice(id, levels);
            if (levels <= 0 || price < 0 || price > g.scrap) return false;
            g.scrap -= price;
            for (std::pair<int, int> &system : g.loadout.systems)
            {
                if (system.first == id) system.second += levels;
            }
            g.loadout.reactor += levels;
            Log("Ai: %s to level %d, the reactor to %d: %d scrap (%d left)", ShipSystem::SystemIdToName(id).c_str(), LevelOf(id),
                g.loadout.reactor, price, g.scrap);
            return true;
        }

        static bool BuyShieldLayer()
        {
            int level = LevelOf(SYS_SHIELDS);
            return level > 0 && BuyLevels(SYS_SHIELDS, level % 2 == 0 ? 2 : 1);
        }

        // A weapon's worth to the AI: the damage of a volley (ion and system damage half; a beam's twice, as it cuts
        // through a room or two more).
        static float Volley(const WeaponBlueprint *bp)
        {
            float damage = (float)bp->damage.iDamage + 0.5f * (float)(bp->damage.iIonDamage + bp->damage.iSystemDamage);
            if (bp->typeName == "BEAM") damage *= 2.f;
            return damage * (float)std::max(1, bp->shots);
        }

        static int WeaponPower()
        {
            int power = 0;
            for (const std::string &name : g.loadout.weapons)
            {
                const WeaponBlueprint *bp = G_->GetBlueprints()->GetWeaponBlueprint(name);
                if (bp) power += bp->power;
            }
            return power;
        }

        static bool BuyWeapon()
        {
            ShipBlueprint *ship = G_->GetBlueprints()->GetShipBlueprint(g.blueprint, -1);
            if (!ship || LevelOf(SYS_WEAPONS) <= 0 || (int)g.loadout.weapons.size() >= ship->weaponSlots) return false;
            int best = -1, bestPrice = 0, bestLevels = 0;
            float bestVolley = 0.f;
            for (size_t i = 0; i < g.stock.size(); ++i)
            {
                const Refit::ShopItem &item = g.stock[i];
                const WeaponBlueprint *bp = item.kind == Refit::KIND_WEAPON ? G_->GetBlueprints()->GetWeaponBlueprint(item.blueprint) : nullptr;
                if (!bp || Volley(bp) <= 0.f) continue;
                int levels = std::max(0, WeaponPower() + bp->power - LevelOf(SYS_WEAPONS));
                int levelsPrice = LevelsPrice(SYS_WEAPONS, levels);
                int price = item.price + levelsPrice;
                if (levelsPrice < 0 || price > g.scrap) continue;
                float volley = Volley(bp);
                if (best < 0 || volley > bestVolley || (volley == bestVolley && price < bestPrice))
                {
                    best = (int)i;
                    bestPrice = price;
                    bestLevels = levels;
                    bestVolley = volley;
                }
            }
            if (best < 0) return false;
            Refit::ShopItem item = g.stock[best];
            g.stock.erase(g.stock.begin() + best);
            g.scrap -= item.price;
            g.loadout.weapons.push_back(item.blueprint);
            Log("Ai: bought %s for %d scrap (%d left)", item.blueprint.c_str(), (int)item.price, g.scrap);
            if (bestLevels > 0) BuyLevels(SYS_WEAPONS, bestLevels);
            return true;
        }

        static bool BuyWeaponPower()
        {
            return WeaponPower() > LevelOf(SYS_WEAPONS) && BuyLevels(SYS_WEAPONS, 1);
        }

        static bool BuyEngines()
        {
            return LevelOf(SYS_ENGINES) > 0 && BuyLevels(SYS_ENGINES, 1);
        }

        // Missiles (a weapon of its fires them) or drone parts (it has drones), up to a stock of 8 and 6.
        static bool BuyResource(uint8_t kind)
        {
            bool missiles = kind == Refit::KIND_MISSILES;
            bool uses = !missiles && !g.loadout.drones.empty();
            for (const std::string &name : g.loadout.weapons)
            {
                const WeaponBlueprint *bp = missiles ? G_->GetBlueprints()->GetWeaponBlueprint(name) : nullptr;
                uses = uses || (bp && bp->missiles > 0);
            }
            int &have = missiles ? g.missiles : g.loadout.droneParts;
            int wanted = (missiles ? MISSILES_WANTED : DRONE_PARTS_WANTED) - have;
            if (!uses || wanted <= 0) return false;
            for (Refit::ShopItem &item : g.stock)
            {
                if (item.kind != kind || item.count == 0) continue;
                // (Price 0: FTL's own, as the store charges it.)
                const ItemBlueprint *bp = G_->GetBlueprints()->GetItemBlueprint(missiles ? "missiles" : "drones");
                int each = item.price > 0 ? (int)item.price : bp ? bp->desc.cost : 0;
                int count = each > 0 ? std::min(std::min((int)item.count, wanted), g.scrap / each) : 0;
                if (count <= 0) return false;
                item.count = (uint8_t)(item.count - count);
                g.scrap -= count * each;
                have += count;
                Log("Ai: bought %d %s for %d scrap (%d left)", count, missiles ? "missiles" : "drone parts", count * each, g.scrap);
                return true;
            }
            return false;
        }

        static void Shop()
        {
            int before = g.scrap;
            for (int pass = 0; pass < 32; ++pass)
            {
                bool bought = BuyShieldLayer();
                bought = BuyWeapon() || bought;
                bought = BuyWeaponPower() || bought;
                bought = BuyEngines() || bought;
                bought = BuyResource(Refit::KIND_MISSILES) || bought;
                bought = BuyResource(Refit::KIND_DRONE_PARTS) || bought;
                if (!bought) break;
            }
            Log("Ai: round %d's shopping: %d scrap spent, %d kept", g.round, before - g.scrap, g.scrap);
        }

        // ---------------------------------------------------------------------------------------------------------
        // Its ship each round: fitted as it has bought, its crew as the last round left them
        // ---------------------------------------------------------------------------------------------------------

        static std::vector<CrewMember*> CrewAboard(ShipManager *ship)
        {
            std::vector<CrewMember*> crew;
            for (CrewMember *member : ship->vCrewList)
            {
                if (member && !member->IsDrone() && member->iShipId == 1) crew.push_back(member);
            }
            return crew;
        }

        // The new ship's crew take the kept crew's names, species by species; the others leave it. The captain comes
        // back. The crew who stay.
        static int FitCrew(ShipManager *ship)
        {
            std::vector<CrewEntry> wanted = g.crew;
            bool captainThere = std::any_of(wanted.begin(), wanted.end(), [](const CrewEntry &entry)
                                            { return entry.name == g.captain.name && entry.species == g.captain.species; });
            if (!captainThere && !g.captain.name.empty()) wanted.insert(wanted.begin(), g.captain);
            std::vector<bool> taken(wanted.size(), false);
            std::vector<CrewMember*> leaving;
            for (CrewMember *member : CrewAboard(ship))
            {
                size_t i = 0;
                while (i < wanted.size() && (taken[i] || wanted[i].species != member->species)) ++i;
                if (i == wanted.size())
                {
                    leaving.push_back(member);
                    continue;
                }
                taken[i] = true;
                TextString name(wanted[i].name, true);
                member->SetName(&name, true);
            }
            for (CrewMember *member : leaving) ship->RemoveCrewmember(member);
            if (!leaving.empty() || !captainThere)
            {
                Log("Ai: round %d: %u of its crew stay dead%s", g.round, (unsigned)leaving.size(),
                    captainThere ? "" : " (its captain is back)");
            }
            return (int)std::count(taken.begin(), taken.end(), true);
        }

        static void FitShip(ShipManager *ship)
        {
            if (!g.fitted)
            {
                // Its first ship: the blueprint's fitting and crew. The first of its crew is its captain.
                g.fitted = true;
                g.loadout = Match::TakeLoadout(ship);
                g.missiles = ship->GetMissileCount();
                for (CrewMember *member : CrewAboard(ship)) g.crew.push_back(Entry(member));
                if (!g.crew.empty()) g.captain = g.crew.front();
                g.crewHere = (int)g.crew.size();
            }
            else g.crewHere = FitCrew(ship);
            Shop();
            Match::Loadout loadout = g.loadout;
            loadout.hull = loadout.hullMax;   // whole again, as ours (Refit::Restore)
            Match::FitShip(ship, loadout);
            ship->ModifyMissileCount(g.missiles - ship->GetMissileCount());
        }

        // During the fight, while its ship stands: what it has left (missiles, drone parts) and who is alive aboard.
        static void Remember(ShipManager *ship)
        {
            if (!ship || ship->bDestroyed || ship->ship.hullIntegrity.first <= 0) return;
            g.missiles = ship->GetMissileCount();
            if (ship->droneSystem) g.loadout.droneParts = ship->GetDroneCount();
            g.aboard.clear();
            for (CrewMember *member : CrewAboard(ship))
            {
                if (Alive(member)) g.aboard.push_back(Entry(member));
            }
        }

        // ---------------------------------------------------------------------------------------------------------
        // The round's end: whose crew is where
        // ---------------------------------------------------------------------------------------------------------

        // To the other ship as FTL's teleporter takes crew (DuelsBoarding.cpp does the same for a recall in a duel).
        static void MoveCrew(CrewMember *crew, ShipManager *from, ShipManager *to, int room)
        {
            crew->EmptySlot();
            from->vCrewList.erase(std::remove(from->vCrewList.begin(), from->vCrewList.end(), crew), from->vCrewList.end());
            crew->SetCurrentShip(to->iShipId);
            to->AddCrewMember(crew, room);
            crew->StartTeleportArrive();
        }

        static void Kill(CrewMember *crew)
        {
            crew->health.first = 0.f;
            crew->Kill(true);
        }

        // Ours aboard its ship as it is destroyed die with it, as in FTL (and in a duel, DuelsRefit.cpp). Here, at once:
        // FTL lets go of a destroyed enemy ship (it is no longer ship 1) before the round's end.
        static int KillOursAboard(ShipManager *ship)
        {
            std::vector<CrewMember*> ours;
            for (CrewMember *member : ship ? ship->vCrewList : std::vector<CrewMember*>())
            {
                if (member && !member->IsDrone() && member->iShipId == 0 && Alive(member)) ours.push_back(member);
            }
            for (CrewMember *member : ours) Kill(member);
            return (int)ours.size();
        }

        void OnRoundEnd(bool ownDestroyed, bool theirsDestroyed)
        {
            if (!g.active) return;
            ShipManager *own = G_->GetShipManager(0);
            ShipManager *ship = G_->GetShipManager(1);
            bool stands = ship && !theirsDestroyed && !ship->bDestroyed && ship->ship.hullIntegrity.first > 0;

            // Its crew aboard ours (its boarding drones too): they die with our destroyed ship.
            std::vector<CrewMember*> boarders;
            if (own)
            {
                for (CrewMember *member : own->vCrewList)
                {
                    if (member && member->iShipId == 1) boarders.push_back(member);
                }
            }
            if (ownDestroyed)
            {
                for (CrewMember *member : boarders)
                {
                    if (Alive(member)) Kill(member);
                }
            }

            // Its crew for the next round, with Permanent Death: those alive aboard its ship (as it last stood, if it
            // was destroyed) and aboard ours. (Without, its whole first crew comes back.)
            if (g.permadeath)
            {
                std::vector<CrewEntry> kept;
                if (stands)
                {
                    for (CrewMember *member : CrewAboard(ship))
                    {
                        if (Alive(member)) kept.push_back(Entry(member));
                    }
                }
                else kept = g.aboard;
                for (CrewMember *member : boarders)
                {
                    if (!member->IsDrone() && Alive(member)) kept.push_back(Entry(member));
                }
                g.crew.swap(kept);
            }

            // Its boarders go home (its ship takes them away at the next preparation); with no ship to go to, they leave
            // ours all the same.
            int left = 0;
            for (CrewMember *member : boarders)
            {
                if (member->bDead) continue;
                if (ship) MoveCrew(member, own, ship, 0);
                else own->RemoveCrewmember(member);
                ++left;
            }

            // Ours aboard its ship: home (aboard a destroyed one they died with it, KillOursAboard, as it was destroyed).
            int home = 0;
            if (own && ship && stands)
            {
                std::vector<CrewMember*> ours;
                for (CrewMember *member : ship->vCrewList)
                {
                    if (member && !member->IsDrone() && member->iShipId == 0 && Alive(member)) ours.push_back(member);
                }
                int room = own->GetSystemRoom(SYS_TELEPORTER);
                for (CrewMember *member : ours) MoveCrew(member, ship, own, room >= 0 ? room : 0);
                home = (int)ours.size();
            }
            else if (ship) KillOursAboard(ship);
            Log("Ai: round %d is over: %u of its crew for the next round; %d of ours came home, %d of its left ours", g.round,
                (unsigned)g.crew.size(), home, left);
        }

        // ---------------------------------------------------------------------------------------------------------
        // The match's frames
        // ---------------------------------------------------------------------------------------------------------

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

            // No pause, as in a duel (rules, section 1): the rounds run on the clock, and a game paused by the store, a
            // menu or a window without focus would stand still while they go on. From the moment FTL's first message
            // box is gone (DuelsLobby.cpp answers it; without a box, after 1.5 s): FTL answers a message box only while
            // its game is paused.
            CommandGui *gui = G_->GetWorld() ? G_->GetWorld()->commandGui : nullptr;
            if (gui && !GetState().noPause)
            {
                g.boxSeen = g.boxSeen || gui->choiceBox.bOpen || Lobby::FirstBoxAnswered();
                if (!gui->choiceBox.bOpen && (g.boxSeen || WallMs() - g.startMs > 1500.0))
                {
                    GetState().noPause = true;
                    Log("Ai: no pause from now on");
                }
            }

            // The ships meet: the AI's ship comes, fitted as it has bought (not a target yet: the fight hasn't begun).
            if (phase == Rounds::Phase::Starting && !g.spawned)
            {
                std::string message;
                if (!ship && SpawnEnemy(g.blueprint, message)) ship = G_->GetShipManager(1);
                if (ship)
                {
                    g.spawned = true;
                    FitShip(ship);
                    Match::ShowOpponent(ship);
                    // A player ship's blueprint has no boarding AI (FTL's enemies with a teleporter have one): without
                    // it the AI would never use its teleporter. FTL's ShipAI takes it at its start (its target kept).
                    WorldManager *world = G_->GetWorld();
                    CompleteShip *enemy = world && world->playerShip ? world->playerShip->enemyShip : nullptr;
                    if (enemy && enemy->shipManager == ship && ship->teleportSystem)
                    {
                        ShipManager *target = enemy->shipAI.target;
                        enemy->shipAI.OnInit(ship, BOARDING_AI);
                        if (!enemy->shipAI.target) enemy->shipAI.target = target;
                        Log("Ai: its teleporter: it boards as FTL's boarding enemies do");
                    }
                    Rounds::OnReplicaBuilt(ship);
                    std::string weapons;
                    for (const std::string &weapon : g.loadout.weapons) weapons += (weapons.empty() ? "" : ", ") + weapon;
                    Log("Ai: round %d: its %s is here (hull %d, reactor %d, shields %d, weapons %d: %s; engines %d, %d missiles, "
                        "%d drone parts, %d crew, %d scrap kept)", g.round, g.blueprint.c_str(), ship->ship.hullIntegrity.first,
                        g.loadout.reactor, LevelOf(SYS_SHIELDS), LevelOf(SYS_WEAPONS), weapons.c_str(), LevelOf(SYS_ENGINES),
                        ship->GetMissileCount(), ship->GetDroneCount(), g.crewHere, g.scrap);
                }
                else Log("Ai: its ship didn't come: %s", message.c_str());
            }

            bool fighting = Rounds::FightBegun() && (phase == Rounds::Phase::Fight || phase == Rounds::Phase::Ending);
            if (fighting && !g.counting && ship) StartCounting(ship);
            if (g.counting && fighting) Count(ship);
            if (fighting) Remember(ship);

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
                    if (destroyed)
                    {
                        int lost = KillOursAboard(ship);
                        if (lost) Log("Ai: %d of ours aboard die with its ship", lost);
                    }
                }
            }
        }
    }
}

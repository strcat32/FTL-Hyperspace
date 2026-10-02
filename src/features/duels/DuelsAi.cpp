#include "Global.h"
#include "Duels.h"
#include "DuelsAi.h"
#include "DuelsBays.h"
#include "DuelsConfig.h"
#include "DuelsConsole.h"
#include "DuelsCrew.h"
#include "DuelsLobby.h"
#include "DuelsMatch.h"
#include "DuelsRounds.h"
#include "DuelsShipControl.h"
#include "DuelsShips.h"
#include "DuelsTrace.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <random>
#include <set>

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
            std::string blueprint;       // the AI's ship ("" until the ship choice gives it one)
            std::string preferred;       // HOST DUEL's ship for it ("" random): its pick from a host's list
            double thinkUntil = 0.0;     // the ship choice: its ban or pick comes then
            std::mt19937 random;
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

            // Its level and FTL's pause in this match (roadmap DC, DD); the pause key's pause now.
            int level = NORMAL;
            bool pause = true;
            bool paused = false;
            // Running from a lost fight (Normal, Hard): FTL's own escape, its FTL drive charging; reported as it jumps.
            bool escaping = false, escaped = false;
            // Hard: our missiles it saw coming at a bay (each once), and how often it dragged a weapon out of the way.
            std::set<std::pair<const void*, unsigned>> missilesSeen;
            int dodges = 0;
            int aimLogs = 0;   // its aims at our bays logged this round (the first few)
            std::map<int, int> aims;   // what it aimed at this round (FTL's system, or our bay), how often
        };

        static AiState g;

        // HOST DUEL's choices for the next match (roadmap DC, DD); duels.cfg's at first.
        static int g_nextLevel = -1;
        static bool g_nextPause = true;

        static void ReadNext()
        {
            if (g_nextLevel >= 0) return;
            std::string level = Config::Value("ai_level");
            g_nextLevel = level == "easy" ? EASY : level == "hard" ? HARD : NORMAL;
            g_nextPause = Config::Value("ai_pause") != "off";
        }

        const char *LevelName(int level)
        {
            return level == EASY ? "easy" : level == HARD ? "hard" : "normal";
        }

        const char *LevelTitle(int level)
        {
            return level == EASY ? "Easy" : level == HARD ? "Hard" : "Normal";
        }

        void SetNext(int level, bool pause)
        {
            ReadNext();
            g_nextLevel = std::max((int)EASY, std::min((int)HARD, level));
            g_nextPause = pause;
            if (SettingsFromConfig())
            {
                Config::SaveValue("ai_level", LevelName(g_nextLevel));
                Config::SaveValue("ai_pause", pause ? "on" : "off");
            }
        }

        int NextLevel()
        {
            ReadNext();
            return g_nextLevel;
        }

        bool NextPause()
        {
            ReadNext();
            return g_nextPause;
        }

        int CurrentLevel()
        {
            return g.level;
        }

        bool PauseAllowed()
        {
            return g.active && g.pause;
        }

        bool Paused()
        {
            return g.active && g.pause && g.paused;
        }

        void TogglePause()
        {
            if (!PauseAllowed()) return;
            g.paused = !g.paused;
            Log("Ai: %s", g.paused ? "paused (the pause key)" : "the pause ends (the pause key)");
        }

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
            g.preferred = blueprint;
            g.level = NextLevel();
            g.pause = NextPause();
            // (The system clock: the game's own milliseconds since its start repeat from run to run.)
            uint64_t ticks = (uint64_t)std::chrono::system_clock::now().time_since_epoch().count();
            g.random.seed((uint32_t)(ticks ^ (ticks >> 32)));
            if (!Rounds::NextChoosesShips())
            {
                g.blueprint = blueprint;
                std::vector<std::string> ships = PlayerShips();
                if (g.blueprint.empty()) g.blueprint = ships.empty() ? std::string("PLAYER_SHIP_HARD") : ships[g.random() % ships.size()];
            }
            // FTL's own ship AI flies it (a duel before it had the opponent's replaced by its owner's game).
            GetState().aiOff[1] = false;
            Log("Ai: a match against FTL's AI in %s, level %s, %s", g.blueprint.empty() ? "the ship it picks in the ship choice"
                                                                                       : (g.blueprint + " (" + ShipTitle(g.blueprint) + ")").c_str(),
                LevelName(g.level), g.pause ? "FTL's pause allowed" : "no pause");
            Rounds::StartLocal();
        }

        void TakeShip(const std::string &blueprint)
        {
            if (!g.active || blueprint.empty() || blueprint == g.blueprint) return;
            g.blueprint = blueprint;
            Log("Ai: its ship for the match: %s (%s)", blueprint.c_str(), ShipTitle(blueprint).c_str());
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
            if (g.blueprint.empty()) return "AI";
            std::string title = ShipTitle(g.blueprint);
            // "Kestrel Cruiser A": the ship's first word is enough on the screen.
            size_t space = title.find(' ');
            return "AI " + (space == std::string::npos ? title : title.substr(0, space));
        }

        // The round's scrap for its level (roadmap DC): Easy 60%, Normal all of it, Hard 130%.
        static int ScrapShare(int scrap)
        {
            return g.level == EASY ? scrap * 6 / 10 : g.level == HARD ? scrap * 13 / 10 : scrap;
        }

        void OnPrep(int round, int scrap, const std::vector<Refit::ShopItem> &stock, bool permadeath)
        {
            if (round != g.round) g.scrap += ScrapShare(scrap);   // once a round, as ours (Refit::GiveScrap)
            g.round = round;
            g.spawned = false;
            g.defeated = false;
            g.counting = false;
            g.escaping = g.escaped = false;
            g.missilesSeen.clear();
            g.aimLogs = 0;
            g.aims.clear();
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
            for (CrewMember *member : leaving) Crew::RemoveForGood(ship, member);
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
                else Crew::RemoveForGood(own, member);
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
            std::string aims;
            for (const std::pair<const int, int> &aim : g.aims)
            {
                // (-1: FTL's AI took a room at random.)
                aims += (aims.empty() ? "" : ", ") + (aim.first < 0 ? std::string("a random room") : ShipSystem::SystemIdToName(aim.first)) +
                        " " + std::to_string(aim.second);
            }
            Log("Ai: round %d: it aimed at %s; %d missile(s) of ours dodged", g.round, aims.empty() ? "nothing" : aims.c_str(), g.dodges);
        }

        // ---------------------------------------------------------------------------------------------------------
        // Its play (roadmap DC): where it aims, running away, out of a missile's way
        // ---------------------------------------------------------------------------------------------------------

        static int Worth(const Blueprint *bp)
        {
            return bp ? bp->desc.cost : 0;
        }

        int AimAtBay(ShipManager *self, ShipManager *target, int system)
        {
            if (!g.active || !self || self->iShipId != 1 || !target || target->iShipId != 0) return system;
            if (system != SYS_WEAPONS && system != SYS_DRONES)
            {
                ++g.aims[system];
                return system;
            }
            // The bays with a weapon (a drone) in them, and what it is worth.
            std::vector<std::pair<int, int>> bays;
            if (system == SYS_WEAPONS && target->weaponSystem)
            {
                for (ProjectileFactory *weapon : target->GetWeaponList())
                {
                    ShipSystem *bay = Bays::BayOfItem(target, weapon);
                    if (bay) bays.push_back({bay->iSystemType, Worth(weapon->blueprint)});
                }
            }
            if (system == SYS_DRONES && target->droneSystem)
            {
                for (Drone *drone : target->GetDroneList())
                {
                    ShipSystem *bay = Bays::BayOfItem(target, drone);
                    if (bay) bays.push_back({bay->iSystemType, Worth(drone->blueprint)});
                }
            }
            if (bays.empty())
            {
                ++g.aims[system];
                return system;
            }
            int aim = g.level == HARD ? std::max_element(bays.begin(), bays.end(), [](const std::pair<int, int> &l, const std::pair<int, int> &r)
                                                         { return l.second < r.second; })->first
                                      : bays[std::uniform_int_distribution<size_t>(0, bays.size() - 1)(g.random)].first;
            ++g.aims[aim];
            if (g.aimLogs < 3)
            {
                ++g.aimLogs;
                Log("Ai: it aims at our %s (FTL chose our %s)", ShipSystem::SystemIdToName(aim).c_str(), ShipSystem::SystemIdToName(system).c_str());
            }
            return aim;
        }

        // Normal and Hard run from a lost fight: its hull at a quarter or less while ours is at half or more. FTL's own
        // escape: its FTL drive charges (the enemy window shows it) while its engines and piloting work; when it jumps,
        // the round is ours with half a point (rules, section 3), unless we destroy it first.
        static void ConsiderEscape(ShipManager *ship)
        {
            WorldManager *world = G_->GetWorld();
            CompleteShip *enemy = world && world->playerShip ? world->playerShip->enemyShip : nullptr;
            ShipManager *own = G_->GetShipManager(0);
            if (g.level == EASY || !enemy || enemy->shipManager != ship || !own) return;
            if (!g.escaping)
            {
                const int hull = ship->ship.hullIntegrity.first, hullMax = std::max(1, ship->ship.hullIntegrity.second);
                const int ours = own->ship.hullIntegrity.first, oursMax = std::max(1, own->ship.hullIntegrity.second);
                if (hull <= 0 || ship->bDestroyed || hull * 4 > hullMax || ours * 2 < oursMax) return;
                g.escaping = true;
                enemy->shipAI.escaping = true;
                Log("Ai: round %d: it runs (its hull %d of %d, ours %d of %d): its FTL drive charges", g.round, hull, hullMax, ours, oursMax);
                Console::Feed(Name() + " charges its FTL drive to run away");
                return;
            }
            if (!g.escaped && (ship->bJumping || enemy->shipAI.Escaped()))
            {
                g.escaped = true;
                g.defeated = true;   // gone, not destroyed: no defeat to report
                Log("Ai: round %d: it jumps away (its FTL drive at %.1f of %.1f)", g.round, ship->jump_timer.first, ship->jump_timer.second);
                Rounds::OpponentEscaped();
            }
        }

        // Hard: a missile of ours in its space, on its way to one of its bays, makes it drag that bay's weapon (drone) to
        // another slot, swapped with the least valuable one there. A shot keeps its tile (roadmap AF): the missile breaks
        // the bay with whatever is in it when it lands.
        static void Dodge(ShipManager *ship)
        {
            WorldManager *world = G_->GetWorld();
            if (g.level != HARD || !world || !ship) return;
            for (Projectile *p : world->space.projectiles)
            {
                if (!p || p->dead || p->ownerId != 0 || p->currentSpace != 1 || p->destinationSpace != 1 || p->GetType() != 3) continue;
                if (!g.missilesSeen.insert({(const void*)p, p->selfId}).second) continue;
                int room = ship->ship.GetSelectedRoomId((int)p->target.x, (int)p->target.y, true);
                ShipSystem *bay = room >= 0 ? ship->GetSystemInRoom(room) : nullptr;
                const void *item = Bays::IsBay(bay) ? Bays::ItemInBay(ship, bay) : nullptr;
                if (!item) continue;
                const int slot = Bays::BayNumber(bay->iSystemType) - 1;
                if (ship->weaponSystem)
                {
                    std::vector<ProjectileFactory*> weapons = ship->GetWeaponList();
                    if (slot >= 0 && slot < (int)weapons.size() && weapons[slot] == item)
                    {
                        int to = -1;
                        for (int i = 0; i < (int)weapons.size(); ++i)
                        {
                            if (i != slot && Worth(weapons[i]->blueprint) < Worth(weapons[slot]->blueprint) &&
                                (to < 0 || Worth(weapons[i]->blueprint) < Worth(weapons[to]->blueprint)))
                                to = i;
                        }
                        if (to < 0) continue;
                        Log("Ai: our missile at its %s (room %d): its %s goes to slot %d, its %s to slot %d", ShipSystem::SystemIdToName(bay->iSystemType).c_str(),
                            room, weapons[slot]->blueprint->name.c_str(), to, weapons[to]->blueprint->name.c_str(), slot);
                        ship->weaponSystem->SwapWeapons(slot, to);
                        ++g.dodges;
                        continue;
                    }
                }
                if (ship->droneSystem)
                {
                    std::vector<Drone*> drones = ship->GetDroneList();
                    if (slot >= 0 && slot < (int)drones.size() && drones[slot] == item)
                    {
                        int to = -1;
                        for (int i = 0; i < (int)drones.size(); ++i)
                        {
                            if (i != slot && Worth(drones[i]->blueprint) < Worth(drones[slot]->blueprint) &&
                                (to < 0 || Worth(drones[i]->blueprint) < Worth(drones[to]->blueprint)))
                                to = i;
                        }
                        if (to < 0) continue;
                        Log("Ai: our missile at its %s (room %d): its drone in slot %d goes to slot %d", ShipSystem::SystemIdToName(bay->iSystemType).c_str(),
                            room, slot, to);
                        ship->droneSystem->SwapDrones(slot, to);
                        ++g.dodges;
                    }
                }
            }
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

        // The ship choice (roadmap 3.9): in its turn the AI bans a type left, at random, and picks a ship of the offer at
        // random (from a host's list, the ship HOST DUEL's window gave it, if the list has it); each after a moment's
        // thought, as a player would take one.
        static void Choose()
        {
            Rounds::Summary s = Rounds::GetSummary();
            const Rounds::Summary::Choice &c = s.choice;
            bool revealed = !c.ships[0].empty();
            bool ban = !revealed && c.banner == 1;
            bool pick = !revealed && c.banner == 2 && !c.offer.empty() && !c.picked[1];
            if (!ban && !pick)
            {
                g.thinkUntil = 0.0;
                return;
            }
            double now = WallMs();
            if (g.thinkUntil <= 0.0)
            {
                g.thinkUntil = now + std::uniform_real_distribution<double>(1200.0, 3000.0)(g.random);
                return;
            }
            if (now < g.thinkUntil) return;
            g.thinkUntil = 0.0;
            if (ban)
            {
                std::vector<int> open;
                for (int type = 0; type < Ships::TYPE_COUNT; ++type)
                {
                    if (((c.pool >> type) & 1) && std::find(c.banned.begin(), c.banned.end(), type) == c.banned.end()) open.push_back(type);
                }
                if (open.empty()) return;
                int type = open[std::uniform_int_distribution<size_t>(0, open.size() - 1)(g.random)];
                Log("Ai: it bans the %s", Ships::TypeName(type));
                Rounds::OpponentBan(type);
                return;
            }
            int index = -1;
            for (size_t i = 0; i < c.offer.size(); ++i)
            {
                if (!g.preferred.empty() && c.offer[i] == g.preferred) index = (int)i;
            }
            bool set = index >= 0;
            if (!set) index = (int)std::uniform_int_distribution<size_t>(0, c.offer.size() - 1)(g.random);
            Log("Ai: it picks a ship (%s)", set ? "the one set for it" : "at random");
            Rounds::OpponentPick(index);
        }

        void OnFrame()
        {
            if (!g.active) return;
            Rounds::Phase phase = Rounds::GetPhase();
            ShipManager *ship = G_->GetShipManager(1);
            if (phase == Rounds::Phase::Choice) Choose();

            // No pause, as in a duel (rules, section 1): the rounds run on the clock, and a game paused by the store, a
            // menu or a window without focus would stand still while they go on. From the moment FTL's first message
            // box is gone (DuelsLobby.cpp answers it; without a box, after 1.5 s): FTL answers a message box only while
            // its game is paused.
            // With FTL's pause allowed (roadmap DD) only the pause key stops the world and the match's clock (Paused,
            // DuelsHooks.cpp, DuelsRounds.cpp); the store, the menus and a window without focus don't, as in a duel: the
            // preparation's store would have held its countdown.
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
            if (fighting && phase == Rounds::Phase::Fight && !g.defeated && ship)
            {
                Dodge(ship);
                ConsiderEscape(ship);
            }

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

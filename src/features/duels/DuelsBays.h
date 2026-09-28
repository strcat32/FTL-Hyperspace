#pragma once

#include <string>
#include <vector>

struct CrewMember;
struct GL_Line;
struct Ship;
struct ProjectileFactory;
struct ShipBlueprint;
struct ShipManager;
struct ShipSystem;

namespace Duels
{
    // Weapon bays (docs/design/weapon-bays.md, roadmap 2.3). Every player ship's weapons room is cut into 1x1 rooms,
    // one per weapon slot, when the game loads the ship's layout. Each is a subsystem "weapon_bay_N" (a Hyperspace
    // custom system) with as many bars as its weapon needs: damage, ion and hacking on a bay switch off only that
    // weapon, and crew repair each bay on its own. Bay 1 shares its room with the weapons system (the gunner console),
    // which takes no damage any more: it is only the reactor power for all weapons.
    namespace Bays
    {
        static const int MAX_BAYS = 4;

        // ResourceControl::LoadFile: a player ship's layout ("data/<layout>.txt") with the weapons room cut into bays.
        // Returns text itself for every other file, else a new buffer (text is freed).
        char *OnLoadFile(const std::string &fileName, char *text);

        // ShipManager::OnInit, before the ship is built: its blueprint gets the bays (once per blueprint).
        void PrepareBlueprint(ShipBlueprint *bp);
        // ShipManager::AddSystem: the layout of the ship getting a system (nullptr afterwards), for its room picture.
        void Building(const std::string *layout);

        // A cut room still looks like one room. Ship::OnInit (layout: the ship's, nullptr when done) gives the ship
        // being built; after ShipGraph::OnInit the doors inside cut rooms leave the ship's door list (hidden, open,
        // still walkable); CSurface::GL_CreateMultiLinePrimitive leaves out the walls inside cut rooms and adds grid
        // lines there instead (returns true when it changed the lines).
        void BuildingShip(Ship *ship, const std::string *layout);
        void OnGraphBuilt();
        bool OnLines(std::vector<GL_Line> &lines, float thickness);

        // After ShipManager::OnLoop: each bay's bars follow its weapon; on our own ship, a weapon whose bay is
        // damaged, ioned or hacked goes off; the weapons system stays undamaged.
        void AfterLoop(ShipManager *ship);

        // --- hook entry points ---

        // ShipManager::GetSystemInRoom: bay 1, not the weapons system beside it (hits, repairs, hacking, targeting).
        ShipSystem *InRoom(ShipManager *ship, int roomId, ShipSystem *found);
        // CrewMember::SetCurrentSystem: a crew member of the ship at the gunner console keeps manning the weapons
        // system (FTL gives a crew member every system in their room in turn; bay 1 would come last).
        bool KeepConsole(CrewMember *crew, ShipSystem *system);
        // The weapons system of a ship with bays takes no damage, ion or sabotage (its bays do).
        bool Untouchable(ShipSystem *system);
        // WeaponSystem::PowerWeapon: not while the weapon's bay is damaged, ioned or hacked (our ship only).
        bool MayPower(ShipManager *ship, ProjectileFactory *weapon);
        // ShipSystem::OnRender (the icon in the room): the weapons system's is not drawn, bay 1's is.
        bool HideRoomIcon(ShipSystem *system);
        // ShipSystem::SetFloorImage1: a system whose room was cut keeps its picture where the whole room was. Gives
        // the top left corner to draw it at (FTL takes the position from the system's room shape).
        bool OriginalRoomCorner(ShipSystem *system, int &x, int &y);

        bool IsBay(const ShipSystem *system);
        int BayNumber(int systemType);   // 1..MAX_BAYS, 0 when not a bay
        std::string Describe(ShipManager *ship);   // console: the rooms of a ship
        // duels_sync.csv: each bay's bars (intact/all), "i" when ioned, "h" when hacked, and its weapon on (+) or off (-).
        std::string Signature(ShipManager *ship);
        std::string Status();
    }
}

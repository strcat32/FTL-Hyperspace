#pragma once

#include <string>
#include <vector>

struct CrewMember;
struct Drone;
struct GL_Line;
struct Ship;
struct ProjectileFactory;
struct ShipBlueprint;
struct ShipManager;
struct ShipSystem;

namespace Duels
{
    // Weapon and drone bays (docs/design/weapon-bays.md, roadmap 2.3). Every player ship's weapons room and drone
    // control room are cut into 1x1 rooms, one per weapon or drone slot, when the game loads the ship's layout. Each
    // is a subsystem "weapon_bay_N" or "drone_bay_N" (Hyperspace custom systems) with as many bars as its weapon or
    // drone needs: damage, ion and hacking on a bay switch off only that one, and crew repair each bay on its own.
    // Bay 1 shares its room with the weapons system (the gunner console) or drone control, which take no damage any
    // more: they are only the reactor power for all weapons or drones.
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
        // WeaponSystem::PowerWeapon, DroneSystem::PowerDrone: not while the bay is damaged, ioned or hacked (our
        // ship only).
        bool MayPower(ShipManager *ship, ProjectileFactory *weapon);
        bool MayPowerDrone(ShipManager *ship, Drone *drone);
        // ShipSystem::OnRender (the icon in the room): the weapons system's is not drawn, bay 1's is.
        bool HideRoomIcon(ShipSystem *system);
        // WeaponBox::StatusColor, DroneBox::StatusColor: a weapon or drone whose bay is out gets a red box.
        bool BayOut(const ProjectileFactory *weapon);
        bool DroneBayOut(const Drone *drone);
        // ShipSystem::RenderPowerBoxes: the weapons system (drone control) shows the power of the weapons (drones)
        // whose bays are out as red (damaged) bars; 0 for every other system.
        int PowerOut(const ShipSystem *system);
        // SystemBox::OnRender: a bay without a weapon or drone has no box (empty slots are always the last ones, so
        // nothing moves).
        bool HideBox(const ShipSystem *system);
        // ShipSystem::SetFloorImage1: a system whose room was cut keeps its picture where the whole room was. Gives
        // the top left corner to draw it at (FTL takes the position from the system's room shape).
        bool OriginalRoomCorner(ShipSystem *system, int &x, int &y);

        bool IsBay(const ShipSystem *system);
        int BayNumber(int systemType);   // 1..MAX_BAYS, 0 when not a bay
        std::string Describe(ShipManager *ship);   // console: the rooms of a ship
        // duels_sync.csv: each bay (W1.., D1..) with its bars (intact/all), "i" when ioned, "h" when hacked, and its
        // weapon or drone on (+) or off (-).
        std::string Signature(ShipManager *ship);
        std::string Status();
    }
}

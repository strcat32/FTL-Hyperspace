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
        // damaged, ioned or hacked goes off. A bay's damage belongs to its room, not to the weapon: when a weapon
        // moves to another slot (dragged in the weapons bar, even in a fight), it leaves its bay's damage behind, and a
        // weapon with fewer bars moving into a damaged bay leaves the damage its bars can't show kept there (it shows
        // again when a bigger one moves in; the crew mend it point by point, as they would the bars).
        void AfterLoop(ShipManager *ship);
        // A system is new (ShipManager::AddSystem), or the round's repair made it whole (DuelsRefit.cpp): no kept
        // damage.
        void SystemAdded(ShipManager *ship, int systemId);
        void Repaired(ShipSystem *system);

        // --- hook entry points ---

        // ShipManager::GetSystemInRoom: bay 1, not the weapons system beside it (hits, repairs, hacking, targeting),
        // except for repairs of its spare bars (SetSelectingRepair).
        ShipSystem *InRoom(ShipManager *ship, int roomId, ShipSystem *found);
        // CrewMember::SetCurrentSystem: a crew member of the ship at the gunner console keeps manning the weapons
        // system (FTL gives a crew member every system in their room in turn; bay 1 would come last).
        bool KeepConsole(CrewMember *crew, ShipSystem *system);
        // The weapons system of a ship with bays takes no damage, ion or sabotage (its bays do), except on its spare
        // bars when a bay hands damage on (TakeBuffer).
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
        // ShipSystem::RenderPowerBoxes of the weapons system (drone control): its bars drawn per weapon (drone), in
        // slot order from the bottom, each part as FTL draws a system of its own: the weapon's power, its bay's damage
        // (red where that weapon's bars are) and repair, blue while the bay is ioned, purple while it is hacked. The
        // system's spare bars go on top, with its own damage (buffer points). False for every other system: FTL
        // draws it.
        struct Segment
        {
            int bars = 0, reactor = 0, battery = 0, bonus = 0, damage = 0;
            float repair = 0.f, partial = 0.f;
            bool ioned = false, hacked = false;
        };
        bool PowerSegments(ShipSystem *system, std::vector<Segment> &segments);

        // Buffer points (rules section 9): the weapons system's (drone control's) bars beyond what its weapons (drones)
        // need protect them. Damage on a bay goes to those spare bars first, as long as some are whole; what they can't
        // take goes to the bay. The spare bars stay damaged until the crew repairs them. TakeBuffer applies it to the
        // system and returns how much it took (0 for anything but a bay of a ship whose game decides its damage);
        // while it does, the system may be damaged (BufferHit). Ion is not buffered: FTL's ion takes power from the
        // whole system, which would switch weapons off and lock all of them.
        int TakeBuffer(ShipSystem *bay, int amount);
        // A bay takes damage (after its buffer points): logged with its room and the weapon or drone in it now.
        void LogDamage(ShipSystem *bay, int amount);
        bool BufferPartial(ShipSystem *bay, float amount, bool overTime, bool &result);
        bool BufferHit();
        // CrewAI::SelectRepair runs: crew pick what to repair in their room (a fire, else "the system in this room",
        // bay 1 in W1 and D1). Meanwhile W1 (D1) gives the weapons system (drone control) instead once bay 1 needs no
        // repair, so its spare bars are repaired there after bay 1 (InRoom).
        void SetSelectingRepair(bool on);
        // SystemBox::OnRender: a bay without a weapon or drone has no box (empty slots are always the last ones, so
        // nothing moves).
        bool HideBox(const ShipSystem *system);
        // SystemControl::CreateSystemBoxes: our own bays get no box in the subsystem panel (the weapons and drones bars
        // show them, and the panel needs the space); while it lays the panel out, our ship has no bays.
        void SetPanelLayout(bool on);
        bool HiddenFromPanel(const ShipManager *ship, const ShipSystem *system);
        // ShipSystem::SetFloorImage1: a system whose room was cut keeps its picture where the whole room was. Gives
        // the top left corner to draw it at (FTL takes the position from the system's room shape).
        bool OriginalRoomCorner(ShipSystem *system, int &x, int &y);

        bool IsBay(const ShipSystem *system);
        // SystemStoreBox::CanHold for a subsystem (roadmap CR): Hyperspace counts every system without reactor power
        // against the ship's subsystem limit, the bays too (custom subsystems), so a duel's ship had no room for doors,
        // sensors, piloting or a backup battery, bought or bought back. Here the bays don't count.
        bool CanFitSubsystem(ShipManager *ship, int systemId);
        // SystemBox::MouseMove: the tooltip of a bay's icon (the enemy window): its weapon or drone, and how the bay is.
        // FTL's own text for a system comes from level descriptions the bays (custom systems) don't have.
        bool Tooltip(const ShipSystem *system, std::string &text);
        int BayNumber(int systemType);   // 1..MAX_BAYS, 0 when not a bay
        std::string Describe(ShipManager *ship);   // console: the rooms of a ship
        // duels_sync.csv: each bay (W1.., D1..) with its bars (intact/all), "i" when ioned, "h" when hacked, and its
        // weapon or drone on (+) or off (-).
        std::string Signature(ShipManager *ship);
        std::string Status();
    }
}

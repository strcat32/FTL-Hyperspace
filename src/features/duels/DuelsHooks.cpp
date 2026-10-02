#include "Global.h"
#include "CommandConsole.h"
#include "Duels.h"
#include "DuelsAi.h"
#include "DuelsBays.h"
#include "DuelsBoarding.h"
#include "DuelsHacking.h"
#include "DuelsMind.h"
#include "DuelsConsole.h"
#include "DuelsCrew.h"
#include "DuelsDemo.h"
#include "DuelsDrones.h"
#include "DuelsEnvironment.h"
#include "DuelsFair.h"
#include "DuelsHud.h"
#include "DuelsLobby.h"
#include "DuelsMatch.h"
#include "DuelsMatchUi.h"
#include "DuelsRefit.h"
#include "DuelsRejoin.h"
#include "DuelsNet.h"
#include "DuelsRooms.h"
#include "DuelsReplayUi.h"
#include "DuelsRounds.h"
#include "DuelsScreen.h"
#include "DuelsShipControl.h"
#include "DuelsView.h"
#include "DuelsWindow.h"

#include <algorithm>
#include <boost/algorithm/string.hpp>
#include <cmath>
#include <set>

// ---------------------------------------------------------------------------------------------
// Frame tick and loop counters
// ---------------------------------------------------------------------------------------------

// The end screen's LOBBY (roadmap 3.5, part 5): FTL's main menu, as its pause menu's MAIN MENU asks for it (CApp::OnLoop
// carries out the command CommandGui::GetCommand gives: 5 saves the score, cleans the game up and opens the menu).
HOOK_METHOD_PRIORITY(CommandGui, GetCommand, -2000, () -> int)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CommandGui::GetCommand -> Begin (DuelsHooks.cpp)\n")
    if (Duels::Lobby::TakeMenuRequest()) return 5;
    return super();
}

// Outermost CApp::OnLoop hook, so the per-frame work sees everything the frame did.
HOOK_METHOD_PRIORITY(CApp, OnLoop, -5000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CApp::OnLoop -> Begin (DuelsHooks.cpp)\n")
    super();
    Duels::OnFrame();
}

// Keeps running while the window is unfocused, unlike parts of the main loop.
HOOK_METHOD(CApp, GenInputEvents, () -> void)
{
    LOG_HOOK("HOOK_METHOD -> CApp::GenInputEvents -> Begin (DuelsHooks.cpp)\n")
    ++Duels::GetState().inputPumps;
    super();
}

HOOK_METHOD(WorldManager, OnLoop, () -> void)
{
    LOG_HOOK("HOOK_METHOD -> WorldManager::OnLoop -> Begin (DuelsHooks.cpp)\n")
    Duels::State &state = Duels::GetState();
    // A replay (roadmap 5.1): its speed is how many steps of FTL's world a frame takes (a half step for half speed), and
    // its records come between the steps, its clock moving on with them (DuelsDemo.cpp).
    int steps = 1;
    float share = 1.f;
    CFPS *fps = G_->GetCFPS();
    Duels::Demo::Pace(steps, share, fps->SpeedFactor * 62.5);
    float frame = fps->SpeedFactor;
    fps->SpeedFactor = frame * share;
    for (int step = 0; step < steps; ++step)
    {
        ++state.worldLoops;
        // Raw field on purpose: GetSpeedFactor() is rescaled by Hyperspace's time dilation hooks.
        state.gameTime += fps->SpeedFactor * 0.0625;
        Duels::Demo::BeforeWorldStep(fps->SpeedFactor * 62.5);
        super();
        if (Duels::Demo::SeekBudgetSpent()) break;   // a seek's steps for this frame (DuelsDemo.cpp)
    }
    fps->SpeedFactor = frame;
}

HOOK_METHOD(WorldManager, PauseLoop, () -> void)
{
    LOG_HOOK("HOOK_METHOD -> WorldManager::PauseLoop -> Begin (DuelsHooks.cpp)\n")
    ++Duels::GetState().pauseLoops;
    super();
}

HOOK_METHOD(CApp, OnExit, () -> void)
{
    LOG_HOOK("HOOK_METHOD -> CApp::OnExit -> Begin (DuelsHooks.cpp)\n")
    Duels::Log("Game exiting");
    Duels::Demo::End("the game closed");
    Duels::Shutdown();
    super();
}

// ---------------------------------------------------------------------------------------------
// Console: "DUEL <verb> <args...>"
// ---------------------------------------------------------------------------------------------

// Outer to Hyperspace's own RunCommand hook, so DUEL never reaches the vanilla console.
HOOK_METHOD_PRIORITY(CommandGui, RunCommand, -100, (std::string& command) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CommandGui::RunCommand -> Begin (DuelsHooks.cpp)\n")
    std::string name = command.substr(0, command.find(' '));
    boost::to_upper(name);
    if (name != "DUEL")
    {
        // Hyperspace's own commands (scrap, hull, crew, ...) are test commands too.
        if (!Duels::GetState().debug)
        {
            Duels::Log("console: %s -> refused (debug mode is off)", command.c_str());
            Duels::Console::Print("DUEL: Hyperspace's commands need debug mode (debug on)");
            return;
        }
        super(command);
        return;
    }

    std::string message;
    Duels::Command cmd;
    if (!Duels::ParseCommand(command.size() > 4 ? command.substr(4) : "", cmd))
    {
        message = "usage: DUEL <verb> <args...>";
    }
    else
    {
        bool ok = Duels::Execute(cmd, message);
        if (message.empty()) message = ok ? "ok" : "failed";
    }
    Duels::Log("console: %s -> %s", command.c_str(), message.c_str());
    Duels::Console::Print("DUEL: " + message);
}

// ---------------------------------------------------------------------------------------------
// No-pause: every pause source is blocked while Duels::State::noPause is set.
// Layered on purpose: callers may use IsPaused(), SetPaused() or read the flags directly.
// ---------------------------------------------------------------------------------------------

HOOK_METHOD_PRIORITY(CommandGui, IsPaused, -1000, () -> bool)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CommandGui::IsPaused -> Begin (DuelsHooks.cpp)\n")
    // A duel pauses only while its connection is lost (roadmap AA) and in a timeout both players took (roadmap BF): FTL's
    // world stands still for both players. A replay's pause stops it too (roadmap 5.1), and a message box of FTL's in a
    // replay (its first one, come late: FTL answers a box only while paused, roadmap BO). Going back into a match after a
    // crash, our ship waits as the file made it until the match is there (roadmap BR). While a duel's room waits for its
    // guest, the run stands still too: nobody prepares before the match (roadmap BT).
    if (Duels::Match::WaitingForMatch()) return true;   // (FTL's first message box is answered in a pause too)
    if (Duels::GetState().noPause)
    {
        return Duels::Rounds::NetPaused() || Duels::Rejoin::Trying() || Duels::Demo::ReplayPaused() || Duels::Rounds::TimeoutPaused() ||
               (Duels::Net::Replaying() && choiceBox.bOpen);
    }
    return super();
}

HOOK_METHOD_PRIORITY(CommandGui, SetPaused, -1000, (bool val, bool isAutoPause) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CommandGui::SetPaused -> Begin (DuelsHooks.cpp)\n")
    if (val && Duels::GetState().noPause)
    {
        ++Duels::GetState().blockedPauses;
        return;
    }
    super(val, isAutoPause);
}

HOOK_METHOD_PRIORITY(CommandGui, OnLoop, -1000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CommandGui::OnLoop -> Begin (DuelsHooks.cpp)\n")
    super();
    if (!Duels::GetState().noPause) return;

    if (Duels::Net::Replaying() && choiceBox.bOpen) return;   // a message box in a replay: FTL's pause for its answer
    if (bPaused || bAutoPaused || menu_pause || event_pause || touch_pause)
    {
        ++Duels::GetState().blockedPauses;
        bPaused = false;
        bAutoPaused = false;
        menu_pause = false;
        event_pause = false;
        touch_pause = false;
    }
}

// ---------------------------------------------------------------------------------------------
// Enemy ship driven by commands instead of its AI
// ---------------------------------------------------------------------------------------------

// Outermost, so no AI decision (power, weapons, crew tasks, surrender, escape) runs for a replaced ship.
// ShipAI also runs for the player ship, hence the ship id filter.
HOOK_METHOD_PRIORITY(ShipAI, OnLoop, -10000, (bool hostile) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipAI::OnLoop -> Begin (DuelsHooks.cpp)\n")
    if (!playerShip && ship && ship->iShipId == 1 && Duels::GetState().aiOff[1])
    {
        Duels::OnAiTakeover(ship);
        return;
    }
    super(hostile);
}

// ShipManager::OnLoop re-powers enemy systems every frame through CheckForRepower, which undid every power
// change we made (Step 1 finding). A replaced ship's power belongs to its owner, so skip it for ship 1.
HOOK_METHOD_PRIORITY(ShipSystem, CheckForRepower, -10000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipSystem::CheckForRepower -> Begin (DuelsHooks.cpp)\n")
    if (_shipObj.iShipId == 1 && Duels::GetState().aiOff[1]) return;
    // A replay's ship 0 is the recorder's: its power follows the recorder's states (roadmap 5.1).
    if (_shipObj.iShipId == 0 && Duels::Match::IsDriven(0)) return;
    super();
}

// No game over in a duel: a destroyed ship or a dead crew loses the round (DuelsRounds.cpp), and the next preparation
// restores the ship.
HOOK_METHOD_PRIORITY(CommandGui, CheckGameover, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CommandGui::CheckGameover -> Begin (DuelsHooks.cpp)\n")
    if (!Duels::Rounds::GameOverAllowed()) return;
    super();
}

// FTL's "enemy crew dead" end (the ship turns derelict, stops being a target, and our FTL drive fills) never comes for
// the duel's replica: its crew are puppets, and a frame without any (before the roster, or while its owner's crew die)
// would end the fight here. The owner's game reports a dead crew (DuelsRounds.cpp).
HOOK_METHOD_PRIORITY(CompleteShip, DeadCrew, -2000, () -> bool)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CompleteShip::DeadCrew -> Begin (DuelsHooks.cpp)\n")
    if (!bPlayerShip && Duels::GetState().aiOff[1] && shipManager && shipManager == G_->GetShipManager(1)) return false;
    return super();
}

// FTL saves the run (continue.sav, and the score profile with it) when its window loses focus, on quitting and after
// a jump. A duel is no run to continue: no saving while one runs; the next save after it catches up. A save that fails
// (two test games on one computer share the file) would show FTL's "unable to save progress" box, which waits for a
// click, often just as a duel begins: in FTL: Duels a failed save is only logged, never shown (the user saw the box
// before a duel too).
static bool DuelRunning()
{
    return Duels::Net::IsConnected() || Duels::Rounds::GetPhase() != Duels::Rounds::Phase::None;
}

HOOK_METHOD_PRIORITY(WorldManager, SaveGame, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> WorldManager::SaveGame -> Begin (DuelsHooks.cpp)\n")
    static bool skipLogged = false;
    if (DuelRunning())
    {
        if (!skipLogged) Duels::Log("Save: FTL doesn't save the run while a duel runs");
        skipLogged = true;
        return;
    }
    skipLogged = false;
    super();
}

HOOK_METHOD_PRIORITY(CommandGui, ShowWriteError, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CommandGui::ShowWriteError -> Begin (DuelsHooks.cpp)\n")
    Duels::Log("Save: FTL couldn't save the run (its message box stays closed)");
}

// A duel never pauses (no-pause): FTL's "PAUSED" banner, drawn while the store or a menu is open, would say it does.
// While a room waits, the run stands still, but SPACE doesn't go on (roadmap BT): the Duels window says why instead.
HOOK_METHOD_PRIORITY(CommandGui, RenderPause, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CommandGui::RenderPause -> Begin (DuelsHooks.cpp)\n")
    if (Duels::GetState().noPause || Duels::Match::WaitingForMatch()) return;
    super();
}

// Upgrades (and the crew and equipment screens) only in a match's preparation (rules, section 1): FTL asks this for
// the upgrade button and the U, C and I keys. Hyperspace's own hook is inside (priority 1000).
HOOK_METHOD_PRIORITY(TutorialManager, AllowUpgrades, -2000, () -> bool)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> TutorialManager::AllowUpgrades -> Begin (DuelsHooks.cpp)\n")
    if (!Duels::Rounds::ShoppingAllowed()) return false;
    return super();
}

// Running away (roadmap AD): in a match FTL's JUMP button (and the jump key) ends the round as an escape, when FTL
// would jump (the drive charged, the engines and piloting working); the star map stays shut, and outside a match too
// (roadmap BU: the ship jumped to other beacons). FTL's tutorial keeps its own jump.
HOOK_METHOD_PRIORITY(FTLButton, MouseClick, -2000, (int mX, int mY) -> bool)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> FTLButton::MouseClick -> Begin (DuelsHooks.cpp)\n")
    bool jump = super(mX, mY);
    TutorialManager *tutorial = G_->GetTutorialManager();
    if (!jump || (tutorial && tutorial->Running())) return jump;
    std::string message;
    if (Duels::Rounds::InMatch()) Duels::Rounds::Escape(message);
    return false;
}

// FTL's anti-ship battery holds its fire while ours runs (roadmap Y): ours fires on the schedule both games share, at
// our own ship only (DuelsEnvironment.cpp).
HOOK_METHOD_PRIORITY(SpaceManager, UpdatePDS, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> SpaceManager::UpdatePDS -> Begin (DuelsHooks.cpp)\n")
    if (Duels::Environment::ReplacesBattery()) return;
    super();
}

// FTL fills the FTL drive whenever the ship is safe (WorldManager::OnLoop: no hostile ship, as in the fight's first
// frames before the opponent's replica turns hostile). In a match the drive charges at FTL's own pace in the fight,
// from empty (roadmap AD); what else SetSafe readies stays.
HOOK_METHOD_PRIORITY(ShipManager, SetSafe, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipManager::SetSafe -> Begin (DuelsHooks.cpp)\n")
    if (!Duels::Rounds::InMatch()) return super();
    float charge = jump_timer.first;
    super();
    jump_timer.first = charge;
}

// The shop buys back (roadmap V): in a match's preparation a right-click in the upgrade screen with nothing waiting
// to be taken back takes a level back, and sells an extra system at its lowest level (DuelsRefit.cpp).
HOOK_METHOD_PRIORITY(UpgradeBox, MouseRightClick, -2000, (int mX, int mY) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> UpgradeBox::MouseRightClick -> Begin (DuelsHooks.cpp)\n")
    if (Duels::Refit::TakeBackLevel(this)) return;
    super(mX, mY);
}

HOOK_METHOD_PRIORITY(ReactorButton, OnRightClick, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ReactorButton::OnRightClick -> Begin (DuelsHooks.cpp)\n")
    if (Duels::Refit::TakeBackReactor(this)) return;
    super();
}

HOOK_METHOD_PRIORITY(UpgradeBox, OnRender, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> UpgradeBox::OnRender -> Begin (DuelsHooks.cpp)\n")
    super();
    Duels::Refit::RenderSaleMark(this);
}

// Our weapon and drone bays are systems of their own (custom ones), but there is nothing to upgrade about them: while
// FTL's upgrade screen builds its boxes (Hyperspace's Upgrades::OnInit gives every custom system one), the bays are
// out of the ship's system keys, so they get none (roadmap AE; they showed as subsystems with a price of 0).
HOOK_METHOD_PRIORITY(Upgrades, OnInit, -2000, (ShipManager *ship) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> Upgrades::OnInit -> Begin (DuelsHooks.cpp)\n")
    std::vector<std::pair<int, int>> hidden;
    if (ship)
    {
        for (ShipSystem *system : ship->vSystemList)
        {
            if (!Duels::Bays::IsBay(system)) continue;
            int id = system->iSystemType;
            if (id < 0 || id >= (int)ship->systemKey.size()) continue;
            hidden.push_back(std::make_pair(id, ship->systemKey[id]));
            ship->systemKey[id] = -1;
        }
    }
    super(ship);
    for (const std::pair<int, int> &key : hidden) ship->systemKey[key.first] = key.second;
}

HOOK_METHOD_PRIORITY(Upgrades, OnLoop, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> Upgrades::OnLoop -> Begin (DuelsHooks.cpp)\n")
    Duels::Refit::OnUpgradesLoop();   // a sale builds the boxes anew before FTL goes through them
    super();
}

HOOK_METHOD_PRIORITY(Upgrades, Open, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> Upgrades::Open -> Begin (DuelsHooks.cpp)\n")
    super();
    Duels::Refit::OnUpgradesOpen();
}

// Game speed must stay at normal: under split authority a faster client would charge weapons faster.
HOOK_METHOD_PRIORITY(CFPS, OnLoop, -1000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CFPS::OnLoop -> Begin (DuelsHooks.cpp)\n")
    if (Duels::GetState().noPause)
    {
        speedEnabled = false;
        speedLevel = 0;
    }
    super();
}

// ---------------------------------------------------------------------------------------------
// Network duel (DuelsMatch.cpp): shots, their timing, and hits decided by the defender.
// All outermost, so the game's and Hyperspace's own code runs inside, unchanged, whenever the duel isn't involved.
// ---------------------------------------------------------------------------------------------

// A projectile leaves one of our weapons (or artillery systems): tell the opponent (exact target point, spawn time).
HOOK_METHOD_PRIORITY(ProjectileFactory, GetProjectile, -2000, () -> Projectile*)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ProjectileFactory::GetProjectile -> Begin (DuelsHooks.cpp)\n")
    // A decided round lets no shot leave (roadmap O): not even the rest of a volley FTL queued just before it (a burst
    // laser's shots leave a few frames apart).
    if (!queuedProjectiles.empty() && !Duels::Match::AllowNewShots(this))
    {
        for (Projectile *queued : queuedProjectiles) delete queued;
        queuedProjectiles.clear();
        return nullptr;
    }
    Projectile *projectile = super();
    if (projectile && iShipId == 0) Duels::Match::OnOwnProjectile(this, projectile);
    return projectile;
}

// The AI's ship (a match against the AI, roadmap 3.6) is drawn as a duel's opponent, mirrored and facing us
// (DuelsView.cpp). FTL fires an enemy's weapons upwards (270), out of its window's top, which went up the screen past
// the ship's nose; a player ship's shots leave forward (0) instead, which the mirror turns towards us, as the duel's
// replica's do. Before Hyperspace's ProjectileFactory::Update (CustomWeapons.cpp) makes the shot with the angle.
HOOK_METHOD_PRIORITY(ProjectileFactory, Update, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ProjectileFactory::Update -> Begin (DuelsHooks.cpp)\n")
    if (iShipId == 1 && Duels::Ai::Active()) currentFiringAngle = 0.f;
    Duels::Fair::CheatCharging(this);
    super();
}

// The replica's artillery charges but never fires by itself: FTL would pick a target of its own, and its shots come
// from its owner's game. While its loop runs, its weapon is never ready (as Hyperspace holds a neutral ship's).
static bool g_replicaArtilleryLoop = false;

HOOK_METHOD_PRIORITY(ArtillerySystem, OnLoop, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ArtillerySystem::OnLoop -> Begin (DuelsHooks.cpp)\n")
    g_replicaArtilleryLoop = Duels::Match::ReplicaArtillery(this);
    super();
    g_replicaArtilleryLoop = false;
}

HOOK_METHOD_PRIORITY(ProjectileFactory, ReadyToFire, -2000, () -> bool)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ProjectileFactory::ReadyToFire -> Begin (DuelsHooks.cpp)\n")
    bool ready = super();
    // No new volley once the round is decided (roadmap O): a bomb still went 0.4 s after (the opponent's ship no longer
    // hostile stops the others).
    if (ready && !Duels::Match::AllowNewShots(this)) return false;
    if (!g_replicaArtilleryLoop || !ready) return ready;
    Duels::Match::OnReplicaArtilleryHeld();
    return false;
}

// Crystal Vengeance: the shards our ship breaks off when hit go to the opponent like shots; the replica breaks off
// none of its own (its owner's game sends them).
HOOK_METHOD_PRIORITY(ShipManager, CheckCrystalAugment, -2000, (Pointf pos) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipManager::CheckCrystalAugment -> Begin (DuelsHooks.cpp)\n")
    if (!Duels::Match::AllowShards(this)) return;
    size_t before = superBarrage.size();
    super(pos);
    if (iShipId != 0) return;
    for (size_t i = before; i < superBarrage.size(); ++i) Duels::Match::OnOwnShard(superBarrage[i]);
}

// Timing of the opponent's shots on our screen: 0 updates = wait, more than 1 = catch up.
HOOK_METHOD_PRIORITY(SpaceManager, UpdateProjectile, -2000, (Projectile *projectile) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> SpaceManager::UpdateProjectile -> Begin (DuelsHooks.cpp)\n")
    int runs = Duels::Match::ProjectileUpdates(projectile);
    int space = projectile->currentSpace;
    for (int run = 0; run < runs; ++run)
    {
        super(projectile);
        if (projectile->dead || projectile->startedDeath || projectile->currentSpace != space) break;
    }
}

// Our shot at the replica waits for the defender's verdict, then plays out that verdict.
HOOK_METHOD_PRIORITY(Projectile, CollisionCheck, -2000, (Collideable *other) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> Projectile::CollisionCheck -> Begin (DuelsHooks.cpp)\n")
    if (!Duels::Match::BeginCollisionCheck(this, other)) return;
    super(other);
    Duels::Match::EndCollisionCheck();
}

HOOK_METHOD_PRIORITY(ShipManager, CollisionShield, -2000, (Pointf start, Pointf finish, Damage damage, bool raytrace) -> CollisionResponse)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipManager::CollisionShield -> Begin (DuelsHooks.cpp)\n")
    CollisionResponse forced;
    if (Duels::Match::ForcedShieldResponse(this, start, finish, damage, forced)) return forced;
    CollisionResponse response = super(start, finish, damage, raytrace);
    Duels::Match::ObserveShield(this, response);
    return response;
}

HOOK_METHOD_PRIORITY(ShipManager, DamageArea, -2000, (Pointf location, Damage dmg, bool forceHit) -> bool)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipManager::DamageArea -> Begin (DuelsHooks.cpp)\n")
    bool hit = false;
    if (Duels::Match::ForcedDamageArea(this, location, hit)) return hit;
    int hullBefore = ship.hullIntegrity.first;
    hit = super(location, dmg, forceHit);
    Duels::Match::ObserveDamageArea(this, hit, hullBefore);
    return hit;
}

// Bombs override the collision check; ours in the replica waits there for the defender's verdict.
HOOK_METHOD_PRIORITY(BombProjectile, CollisionCheck, -2000, (Collideable *other) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> BombProjectile::CollisionCheck -> Begin (DuelsHooks.cpp)\n")
    if (!Duels::Match::BeginBombCheck(this, other)) return;
    super(other);
    Duels::Match::EndCollisionCheck();
}

// A bomb's dodge roll, when it appears in its target room.
// What FTL shows of the opponent's ship follows what their game sent (roadmap 4.5): FTL's sensor levels, but no more
// than their state's vision. A replay with full sensors shows everything (roadmap BA).
HOOK_METHOD_PRIORITY(ShipManager, DoSensorsProvide, -2000, (int vision) -> bool)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipManager::DoSensorsProvide -> Begin (DuelsHooks.cpp)\n")
    if (Duels::Match::FullSensors(this)) return true;
    return super(vision) && Duels::Match::SensorsAllow(this, vision);
}

HOOK_METHOD_PRIORITY(ShipManager, CheckVision, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipManager::CheckVision -> Begin (DuelsHooks.cpp)\n")
    super();
    Duels::Match::ClampVision(this);
}

HOOK_METHOD_PRIORITY(ShipManager, GetDodged, -2000, () -> bool)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipManager::GetDodged -> Begin (DuelsHooks.cpp)\n")
    bool dodged = false;
    if (Duels::Match::ForcedDodge(this, dodged)) return dodged;
    if (!Duels::Match::RolledDodge(this, dodged)) dodged = super();
    Duels::Match::ObserveDodge(this, dodged);
    return dodged;
}

// The replica's rooms follow their owner's (DuelsRooms.cpp): its own fire spreading, oxygen and breaches don't run.
// Outer to Hyperspace's rewrite of this function.
HOOK_METHOD_PRIORITY(ShipManager, UpdateEnvironment, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipManager::UpdateEnvironment -> Begin (DuelsHooks.cpp)\n")
    if (!Duels::Rooms::RunsEnvironment(this)) return;
    super();
}

// The replica's subsystems keep their owner's power, whatever this game's environment does to them.
HOOK_METHOD_PRIORITY(ShipManager, OnLoop, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipManager::OnLoop -> Begin (DuelsHooks.cpp)\n")
    Duels::Match::HoldReplicaHacking(this);
    super();
    Duels::Bays::AfterLoop(this);
    Duels::Match::HoldReplicaSubsystems(this);
}

// Bonus power (Zoltan crew): a replica system has its owner's, not what its puppets in the room would give.
HOOK_METHOD_PRIORITY(ShipSystem, SetBonusPower, -2000, (int amount, int permanentPower) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipSystem::SetBonusPower -> Begin (DuelsHooks.cpp)\n")
    int owner = 0;
    if (Duels::Match::ReplicaBonusPower(this, owner)) return super(owner, owner);
    super(amount, permanentPower);
}

HOOK_METHOD_PRIORITY(WeaponSystem, SetBonusPower, -2000, (int amount, int permanentPower) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> WeaponSystem::SetBonusPower -> Begin (DuelsHooks.cpp)\n")
    int owner = 0;
    if (Duels::Match::ReplicaBonusPower(this, owner)) return super(owner, owner);
    super(amount, permanentPower);
}

HOOK_METHOD_PRIORITY(DroneSystem, SetBonusPower, -2000, (int amount, int permanentPower) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> DroneSystem::SetBonusPower -> Begin (DuelsHooks.cpp)\n")
    int owner = 0;
    if (Duels::Match::ReplicaBonusPower(this, owner)) return super(owner, owner);
    super(amount, permanentPower);
}

// FTL's ESC menu in a duel or a replay (roadmap BO): HANGAR and RESTART would start a run of FTL's own (greyed out),
// and the duel's box goes where FTL shows its run's difficulty, content, ship achievements and seed (DuelsMatchUi.cpp).
HOOK_METHOD_PRIORITY(MenuScreen, Open, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> MenuScreen::Open -> Begin (DuelsHooks.cpp)\n")
    super();
    Duels::MatchUi::OnEscMenuOpen(this);
}

HOOK_METHOD_PRIORITY(MenuScreen, OnRender, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> MenuScreen::OnRender -> Begin (DuelsHooks.cpp)\n")
    super();
    Duels::MatchUi::RenderEscMenu(this);
}

// The opponent's crew in our game are puppets: their health is their owner's (DuelsCrew.cpp), so nothing here
// changes it, and they repair nothing (the owner's state brings the replica's system health).
HOOK_METHOD_PRIORITY(CrewMember, DirectModifyHealth, -2000, (float health) -> bool)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CrewMember::DirectModifyHealth -> Begin (DuelsHooks.cpp)\n")
    if (Duels::Crew::IsPuppet(this)) return false;
    return super(health);
}

// No achievements in FTL: Duels (roadmap BP): none of FTL's (nor Steam's: they go through here), none of Hyperspace's
// own (CustomAchievementTracker::SetAchievement, CustomAchievements.cpp), and no popup of them in a duel.
HOOK_METHOD_PRIORITY(AchievementTracker, SetAchievement, -2000, (const std::string& achievement, bool noPopup, bool sendToServer) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> AchievementTracker::SetAchievement -> Begin (DuelsHooks.cpp)\n")
    static bool told = false;
    if (!told)
    {
        told = true;
        Duels::Log("Achievements: none in FTL: Duels (%s not given)", achievement.c_str());
    }
}

// Between a match's fights nothing harms a crew: no lack of air (a sold oxygen system), no fire (Rounds::BetweenFights).
HOOK_METHOD_PRIORITY(CrewMember, UpdateHealth, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CrewMember::UpdateHealth -> Begin (DuelsHooks.cpp)\n")
    if (Duels::Rounds::BetweenFights()) return;
    super();
}

HOOK_METHOD_PRIORITY(CrewMember, ModifyHealth, -2000, (float health) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CrewMember::ModifyHealth -> Begin (DuelsHooks.cpp)\n")
    if (Duels::Crew::IsPuppet(this)) return;
    super(health);
}

HOOK_METHOD_PRIORITY(CrewMember, ApplyDamage, -2000, (float damage) -> bool)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CrewMember::ApplyDamage -> Begin (DuelsHooks.cpp)\n")
    if (Duels::Crew::IsPuppet(this)) return false;
    return super(damage);
}

HOOK_METHOD_PRIORITY(ShipSystem, PartialRepair, -2000, (float speed, bool autoRepair) -> bool)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipSystem::PartialRepair -> Begin (DuelsHooks.cpp)\n")
    if (!Duels::Crew::MayRepair(this)) return false;
    return super(speed, autoRepair);
}

// Weapon bays (DuelsBays.cpp): the weapons room of every player ship is cut into one room per weapon slot as its
// layout loads, and the ship's blueprint gets a bay system in each.
HOOK_METHOD_PRIORITY(ResourceControl, LoadFile, -2000, (const std::string& fileName) -> char*)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ResourceControl::LoadFile -> Begin (DuelsHooks.cpp)\n")
    return Duels::Bays::OnLoadFile(fileName, super(fileName));
}

HOOK_METHOD_PRIORITY(ShipManager, OnInit, -2000, (ShipBlueprint *bp, int shipLevel) -> int)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipManager::OnInit -> Begin (DuelsHooks.cpp)\n")
    Duels::Bays::PrepareBlueprint(bp);
    return super(bp, shipLevel);
}

HOOK_METHOD_PRIORITY(ShipManager, AddSystem, -2000, (int systemId) -> int)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipManager::AddSystem -> Begin (DuelsHooks.cpp)\n")
    Duels::Bays::Building(&myBlueprint.layoutFile);
    int ret = super(systemId);
    Duels::Bays::Building(nullptr);
    Duels::Bays::SystemAdded(this, systemId);
    return ret;
}

// A cut room still looks like one: no doors or walls inside it (DuelsBays.cpp).
HOOK_METHOD_PRIORITY(Ship, OnInit, -2000, (ShipBlueprint *bp) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> Ship::OnInit -> Begin (DuelsHooks.cpp)\n")
    Duels::Bays::BuildingShip(this, bp ? &bp->layoutFile : nullptr);
    super(bp);
    Duels::Bays::BuildingShip(this, nullptr);
}

HOOK_METHOD_PRIORITY(ShipGraph, OnInit, -2000, (std::vector<Room*> *pRooms, std::vector<Door*> *pDoors) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipGraph::OnInit -> Begin (DuelsHooks.cpp)\n")
    super(pRooms, pDoors);
    Duels::Bays::OnGraphBuilt();
}

HOOK_STATIC_PRIORITY(CSurface, GL_CreateMultiLinePrimitive, -2000, (std::vector<GL_Line>& vec, GL_Color color, float thickness) -> GL_Primitive*)
{
    LOG_HOOK("HOOK_STATIC_PRIORITY -> CSurface::GL_CreateMultiLinePrimitive -> Begin (DuelsHooks.cpp)\n")
    Duels::Bays::OnLines(vec, thickness);
    return super(vec, color, thickness);
}

// The weapons system keeps its room picture where the whole weapons room was (FTL places it at the room's corner).
HOOK_METHOD_PRIORITY(ShipSystem, SetFloorImage1, -2000, (const std::string &name) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipSystem::SetFloorImage1 -> Begin (DuelsHooks.cpp)\n")
    int x = 0, y = 0;
    if (!Duels::Bays::OriginalRoomCorner(this, x, y)) return super(name);
    Globals::Rect shape = roomShape;
    roomShape.x = x;
    roomShape.y = y;
    super(name);
    roomShape = shape;
}

// Bay 1 shares its room with the weapons system: hits, repairs and hacking there are bay 1's.
HOOK_METHOD_PRIORITY(ShipManager, GetSystemInRoom, -2000, (int roomId) -> ShipSystem*)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipManager::GetSystemInRoom -> Begin (DuelsHooks.cpp)\n")
    return Duels::Bays::InRoom(this, roomId, super(roomId));
}

// Repairs in W1 (D1): bay 1 first, then the weapons system's (drone control's) spare bars (DuelsBays.cpp, InRoom).
HOOK_METHOD_PRIORITY(CrewAI, SelectRepair, -2000, (CrewMember *crewmember) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CrewAI::SelectRepair -> Begin (DuelsHooks.cpp)\n")
    Duels::Bays::SetSelectingRepair(true);
    super(crewmember);
    Duels::Bays::SetSelectingRepair(false);
}

HOOK_METHOD_PRIORITY(CrewMember, SetCurrentSystem, -2000, (ShipSystem *sys) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CrewMember::SetCurrentSystem -> Begin (DuelsHooks.cpp)\n")
    if (Duels::Bays::KeepConsole(this, sys)) return;
    super(sys);
}

// ShipManager::DamageSystem damages every system in the hit room (its first argument is a room): in W1 that is bay 1
// only, the weapons system beside it takes damage only on its spare bars, when a bay hands it on (buffer points).
HOOK_METHOD_PRIORITY(ShipSystem, AddDamage, -2000, (int amount) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipSystem::AddDamage -> Begin (DuelsHooks.cpp)\n")
    if (Duels::Bays::Untouchable(this) && !Duels::Bays::BufferHit()) return;
    amount -= Duels::Bays::TakeBuffer(this, amount);
    if (amount > 0) Duels::Bays::LogDamage(this, amount);
    if (amount > 0) super(amount);
}

HOOK_METHOD_PRIORITY(WeaponSystem, AddDamage, -2000, (int amount) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> WeaponSystem::AddDamage -> Begin (DuelsHooks.cpp)\n")
    if (Duels::Bays::Untouchable(this) && !Duels::Bays::BufferHit()) return;
    super(amount);
}

HOOK_METHOD_PRIORITY(ShipSystem, DamageOverTime, -2000, (float unk) -> bool)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipSystem::DamageOverTime -> Begin (DuelsHooks.cpp)\n")
    if (Duels::Bays::Untouchable(this) && !Duels::Bays::BufferHit()) return false;
    bool result = false;
    if (Duels::Bays::BufferPartial(this, unk, true, result)) return result;
    return super(unk);
}

HOOK_METHOD_PRIORITY(ShipSystem, PartialDamage, -2000, (float amount) -> bool)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipSystem::PartialDamage -> Begin (DuelsHooks.cpp)\n")
    if ((Duels::Bays::Untouchable(this) && !Duels::Bays::BufferHit()) || !Duels::Boarding::MayDamage(this)) return false;
    bool result = false;
    if (Duels::Bays::BufferPartial(this, amount, false, result)) return result;
    return super(amount);
}

HOOK_METHOD_PRIORITY(ShipSystem, IonDamage, -2000, (int amount) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipSystem::IonDamage -> Begin (DuelsHooks.cpp)\n")
    if (Duels::Bays::Untouchable(this)) return;
    super(amount);
}

HOOK_METHOD_PRIORITY(WeaponSystem, PowerWeapon, -2000, (ProjectileFactory *weapon, bool userDriven, bool force) -> bool)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> WeaponSystem::PowerWeapon -> Begin (DuelsHooks.cpp)\n")
    if (!Duels::Bays::MayPower(G_->GetShipManager(_shipObj.iShipId), weapon)) return false;
    return super(weapon, userDriven, force);
}

// A weapon whose bay is out shows red: its box in the weapons bar, and its power as red bars on the weapons system.
HOOK_METHOD_PRIORITY(WeaponBox, StatusColor, -2000, () -> GL_Color)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> WeaponBox::StatusColor -> Begin (DuelsHooks.cpp)\n")
    GL_Color color = super();
    if (pWeapon && Duels::Bays::BayOut(pWeapon)) return GL_Color(1.f, 50.f / 255.f, 50.f / 255.f, 1.f);
    return color;
}

HOOK_METHOD_PRIORITY(DroneBox, StatusColor, -2000, () -> GL_Color)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> DroneBox::StatusColor -> Begin (DuelsHooks.cpp)\n")
    GL_Color color = super();
    if (pDrone && Duels::Bays::DroneBayOut(pDrone)) return GL_Color(1.f, 50.f / 255.f, 50.f / 255.f, 1.f);
    return color;
}

// Diagnostics: FTL draws an image it can't find as its "nullResource" warning sign. Each missing name goes to the log
// once, so a sign on screen can be traced to the file it stands for.
HOOK_METHOD_PRIORITY(ResourceControl, GetImageId, -2000, (const std::string &name) -> GL_Texture*)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ResourceControl::GetImageId -> Begin (DuelsHooks.cpp)\n")
    static std::set<std::string> missing;
    if (!name.empty() && !missing.count(name) && !ImageExists(name))
    {
        missing.insert(name);
        Duels::Log("Resources: image %s is missing (FTL shows its warning sign instead)", name.c_str());
    }
    return super(name);
}

HOOK_METHOD_PRIORITY(Ship, DamageHull, -2000, (int amount) -> int)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> Ship::DamageHull -> Begin (DuelsHooks.cpp)\n")
    return super(Duels::Match::HullDamage(this, amount));
}

HOOK_METHOD_PRIORITY(ShipManager, AddCrewMember, -2000, (CrewMember *crew, int roomId) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipManager::AddCrewMember -> Begin (DuelsHooks.cpp)\n")
    super(crew, roomId);
    Duels::Boarding::OnCrewArrived(this, crew, roomId);
}

HOOK_METHOD_PRIORITY(CompleteShip, InitiateTeleport, -2000, (int targetRoom, int command) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CompleteShip::InitiateTeleport -> Begin (DuelsHooks.cpp)\n")
    if (Duels::Boarding::RefusesTeleport(this)) return;
    super(targetRoom, command);
    Duels::Boarding::AfterTeleport(this, command);
}

// Our mind control aimed at the opponent's ship: their game picks whom it takes (DuelsMind.cpp; roadmap 4.5).
HOOK_METHOD_PRIORITY(MindSystem, QueueMindControl, -2000, (std::vector<CrewMember*> *crew, int roomId, int shipId) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> MindSystem::QueueMindControl -> Begin (DuelsHooks.cpp)\n")
    if (Duels::Mind::QueueToOwner(this, roomId, shipId)) return;
    super(crew, roomId, shipId);
}

// A puppet its owner's state left out (roadmap 4.5: it is where we can't see it) isn't drawn: it stands where it was
// last seen, and FTL would draw it in a room our crew light up.
HOOK_METHOD_PRIORITY(CrewMember, OnRender, -2000, (bool outlineOnly) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CrewMember::OnRender -> Begin (DuelsHooks.cpp)\n")
    if (Duels::Crew::IsHidden(this)) return;
    super(outlineOnly);
}

HOOK_METHOD_PRIORITY(CrewMember, OnRenderHealth, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CrewMember::OnRenderHealth -> Begin (DuelsHooks.cpp)\n")
    if (Duels::Crew::IsHidden(this)) return;
    super();
}

// A crew member of the opponent's that our mind control holds aboard our ship repairs our systems as ours do (roadmap
// 3.8): FTL has a mind-controlled crew member sabotage where it stands, aboard either ship. While FTL repairs, it is
// one of ours.
HOOK_METHOD_PRIORITY(CrewMember, UpdateRepair, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CrewMember::UpdateRepair -> Begin (DuelsHooks.cpp)\n")
    if (!Duels::Mind::RepairsForUs(this)) return super();
    int shipId = iShipId;
    iShipId = 0;
    bMindControlled = false;
    super();
    bMindControlled = true;
    iShipId = shipId;
}

HOOK_METHOD_PRIORITY(ShipManager, CommandCrewMoveRoom, -2000, (CrewMember *crew, int roomId) -> bool)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipManager::CommandCrewMoveRoom -> Begin (DuelsHooks.cpp)\n")
    if (Duels::Boarding::RefusesAiOrder(crew)) return false;
    if (Duels::Mind::OrderToOwner(this, crew, roomId)) return true;
    return super(crew, roomId);
}

HOOK_METHOD_PRIORITY(BoarderPodDrone, OnLoop, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> BoarderPodDrone::OnLoop -> Begin (DuelsHooks.cpp)\n")
    if (!Duels::Boarding::MayPodLoop(this)) return;
    super();
}

// A frozen crew member (a drone switched off) walks to the nearest free slot first; a puppet stands where its owner
// does instead (DuelsCrew.cpp).
HOOK_METHOD_PRIORITY(CrewMember, NeedFrozenLocation, -2000, () -> bool)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CrewMember::NeedFrozenLocation -> Begin (DuelsHooks.cpp)\n")
    if (Duels::Crew::IsPuppet(this)) return false;
    return super();
}

HOOK_METHOD_PRIORITY(CrewAI, OnLoop, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CrewAI::OnLoop -> Begin (DuelsHooks.cpp)\n")
    Duels::Boarding::SetAiRunning(true);
    super();
    Duels::Boarding::SetAiRunning(false);
}

HOOK_METHOD_PRIORITY(HackingSystem, InitiatePulse, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> HackingSystem::InitiatePulse -> Begin (DuelsHooks.cpp)\n")
    if (!Duels::Hacking::MayPulse(this)) return;
    super();
}

HOOK_METHOD_PRIORITY(CloakingSystem, SetTurnedOn, -2000, (bool val) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CloakingSystem::SetTurnedOn -> Begin (DuelsHooks.cpp)\n")
    if (!Duels::Match::MaySwitchCloak(this)) return;
    super(val);
}

HOOK_METHOD_PRIORITY(DroneSystem, PowerDrone1, -2000, (Drone *drone, bool userDriven, bool force) -> bool)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> DroneSystem::PowerDrone1 -> Begin (DuelsHooks.cpp)\n")
    if (!Duels::Bays::MayPowerDrone(G_->GetShipManager(_shipObj.iShipId), drone)) return false;
    return super(drone, userDriven, force);
}

// The weapons system's (drone control's) bars per weapon (drone), in slot order: FTL draws each part as a system of
// its own with that bay's damage and repair, its bars blue while the bay is ioned (FTL's look for bars above what a
// system may power) and purple while it is hacked; the system's spare bars go on top (DuelsBays.cpp). What FTL draws
// above a system's bars (manning, ion lock, hacking, erosion, sabotage and fire icons) comes once, above the whole
// bar, from a last call for the system itself without bars; it gives the top for the system box. The system's real
// state comes back after the draw.
HOOK_METHOD_PRIORITY(ShipSystem, RenderPowerBoxes, -2000, (int x, int y, int width, int height, int gap, int heightMod, bool flash) -> int)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipSystem::RenderPowerBoxes -> Begin (DuelsHooks.cpp)\n")
    std::vector<Duels::Bays::Segment> segments;
    if (!Duels::Bays::PowerSegments(this, segments)) return super(x, y, width, height, gap, heightMod, flash);
    const std::pair<int, int> power = powerState, health = healthState;
    const float damageOverTime = fDamageOverTime, repairOverTime = fRepairOverTime;
    const int lockCount = iLockCount, bonus = iBonusPower, battery = iBatteryPower, hack = iHackEffect;
    const int room = roomId, powerCap = iTempPowerCap, powerLoss = iTempPowerLoss;
    const bool boostable = bBoostable, onFire = bOnFire, occupied = bOccupied, attacked = bUnderAttack;
    static const GL_Color HACKED(207.f / 255.f, 70.f / 255.f, 253.f / 255.f, 1.f);

    bBoostable = bOnFire = bOccupied = bUnderAttack = false;
    iLockCount = iHackEffect = 0;
    roomId = -1;
    int below = 0;
    for (const Duels::Bays::Segment &segment : segments)
    {
        powerState = std::make_pair(segment.reactor, segment.bars);
        healthState = std::make_pair(segment.bars - segment.damage, segment.bars);
        iBonusPower = segment.bonus;
        iBatteryPower = segment.battery;
        fRepairOverTime = segment.repair;
        fDamageOverTime = segment.partial;
        iTempPowerCap = segment.ioned ? 0 : powerCap;
        if (segment.hacked)
        {
            GL_Color tint = CSurface::GetColorTint();
            CSurface::GL_SetColorTint(GL_Color(tint.r * HACKED.r, tint.g * HACKED.g, tint.b * HACKED.b, tint.a));
        }
        super(x, y - below * (height + gap), width, height, gap, 0, flash);
        if (segment.hacked) CSurface::GL_RemoveColorTint();
        below += segment.bars;
    }

    powerState = std::make_pair(power.first, 0);
    healthState = health;
    fDamageOverTime = damageOverTime;
    fRepairOverTime = repairOverTime;
    iLockCount = lockCount;
    iBonusPower = bonus;
    iBatteryPower = battery;
    iHackEffect = hack;
    roomId = room;
    iTempPowerCap = powerCap;
    bBoostable = boostable;
    bOnFire = onFire;
    bOccupied = occupied;
    bUnderAttack = attacked;
    // FTL starts its bars at y and puts what comes above them one bar's space above the last: with no bars of its
    // own, the call starts where the whole bar would have had one more.
    int ret = super(x, y - below * (height + gap), width, height, gap, heightMod, flash);
    powerState = power;
    iTempPowerLoss = powerLoss;
    return ret;
}

// A bay without a weapon shows nothing; the opponent's icons are drawn where the duel view placed them.
HOOK_METHOD_PRIORITY(SystemBox, OnRender, -2000, (bool ignoreStatus) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> SystemBox::OnRender -> Begin (DuelsHooks.cpp)\n")
    if (Duels::Bays::HideBox(pSystem)) return;
    int dx = 0, dy = 0;
    if (!Duels::View::SysBoxShift(this, dx, dy))
    {
        super(ignoreStatus);
    }
    else
    {
        Duels::View::BeforeSysBoxRender(this);
        CSurface::GL_PushMatrix();
        CSurface::GL_Translate((float)dx, (float)dy, 0.f);
        super(ignoreStatus);
        CSurface::GL_PopMatrix();
        Duels::View::AfterSysBoxRender(this, dx, dy);
    }
    // A bay's icon (the enemy window) names its weapon or drone and how the bay is (roadmap J). FTL sets a system's
    // tooltip while it draws its box, from texts the bays (custom systems) don't have.
    std::string text;
    if (mouseHover && pSystem && Duels::Bays::Tooltip(pSystem, text)) G_->GetMouseControl()->SetTooltip(text);
}

// Our own bays get no box in the subsystem panel (while it is laid out, our ship has none).
HOOK_METHOD_PRIORITY(SystemControl, CreateSystemBoxes, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> SystemControl::CreateSystemBoxes -> Begin (DuelsHooks.cpp)\n")
    Duels::Bays::SetPanelLayout(true);
    super();
    Duels::Bays::SetPanelLayout(false);
}

HOOK_METHOD_PRIORITY(ShipManager, GetSystem, -2000, (int systemId) -> ShipSystem*)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipManager::GetSystem -> Begin (DuelsHooks.cpp)\n")
    ShipSystem *system = super(systemId);
    return Duels::Bays::HiddenFromPanel(this, system) ? nullptr : system;
}

// The icon in W1 is bay 1's.
HOOK_METHOD_PRIORITY(ShipSystem, OnRender, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipSystem::OnRender -> Begin (DuelsHooks.cpp)\n")
    if (Duels::Bays::HideRoomIcon(this)) return;
    super();
}

// Crew experience: in a duel, each skill gain of our crew counts as often as the host's setting says.
HOOK_METHOD_PRIORITY(CrewMember, IncreaseSkill, -2000, (int skillId) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CrewMember::IncreaseSkill -> Begin (DuelsHooks.cpp)\n")
    int gains = Duels::Match::SkillGains(this);
    for (int i = 0; i < gains; ++i) super(skillId);
}

// A puppet repairs, fights fires and fights when its owner's crew member does (its own work here has no effect).
HOOK_METHOD_PRIORITY(CrewAnimation, OnUpdate, -2000, (Pointf position, bool moving, bool fighting, bool repairing, bool dying, bool onFire) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CrewAnimation::OnUpdate -> Begin (DuelsHooks.cpp)\n")
    Duels::Crew::Animate(this, fighting, repairing, onFire);
    super(position, moving, fighting, repairing, dying, onFire);
}

// A beam's sweep over the rooms behind the shields.
HOOK_METHOD_PRIORITY(ShipManager, DamageBeam, -2000, (Pointf location1, Pointf location2, Damage dmg) -> bool)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipManager::DamageBeam -> Begin (DuelsHooks.cpp)\n")
    Duels::Match::MuteBeamDamage(this, dmg);
    int hullBefore = ship.hullIntegrity.first;
    bool hit = super(location1, location2, dmg);
    Duels::Match::ObserveBeam(this, hit, hullBefore);
    return hit;
}

// ---------------------------------------------------------------------------------------------
// Drones in a network duel (DuelsDrones.cpp): our drones act; the replica's drones are puppets that follow their owner.
// ---------------------------------------------------------------------------------------------

// A drone's shot (combat and defense drones): ours at the replica goes like a weapon's shot, a defense drone's in our
// space is shown to the opponent. A puppet never fires on its own.
HOOK_METHOD_PRIORITY(SpaceDrone, GetNextProjectile, -2000, () -> Projectile*)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> SpaceDrone::GetNextProjectile -> Begin (DuelsHooks.cpp)\n")
    if (!Duels::Drones::MayFire(this)) return nullptr;
    Projectile *projectile = super();
    if (projectile) Duels::Match::OnOwnDroneProjectile(this, projectile);
    return projectile;
}

// After every drone moved this frame: the puppets take their owners' positions (the frame is drawn after this).
HOOK_METHOD_PRIORITY(SpaceManager, OnLoop, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> SpaceManager::OnLoop -> Begin (DuelsHooks.cpp)\n")
    Duels::Environment::BeforeSpaceLoop();
    super();
    Duels::Environment::AfterSpaceLoop();
    Duels::Drones::AfterSpaceLoop();
}

// The fight's environment (DuelsEnvironment.cpp): a solar flare or an ion pulse acts in the game whose ship it is.
HOOK_METHOD_PRIORITY(ShipManager, SunDamage, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipManager::SunDamage -> Begin (DuelsHooks.cpp)\n")
    if (!Duels::Environment::AllowsHazardDamage(this)) return;
    super();
}

HOOK_METHOD_PRIORITY(ShipManager, PulsarDamage, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipManager::PulsarDamage -> Begin (DuelsHooks.cpp)\n")
    if (!Duels::Environment::AllowsHazardDamage(this)) return;
    if (Duels::Environment::SmartPulse(this)) return;   // powered systems only (roadmap W)
    super();
}

// FTL's warning before a flare or pulse: its sound plays, its text stays off in a duel (the display is calm, roadmap N).
HOOK_METHOD_PRIORITY(WarningMessage, OnRender, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> WarningMessage::OnRender -> Begin (DuelsHooks.cpp)\n")
    if (Duels::Environment::HidesWarning(this)) return;
    super();
}

// A duel's asteroid field makes its rocks on the shared schedule; FTL's generator (its own random rocks) waits.
HOOK_METHOD_PRIORITY(AsteroidGenerator, OnLoop, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> AsteroidGenerator::OnLoop -> Begin (DuelsHooks.cpp)\n")
    if (Duels::Environment::ReplacesAsteroidGenerator()) return;
    super();
}

// Our defense drones take the opponent's shots in our space as FTL's own once they fly as FTL's shots do (roadmap P):
// not while one waits at its entry point or makes up the network's delay.
HOOK_METHOD_PRIORITY(DefenseDrone, ValidTargetObject, -2000, (Targetable *target) -> bool)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> DefenseDrone::ValidTargetObject -> Begin (DuelsHooks.cpp)\n")
    if (Duels::Match::HiddenFromDefense(target)) return false;
    return super(target);
}

// A projectile runs into a drone: we report hits on the opponent's drones in our space.
HOOK_METHOD_PRIORITY(SpaceDrone, CollisionMoving, -2000, (Pointf start, Pointf finish, Damage damage, bool raytrace) -> CollisionResponse)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> SpaceDrone::CollisionMoving -> Begin (DuelsHooks.cpp)\n")
    bool exploding = explosion.tracker.running;
    float ionStunBefore = ionStun;
    CollisionResponse response = super(start, finish, damage, raytrace);
    Duels::Drones::ObserveDroneCollision(this, exploding, ionStunBefore);
    return response;
}

// A stunned or hacked drone may explode each second: that roll belongs to the drone's owner.
HOOK_METHOD_PRIORITY(Drone, OnLoop, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> Drone::OnLoop -> Begin (DuelsHooks.cpp)\n")
    if (!Duels::Drones::RunsOwnLoop(this)) return;
    super();
}

// The replica's hull repair drone repairs nothing here: the owner's hull comes with the state.
HOOK_METHOD_PRIORITY(ShipManager, DamageTarget, -2000, (Pointf location, Damage damage) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipManager::DamageTarget -> Begin (DuelsHooks.cpp)\n")
    if (Duels::Drones::BlocksReplicaRepair(this, damage.iDamage)) return;
    super(location, damage);
}

// Nor does the replica's shield drone add super shields here (Hyperspace rewrites this loop; its pulse still shows).
HOOK_METHOD_PRIORITY(SuperShieldDrone, OnLoop, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> SuperShieldDrone::OnLoop -> Begin (DuelsHooks.cpp)\n")
    if (!Duels::Drones::IsPuppet(this) || !shieldSystem)
    {
        super();
        return;
    }
    std::pair<int, int> superShield = shieldSystem->shields.power.super;
    super();
    shieldSystem->shields.power.super = superShield;
}

// ---------------------------------------------------------------------------------------------
// Duel view (DuelsView.cpp): both ships at one scale, the opponent mirrored in a grown enemy window, mouse input
// mapped back to each ship's coordinates.
// ---------------------------------------------------------------------------------------------

HOOK_METHOD_PRIORITY(CombatControl, RenderTarget, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CombatControl::RenderTarget -> Begin (DuelsHooks.cpp)\n")
    Duels::View::BeginTarget();
    Duels::View::BeginDecorations();
    Duels::View::PlaceSysBoxes(this);
    super();
    Duels::View::EndDecorations();
    Duels::View::EndTarget();
}

// Hyperspace's ship icons and event timers in the enemy window show tooltips from here.
HOOK_METHOD_PRIORITY(CommandGui, MouseMove, -2000, (int mX, int mY) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CommandGui::MouseMove -> Begin (DuelsHooks.cpp)\n")
    // Over the Duels window, the game underneath doesn't see the mouse.
    Duels::MatchUi::MouseMove(mX, mY);
    Duels::ReplayUi::MouseMove(mX, mY);
    if (Duels::Window::MouseMove(mX, mY)) return;
    Duels::View::BeginDecorations();
    super(mX, mY);
    Duels::View::EndDecorations();
}

HOOK_METHOD_PRIORITY(CachedPrimitive, OnRender, -2000, (const GL_Color &color) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CachedPrimitive::OnRender -> Begin (DuelsHooks.cpp)\n")
    float x, y;
    // The teleport, hacking and mind-control markers on the opponent's rooms, the right way round.
    if (Duels::View::TargetMarkerCenter(this, x, y) && Duels::View::BeginUnmirrored(x, y, 0.f))
    {
        super(color);
        Duels::View::EndUnmirrored();
        return;
    }
    if (float rest = Duels::View::TakeHullBarRest(this))
    {
        // The opponent's hull bar beyond FTL's 22 segments: the whole image first, then its right-hand part after it.
        super(color);
        CachedImage *image = static_cast<CachedImage*>(static_cast<CachedPrimitive*>(this));
        float width = image->texture ? (float)image->texture->width_ * image->wScale : 0.f;
        for (float offset = width; rest > 0.f && width > 0.f; offset += width, rest -= 1.f)
        {
            // SetPartial takes the texture's start and end, and draws as wide as the end: the image's last `part`
            // is drawn at full width and squeezed back to its own width, around the image's corner.
            // Whole segments: 22 of them, a gap pixel after each but the last (241 px for 11 px segments).
            float segments = std::min(rest, 1.f) * 22.f;
            float part = std::min(1.f, segments * (width + 1.f) / 22.f / width);
            image->SetPartial(1.f - part, 0.f, 1.f, 1.f);
            float left = (float)image->x, top = (float)image->y;
            CSurface::GL_PushMatrix();
            CSurface::GL_Translate(left + offset, top, 0.f);
            CSurface::GL_Scale(part, 1.f, 1.f);
            CSurface::GL_Translate(-left, -top, 0.f);
            super(color);
            CSurface::GL_PopMatrix();
        }
        return;
    }
    if (!Duels::View::HullBarShift(this, x, y)) return super(color);
    CSurface::GL_PushMatrix();
    CSurface::GL_Translate(x, y, 0.f);
    super(color);
    CSurface::GL_PopMatrix();
}

// The opponent's hull bar with more hull points than FTL's enemy bar has segments (see above).
HOOK_METHOD_PRIORITY(CachedImage, SetPartial, -2000, (float x_start, float y_start, float x_size, float y_size) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CachedImage::SetPartial -> Begin (DuelsHooks.cpp)\n")
    Duels::View::LimitHullBar(this, x_size);
    super(x_start, y_start, x_size, y_size);
}

// The system icons in the opponent's rooms, the right way round and at a readable size. (FTL builds each icon at its
// room's centre, so it turns around that.)
HOOK_METHOD_PRIORITY(ShipSystem, OnRender, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipSystem::OnRender -> Begin (DuelsHooks.cpp)\n")
    float x, y;
    if (!Duels::View::TargetRoomCenter(_shipObj.iShipId, roomId, x, y) || !Duels::View::BeginUnmirrored(x, y, 0.8f))
    {
        return super();
    }
    super();
    Duels::View::EndUnmirrored();
}

// The crosshairs on the opponent's rooms, numbered by weapon: Hyperspace draws each at the local origin, translated to
// the target point (AdditionalWeaponSlots.cpp). The flak radius and beam lines are drawn otherwise.
HOOK_METHOD_PRIORITY(WeaponControl, RenderAiming, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> WeaponControl::RenderAiming -> Begin (DuelsHooks.cpp)\n")
    Duels::View::SetAiming(true);
    super();
    Duels::View::SetAiming(false);
}

HOOK_STATIC_PRIORITY(CSurface, GL_RenderPrimitive, -2000, (GL_Primitive *primitive) -> void)
{
    LOG_HOOK("HOOK_STATIC_PRIORITY -> CSurface::GL_RenderPrimitive -> Begin (DuelsHooks.cpp)\n")
    if (!Duels::View::Aiming() || !primitive || !primitive->hasTexture || !Duels::View::BeginUnmirrored(0.f, 0.f, 0.f))
    {
        return super(primitive);
    }
    super(primitive);
    Duels::View::EndUnmirrored();
}

// FTL's screen shake (a hard hit): the offset it moves its interface by this frame, for what Duels draws on it.
HOOK_METHOD_PRIORITY(CommandGui, UpdateShake, -2000, () -> Pointf)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CommandGui::UpdateShake -> Begin (DuelsHooks.cpp)\n")
    Pointf shake = super();
    Duels::Hud::SetShake(shake.x, shake.y);
    return shake;
}

HOOK_METHOD_PRIORITY(CommandGui, RenderPlayerShip, -2000, (Point &shipCenter, float jumpScale) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CommandGui::RenderPlayerShip -> Begin (DuelsHooks.cpp)\n")
    Duels::View::BeginPlayerShip();
    super(shipCenter, jumpScale);
    Duels::View::EndPlayerShip();
}

// Inside RenderTarget's and RenderPlayerShip's own push and translate: the transform covers the ship, its space
// (projectiles), and the charge bars and aiming marks drawn after it; their pop removes it.
HOOK_METHOD_PRIORITY(CompleteShip, OnRenderShip, -2000, (bool unk1, bool unk2) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CompleteShip::OnRenderShip -> Begin (DuelsHooks.cpp)\n")
    Duels::View::ApplyShipTransform(shipManager);
    super(unk1, unk2);
}

// "MISS", damage numbers: readable, at normal size, although drawn inside a scaled or mirrored ship.
HOOK_METHOD_PRIORITY(DamageMessage, OnRender, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> DamageMessage::OnRender -> Begin (DuelsHooks.cpp)\n")
    if (!Duels::View::Transforming())
    {
        super();
        return;
    }
    float sx, sy;
    Duels::View::InverseScale(sx, sy);
    CSurface::GL_PushMatrix();
    CSurface::GL_Translate(position.x, position.y, 0.f);
    CSurface::GL_Scale(sx, sy, 1.f);
    CSurface::GL_Translate(-position.x, -position.y, 0.f);
    super();
    CSurface::GL_PopMatrix();
}

extern Point g_enemyShipCorner;   // Hyperspace, CustomWeapons.cpp

// Hyperspace keeps the opponent's weapon charge bars on screen, reckoning with where FTL would draw its ship; ours is
// scaled and mirrored inside the window. Its check passes while our transform is on.
HOOK_METHOD_PRIORITY(WeaponAnimation, RenderChargeBar, -2000, (float alpha) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> WeaponAnimation::RenderChargeBar -> Begin (DuelsHooks.cpp)\n")
    if (!Duels::View::Transforming()) return super(alpha);
    Point saved = g_enemyShipCorner;
    int width = (int)(anim.info.frameWidth * anim.fScale);
    int barX = bMirrored ? renderPoint.x + mountPoint.x - width - 18 : renderPoint.x - mountPoint.x + width + 10;
    int barY = renderPoint.y - mountPoint.y;
    g_enemyShipCorner = Point(640 - barX, 360 - barY);
    super(alpha);
    g_enemyShipCorner = saved;
}

// The enemy window: its size, its frame, and the opponent's system boxes at their usual place.
HOOK_METHOD_PRIORITY(CombatControl, GetHostileBoxSize, -2000, () -> Point)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CombatControl::GetHostileBoxSize -> Begin (DuelsHooks.cpp)\n")
    Point size = super();
    Duels::View::AdjustHostileBoxSize(this, size);
    return size;
}

HOOK_METHOD_PRIORITY(CombatControl, DrawHostileBox, -2000, (GL_Color color, int stencilBit) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CombatControl::DrawHostileBox -> Begin (DuelsHooks.cpp)\n")
    if (!Duels::View::DrawHostileBox(this, color, stencilBit)) super(color, stencilBit);
}

HOOK_METHOD_PRIORITY(CombatControl, UpdateSysBoxes, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CombatControl::UpdateSysBoxes -> Begin (DuelsHooks.cpp)\n")
    Duels::View::BeginSysBoxes(this);
    super();
    Duels::View::EndSysBoxes(this);
}

namespace
{
    // A beam weapon whose first point is placed: its line follows the mouse, in or out of the enemy window.
    bool AimingBeam(CombatControl *combat)
    {
        ProjectileFactory *weapon = combat->weapControl.armedWeapon;
        return weapon && weapon->blueprint && weapon->blueprint->type == 2 && !combat->aimingPoints.empty();
    }

    // A point in the opponent's ship coordinates as the screen point that FTL's own "minus the ship's origin" expects.
    Point TargetScreenPoint(CombatControl *combat, float shipX, float shipY)
    {
        return Point(combat->position.x + combat->targetPosition.x + (int)std::floor(shipX),
                     combat->position.y + combat->targetPosition.y + (int)std::floor(shipY));
    }
}

// FTL reads lastMouse minus a ship's position here: the opponent's rooms and the beam line (UpdateAiming), and our
// drones and rooms (weapons aimed at our own ship). The weapon buttons and system boxes got the real point before.
HOOK_METHOD_PRIORITY(CombatControl, UpdateTarget, -2000, () -> bool)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CombatControl::UpdateTarget -> Begin (DuelsHooks.cpp)\n")
    Pointf real = lastMouse;
    float targetX, targetY, ownX, ownY;
    bool inside = false;
    if (!Duels::View::TargetShipPoint(real.x, real.y, targetX, targetY, inside) ||
        !Duels::View::OwnShipPoint(real.x, real.y, ownX, ownY))
    {
        return super();
    }

    bool atTarget = inside || AimingBeam(this);
    lastMouse = atTarget ? Pointf(position.x + targetPosition.x + targetX, position.y + targetPosition.y + targetY)
                         : Pointf(playerShipPosition.x + ownX, playerShipPosition.y + ownY);
    bool result = super();
    lastMouse = real;

    // The other ship's arithmetic ran on a point that isn't over that ship: nothing of it is under the mouse.
    if (atTarget)
    {
        currentDrone = nullptr;
        if (selectedSelfRoom != -1 && shipManager) shipManager->ship.SetSelectedRoom(-1);
        selectedSelfRoom = -1;
    }
    else
    {
        if (selectedRoom != -1 && currentTarget && currentTarget->shipManager) currentTarget->shipManager->ship.SetSelectedRoom(-1);
        selectedRoom = -1;
    }
    return result;
}

HOOK_METHOD_PRIORITY(CombatControl, GetSelectedCrew, -2000, (int mX, int mY) -> CrewMember*)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CombatControl::GetSelectedCrew -> Begin (DuelsHooks.cpp)\n")
    float shipX, shipY;
    bool inside = false;
    if (!Duels::View::TargetShipPoint((float)mX, (float)mY, shipX, shipY, inside)) return super(mX, mY);
    if (!inside) return nullptr;
    Point point = TargetScreenPoint(this, shipX, shipY);
    return super(point.x, point.y);
}

HOOK_METHOD_PRIORITY(CombatControl, GetSelectedCrew, -2000, (int x, int y, int firstX, int firstY) -> std::vector<CrewMember*>)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CombatControl::GetSelectedCrew (area) -> Begin (DuelsHooks.cpp)\n")
    float shipX, shipY, firstShipX, firstShipY;
    bool inside = false, firstInside = false;
    if (!Duels::View::TargetShipPoint((float)x, (float)y, shipX, shipY, inside) ||
        !Duels::View::TargetShipPoint((float)firstX, (float)firstY, firstShipX, firstShipY, firstInside))
    {
        return super(x, y, firstX, firstY);
    }
    Point point = TargetScreenPoint(this, shipX, shipY);
    Point first = TargetScreenPoint(this, firstShipX, firstShipY);
    return super(point.x, point.y, first.x, first.y);
}

HOOK_METHOD_PRIORITY(CombatControl, GetCrewTooltip, -2000, (int x, int y) -> std::string)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CombatControl::GetCrewTooltip -> Begin (DuelsHooks.cpp)\n")
    float shipX, shipY;
    bool inside = false;
    if (!Duels::View::TargetShipPoint((float)x, (float)y, shipX, shipY, inside)) return super(x, y);
    if (!inside) return std::string();
    Point point = TargetScreenPoint(this, shipX, shipY);
    return super(point.x, point.y);
}

// Our ship: crew selection, crew orders, doors and tooltips work in its coordinates, which FTL computes as the mouse
// minus the ship's position, in CommandGui::MouseMove (passed on here) and GetWorldCoordinates (for clicks).
HOOK_METHOD_PRIORITY(CrewControl, MouseMove, -2000, (int mX, int mY, int wX, int wY) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CrewControl::MouseMove -> Begin (DuelsHooks.cpp)\n")
    float shipX, shipY;
    if (Duels::View::OwnShipPoint((float)mX, (float)mY, shipX, shipY))
    {
        wX = (int)std::floor(shipX);
        wY = (int)std::floor(shipY);
    }
    super(mX, mY, wX, wY);
}

HOOK_METHOD_PRIORITY(CommandGui, GetWorldCoordinates, -2000, (Point point, bool fromTarget) -> Point)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CommandGui::GetWorldCoordinates -> Begin (DuelsHooks.cpp)\n")
    float shipX, shipY;
    bool inside = false;
    if (fromTarget && combatControl.currentTarget)
    {
        if (Duels::View::TargetShipPoint((float)point.x, (float)point.y, shipX, shipY, inside))
        {
            return Point((int)std::floor(shipX), (int)std::floor(shipY));
        }
    }
    else if (Duels::View::OwnShipPoint((float)point.x, (float)point.y, shipX, shipY))
    {
        return Point((int)std::floor(shipX), (int)std::floor(shipY));
    }
    return super(point, fromTarget);
}

// ---------------------------------------------------------------------------------------------
// Console (DuelsConsole.cpp): F1 opens an input line where messages are printed, on every keyboard (Hyperspace's key
// is "\", which German and many other layouts only produce with AltGr). While it is open it has the keyboard.
// ---------------------------------------------------------------------------------------------

// While a duel is paused (a lost connection, roadmap AA) the ships take no orders: only the menu, the options button,
// the console, the chat and the Duels window answer. FTL itself would take orders in a pause.
static bool OrdersHeld(CommandGui *gui, int mX, int mY)
{
    // The connection lost (the match waits), or a replay (the recorder's ship isn't ours to command). A message box of
    // FTL's takes its click (a replay's first box, come late, roadmap BO).
    if ((!Duels::Rounds::NetPaused() && !Duels::Rejoin::Trying() && !Duels::Net::Replaying() && !Duels::Match::WaitingForMatch()) ||
        gui->menuBox.bOpen || gui->choiceBox.bOpen) return false;
    const Globals::Rect &options = gui->optionsButton.hitbox;
    return !(mX >= options.x && mX < options.x + options.w && mY >= options.y && mY < options.y + options.h);
}

HOOK_METHOD_PRIORITY(CommandGui, KeyDown, -2000, (SDLKey key, bool shiftHeld) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CommandGui::KeyDown -> Begin (DuelsHooks.cpp)\n")
    if (Duels::Console::KeyDown(this, key)) return;
    if (Duels::Window::KeyDown((int)key)) return;
    if (Duels::ReplayUi::KeyDown((int)key)) return;   // a replay's keys, and no other reaches the game then
    if ((Duels::Rounds::NetPaused() || Duels::Rejoin::Trying() || Duels::Match::WaitingForMatch()) && !menuBox.bOpen && key != SDLK_ESCAPE) return;
    Duels::Refit::SwitchScreensKey((int)key);   // the preparation: the store and the ship's screens switch
    TutorialManager *tutorial = G_->GetTutorialManager();
    if (key == Settings::GetHotkey("jump") && (int)key > 0 && !(tutorial && tutorial->Running()))
    {
        // The jump key as the JUMP button: running away when FTL would jump in a match's fight, never the star map
        // (roadmap AD; BU: outside a match it jumped to other beacons). FTL's tutorial keeps its own jump.
        std::string message;
        if (Duels::Rounds::InMatch() && Duels::Rounds::EscapeAllowed() && Duels::Rounds::DriveReady()) Duels::Rounds::Escape(message);
        return;
    }
    super(key, shiftHeld);
}

// The Duels button and window take their clicks before the game does.
HOOK_METHOD_PRIORITY(CommandGui, LButtonDown, -2000, (int mX, int mY, bool shiftHeld) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CommandGui::LButtonDown -> Begin (DuelsHooks.cpp)\n")
    if (Duels::Window::LButtonDown(mX, mY)) return;
    if (Duels::ReplayUi::LButtonDown(mX, mY)) return;
    if (Duels::MatchUi::LButtonDown(mX, mY)) return;
    if (Duels::Refit::SwitchScreensClick(mX, mY)) return;
    if (OrdersHeld(this, mX, mY)) return;
    super(mX, mY, shiftHeld);
}

HOOK_METHOD_PRIORITY(CommandGui, LButtonUp, -2000, (int mX, int mY, bool shiftHeld) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CommandGui::LButtonUp -> Begin (DuelsHooks.cpp)\n")
    if (OrdersHeld(this, mX, mY)) return;
    super(mX, mY, shiftHeld);
}

HOOK_METHOD_PRIORITY(CommandGui, RButtonDown, -2000, (int mX, int mY, bool shift) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CommandGui::RButtonDown -> Begin (DuelsHooks.cpp)\n")
    if (OrdersHeld(this, mX, mY)) return;
    super(mX, mY, shift);
}

HOOK_METHOD_PRIORITY(CommandGui, RButtonUp, -2000, (int mX, int mY, bool shift) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CommandGui::RButtonUp -> Begin (DuelsHooks.cpp)\n")
    if (OrdersHeld(this, mX, mY)) return;
    super(mX, mY, shift);
}

HOOK_METHOD_PRIORITY(CommandGui, OnTextInput, -2000, (int ch) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CommandGui::OnTextInput -> Begin (DuelsHooks.cpp)\n")
    if (Duels::Console::TextInput(ch)) return;
    super(ch);
}

HOOK_METHOD_PRIORITY(CommandGui, OnTextEvent, -2000, (CEvent::TextEvent event) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CommandGui::OnTextEvent -> Begin (DuelsHooks.cpp)\n")
    if (Duels::Console::TextEvent(this, (int)event)) return;
    super(event);
}

// Over the game and under the mouse cursor. While the console is open it shows the recent messages itself, so
// Hyperspace's message list (drawn inside) is moved out of sight for the frame.
HOOK_METHOD_PRIORITY(MouseControl, OnRender, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> MouseControl::OnRender -> Begin (DuelsHooks.cpp)\n")
    Duels::Hud::Render();
    Duels::MatchUi::Render();
    Duels::ReplayUi::Render();
    Duels::Window::Render();
    Duels::MatchUi::RenderSplash();
    Duels::Rejoin::Render();
    Duels::ReplayUi::RenderSeekCover();
    Duels::Lobby::RenderCover();
    Duels::Hud::EndFrame();
    if (!Duels::Console::Render()) return super();
    PrintHelper *printer = PrintHelper::GetInstance();
    int x = printer->x;
    printer->x = -100000;
    super();
    printer->x = x;
}

// The console's text line (Hyperspace creates it without a prompt) starts with "> ".
HOOK_METHOD_PRIORITY(InputBox, OnRender, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> InputBox::OnRender -> Begin (DuelsHooks.cpp)\n")
    struct TextInput *input = CommandConsole::GetInstance()->textInput;   // "struct": InputBox has a TextInput() method
    if (input && input->prompt.empty()) input->prompt = "> ";
    super();
}

// A player ship as the duel opponent keeps the player's shield position (vanilla adds 110 px for enemies).
HOOK_METHOD_PRIORITY(Ship, GetBaseEllipse, -2000, () -> Globals::Ellipse)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> Ship::GetBaseEllipse -> Begin (DuelsHooks.cpp)\n")
    Globals::Ellipse ellipse = super();
    if (Duels::View::PlayerShieldPosition(iShipId)) ellipse.center.y -= 110;
    return ellipse;
}

// The enemy window's header texts (ship class, relationship) clear the hull bar of a player ship.
HOOK_STATIC_PRIORITY(freetype, easy_printRightAlign, -2000, (int fontSize, float x, float y, const std::string &text) -> Pointf)
{
    LOG_HOOK("HOOK_STATIC_PRIORITY -> freetype::easy_printRightAlign -> Begin (DuelsHooks.cpp)\n")
    std::string label;
    if (Duels::Screen::VersionLabel(fontSize, x, y, text, label)) return super(fontSize, x, y, label);
    Duels::View::AdjustHeaderText(fontSize, x, y, text);
    return super(fontSize, x, y, text);
}

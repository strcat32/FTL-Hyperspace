#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct ArtillerySystem;
struct BombProjectile;
struct CloakingSystem;
struct Collideable;
struct CrewMember;
struct CollisionResponse;
struct Damage;
struct Pointf;
struct Projectile;
struct Targetable;
struct ProjectileFactory;
struct Ship;
struct ShipManager;
struct ShipSystem;
struct SpaceDrone;

// A duel between two players over the network (Step 2), under split authority: each game owns its own ship
// (ship 0). The opponent is ship 1, a replica driven by the owner's messages.
//
//  - On connecting, both send their loadout; each spawns the other's ship as ship 1 and applies it.
//  - Ten times a second each sends its ship's state (hull, shields, systems, weapons); the replica follows it.
//  - A shot is captured when the owner's weapon releases a projectile (laser, missile, flak shard, beam or bomb) and
//    sent with its exact target point. The defender creates it on its side and lets its own game decide the hit
//    (dodge, shields, damage). The verdict goes back; until it arrives, the attacker's copy of the shot waits at the
//    edge of the target's shield (a bomb: where it goes off). A beam's damage is only done by the defender's game.
//
// Hooks live in DuelsHooks.cpp and call in here.
namespace Duels
{
    namespace Match
    {
        void Init();
        void OnFrame(double now);

        void SetPlayerName(const std::string &name);
        bool Host(uint16_t port, bool loopbackOnly, std::string &message);
        bool Join(const std::string &host, uint16_t port, std::string &message);
        // Through a relay server: the host gets a room code to give the other player, who joins with it. The room
        // has a name and a password (or ""), and shows in the relay's room list or not.
        bool HostRelay(const std::string &server, uint16_t port, const std::string &roomName, const std::string &password,
                       bool listed, std::string &message);
        bool JoinRelay(const std::string &server, uint16_t port, const std::string &code, const std::string &password,
                       std::string &message);
        void Leave();
        // Chat to the other player (cleaned, cut to a length, a few lines per 10 s at most; rules, section 8).
        bool Say(const std::string &text, std::string &message);
        // Test verb "chatflood <count>": that many chat lines at once, past the sender's limits.
        int ChatFlood(int count);
        // A ship's fitting (the loadout a duel sends, roadmap 2.1; the AI's ship between rounds, 3.6): its blueprint,
        // hull, reactor, systems and their levels, weapons and drones in slot order, drone parts, augments, and its
        // crew's species. FitShip fits a ship built from that blueprint to it (hull, reactor, systems removed, added,
        // raised and lowered, weapons and drones, drone parts, augments; not the crew).
        struct Loadout
        {
            std::string blueprint;
            int hullMax = 0, hull = 0, reactor = 0;
            std::vector<std::pair<int, int>> systems;   // id, level
            std::vector<std::string> weapons, crew, drones, augments;
            int droneParts = 0;
        };
        Loadout TakeLoadout(ShipManager *ship);
        void FitShip(ShipManager *ship, const Loadout &loadout);
        // A replay (roadmap 5.1, DuelsDemo.cpp): the recorder's own loadout (its MSG_LOADOUT as it went): our ship
        // becomes its ship, fitted as it was.
        void ReplayOwnLoadout(const uint8_t *data, size_t size);
        // And its full states (KIND_FULL_STATE): our ship's hull, shields, systems, weapons, cloak, battery, crew and
        // rooms follow them as the opponent's copy follows its owner's (its drones come with a later stage).
        void ReplayOwnState(const uint8_t *data, size_t size);
        // Its shots (MSG_SHOT as it sent them) leave our ship as they left its, and wait at the opponent's ship for the
        // opponent's verdict (recorded as it came); its verdicts on the opponent's shots (MSG_RESULT) decide them at our
        // ship; its shots that ran into something in its own space (MSG_SHOT_DOWNED) explode there.
        void ReplayOwnShot(const uint8_t *data, size_t size);
        void ReplayOwnResult(const uint8_t *data, size_t size);
        void ReplayOwnShotDowned(const uint8_t *data, size_t size);
        // Its own chat lines (MSG_CHAT as it sent them), under its name.
        void ReplayChat(const std::string &name, const uint8_t *data, size_t size);
        // The other ship's next state counts even when older than the last one (a replay changing the states the other
        // ship follows: full sensors on or off, roadmap BA).
        void ReplayRestate();
        // A ship our game doesn't decide but follows: the opponent's copy (ship 1, once built), and in a replay our ship
        // too, once the recorder's states drive it.
        bool IsDriven(int shipId);
        // A player ship as the opponent's (the replica, the AI's ship): its shields where a player ship has them, and
        // our combat drones' orbits around them (DuelsView.cpp; the enemy window's layout goes by SetDuelOpponent).
        void ShowOpponent(ShipManager *ship);

        const std::string &PlayerName();
        // Players' names (roadmap AG): the name prompt and the name command take up to NAME_MAX characters, and the
        // console and the chat log show them whole. On the screen (the score panel, the lines under the buttons, the
        // splashes, the Duels window) a name is cut after SCREEN_NAME_MAX characters, as FTL cuts crew names: what the
        // score panel's half has room for (measured: about 9 letters of font 10 beside "..").
        static const size_t NAME_MAX = 24, SCREEN_NAME_MAX = 10;
        std::string ScreenName(const std::string &name);
        // The opponent's shots received in this game (tests).
        uint32_t ShotsReceived();

        // The match flow (DuelsRounds.cpp): both ships stand for this round (we built theirs, they built ours); and a
        // new round: the opponent's ship leaves and everything of the last fight is forgotten, the connection stays.
        bool ShipsStand();
        void NewFight();
        // Debug mode on (Duels::EnableDebug): the handshake tells the other player.
        void SetDebug(bool debug);
        // Crew experience (rules, roadmap 2.3): in a duel each skill gain of the crew counts this many times. The
        // host's setting counts for both players; 1 is FTL's own pace.
        bool SetCrewXp(float factor, std::string &message);
        float CrewXpSetting();   // ours, for the duels we host (a preset keeps it, roadmap BE)
        float MatchXp();         // this duel's (the host's; in a ranked room the season's, roadmap BG)
        // A ranked room's players go by their Steam names (rules, section 6): the name for the next session's
        // handshake ("": our own again).
        void UseRankedName(const std::string &name);
        std::string CrewXpStatus();
        std::string Status();

        // --- hook entry points ---

        // CrewMember::IncreaseSkill: how many times this gain counts (our crew in a duel: the host's setting; the
        // fractions add up).
        int SkillGains(const CrewMember *crew);

        // ProjectileFactory::GetProjectile released a projectile of one of our weapons or artillery systems.
        void OnOwnProjectile(ProjectileFactory *weapon, Projectile *projectile);

        // ShipManager::CheckCrystalAugment (Crystal Vengeance): the replica breaks off no shards of its own (its owner's
        // game does, and sends them); each shard of ours at the replica goes to the opponent like a shot. No ship breaks
        // one off once the round is decided (roadmap O: no new shots).
        bool AllowShards(const ShipManager *ship);
        // ProjectileFactory::ReadyToFire: none of our weapons (nor the AI's, in a match against it) starts a new volley
        // once the round is decided.
        bool AllowNewShots(const ProjectileFactory *weapon);
        void OnOwnShard(Projectile *projectile);

        // ArtillerySystem::OnLoop: the replica's artillery never fires by itself (FTL would pick its own target); its
        // shots come from its owner's game. OnReplicaArtilleryHeld counts the frames it was ready (status line).
        bool ReplicaArtillery(const ArtillerySystem *artillery);
        void OnReplicaArtilleryHeld();

        // ShipSystem::SetBonusPower (Hyperspace calls it every frame with the bonus of the crew in the room, Zoltans):
        // a replica system gets its owner's bonus instead of what its puppets give (they walk behind their owners, and
        // a larger bonus takes reactor bars away).
        bool ReplicaBonusPower(const ShipSystem *system, int &amount);

        // SpaceDrone::GetNextProjectile released a projectile of one of our drones: a combat drone's shot at the replica
        // goes like a weapon's; a defense drone's shot in our space is shown to the opponent (DuelsDrones.cpp).
        void OnOwnDroneProjectile(SpaceDrone *drone, Projectile *projectile);
        // The opponent's shot in our space while it waits at its entry point or makes up the network's delay: it
        // doesn't move as its speed says then, so our defense drones leave it until it flies as FTL's shots do
        // (roadmap P: they aimed ahead of it, missed, and FTL's defense drone never shoots at the same shot twice).
        bool HiddenFromDefense(const Targetable *target);

        // SpaceManager::UpdateProjectile: returns how many times to run the update this frame (0 = hold, 1 = normal,
        // more = catch up).
        int ProjectileUpdates(Projectile *projectile);

        // Projectile::CollisionCheck: false = skip this check. In a duel, what may collide follows whose space it is
        // (DuelsDrones.h), and our shot at the replica waits for the verdict. Sets up the forced outcome for the calls
        // below when a verdict exists.
        bool BeginCollisionCheck(Projectile *projectile, Collideable *other);
        void EndCollisionCheck();

        // The forced outcome of our shot at the replica, if one applies to this ship and projectile.
        bool ForcedShieldResponse(ShipManager *ship, Pointf start, Pointf finish, const Damage &damage,
                                  CollisionResponse &response);
        bool ForcedDamageArea(ShipManager *ship, Pointf location, bool &hit);

        // The defender's own game decided an incoming shot (after the original functions ran).
        void ObserveShield(ShipManager *ship, const CollisionResponse &response);
        void ObserveDamageArea(ShipManager *ship, bool hit, int hullBefore);

        // Bombs (BombProjectile::CollisionCheck, ShipManager::GetDodged): ours in the replica goes off, or misses,
        // as the defender's did, and waits for that verdict; the defender's dodge is reported when it is rolled.
        bool BeginBombCheck(BombProjectile *bomb, Collideable *other);
        bool ForcedDodge(ShipManager *ship, bool &dodged);
        void ObserveDodge(ShipManager *ship, bool dodged);
        // The opponent's shot at our ship dodges by both players' hash chains (roadmap 4.1, DuelsFair.h), not by FTL's
        // own roll; false when it can't (no chains yet): FTL rolls then.
        bool RolledDodge(ShipManager *ship, bool &dodged);

        // ShipManager::DoSensorsProvide for our ship: what FTL shows of the opponent's ship (its interior, its weapons'
        // charge, its power use) only as far as their state's vision says it was sent (roadmap 4.5). FTL's vision
        // numbers: 2 the enemy's interior, 3 its power use, 4 its weapons' charge.
        bool SensorsAllow(const ShipManager *ship, int vision);
        // After ShipManager::CheckVision for the replica (FTL's blackout reads the sensors itself): its rooms dark where
        // their state's vision says we don't see inside, but for the rooms our crew light up (FTL's tempVision). In a
        // replay with full sensors (roadmap BA) every room of both ships is lit.
        void ClampVision(ShipManager *ship);
        // A replay with full sensors (BA): FTL shows the opponent's power and weapons' charge too, whatever our ship's
        // sensors (the DoSensorsProvide hook asks for our ship before FTL does).
        bool FullSensors(const ShipManager *ship);

        // Beams (ShipManager::DamageBeam): ours sweeps the replica without damage; the defender's is reported when over.
        void MuteBeamDamage(ShipManager *ship, Damage &damage);
        void ObserveBeam(ShipManager *ship, bool hit, int hullBefore);

        // After ShipManager::OnLoop: the replica's subsystems (piloting, sensors, doors, battery) keep their owner's
        // power. This game's loop sets them from this game's environment, which isn't the owner's (a nebula here or
        // there switches the sensors off).
        void HoldReplicaSubsystems(ShipManager *ship);

        // Before and after ShipManager::OnLoop: the replica's systems show their owner's hacking (the state). Our own
        // hacking system would hack them here at its own moment, and FTL's hacked effects on them (shields draining,
        // drones losing power) would run ahead of the owner's.
        void HoldReplicaHacking(ShipManager *ship);

        // CloakingSystem::SetTurnedOn: the replica's cloak goes on and off only with its owner's (roadmap 2.4); its
        // own timer, power or damage would end it a moment before the owner's does.
        bool MaySwitchCloak(const CloakingSystem *cloak);

        // Ship::DamageHull: how much of this damage the ship takes. The replica's hull reaches 0 only when its
        // owner's state says so: our copy of a hit can come after the update that already counted it, and at 1 hull
        // FTL would wreck the replica here while the owner's ship still flies.
        int HullDamage(const Ship *ship, int amount);
    }
}

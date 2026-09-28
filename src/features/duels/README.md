# FTL: Duels module

This module turns Hyperspace into the base of **FTL: Duels**, a 1v1 real-time PvP mode for FTL: Faster Than Light. It lives in this folder. Outside it there are a few small changes: one line in `CMakeLists.txt` links Winsock (`ws2_32`) for the netcode, and two functions added to Hyperspace's function list for Windows (`libzhlgen/test/functions/win32/1.6.9/`): `graphics_read_pixels` in `Global.zhl` (screenshots) and `BatterySystem::SetTurnedOn` in `BatterySystem.zhl` (the backup battery's button).

## Current state: network duel (step 2, 0.5.0-dev)

- **Two players over UDP:** one hosts, the other joins, directly or through a relay server. Each game owns its own ship; the opponent is ship 1, a replica built from the opponent's loadout and driven by their messages (split authority).
- **State sync:** 10 times a second, hull, shields, system power and damage, weapon power and charge, the lock on each system (ion damage, and the backup battery while it runs and cools down) with its timer, and the battery. The replica's power is set the way the owner's changed: taken away as ion and damage take it, added as the power bars add it; subsystems (a nebula switches the sensors off) take the owner's value and keep it, whatever this game's environment does.
- **Shots decided by the defender:** a shot is sent with its exact target point when it leaves the weapon. The defender's own game decides dodge, shields and damage, and sends the verdict back. The attacker's copy waits at the target's shield until the verdict arrives, then plays it out. Lasers (burst and ion too), missiles and flak shards work this way. A bomb's copy waits where it goes off, then goes off or misses as the defender's did. A beam's copy sweeps the replica without damage; the defender's game does the damage.
- **Drones:** each game flies its own drones; the opponent's drones on our screen are puppets that are launched, powered, moved and destroyed as their owner's are, and never act on their own. What happens in a space is decided by the game whose space it is: our defense drones shoot the opponent's shots down in our space (the verdict "downed", shown on both screens), and hits on the opponent's drones in our space are reported to their owner. A combat drone's shot travels like a weapon's shot and is decided by the defender. Combat, beam, defense, anti-combat, hull repair and shield drones work; boarding drones and the drones that work inside ships come with crew (step 3). The puppets are drawn behind their owner's updates by the measured latency plus one update interval, so they move smoothly at lag.
- **Timing:** the defender's copy flies out of the enemy window faster, to make up for the network delay. It enters the defender's space when the attacker's copy entered the enemy window on the attacker's screen.
- **Duel view:** our ship stays as FTL draws it. The enemy window grows to the left up to a gap after it, and moves below the jump and menu buttons when it reaches them. The opponent's ship is drawn in it as large as it fits (about 0.65 for a Kestrel), mirrored so it faces us, with its icons the right way round, on the same centre line as ours. It is drawn with smooth filtering. In windows larger than 1280 x 720, frames are drawn at the window's size, so it keeps its detail. Mouse input goes through the inverse transforms: aiming at rooms, beam lines, crew orders, doors and tooltips work as usual. (`view equal on` draws both ships at one scale instead.)
- **Relay:** players who can't reach each other directly (routers) connect through a relay server (`server/relay` in the FTL: Duels repository, Rust). The host opens a room and gets a six-character code; the other player joins with it. After a cookie handshake, every packet is signed with a key per player, and the relay forwards the duel's packets without reading them. Protocol: `docs/design/relay-protocol.md` in the FTL: Duels repository.
- **Console:** F1 opens it where the game prints its messages (top left, under the status bar): the last 12 messages and an input line with a caret. It stays open after a command until F1 or Escape; Up and Down bring back earlier lines. `DUEL` can be left out, and `HS <command>` runs Hyperspace's own commands. Letters come out as typed. The corner label and the window title say "FTL: Duels".
- **From step 1:** enemy AI replaced by commands, no pause, speed locked to normal, tracing, and the autotest harness that plays a scripted match without a human.

Results and findings: `docs/dev/step2-results.md` in the FTL: Duels repository.

## Command language

The same commands are used by scenario scripts, the in-game console (F1, then `<verb> …`, with or without `DUEL`) and the autotest harness. Ship ids are 0 (the local player) and 1 (the opponent).

| Group | Verbs |
|---|---|
| Network duel | `host [port] [local]`, `join <address> [port]`, `relay [server[:port]]`, `host relay [server[:port]]`, `join relay <code\|@file> [server[:port]]`, `leave`, `net` (status), `name <player>`, `say <text>`, `netsim <delay ms> [jitter ms] [loss %]` |
| Session | `version`, `status`, `nopause on\|off`, `trace on\|off`, `tracepower on\|off`, `ai <ship> on\|off`, `script <file>`, `stop`, `note <text>`, `quit` |
| Power and weapons | `power <ship> <system> <level>`, `weapon <ship> <slot> on\|off`, `fire <ship> <slot> room <room>`, `autofire <ship> <slot> on\|off` |
| Crew and systems | `crew <ship> <index> room <room>`, `door <ship> <id> open\|close`, `cloak <ship>` |
| View and console | `view [auto\|fit\|off]` (the duel view: for the duel opponent, for any enemy, or vanilla), `view equal\|hires\|icons on\|off` (both ships at one scale; window-size frames; icons turned the right way round), `aimcheck [full]` (the mouse over every room of both ships, through the game's own mouse handling), `console [text]`, `keys <text>` |
| Setup and diagnostics | `spawn <SHIP_BLUEPRINT>`, `arm <ship> <slot> <WEAPON_BLUEPRINT>`, `upgrade <ship> <system> <levels>`, `install <ship> <system>`, `drone <ship> <slot> <DRONE_BLUEPRINT>`, `droneparts <ship> <count>`, `dronepower <ship> <slot> on\|off`, `battery <ship> on\|off`, `ionize <ship> <system> <amount>`, `supershield <ship> <layers>`, `nebula on|off`, `export <ship> <file>`, `describe <ship>`, `pausetest <kind>`, `window <x> <y> [width height]`, `screenshot <file.bmp>` |

- The default port is 47620 (UDP). `host local` accepts only a second game on the same computer and triggers no firewall prompt.
- `join` takes an IPv4 or IPv6 address or a host name.
- `relay` sets the relay server for `host relay` and `join relay` (UDP port 47700 by default); both also take it directly. `host relay` announces the room code, which the other player passes to `join relay` (or `@file`: the code is read from that file, for tests).
- With `trace on`, a duel writes `duels_shots.csv` (every shot, both directions) and `duels_sync.csv` (every change of our ship and of the replica).
- `screenshot` saves the next frame from FTL's own frame buffer, so it works with the window covered or the computer locked.
- `keys` types into the game through its own input handlers: characters as they are, and `{f1}`, `{enter}`, `{esc}`, `{up}`, `{down}`, `{back}` for keys (console tests).
- Test runs keep the games off the main monitor: with `DUELS_DISPLAY` (`x,y,width,height` of another monitor) and `DUELS_DISPLAY_SCALE`, window positions count from that monitor's corner and are scaled; `DUELS_WINDOW` (`x,y`) places the window there at start (`DuelsWin32.h`).
- `arm`, `upgrade`, `install`, `drone` and `droneparts` prepare test ships (e.g. flak, beams and bombs, a backup battery, drone control with drones); in a duel, before connecting, so the loadout carries them. `battery` and `dronepower` press the battery's and a drone's button; `ionize` does ion damage to a system, as an ion shot does; `supershield` gives a ship a Zoltan super shield; `nebula` puts our ship under a nebula beacon's effect (sensors off), in this game only.

## Files

| File | Role |
|---|---|
| `Duels.h`, `DuelsCore.cpp` | Shared state, log (`duels_log.txt`), frame trace, script runner |
| `DuelsHooks.cpp` | All hooks except the screenshot's |
| `DuelsDriver.cpp`, `DuelsShipControl.*` | The verbs, the AI takeover and projectile tracing |
| `DuelsScript.*` | The command/script parser |
| `DuelsAutotest.cpp` | The harness |
| `DuelsMatch.*` | The duel: loadout, replica, state sync, shots and verdicts |
| `DuelsDrones.*` | Drones: their state in the sync, the replica's drones as puppets, drone hits, copies of the opponent's defense drone shots |
| `DuelsNet.*` | Session: host/join (directly or through the relay), handshake, timeouts, test conditions |
| `DuelsRelay.*`, `DuelsCrypto.*` | The relay client (cookie handshake, room codes, signed packets; no game headers, unit-tested outside the game) and SHA-256/HMAC for it |
| `DuelsConsole.*` | The console: input line, recent messages, command history |
| `DuelsLink.*`, `DuelsWire.h` | Reliable-UDP link and message encoding (no game headers; unit-tested outside the game) |
| `DuelsSocket.*` | UDP sockets (Windows, and POSIX for later) |
| `DuelsView.*` | The duel view: layout, the enemy window's size and frame, both ships' transforms, mouse mapping |
| `DuelsScreen.*` | Frames at the window's size, screenshots from the frame buffer, the "FTL: Duels" label and window title |
| `DuelsTrace.*`, `DuelsWin32.*` | CSV output, window helpers (minimize, move, the test display) |

## License

Like the rest of this fork of FTL: Hyperspace, this module is licensed under CC BY-SA 4.0 (see `LICENSE.md` in the repository root).

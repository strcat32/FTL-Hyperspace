# FTL: Duels module

This module turns Hyperspace into the base of **FTL: Duels**, a 1v1 real-time PvP mode for FTL: Faster Than Light. It lives in this folder. Outside it there are two small changes: one line in `CMakeLists.txt` links Winsock (`ws2_32`) for the netcode, and one function in Hyperspace's function list (`libzhlgen/test/functions/win32/1.6.9/Global.zhl`: `graphics_read_pixels`, for screenshots).

## Current state: network duel (step 2)

- **Two players over UDP:** one hosts, the other joins. Each game owns its own ship; the opponent is ship 1, a replica built from the opponent's loadout and driven by their messages (split authority).
- **State sync:** 10 times a second, hull, shields, system power and damage, and weapon power and charge.
- **Shots decided by the defender:** a shot is sent with its exact target point when it leaves the weapon. The defender's own game decides dodge, shields and damage, and sends the verdict back. The attacker's copy waits at the target's shield until the verdict arrives, then plays it out. Lasers (burst and ion too), missiles and flak shards work this way. A bomb's copy waits where it goes off, then goes off or misses as the defender's did. A beam's copy sweeps the replica without damage; the defender's game does the damage.
- **Timing:** the defender's copy flies out of the enemy window faster, to make up for the network delay. It enters the defender's space when the attacker's copy entered the enemy window on the attacker's screen.
- **Duel view:** our ship stays as FTL draws it. The enemy window grows to the left up to a gap after it, and moves below the jump and menu buttons when it reaches them. The opponent's ship is drawn in it as large as it fits (about 0.65 for a Kestrel), mirrored so it faces us, with its icons the right way round, on the same centre line as ours. It is drawn with smooth filtering. In windows larger than 1280 x 720, frames are drawn at the window's size, so it keeps its detail. Mouse input goes through the inverse transforms: aiming at rooms, beam lines, crew orders, doors and tooltips work as usual. (`view equal on` draws both ships at one scale instead.)
- **Console:** F1 opens it on every keyboard layout; the input line starts with `>`. The corner label and the window title say "FTL: Duels".
- **From step 1:** enemy AI replaced by commands, no pause, speed locked to normal, tracing, and the autotest harness that plays a scripted match without a human.

Results and findings: `docs/dev/step2-results.md` in the FTL: Duels repository.

## Command language

The same commands are used by scenario scripts, the in-game console (F1, then `DUEL <verb> …`) and the autotest harness. Ship ids are 0 (the local player) and 1 (the opponent).

| Group | Verbs |
|---|---|
| Network duel | `host [port] [local]`, `join <address> [port]`, `leave`, `net` (status), `name <player>`, `say <text>`, `netsim <delay ms> [jitter ms] [loss %]` |
| Session | `version`, `status`, `nopause on\|off`, `trace on\|off`, `tracepower on\|off`, `ai <ship> on\|off`, `script <file>`, `stop`, `note <text>`, `quit` |
| Power and weapons | `power <ship> <system> <level>`, `weapon <ship> <slot> on\|off`, `fire <ship> <slot> room <room>`, `autofire <ship> <slot> on\|off` |
| Crew and systems | `crew <ship> <index> room <room>`, `door <ship> <id> open\|close`, `cloak <ship>` |
| View and console | `view [auto\|fit\|off]` (the duel view: for the duel opponent, for any enemy, or vanilla), `view equal\|hires\|icons on\|off` (both ships at one scale; window-size frames; icons turned the right way round), `aimcheck [full]` (the mouse over every room of both ships, through the game's own mouse handling), `console [text]` |
| Setup and diagnostics | `spawn <SHIP_BLUEPRINT>`, `arm <ship> <slot> <WEAPON_BLUEPRINT>`, `upgrade <ship> <system> <levels>`, `export <ship> <file>`, `describe <ship>`, `pausetest <kind>`, `window <x> <y> [width height]`, `screenshot <file.bmp>` |

- The default port is 47620 (UDP). `host local` accepts only a second game on the same computer and triggers no firewall prompt.
- `join` takes an IPv4 or IPv6 address or a host name.
- With `trace on`, a duel writes `duels_shots.csv` (every shot, both directions) and `duels_sync.csv` (every change of our ship and of the replica).
- `screenshot` saves the next frame from FTL's own frame buffer, so it works with the window covered or the computer locked.
- `arm` and `upgrade` prepare test ships (e.g. flak, beams and bombs); in a duel, before connecting, so the loadout carries them.

## Files

| File | Role |
|---|---|
| `Duels.h`, `DuelsCore.cpp` | Shared state, log (`duels_log.txt`), frame trace, script runner |
| `DuelsHooks.cpp` | All hooks except the screenshot's |
| `DuelsDriver.cpp`, `DuelsShipControl.*` | The verbs, the AI takeover and projectile tracing |
| `DuelsScript.*` | The command/script parser |
| `DuelsAutotest.cpp` | The harness |
| `DuelsMatch.*` | The duel: loadout, replica, state sync, shots and verdicts |
| `DuelsNet.*` | Session: host/join, handshake, timeouts, test conditions |
| `DuelsLink.*`, `DuelsWire.h` | Reliable-UDP link and message encoding (no game headers; unit-tested outside the game) |
| `DuelsSocket.*` | UDP sockets (Windows, and POSIX for later) |
| `DuelsView.*` | The duel view: layout, the enemy window's size and frame, both ships' transforms, mouse mapping |
| `DuelsScreen.*` | Frames at the window's size, screenshots from the frame buffer, the "FTL: Duels" label and window title |
| `DuelsTrace.*`, `DuelsWin32.*` | CSV output, window helpers (minimize, move) |

## License

Like the rest of this fork of FTL: Hyperspace, this module is licensed under CC BY-SA 4.0 (see `LICENSE.md` in the repository root).

# FTL: Duels module

This module turns Hyperspace into the base of **FTL: Duels**, a 1v1 real-time PvP mode for FTL: Faster Than Light. It lives in this folder. The only change outside it is one line in `CMakeLists.txt` that links Winsock (`ws2_32`) for the netcode.

## Current state: network duel (step 2)

- **Two players over UDP:** one hosts, the other joins. Each game owns its own ship; the opponent is ship 1, a replica built from the opponent's loadout and driven by their messages (split authority).
- **State sync:** 10 times a second, hull, shields, system power and damage, and weapon power and charge.
- **Shots decided by the defender:** a shot is sent with its exact target point when it leaves the weapon. The defender's own game decides dodge, shields and damage, and sends the verdict back. The attacker's copy waits at the target's shield until the verdict arrives, then plays it out. Lasers and missiles so far.
- **Timing:** the defender's copy flies out of the enemy window faster, to make up for the network delay. It enters the defender's space when the attacker's copy entered the enemy window on the attacker's screen.
- **From step 1:** enemy AI replaced by commands, no pause, speed locked to normal, tracing, and the autotest harness that plays a scripted match without a human.

Results and findings: `docs/dev/step2-results.md` in the FTL: Duels repository.

## Command language

The same commands are used by scenario scripts, the in-game console (`\`, then `DUEL <verb> …`) and the autotest harness. Ship ids are 0 (the local player) and 1 (the opponent).

| Group | Verbs |
|---|---|
| Network duel | `host [port] [local]`, `join <address> [port]`, `leave`, `net` (status), `name <player>`, `say <text>`, `netsim <delay ms> [jitter ms] [loss %]` |
| Session | `version`, `status`, `nopause on\|off`, `trace on\|off`, `tracepower on\|off`, `ai <ship> on\|off`, `script <file>`, `stop`, `note <text>`, `quit` |
| Power and weapons | `power <ship> <system> <level>`, `weapon <ship> <slot> on\|off`, `fire <ship> <slot> room <room>`, `autofire <ship> <slot> on\|off` |
| Crew and systems | `crew <ship> <index> room <room>`, `door <ship> <id> open\|close`, `cloak <ship>` |
| Setup and diagnostics | `spawn <SHIP_BLUEPRINT>`, `export <ship> <file>`, `describe <ship>`, `pausetest <kind>`, `window <x> <y>`, `screenshot <file.bmp>` |

- The default port is 47620 (UDP). `host local` accepts only a second game on the same computer and triggers no firewall prompt.
- `join` takes an IPv4 or IPv6 address or a host name.
- With `trace on`, a duel writes `duels_shots.csv` (every shot, both directions) and `duels_sync.csv` (every change of our ship and of the replica).

## Files

| File | Role |
|---|---|
| `Duels.h`, `DuelsCore.cpp` | Shared state, log (`duels_log.txt`), frame trace, script runner |
| `DuelsHooks.cpp` | All hooks |
| `DuelsDriver.cpp`, `DuelsShipControl.*` | The verbs, the AI takeover and projectile tracing |
| `DuelsScript.*` | The command/script parser |
| `DuelsAutotest.cpp` | The harness |
| `DuelsMatch.*` | The duel: loadout, replica, state sync, shots and verdicts |
| `DuelsNet.*` | Session: host/join, handshake, timeouts, test conditions |
| `DuelsLink.*`, `DuelsWire.h` | Reliable-UDP link and message encoding (no game headers; unit-tested outside the game) |
| `DuelsSocket.*` | UDP sockets (Windows, and POSIX for later) |
| `DuelsTrace.*`, `DuelsWin32.*` | CSV output, window helpers (minimize, move, screenshot) |

## License

Like the rest of this fork of FTL: Hyperspace, this module is licensed under CC BY-SA 4.0 (see `LICENSE.md` in the repository root).

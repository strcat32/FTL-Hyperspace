# FTL: Duels module

This module turns Hyperspace into the base of **FTL: Duels**, a 1v1 real-time PvP mode for FTL: Faster Than Light. It lives entirely in this folder; no upstream Hyperspace file is modified.

## Current state: offline replica (step 1)

- **Enemy under command control:** the enemy ship's AI is replaced by explicit commands, later to come from the network. This covers `ShipAI::OnLoop`, plus `ShipSystem::CheckForRepower`, which vanilla uses to re-power enemy systems every frame.
- **No pause:** the game can't be paused; spacebar, menus, event dialogs, focus loss and minimizing all keep the simulation running. The game speed is locked to normal.
- **Tracing:** frame timing and projectile flights are written to CSV files in the game folder.
- **Autotest harness:** a scenario file (`duels_autotest.txt` in the game folder) plays a whole scripted match without a human, then quits.

## Command language

The same commands are used by scenario scripts, the in-game console (`\`, then `DUEL <verb> …`) and, later, the network. Ship ids are 0 (the local player) and 1 (the opponent).

| Group | Verbs |
|---|---|
| Session | `version`, `status`, `nopause on\|off`, `trace on\|off`, `tracepower on\|off`, `ai <ship> on\|off`, `script <file>`, `stop`, `note <text>`, `quit` |
| Power and weapons | `power <ship> <system> <level>`, `weapon <ship> <slot> on\|off`, `fire <ship> <slot> room <room>`, `autofire <ship> <slot> on\|off` |
| Crew and systems | `crew <ship> <index> room <room>`, `door <ship> <id> open\|close`, `cloak <ship>` |
| Setup and diagnostics | `spawn <SHIP_BLUEPRINT>`, `export <ship> <file>`, `describe <ship>`, `pausetest <kind>` |

## Files

- `Duels.h`, `DuelsCore.cpp`: shared state, log (`duels_log.txt`), frame trace, script runner
- `DuelsHooks.cpp`: all hooks
- `DuelsDriver.cpp`, `DuelsShipControl.*`: the verbs, the AI takeover and projectile tracing
- `DuelsScript.*`: the command/script parser
- `DuelsAutotest.cpp`: the harness
- `DuelsTrace.*`, `DuelsWin32.*`: CSV output and window helpers

## License

Like the rest of this fork of FTL: Hyperspace, this module is licensed under CC BY-SA 4.0 (see `LICENSE.md` in the repository root).

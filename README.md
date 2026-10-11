# AntiAfk (Plugify)

Anti-AFK, ported from the Metamod plugin Anti-Afk (ABKAM). A one-second check walks every in-game human player: moving a WASD key or turning the view by aim_move_threshold_deg resets the idle timer (warmup and respawn states reset it too), after initial_warning_time the player is warned, the last five seconds count down, and at warning_interval the plugin kills the idle player (warnCount), escalating per max_warnings and ending with a kick (kick_instead_of_spec) or a move to spectators. Settings and phrases come from addons/configs/AntiAfk/settings.ini and addons/translations/antiafk.phrases.txt. No exported methods.

Plugify port (fork) of **Anti-Afk** by ABKAM (Metamod (C++)).
Original: https://github.com/ABKAM2023/Anti-Afk
The code was rewritten for Plugify / s2sdk; behaviour follows the original.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugify dependencies: s2sdk, hudmenu (optional)

## Installation
Download `AntiAfk-1.0.0.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/AntiAfk`.

## License
GPL-3.0, see LICENSE.


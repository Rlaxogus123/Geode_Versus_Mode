# Versus

Source code of Versus, a multiplayer battle mod for Geometry Dash (Geode mod `tipp7.versus`).
It includes 1 vs 1 and Group rooms, Ranked, LIVE spectating, replays, cosmetics and anti-cheat checks.

## Download

Get `tipp7.versus.geode` from the **[latest release](https://github.com/Rlaxogus123/Geode_Versus_Mode/releases/latest)**
and put it in Geometry Dash's Geode mods folder (`geode/mods`). One file works on Windows, macOS, iOS and Android.

## Repository layout

| Path | Contents |
|---|---|
| `src/` | The mod that runs in the game (C++, Geode SDK) |
| `resources/` | Images (`art/`, `auras/`), sounds (`sounds/`) and third-party licenses (`licenses/`) |
| `server/battle_rules.py` | The server's battle rules, as they run on the live server: how a progress report is checked, how the winner is decided, and how the game's anti-cheat detections are handled |

- Code comments are written in Korean.
- The rest of the Versus server (accounts, rooms, matchmaking, ratings, the database) is not part of this repository.

## Skip Music

During a map download, the Ready button in Classic/Group rooms and the main button in Ranked become
**Skip Music** (**음악 건너뛰기** with the saved Korean language choice). Skipping cancels the remaining
song and sound-effect downloads requested by Versus. Once valid level data is available, you are marked ready
through the normal readiness flow. Already downloaded audio is kept and can still play.

The choice applies to the selected map in the current room, including match loading; changing rooms,
maps or draws clears it. It does not change the ranked **Always Download Song** preference.
See [the test guide](tests/README.md) for automated checks and the in-game checklist.

## Anti-cheat

Cheat detection runs inside the game, in this mod (`src/ExternalCheatMonitor.cpp`, `src/IntegrityMonitor.cpp`,
`src/IntegrityFeatures.hpp`): during a battle with anti-cheat on, it checks the settings and the behaviour of the
known hack menus (Mega Hack, Eclipse, QOLMod, Prism Menu) and whether a banned mod is loaded, and reports a
detection as a code plus the feature names. Nothing else about the computer is read or sent. The server
(`server/battle_rules.py`) only validates each report and decides the result; a detection makes that seat forfeit
and cannot be withdrawn.

## License

The source code is released under the [MIT License](LICENSE). The images and sounds in `resources/` are not
covered by it: they may only be used as part of Versus.

- This project was developed with the help of an AI coding tool (Anthropic Claude).
- Argon (GD account verification) is an MIT-licensed library by the Globed Team (`resources/licenses/argon-LICENSE.txt`).

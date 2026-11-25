# Versus

Source code of Versus, a multiplayer battle mod for Geometry Dash (Geode mod `tipp7.versus`).
It includes 1 vs 1 and Group rooms, Ranked, LIVE spectating, replays, cosmetics and anti-cheat checks.

## Download

Get the file for your Geometry Dash version from the **[latest release](https://github.com/Rlaxogus123/Geode_Versus_Mode/releases/latest)**
and put it in Geometry Dash's Geode mods folder (`geode/mods`). Each file works on Windows, macOS, iOS and Android.

| Geometry Dash | Geode | File |
|---|---|---|
| 2.2081 | 5 | `tipp7.versus.geode` |
| 2.2091 | 6 | `tipp7.versus-2.2091.geode` |

On 2.2091 Versus also needs the Argon mod: install it from Geode's mod list when Geode says Versus is missing a dependency.

## Repository layout

| Path | Contents |
|---|---|
| `src/` | The mod that runs in the game (C++, Geode SDK). The few parts that differ between GD 2.2081 and 2.2091 are in `src/compat/` |
| `resources/` | Images (`art/`, `auras/`), sounds (`sounds/`) and third-party licenses (`licenses/`) |
| `tools/geode_package.py` | Checks a release package against this repository, and writes it reproducibly |
| `server/battle_rules.py` | The server's battle rules, as they run on the live server: how a progress report is checked, how the winner is decided, and how the game's anti-cheat detections are handled |

- Code comments are written in Korean.
- The rest of the Versus server (accounts, rooms, matchmaking, ratings, the database) is not part of this repository.

## Anti-cheat

Cheat detection runs inside the game, in this mod (`src/ExternalCheatMonitor.cpp`, `src/IntegrityMonitor.cpp`,
`src/IntegrityFeatures.hpp`): during a battle with anti-cheat on, it checks the settings and the behaviour of the
known hack menus (Mega Hack, Eclipse, QOLMod, Prism Menu) and whether a banned mod is loaded, and reports a
detection as a code plus the feature names. Nothing else about the computer is read or sent. The server
(`server/battle_rules.py`) only validates each report and decides the result; a detection makes that seat forfeit
and cannot be withdrawn.

## Verifying a release

Every release is built by this repository's GitHub Actions workflow (`.github/workflows/build.yml`) from the tagged
commit only: no other repository, no secrets (the only token is the one GitHub gives each run to upload the release).

- **Provenance:** each package has a GitHub build attestation:
  `gh attestation verify tipp7.versus.geode --repo Rlaxogus123/Geode_Versus_Mode` (or `tipp7.versus-2.2091.geode`)
  shows the workflow run and the commit that produced exactly that file.
- **Contents:** `python3 tools/geode_package.py check tipp7.versus.geode` (needs Pillow) checks that every file in
  the package other than the five binaries is this repository's own file (`mod.json`, `about.md`, `changelog.md`,
  `logo.png`, `resources/`). `mod.json` is compared with this repository's `mod.json` set for the package's GD version
  (`tools/geode_package.py modjson` changes only the Geode version, the GD version and, for 2.2091, the Argon
  dependency). PNGs are compared by pixels and text by content, because Geode's packer recompresses PNGs and the
  Windows build writes text with CRLF. Each release also lists every file's SHA-256 (`contents.sha256` for 2.2081,
  `contents-2.2091.sha256` for 2.2091).
- **Rebuild:** the build is set up to be reproducible. Running the workflow on a release tag (Actions > Build and release >
  Run workflow, "Use workflow from" the tag) builds both packages again and compares them with the published files byte
  for byte. The build actions and the Geode CLI are pinned, the packages are written in a fixed order with fixed times
  (`tools/geode_package.py normalize`), and the Windows DLL is linked with `/Brepro` (no link time in it).

Up to v1.0.5 the images and sounds came from a separate private repository during the build. They are the files in
`resources/` now: the check above passes for the v1.0.5 release against the v1.0.5 tag plus today's `resources/`.

## License

The source code is released under the [MIT License](LICENSE). The images and sounds in `resources/` are not
covered by it: they may only be used as part of Versus.

- This project was developed with the help of an AI coding tool (Anthropic Claude).
- Argon (GD account verification) is an MIT-licensed library by the Globed Team (`resources/licenses/argon-LICENSE.txt`).

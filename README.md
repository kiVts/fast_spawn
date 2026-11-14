# Fast Spawn

[![Join Discord](https://img.shields.io/badge/Join-Discord-5865F2?style=for-the-badge&logo=discord&logoColor=white)](https://discord.gg/bX4RzunsxC)

Shortens Stormworks' **“Spawning...”** screen when joining a multiplayer game or respawning, and speeds up parts of world loading.

- Finishes ready map tiles sooner while someone is waiting to spawn.
- Loads several vehicles in parallel while keeping vehicle saves sequential.
- Replaces the loading screen's fixed frame-count wait with a minimum time, so low frame rates don't stretch it into seconds.
- Caches missing-file lookups and warms game files in the background to reduce loading delays.
- Uses separate memory arenas under Wine/Proton to reduce contention during loading and play.

The main multiplayer improvements need the mod on the **host**. Players without it can still join and benefit. Installing it on a client also provides local loading improvements and the loading-screen wait adjustment. It works in single player too.

This mod is made in compliance of Geometa EULA. Under Section 3 Paragraph 4, `You must not modify, merge, distribute, translate, reverse engineer, decompile, or try to obtain or use source code for the Geometa Services, except for mods as per section 5 or where permitted by applicable law.` In section 5, `You may create and share mods and other content for our games, provided they comply with this Agreement. You own the content you create, including the content created in game, mods created with Geometa tool kits, community websites, etc.`

## Installation

Install [SWML](https://github.com/kiVts/swml), then add `fast_spawn.zip` from this repository's releases through the loader's **Add mod...** button. Enable Fast Spawn and restart the game. I also recommend you install [No Crash Uploads](https://github.com/kiVts/no_crashlog_upload) so you don't upload a crash-report with an injected DLL to Geometa ( And a crash reason overwhelmingly likely to be due to one of these mods). consider other mods as well!

[Composite Tracer](https://github.com/kiVts/composite_tracer)
[Editor Rotation Rings](https://github.com/kiVts/editor_rotation_rings)
[Mod Status](https://github.com/kiVts/mod_status)
[Network Fixes(WIP)](https://github.com/kiVts/network_fixes) (Slightly unstable)
[Physics Fixes](https://github.com/kiVts/physics_fixes)

While this mod is yet to do anything destructive to any of my saves, it still messes with the way saves are loaded and parsed. There is a non-zero, low chance it will corrupt something.

## Settings

Open Fast Spawn's **Settings** tab in SWML. Defaults are listed in `settings.default.ini`; SWML 4.4 stores your changes in `settings.overrides.ini`.

The defaults allow four vehicle loads at once, with tile-completion budgets of 12 ms per tick when other players are connected and 60 ms when alone. The minimum loading-screen wait is 500 ms.

These budgets control additional tile-completion work. A single expensive operation can still take longer, and complex vehicles can still cause stalls. Loading speed depends on the world, storage and available CPU time.

## Troubleshooting

Check `fast_spawn.log` through SWML's **Open logs** button for spawn progress and vehicle-loading timings. Include the relevant session logs when reporting an issue.

This mod is a work in progress. While stable*, there might be further improvements or regressions. See [CHANGELOG.md](CHANGELOG.md) for version history.


Like what I do?

<a href="https://buymeacoffee.com/kivts">
  <img src="https://cdn.buymeacoffee.com/buttons/v2/default-yellow.png" width="120" alt="Buy Me a Coffee">
</a>

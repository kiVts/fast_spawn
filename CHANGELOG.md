1.8.0: under Wine every game thread allocates from an arena of its own, not only the vehicle load threads ([spawn] all_threads, "game heap 60s").

1.7.8: runs on the 2026-10-28 game build (0x6ae218ec) through the SDK port table; the world's tile grid ring moved +8 (0x6470 -> 0x6478). The game now loads tiles within 3 of a player (was 2); fast_spawn counts pending tiles, so nothing else changes.

# Fast Spawn

Makes the "Spawning..." screen shorter, mostly when joining or respawning in multiplayer.

1.7.7: the missing-file cache also covers `mods_temp` (where the game stages every enabled mod's data, on a joining
player the host's mods). A joining player's world load looks every tile, definition and mesh up there too; on the
friend's Proton client that was ~54,000 failed opens and 4.6 s of a 14 s load. Those folders change while the game
runs, so each one's last-write time is checked before its listing is used.

1.7.2 fixes a missing-file-cache lifetime race: a file lookup already in progress now keeps its immutable
directory listing alive when the last loading task clears the cache. The previous code returned a reference
after releasing the cache lock; concurrent load completion could delete that referenced entry. A deterministic
AddressSanitizer test reproduces the old use-after-free. This is a confirmed bug; attribution of the
2026-10-22 01:05 heap-corruption crash to it remains unproven because its dump omits the heap blocks.

**Why it is slow.** When you spawn, the host marks the 5x5 map tiles (1 km each) around your spawn point
as needed, and your game waits until every one is loaded on the host and on your side. The host loads a
tile in the background, but then only *finishes* one tile (puts its objects and vehicles into the world)
every 6 ticks. 25 tiles = 150 ticks: 2.5 s at a full 60 ticks/s, 4-5 s on a busy host running at
30-40 ticks/s, before the network and your own loading.

It gets worse from there: the host only adds the vehicles near you to the world on ticks when *no* tile is
still loading, so every vehicle around the spawn point waits for the last tile too. That wait is what the
Spawning screen spends most of its time on.

**What this mod does (on the host / in single player).** While someone is on the Spawning screen, the
host finishes every tile whose loading is done straight away, up to `budget_ms` of work per tick
(`budget_ms_alone` when nobody else is connected). When no tiles are left to load, the game's normal
pacing comes back, so flying around the map is unchanged.

The vehicles themselves load on a single background thread in the game, one after another (about 150 ms
each in testing). The mod loads up to `vehicle_load_threads` (default 4) at once. Vehicle saves share that
thread; they still run one at a time, in order, and never alongside a load.

The screen also waits a fixed 30 frames once the host answers, which at the loading screen's low frame
rate (often 10-15 fps) takes 2-3 s instead of 0.5 s. The mod ends that wait after `min_wait_ms` (500 ms)
instead, whatever the frame rate. This part works on clients too.

Only the host needs it: players joining a host that runs it spawn faster without installing anything.
On a client it only logs what the Spawning screen is waiting for.

`fast_spawn.log`:
- `spawn: ...` every couple of seconds on the Spawning screen (host or client): what it is waiting for
  (host reply, tiles, the tile under the player...), and `spawn: ready in X s` with the time per phase.
- `vehicle loads: ...` on the host: each batch of vehicle loads (how many, how long, how many ran at once).
- `boost: ...` on the host: when a player starts waiting, and a summary (tiles finished early, ms spent,
  how long tiles waited for the game's gap).

- `watch world+0x88: ...` (diagnostic, off by default, `watch_world88=1`): the Spawning screen also waits for a counter in the
  game's world object to reach 0. While the screen is up, a CPU debug register traps every write to it and the
  log names the game code that wrote it (with callers), how often it went up or down, and the order of the writes.
  It stops when the screen closes.

Settings (settings.ini, `[spawn]`): `boost` (0 = only log), `budget_ms`, `budget_ms_alone`, `max_boost_s`,
`log_every_s`, `min_wait_ms`, `vehicle_load_threads`, `watch_world88`.

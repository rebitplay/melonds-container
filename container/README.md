# melonDS Container Demo

This folder is the Rebit room-container layer for a four-player melonDS DS LAN prototype.

It is intentionally separated from the core source. The core remains the upstream-compatible
`melondsds_libretro` fork; this folder defines how Rebit should run 2-4 linked instances,
stream one player view per user, and expose a browser demo page.

## Current State

- `web/` contains the four-player browser index.
- `server/` serves the index and exposes the room/signaling contract.
- `room-config.example.json` defines one demo room with four emulator slots.
- `Dockerfile` builds the core and packages the room server.
- `runner/lan_room_runner.c` is a native proof runner for 2-4 colocated melonDS
  libretro instances with a room-local netpacket switch.

The proof runner boots isolated core processes, wires
`RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE`, creates per-player save/system dirs,
captures video/audio callback counts, and routes libretro netpackets between slots.
WebRTC publishing is still the next integration step.

## ROMs

Do not commit commercial ROMs. Mount a legally supplied `.nds` file into the container:

```sh
mkdir -p container/roms
cp /path/to/your/game.nds container/roms/demo.nds
```

The demo server reads `ROM_PATH`, defaulting to `/roms/demo.nds` inside the container and
`container/roms/demo.nds` for local development.

## Local Control-Plane Demo

This starts the web index and validates the room contract. It does not emulate yet.

```sh
cd container/server
npm install
ROM_PATH=../roms/demo.nds npm start
```

Open `http://localhost:8787`.

## Local 2-Player LAN Proof

This compiles the native proof runner with `gcc`, starts two melonDS libretro core
processes, and routes packets between them in one room process.

```sh
container/run-lan2-demo.sh
```

By default, the wrapper uses Rebit's local core and homebrew test ROM paths:

```text
CORE_PATH=../cloud-game/assets/cores/melondsds_libretro.so
ROM_PATH=../cloud-game/assets/games/nds/blocksds-local-multiplayer.nds
```

You can override either path:

```sh
ROM_PATH=/path/to/test.nds FRAMES=300 container/run-lan2-demo.sh
```

After both cores report ready, the runner injects one labelled netpacket probe
through each child process. That validates the frontend send callback, parent
switch, peer receive callback, and per-slot runtime isolation even if the loaded
ROM does not enter its local multiplayer flow without UI input. Disable the probe
with `MELONDS_RUNNER_NO_PROBE=1` when testing game-generated traffic only.

Useful success lines:

```text
[slot 1] core info: Starting multiplayer on libretro side
[slot 2] core info: Starting multiplayer on libretro side
[room] netpacket totals: sent=2 received=2
```

The Docker image packages the same proof runner at:

```text
/opt/melonds/melonds-lan-room-runner
```

## Container Shape

```text
room container
  server/server.mjs        HTTP + WebSocket signaling contract
  native room-runner       2-4 melonDS instances + netpacket switch
  melondsds_libretro.so    built from this repo
  /roms/demo.nds           mounted legal test ROM
  /runtime/player-1        save/system/runtime dir
  /runtime/player-2
  /runtime/player-3
  /runtime/player-4
```

Each player slot needs an isolated emulator runtime but shared room networking:

```text
player 1 input -> melonDS #1 -> encoder #1 -> WebRTC #1
player 2 input -> melonDS #2 -> encoder #2 -> WebRTC #2
player 3 input -> melonDS #3 -> encoder #3 -> WebRTC #3
player 4 input -> melonDS #4 -> encoder #4 -> WebRTC #4

melonDS #1..#4 <-> in-container netpacket switch <-> local multiplayer
```

## Runner Contract

The native runner should expose the contract documented in `runner-contract.md`.

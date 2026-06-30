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

The native runner still has to be wired in. It must implement libretro netpacket routing,
audio/video capture, and WebRTC publishing for each player slot. The existing `cloud-game`
frontend does not implement `RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE`, so melonDS LAN will
not work there by configuration alone.

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

## Container Shape

```text
room container
  server/server.mjs        HTTP + WebSocket signaling contract
  native room-runner       TODO: 2-4 melonDS instances
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

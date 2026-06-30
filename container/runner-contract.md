# Room Runner Contract

The room server owns HTTP, browser assets, and signaling. The native runner owns emulator
execution, netpacket routing, encoding, and WebRTC media.

## Process Model

Start one runner per room:

```sh
melonds-room-runner \
  --room demo \
  --players 4 \
  --rom /roms/demo.nds \
  --core /opt/melonds/melondsds_libretro.so \
  --runtime /runtime \
  --control /run/rebit-room.sock
```

The first implementation should use separate OS processes for each player instance. That avoids
the global-libretro-state problem while we validate melonDS LAN behavior.

## Required Native Pieces

1. Libretro frontend support for `RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE`.
2. A room-local netpacket switch:
   - assigns client IDs `1..N`
   - broadcasts `RETRO_NETPACKET_BROADCAST`
   - routes unicast replies by client ID
   - supports unreliable/unsequenced packets immediately
   - treats reliable packets as ordered delivery
3. Per-player runtime dirs:
   - save directory
   - system directory
   - firmware identity/user name/MAC where supported
4. Per-player media path:
   - video callback -> encoder -> WebRTC video track
   - audio callback -> encoder -> WebRTC audio track
5. Per-player input path:
   - browser data channel -> retropad/touch/mic state for that player
   - set `melonds_touch_mode=touch` or `auto` so browser pointer input is used

## Control Socket

The runner should listen on a Unix socket owned by the HTTP server.

### `GET /status`

Returns:

```json
{
  "roomId": "demo",
  "state": "starting|ready|stopping|stopped|error",
  "players": [
    {"slot": 1, "state": "ready", "connected": false},
    {"slot": 2, "state": "ready", "connected": false},
    {"slot": 3, "state": "ready", "connected": false},
    {"slot": 4, "state": "ready", "connected": false}
  ]
}
```

### `POST /signal/:slot`

Browser-to-runner WebRTC signaling message:

```json
{
  "type": "offer|answer|ice",
  "payload": {}
}
```

Runner returns the matching answer or ICE candidate messages through the same WebSocket session.

### `POST /input/:slot`

Binary input messages should match the Rebit/cloud-game retropad format first:

```text
uint16 buttons
int16  left_x
int16  left_y
int16  right_x
int16  right_y
```

Touch input should be sent on a second negotiated data channel or tagged control message:

```json
{"type":"touch","down":true,"x":0,"y":16384}
```

`x` and `y` are libretro pointer coordinates, not DS pixels. They must be signed
16-bit values in libretro's `RETRO_DEVICE_POINTER` range:

```text
left/top     = -32768
center       = 0
right/bottom = 32767
```

The runner stores this state per slot and returns it from:

```c
input_state(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED)
input_state(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X)
input_state(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y)
```

The melonDS DS core transforms those coordinates into DS touchscreen coordinates using
the active screen layout. The browser should send pointer coordinates relative to the
rendered video element.

## Done Criteria For The Native Runner

- Four instances boot the same `.nds` file.
- melonDS DS logs show multiplayer netpacket callbacks started.
- Each player gets a unique WebRTC stream.
- Local multiplayer lobby can see all players with the instances colocated.
- Stopping the room saves per-slot data and exits all emulator processes.

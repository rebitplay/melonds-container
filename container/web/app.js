const playersEl = document.getElementById("players");
const template = document.getElementById("player-template");
const roomStateEl = document.getElementById("room-state");
const refreshEl = document.getElementById("refresh");
const startEl = document.getElementById("start");

const state = {
  slots: new Map(),
};

const pointerCoord = (value, size) => {
  if (!size) return 0;
  const unit = Math.max(0, Math.min(1, value / size));
  return Math.round(unit * 65535 - 32768);
};

const setRoomState = (message) => {
  roomStateEl.textContent = message;
};

const sendTouch = (slot, refs, event, down) => {
  const rect = refs.screen.getBoundingClientRect();
  const x = pointerCoord(event.clientX - rect.left, rect.width);
  const y = pointerCoord(event.clientY - rect.top, rect.height);
  refs.detail.textContent = down ? `Touch ${x}, ${y}` : "Touch released";

  if (refs.ws?.readyState === WebSocket.OPEN) {
    refs.ws.send(JSON.stringify({
      type: "input",
      device: "touch",
      down,
      x,
      y,
    }));
  }
};

const playerNode = (slot) => {
  if (state.slots.has(slot.slot)) return state.slots.get(slot.slot);

  const node = template.content.firstElementChild.cloneNode(true);
  const screen = node.querySelector(".screen");
  const video = node.querySelector("video");
  const title = node.querySelector(".placeholder strong");
  const detail = node.querySelector(".placeholder span");
  const label = node.querySelector(".label");
  const status = node.querySelector(".status");

  label.textContent = slot.label;
  title.textContent = slot.label;
  detail.textContent = "Waiting";
  status.textContent = "idle";

  playersEl.append(node);
  const refs = {node, screen, video, detail, status, ws: null};

  screen.addEventListener("pointerdown", (event) => {
    event.preventDefault();
    screen.setPointerCapture(event.pointerId);
    sendTouch(slot.slot, refs, event, true);
  });
  screen.addEventListener("pointermove", (event) => {
    if (!screen.hasPointerCapture(event.pointerId)) return;
    event.preventDefault();
    sendTouch(slot.slot, refs, event, true);
  });
  const release = (event) => {
    if (screen.hasPointerCapture(event.pointerId)) {
      screen.releasePointerCapture(event.pointerId);
    }
    sendTouch(slot.slot, refs, event, false);
  };
  screen.addEventListener("pointerup", release);
  screen.addEventListener("pointercancel", release);

  state.slots.set(slot.slot, refs);
  return refs;
};

const connectSignaling = (slot) => {
  const refs = playerNode(slot);
  const scheme = location.protocol === "https:" ? "wss" : "ws";
  const ws = new WebSocket(`${scheme}://${location.host}/signal?slot=${slot.slot}`);

  ws.addEventListener("open", () => {
    refs.ws = ws;
    refs.status.textContent = "signaling";
    refs.detail.textContent = "Signaling connected";
  });

  ws.addEventListener("message", (event) => {
    const message = JSON.parse(event.data);
    refs.status.textContent = message.state || message.type;
    if (message.error) refs.detail.textContent = message.error;
  });

  ws.addEventListener("close", () => {
    if (refs.ws === ws) refs.ws = null;
    refs.status.textContent = "closed";
  });
};

const loadRoom = async () => {
  const response = await fetch("/api/room");
  const room = await response.json();

  playersEl.textContent = "";
  state.slots.clear();
  for (const slot of room.players) {
    const refs = playerNode(slot);
    refs.status.textContent = slot.state;
    refs.detail.textContent = slot.connected ? "Connected" : "No stream";
    connectSignaling(slot);
  }

  const rom = room.rom.ready ? "ROM ready" : "ROM missing";
  const runner = room.runner.ready ? "runner ready" : "runner missing";
  setRoomState(`${room.roomId}: ${rom}, ${runner}`);
  startEl.disabled = !room.rom.ready;
};

const startRoom = async () => {
  startEl.disabled = true;
  const response = await fetch("/api/room/start", {method: "POST"});
  const data = await response.json();
  if (!response.ok) {
    setRoomState(`${data.error}: ${data.romPath || data.contract || ""}`.trim());
  } else {
    setRoomState(data.state);
  }
  startEl.disabled = false;
};

refreshEl.addEventListener("click", loadRoom);
startEl.addEventListener("click", startRoom);

loadRoom().catch((error) => {
  setRoomState(error.message);
});

const playersEl = document.getElementById("players");
const template = document.getElementById("player-template");
const roomStateEl = document.getElementById("room-state");
const refreshEl = document.getElementById("refresh");
const startEl = document.getElementById("start");
const stopEl = document.getElementById("stop");
const metricsEl = document.getElementById("metrics");
const logsEl = document.getElementById("logs");
const slotTabs = [...document.querySelectorAll(".slot-tab")];

const BUTTONS = {
  b: 0,
  y: 1,
  select: 2,
  start: 3,
  up: 4,
  down: 5,
  left: 6,
  right: 7,
  a: 8,
  x: 9,
  l: 10,
  r: 11,
};

const KEY_TO_BUTTON = new Map([
  ["KeyW", "up"],
  ["ArrowUp", "up"],
  ["KeyS", "down"],
  ["ArrowDown", "down"],
  ["KeyA", "left"],
  ["ArrowLeft", "left"],
  ["KeyD", "right"],
  ["ArrowRight", "right"],
  ["KeyJ", "a"],
  ["KeyK", "b"],
  ["KeyU", "x"],
  ["KeyI", "y"],
  ["KeyQ", "l"],
  ["KeyE", "r"],
  ["Enter", "start"],
  ["ShiftLeft", "select"],
  ["ShiftRight", "select"],
]);

const state = {
  activeSlot: 1,
  room: null,
  slots: new Map(),
};

const pointerCoord = (value, size) => {
  if (!size) return 0;
  const unit = Math.max(0, Math.min(1, value / size));
  return Math.round(unit * 65535 - 32768);
};

const buttonMask = (buttons) => {
  let mask = 0;
  for (const button of buttons) mask |= 1 << BUTTONS[button];
  return mask;
};

const setActiveSlot = (slot) => {
  state.activeSlot = slot;
  slotTabs.forEach((tab) => tab.classList.toggle("active", Number(tab.dataset.slot) === slot));
  for (const [id, refs] of state.slots) refs.node.classList.toggle("active", id === slot);
};

const sendInput = (slot) => {
  const refs = state.slots.get(slot);
  if (!refs) return;
  const payload = {
    buttons: buttonMask(refs.buttons),
    pointerX: refs.pointer.x,
    pointerY: refs.pointer.y,
    pointerPressed: refs.pointer.pressed,
  };
  fetch(`/api/input/${slot}`, {
    method: "POST",
    headers: {"content-type": "application/json"},
    body: JSON.stringify(payload),
  }).catch(() => {});
};

const setButton = (slot, button, pressed) => {
  const refs = state.slots.get(slot);
  if (!refs || !(button in BUTTONS)) return;
  if (pressed) refs.buttons.add(button);
  else refs.buttons.delete(button);
  sendInput(slot);
};

const setPointer = (slot, refs, event, pressed) => {
  const rect = refs.screen.getBoundingClientRect();
  refs.pointer.x = pointerCoord(event.clientX - rect.left, rect.width);
  refs.pointer.y = pointerCoord(event.clientY - rect.top, rect.height);
  refs.pointer.pressed = pressed;
  refs.status.textContent = pressed ? `touch ${refs.pointer.x}, ${refs.pointer.y}` : "running";
  sendInput(slot);
};

const updateFrame = (slot, refs) => {
  const next = new Image();
  next.onload = () => {
    refs.img.src = next.src;
    refs.placeholder.hidden = true;
    refs.status.textContent = "streaming";
  };
  next.onerror = () => {
    refs.placeholder.hidden = false;
    if (state.room?.runner?.running) refs.status.textContent = "waiting for frame";
  };
  next.src = `/frame/${slot}?t=${Date.now()}`;
};

const createPlayer = (slot) => {
  const node = template.content.firstElementChild.cloneNode(true);
  const screen = node.querySelector(".screen");
  const img = node.querySelector("img");
  const placeholder = node.querySelector(".placeholder");
  const select = node.querySelector(".select-player");
  const status = node.querySelector(".status");

  node.dataset.slot = slot.slot;
  select.textContent = slot.label || `Player ${slot.slot}`;
  img.alt = `${select.textContent} screen`;
  status.textContent = slot.state || "idle";
  playersEl.append(node);

  const refs = {
    node,
    screen,
    img,
    placeholder,
    select,
    status,
    buttons: new Set(),
    pointer: {x: 0, y: 0, pressed: false},
    frameTimer: null,
  };

  select.addEventListener("click", () => setActiveSlot(slot.slot));
  screen.addEventListener("pointerdown", (event) => {
    event.preventDefault();
    setActiveSlot(slot.slot);
    screen.setPointerCapture(event.pointerId);
    setPointer(slot.slot, refs, event, true);
  });
  screen.addEventListener("pointermove", (event) => {
    if (!screen.hasPointerCapture(event.pointerId)) return;
    event.preventDefault();
    setPointer(slot.slot, refs, event, true);
  });
  const release = (event) => {
    if (screen.hasPointerCapture(event.pointerId)) screen.releasePointerCapture(event.pointerId);
    setPointer(slot.slot, refs, event, false);
  };
  screen.addEventListener("pointerup", release);
  screen.addEventListener("pointercancel", release);

  for (const button of node.querySelectorAll(".pad button")) {
    const name = button.dataset.button;
    const down = (event) => {
      event.preventDefault();
      setActiveSlot(slot.slot);
      button.setPointerCapture?.(event.pointerId);
      setButton(slot.slot, name, true);
    };
    const up = (event) => {
      event.preventDefault();
      if (event.pointerId !== undefined && button.hasPointerCapture?.(event.pointerId)) {
        button.releasePointerCapture(event.pointerId);
      }
      setButton(slot.slot, name, false);
    };
    button.addEventListener("pointerdown", down);
    button.addEventListener("pointerup", up);
    button.addEventListener("pointercancel", up);
    button.addEventListener("pointerleave", up);
  }

  state.slots.set(slot.slot, refs);
  refs.frameTimer = window.setInterval(() => updateFrame(slot.slot, refs), 80);
  updateFrame(slot.slot, refs);
};

const renderPlayers = (room) => {
  for (const refs of state.slots.values()) window.clearInterval(refs.frameTimer);
  state.slots.clear();
  playersEl.textContent = "";
  for (const slot of room.players.slice(0, 2)) createPlayer(slot);
  setActiveSlot(Math.min(state.activeSlot, 2));
};

const setRoomSummary = (room) => {
  const rom = room.rom.ready ? "ROM ready" : "ROM missing";
  const core = room.core.ready ? "core ready" : "core missing";
  roomStateEl.textContent = `${room.state} · ${rom} · ${core}`;
  metricsEl.textContent = `${room.rom.path} · runtime ${room.runtimePath}`;
  startEl.disabled = room.runner.running || !room.rom.ready || !room.core.ready;
  stopEl.disabled = !room.runner.running;
};

const loadRoom = async () => {
  const response = await fetch("/api/room");
  const room = await response.json();
  state.room = room;
  setRoomSummary(room);
  renderPlayers(room);
};

const refreshRoomState = async () => {
  const response = await fetch("/api/room");
  const room = await response.json();
  state.room = room;
  setRoomSummary(room);
  for (const slot of room.players) {
    const refs = state.slots.get(slot.slot);
    if (refs && !room.runner.running) refs.status.textContent = slot.state;
  }
};

const startRoom = async () => {
  startEl.disabled = true;
  const response = await fetch("/api/room/start", {method: "POST"});
  const data = await response.json();
  if (!response.ok) roomStateEl.textContent = `${data.error}: ${data.romPath || data.corePath || ""}`;
  await refreshRoomState();
};

const stopRoom = async () => {
  stopEl.disabled = true;
  await fetch("/api/room/stop", {method: "POST"});
  await refreshRoomState();
};

const refreshLogs = async () => {
  const response = await fetch("/api/logs");
  const data = await response.json();
  logsEl.textContent = data.logs.slice(-40).map((entry) => entry.line).join("\n");
  logsEl.scrollTop = logsEl.scrollHeight;
};

window.addEventListener("keydown", (event) => {
  const button = KEY_TO_BUTTON.get(event.code);
  if (!button) return;
  event.preventDefault();
  setButton(state.activeSlot, button, true);
});

window.addEventListener("keyup", (event) => {
  const button = KEY_TO_BUTTON.get(event.code);
  if (!button) return;
  event.preventDefault();
  setButton(state.activeSlot, button, false);
});

window.addEventListener("blur", () => {
  for (const [slot, refs] of state.slots) {
    refs.buttons.clear();
    refs.pointer.pressed = false;
    sendInput(slot);
  }
});

slotTabs.forEach((tab) => tab.addEventListener("click", () => setActiveSlot(Number(tab.dataset.slot))));
refreshEl.addEventListener("click", refreshRoomState);
startEl.addEventListener("click", startRoom);
stopEl.addEventListener("click", stopRoom);

await loadRoom();
window.setInterval(refreshRoomState, 1500);
window.setInterval(refreshLogs, 1500);
refreshLogs();

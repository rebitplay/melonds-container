import { createReadStream, existsSync, mkdirSync, readFileSync, statSync, writeFileSync } from "node:fs";
import { createServer } from "node:http";
import { spawn } from "node:child_process";
import { extname, join, normalize } from "node:path";
import { fileURLToPath } from "node:url";
import { WebSocketServer } from "ws";

const root = join(fileURLToPath(new URL(".", import.meta.url)), "..");
const webRoot = join(root, "web");
const rebitRoot = join(root, "..", "..");
const port = Number(process.env.PORT || 8787);
const players = Math.max(2, Math.min(4, Number(process.env.PLAYERS || 2)));
const defaultConfigPath = join(root, "room-config.json");
const fallbackConfigPath = join(root, "room-config.example.json");
const configPath = process.env.ROOM_CONFIG || (existsSync(defaultConfigPath) ? defaultConfigPath : fallbackConfigPath);

const firstExisting = (...paths) => paths.find((path) => path && existsSync(path)) || paths.find(Boolean);
const config = JSON.parse(readFileSync(configPath, "utf8"));
const runtimePath = process.env.RUNTIME_DIR || process.env.RUNTIME_PATH || config.runtimePath || join(root, "runtime", "playable");
const corePath = firstExisting(
  process.env.CORE_PATH,
  config.corePath,
  "/opt/melonds/melondsds_libretro.so",
  join(rebitRoot, "cloud-game", "assets", "cores", "melondsds_libretro.so"),
);
const romPath = firstExisting(
  process.env.ROM_PATH,
  join(rebitRoot, "cloud-game", "assets", "games", "nds", "Mario-Kart-DS-USA.nds"),
  join(rebitRoot, "cloud-game", "assets", "games", "nds", "blocksds-local-multiplayer.nds"),
  join(root, "roms", "demo.nds"),
  config.romPath,
);
const runnerBin = firstExisting(
  process.env.RUNNER_BIN,
  "/opt/melonds/melonds-lan-room-runner",
  join(root, "build", "melonds-lan-room-runner"),
);
const runnerScript = join(root, "run-lan2-demo.sh");

const mime = new Map([
  [".html", "text/html; charset=utf-8"],
  [".js", "text/javascript; charset=utf-8"],
  [".css", "text/css; charset=utf-8"],
  [".json", "application/json; charset=utf-8"],
  [".bmp", "image/bmp"],
  [".svg", "image/svg+xml"],
]);

let runner = null;
let runnerState = "stopped";
let runnerExit = null;
const logs = [];

const appendLog = (chunk) => {
  const text = chunk.toString();
  for (const line of text.split(/\r?\n/)) {
    if (!line) continue;
    logs.push({time: new Date().toISOString(), line});
  }
  while (logs.length > 500) logs.shift();
  process.stderr.write(text);
};

const slotDir = (slot) => join(runtimePath, `player-${slot}`);
const framePath = (slot) => join(slotDir(slot), "latest.bmp");
const inputPath = (slot) => join(slotDir(slot), "input.bin");

const writeInput = (slot, input) => {
  mkdirSync(slotDir(slot), {recursive: true});
  const buffer = Buffer.alloc(18);
  buffer.writeUInt32LE(0x504e4952, 0);
  buffer.writeUInt16LE(Number(input.buttons || 0) & 0xffff, 4);
  buffer.writeInt16LE(Math.max(-32768, Math.min(32767, Number(input.pointerX || 0))), 6);
  buffer.writeInt16LE(Math.max(-32768, Math.min(32767, Number(input.pointerY || 0))), 8);
  buffer.writeUInt8(input.pointerPressed ? 1 : 0, 10);
  writeFileSync(inputPath(slot), buffer);
};

const sendJson = (res, code, data) => {
  const body = JSON.stringify(data);
  res.writeHead(code, {
    "content-type": "application/json; charset=utf-8",
    "cache-control": "no-store",
    "content-length": Buffer.byteLength(body),
  });
  res.end(body);
};

const readBody = async (req) => new Promise((resolve, reject) => {
  const chunks = [];
  req.on("data", (chunk) => chunks.push(chunk));
  req.on("end", () => {
    try {
      const raw = Buffer.concat(chunks).toString("utf8") || "{}";
      resolve(JSON.parse(raw));
    } catch (error) {
      reject(error);
    }
  });
  req.on("error", reject);
});

const processRunning = () => runner && runner.exitCode === null && runner.signalCode === null && !runner.killed;

const roomStatus = () => ({
  roomId: config.roomId || "demo",
  title: config.title || "melonDS LAN demo",
  players: config.slots.slice(0, players).map((slot) => ({
    ...slot,
    state: processRunning() ? "running" : runnerState,
    connected: existsSync(framePath(slot.slot)),
    framePath: framePath(slot.slot),
  })),
  rom: {
    path: romPath,
    ready: existsSync(romPath),
  },
  core: {
    path: corePath,
    ready: existsSync(corePath),
  },
  runner: {
    path: existsSync(runnerBin) ? runnerBin : runnerScript,
    ready: existsSync(runnerBin) || existsSync(runnerScript),
    state: runnerState,
    running: processRunning(),
    exit: runnerExit,
  },
  runtimePath,
  state: processRunning() ? "running" : runnerState,
});

const startRunner = () => {
  if (processRunning()) return {ok: true, state: "running"};
  if (!existsSync(romPath)) return {ok: false, code: 409, error: "rom-missing", romPath};
  if (!existsSync(corePath)) return {ok: false, code: 409, error: "core-missing", corePath};
  mkdirSync(runtimePath, {recursive: true});
  for (let slot = 1; slot <= players; slot++) {
    writeInput(slot, {});
  }

  const env = {
    ...process.env,
    CORE_PATH: corePath,
    ROM_PATH: romPath,
    RUNTIME_DIR: runtimePath,
    PLAYERS: String(players),
    FRAMES: "0",
    MELONDS_RUNNER_NO_INPUT: "1",
    MELONDS_RUNNER_NO_PROBE: "1",
  };

  const useBinary = existsSync(runnerBin);
  const command = useBinary ? runnerBin : runnerScript;
  const args = useBinary
    ? ["--core", corePath, "--rom", romPath, "--runtime", runtimePath, "--players", String(players), "--frames", "0"]
    : [];

  runnerExit = null;
  runnerState = "starting";
  runner = spawn(command, args, {
    cwd: root,
    env,
    detached: true,
    stdio: ["ignore", "pipe", "pipe"],
  });
  runner.stdout.on("data", appendLog);
  runner.stderr.on("data", appendLog);
  runner.on("spawn", () => {
    runnerState = "running";
    appendLog(`[server] runner started pid=${runner.pid}\n`);
  });
  runner.on("exit", (code, signal) => {
    runnerExit = {code, signal};
    runnerState = code === 0 || signal === "SIGTERM" ? "stopped" : "error";
    appendLog(`[server] runner exited code=${code} signal=${signal || ""}\n`);
  });
  runner.on("error", (error) => {
    runnerState = "error";
    runnerExit = {error: error.message};
    appendLog(`[server] runner error ${error.message}\n`);
  });

  return {ok: true, state: "starting"};
};

const stopRunner = () => {
  if (!processRunning()) {
    runnerState = "stopped";
    return {ok: true, state: "stopped"};
  }
  try {
    process.kill(-runner.pid, "SIGTERM");
  } catch {
    runner.kill("SIGTERM");
  }
  runnerState = "stopping";
  return {ok: true, state: "stopping"};
};

const serveFrame = (slot, res) => {
  const file = framePath(slot);
  if (!existsSync(file)) {
    res.writeHead(404, {"cache-control": "no-store"});
    res.end("frame not ready");
    return;
  }
  const stat = statSync(file);
  res.writeHead(200, {
    "content-type": "image/bmp",
    "cache-control": "no-store, no-cache, must-revalidate",
    "content-length": stat.size,
  });
  createReadStream(file).pipe(res);
};

const serveStatic = (req, res) => {
  const url = new URL(req.url, "http://localhost");
  const pathname = url.pathname === "/" ? "/index.html" : url.pathname;
  const safe = normalize(pathname).replace(/^(\.\.(\/|\\|$))+/, "");
  const file = join(webRoot, safe);

  if (!file.startsWith(webRoot)) {
    res.writeHead(403);
    res.end();
    return;
  }
  if (!existsSync(file)) {
    res.writeHead(404);
    res.end("Not found");
    return;
  }

  res.writeHead(200, {"content-type": mime.get(extname(file)) || "application/octet-stream"});
  createReadStream(file).pipe(res);
};

const server = createServer(async (req, res) => {
  try {
    const url = new URL(req.url, "http://localhost");

    if (req.method === "GET" && url.pathname === "/api/health") {
      sendJson(res, 200, {ok: true});
      return;
    }

    if (req.method === "GET" && url.pathname === "/api/room") {
      sendJson(res, 200, roomStatus());
      return;
    }

    if (req.method === "GET" && url.pathname === "/api/logs") {
      sendJson(res, 200, {logs});
      return;
    }

    if (req.method === "POST" && url.pathname === "/api/room/start") {
      const result = startRunner();
      sendJson(res, result.code || 202, result);
      return;
    }

    if (req.method === "POST" && url.pathname === "/api/room/stop") {
      sendJson(res, 202, stopRunner());
      return;
    }

    const frameMatch = url.pathname.match(/^\/frame\/([1-4])$/);
    if (req.method === "GET" && frameMatch) {
      serveFrame(Number(frameMatch[1]), res);
      return;
    }

    const inputMatch = url.pathname.match(/^\/api\/input\/([1-4])$/);
    if (req.method === "POST" && inputMatch) {
      const slot = Number(inputMatch[1]);
      writeInput(slot, await readBody(req));
      sendJson(res, 200, {ok: true, slot});
      return;
    }

    serveStatic(req, res);
  } catch (error) {
    sendJson(res, 500, {ok: false, error: error.message});
  }
});

const wss = new WebSocketServer({server, path: "/signal"});
wss.on("connection", (ws, req) => {
  const url = new URL(req.url, "http://localhost");
  const slot = Number(url.searchParams.get("slot") || 0);
  ws.send(JSON.stringify({type: "status", slot, state: roomStatus().state}));
  ws.on("message", async (message) => {
    try {
      const data = JSON.parse(message.toString());
      if (data.type === "input" && slot >= 1 && slot <= players) {
        writeInput(slot, data);
        ws.send(JSON.stringify({type: "input", ok: true, slot}));
      }
    } catch (error) {
      ws.send(JSON.stringify({type: "error", error: error.message}));
    }
  });
});

server.listen(port, "0.0.0.0", () => {
  console.log(`melonDS playable room listening on http://0.0.0.0:${port}`);
  console.log(`ROM: ${romPath}`);
  console.log(`core: ${corePath}`);
});

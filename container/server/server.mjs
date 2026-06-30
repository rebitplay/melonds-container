import { createReadStream, existsSync, readFileSync } from "node:fs";
import { createServer } from "node:http";
import { extname, join, normalize } from "node:path";
import { fileURLToPath } from "node:url";
import { WebSocketServer } from "ws";

const root = join(fileURLToPath(new URL(".", import.meta.url)), "..");
const webRoot = join(root, "web");
const port = Number(process.env.PORT || 8787);
const romPath = process.env.ROM_PATH || join(root, "roms", "demo.nds");
const defaultConfigPath = join(root, "room-config.json");
const fallbackConfigPath = join(root, "room-config.example.json");
const configPath = process.env.ROOM_CONFIG || (existsSync(defaultConfigPath) ? defaultConfigPath : fallbackConfigPath);
const runnerSocket = process.env.RUNNER_SOCKET || "/run/rebit-room.sock";

const mime = new Map([
  [".html", "text/html; charset=utf-8"],
  [".js", "text/javascript; charset=utf-8"],
  [".css", "text/css; charset=utf-8"],
  [".json", "application/json; charset=utf-8"],
  [".svg", "image/svg+xml"],
]);

const readConfig = () => {
  const raw = readFileSync(configPath, "utf8");
  const config = JSON.parse(raw);
  config.romPath = romPath;
  return config;
};

const sendJson = (res, code, data) => {
  const body = JSON.stringify(data);
  res.writeHead(code, {
    "content-type": "application/json; charset=utf-8",
    "content-length": Buffer.byteLength(body),
  });
  res.end(body);
};

const roomStatus = () => {
  const config = readConfig();
  const romReady = existsSync(romPath);
  const runnerReady = existsSync(runnerSocket);
  return {
    roomId: config.roomId,
    title: config.title,
    players: config.slots.map((slot) => ({
      ...slot,
      state: runnerReady ? "waiting" : "runner-missing",
      connected: false,
    })),
    rom: {
      path: romPath,
      ready: romReady,
    },
    runner: {
      socket: runnerSocket,
      ready: runnerReady,
    },
    state: runnerReady ? "ready" : "runner-missing",
  };
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

const server = createServer((req, res) => {
  if (req.method === "GET" && req.url === "/api/health") {
    sendJson(res, 200, {ok: true});
    return;
  }

  if (req.method === "GET" && req.url === "/api/room") {
    sendJson(res, 200, roomStatus());
    return;
  }

  if (req.method === "POST" && req.url === "/api/room/start") {
    const status = roomStatus();
    if (!status.rom.ready) {
      sendJson(res, 409, {
        ok: false,
        error: "rom-missing",
        romPath,
      });
      return;
    }
    if (!status.runner.ready) {
      sendJson(res, 501, {
        ok: false,
        error: "runner-not-implemented",
        contract: "/runner-contract.md",
      });
      return;
    }
    sendJson(res, 202, {ok: true, state: "starting"});
    return;
  }

  serveStatic(req, res);
});

const wss = new WebSocketServer({server, path: "/signal"});
wss.on("connection", (ws, req) => {
  const url = new URL(req.url, "http://localhost");
  const slot = Number(url.searchParams.get("slot") || 0);
  ws.send(JSON.stringify({
    type: "status",
    slot,
    state: existsSync(runnerSocket) ? "runner-ready" : "runner-missing",
  }));
  ws.on("message", (message) => {
    ws.send(JSON.stringify({
      type: "error",
      slot,
      error: "runner-not-implemented",
      echo: JSON.parse(message.toString()),
    }));
  });
});

server.listen(port, "0.0.0.0", () => {
  console.log(`melonDS room demo listening on http://0.0.0.0:${port}`);
});

const http = require("http");
const fs = require("fs");
const path = require("path");
const { spawn } = require("child_process");

const root = path.resolve(__dirname, "public");
const port = Number(process.env.UC_DASHBOARD_PORT || 3100);

const clients = new Set();
const lanes = new Map();

function defaultLanes() {
  return [
    { id: "direct", name: "Maintenance PC", host: "192.168.1.100", port: 22, login: "sdk_system", kind: "direct" },
    { id: "esp1", name: "ESP32 #1", host: "192.168.11.188", port: 2222, login: "sdk_system", kind: "esp", espUrl: "http://192.168.11.188" },
    { id: "esp2", name: "ESP32 #2", host: "192.168.11.189", port: 2222, login: "sdk_system", kind: "esp", espUrl: "http://192.168.11.189" },
    { id: "esp3", name: "ESP32 #3", host: "192.168.11.190", port: 2222, login: "sdk_system", kind: "esp", espUrl: "http://192.168.11.190" },
  ];
}

for (const lane of defaultLanes()) {
  lanes.set(lane.id, { ...lane, connected: false, state: "idle", buffer: "", process: null });
}

function sendEvent(res, event, data) {
  res.write(`event: ${event}\n`);
  res.write(`data: ${JSON.stringify(data)}\n\n`);
}

function broadcast(event, data) {
  for (const res of clients) sendEvent(res, event, data);
}

function publicLane(lane) {
  const { process, password, buffer, ...safe } = lane;
  return safe;
}

function allPublicLanes() {
  return Array.from(lanes.values()).map(publicLane);
}

function stripAnsi(text) {
  return text
    .replace(/\x1bc/g, "\n")
    .replace(/\x1b\[[0-?]*[ -/]*[@-~]/g, "")
    .replace(/\x1b[=>]/g, "");
}

function normalizeText(text) {
  return stripAnsi(text)
    .normalize("NFKD")
    .replace(/[\u0300-\u036f]/g, "")
    .toLowerCase();
}

function recognizeState(buffer, previous) {
  const text = normalizeText(buffer);
  if (/fatal error|unexpectedly closed|connection refused|timed out|network error/.test(text)) return "error";
  if (/password:|login as:|using username/.test(text) && !/root menu/.test(text)) return "login";
  if (/installation/.test(text) && /usb/.test(text)) return "usb_install";
  if (/root menu > system/.test(text) || /gestion systeme/.test(text)) return "system_menu";
  if (/root menu > pfe/.test(text) || /plateforme pfe/.test(text)) return "pfe_menu";
  if (/root menu > device/.test(text) || /gestion de l.equipement/.test(text)) return "device_menu";
  if (/version/.test(text) && /1\.9\.4|display|pfe/.test(text)) return "version_check";
  if (/detection card|detection carte/.test(text)) return "card_detection";
  if (/system card/.test(text)) return "system_card";
  if (/printer|papier/.test(text)) return "printer_check";
  if (/root menu/.test(text)) return "root_menu";
  return previous || "unknown";
}

function appendOutput(lane, stream, chunk) {
  const data = chunk.toString("utf8");
  const clearSeen = /\x1bc|\x1b\[2J|\x1b\[H/.test(data);
  if (clearSeen) lane.buffer = "";
  lane.buffer = (lane.buffer + data).slice(-16000);
  lane.state = recognizeState(lane.buffer, lane.state);
  broadcast("output", { laneId: lane.id, stream, data, clear: clearSeen, state: lane.state });
  broadcast("lanes", allPublicLanes());
}

function startLane(id, password) {
  const lane = lanes.get(id);
  if (!lane) throw new Error(`Unknown lane ${id}`);
  if (lane.process && !lane.process.killed) return;

  const args = ["-ssh", "-t", "-P", String(lane.port || 22), "-l", lane.login || "sdk_system"];
  if (password) args.push("-pw", password);
  args.push(lane.host);

  lane.buffer = "";
  lane.state = "connecting";
  lane.connected = false;
  lane.password = password || "";
  const proc = spawn("plink.exe", args, {
    stdio: ["pipe", "pipe", "pipe"],
    windowsHide: true,
    env: { ...process.env, TERM: "xterm-256color", LANG: "fr_FR.UTF-8" },
  });
  lane.process = proc;

  appendOutput(lane, "local", Buffer.from(`[local] opening ${lane.name} via ${lane.host}:${lane.port} as ${lane.login}\r\n`));

  proc.stdout.on("data", (chunk) => appendOutput(lane, "stdout", chunk));
  proc.stderr.on("data", (chunk) => appendOutput(lane, "stderr", chunk));
  proc.on("spawn", () => {
    lane.connected = true;
    lane.state = "connecting";
    broadcast("lanes", allPublicLanes());
  });
  proc.on("exit", (code) => {
    lane.connected = false;
    lane.process = null;
    lane.state = code === 0 ? "disconnected" : "error";
    appendOutput(lane, "local", Buffer.from(`[local] disconnected, exit code ${code}\r\n`));
    broadcast("lanes", allPublicLanes());
  });
}

function stopLane(id) {
  const lane = lanes.get(id);
  if (!lane) throw new Error(`Unknown lane ${id}`);
  if (lane.process) {
    lane.process.stdin.write("exit\r\n");
    setTimeout(() => {
      if (lane.process) lane.process.kill();
    }, 250);
  }
}

function sendLine(id, line) {
  const lane = lanes.get(id);
  if (!lane) throw new Error(`Unknown lane ${id}`);
  if (!lane.process) throw new Error(`${lane.name} is not connected`);
  lane.process.stdin.write(`${line}\r\n`);
  appendOutput(lane, "local", Buffer.from(`\r\n> ${line}\r\n`));
}

function sendRawInput(id, data) {
  const lane = lanes.get(id);
  if (!lane) throw new Error(`Unknown lane ${id}`);
  if (!lane.process) throw new Error(`${lane.name} is not connected`);
  lane.process.stdin.write(data);
}

function setEspMode(lane, mode) {
  return new Promise((resolve, reject) => {
    if (!lane.espUrl) return reject(new Error(`${lane.name} has no ESP URL`));
    const url = new URL(`/mode/${mode}`, lane.espUrl);
    const req = http.request(url, { method: "POST", timeout: 5000 }, (res) => {
      let body = "";
      res.on("data", (chunk) => (body += chunk));
      res.on("end", () => resolve({ statusCode: res.statusCode, body }));
    });
    req.on("timeout", () => req.destroy(new Error("ESP mode request timed out")));
    req.on("error", reject);
    req.end();
  });
}

function readBody(req) {
  return new Promise((resolve, reject) => {
    let body = "";
    req.on("data", (chunk) => {
      body += chunk;
      if (body.length > 1024 * 1024) req.destroy();
    });
    req.on("end", () => {
      try {
        resolve(body ? JSON.parse(body) : {});
      } catch (err) {
        reject(err);
      }
    });
    req.on("error", reject);
  });
}

function serveStatic(req, res) {
  const url = new URL(req.url, "http://localhost");
  const filePath = path.join(root, url.pathname === "/" ? "index.html" : url.pathname);
  if (!filePath.startsWith(root)) {
    res.writeHead(403);
    res.end("Forbidden");
    return;
  }
  fs.readFile(filePath, (err, data) => {
    if (err) {
      res.writeHead(404);
      res.end("Not found");
      return;
    }
    const ext = path.extname(filePath);
    const type = ext === ".js" ? "text/javascript" : ext === ".css" ? "text/css" : "text/html";
    res.writeHead(200, { "content-type": `${type}; charset=utf-8` });
    res.end(data);
  });
}

async function handleApi(req, res) {
  try {
    const url = new URL(req.url, "http://localhost");
    if (req.method === "GET" && url.pathname === "/events") {
      res.writeHead(200, {
        "content-type": "text/event-stream",
        "cache-control": "no-cache",
        connection: "keep-alive",
      });
      clients.add(res);
      sendEvent(res, "lanes", allPublicLanes());
      req.on("close", () => clients.delete(res));
      return;
    }

    if (req.method === "GET" && url.pathname === "/api/lanes") {
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify(allPublicLanes()));
      return;
    }

    if (req.method === "POST" && url.pathname === "/api/config") {
      const body = await readBody(req);
      for (const incoming of body.lanes || []) {
        const current = lanes.get(incoming.id) || { id: incoming.id };
        lanes.set(incoming.id, {
          ...current,
          ...incoming,
          port: Number(incoming.port || current.port || 22),
          process: current.process || null,
          connected: Boolean(current.process),
          buffer: current.buffer || "",
          state: current.state || "idle",
        });
      }
      broadcast("lanes", allPublicLanes());
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ ok: true, lanes: allPublicLanes() }));
      return;
    }

    if (req.method === "POST" && url.pathname === "/api/connect") {
      const body = await readBody(req);
      const ids = body.laneIds || [];
      for (const id of ids) startLane(id, body.password || "");
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ ok: true }));
      return;
    }

    if (req.method === "POST" && url.pathname === "/api/disconnect") {
      const body = await readBody(req);
      for (const id of body.laneIds || []) stopLane(id);
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ ok: true }));
      return;
    }

    if (req.method === "POST" && url.pathname === "/api/send") {
      const body = await readBody(req);
      for (const id of body.laneIds || []) sendLine(id, body.line || "");
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ ok: true }));
      return;
    }

    if (req.method === "POST" && url.pathname === "/api/input") {
      const body = await readBody(req);
      sendRawInput(body.laneId, body.data || "");
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ ok: true }));
      return;
    }

    if (req.method === "POST" && url.pathname === "/api/esp-mode") {
      const body = await readBody(req);
      const results = [];
      for (const id of body.laneIds || []) {
        const lane = lanes.get(id);
        if (!lane) throw new Error(`Unknown lane ${id}`);
        results.push({ id, ...(await setEspMode(lane, body.mode)) });
      }
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ ok: true, results }));
      return;
    }

    res.writeHead(404);
    res.end("Not found");
  } catch (err) {
    res.writeHead(500, { "content-type": "application/json" });
    res.end(JSON.stringify({ ok: false, error: err.message }));
  }
}

const server = http.createServer((req, res) => {
  if (req.url === "/events" || req.url.startsWith("/api/")) {
    handleApi(req, res);
  } else {
    serveStatic(req, res);
  }
});

server.listen(port, "127.0.0.1", () => {
  console.log(`UC cockpit running at http://127.0.0.1:${port}`);
});

process.on("SIGINT", () => {
  for (const lane of lanes.values()) {
    if (lane.process) lane.process.kill();
  }
  process.exit(0);
});

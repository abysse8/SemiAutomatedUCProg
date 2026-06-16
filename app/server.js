const http = require("http");
const fs = require("fs");
const net = require("net");
const path = require("path");
const { spawn, spawnSync } = require("child_process");

const root = path.resolve(__dirname, "public");
const port = Number(process.env.UC_DASHBOARD_PORT || 3100);
const host = process.env.UC_DASHBOARD_HOST || "0.0.0.0";
const logsDir = path.resolve(__dirname, "..", "logs");
fs.mkdirSync(logsDir, { recursive: true });
const auditFile = path.join(logsDir, `uc-audit-${new Date().toISOString().replace(/[:.]/g, "-")}.jsonl`);

const clients = new Set();
const lanes = new Map();
const simulatedEspModes = new Map([
  ["esp1", { mode: "OFF", usb_active: false, ssh_port: 2222, uc_host: "192.168.1.100" }],
  ["esp2", { mode: "OFF", usb_active: false, ssh_port: 2222, uc_host: "192.168.1.100" }],
  ["esp3", { mode: "OFF", usb_active: false, ssh_port: 2222, uc_host: "192.168.1.100" }],
]);

function appendAudit(type, payload) {
  const entry = {
    ts: new Date().toISOString(),
    type,
    ...payload,
  };
  fs.appendFile(auditFile, JSON.stringify(entry) + "\n", () => {});
}

function commandExists(command) {
  const result = spawnSync(process.platform === "win32" ? "where.exe" : "which", [command], { encoding: "utf8" });
  return result.status === 0;
}

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
  appendAudit("output", { laneId: lane.id, laneName: lane.name, stream, data, state: lane.state });
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
  appendAudit("connect", { laneId: lane.id, laneName: lane.name, host: lane.host, port: lane.port, login: lane.login });

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
    appendAudit("disconnect", { laneId: lane.id, laneName: lane.name });
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
  appendAudit("command", { laneId: lane.id, laneName: lane.name, line });
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

function getEspMode(lane) {
  return new Promise((resolve, reject) => {
    if (!lane.espUrl) return reject(new Error(`${lane.name} has no ESP URL`));
    const url = new URL("/mode", lane.espUrl);
    const req = http.request(url, { method: "GET", timeout: 5000 }, (res) => {
      let body = "";
      res.on("data", (chunk) => (body += chunk));
      res.on("end", () => {
        try {
          resolve({ statusCode: res.statusCode, body, json: JSON.parse(body) });
        } catch {
          resolve({ statusCode: res.statusCode, body });
        }
      });
    });
    req.on("timeout", () => req.destroy(new Error("ESP status request timed out")));
    req.on("error", reject);
    req.end();
  });
}

function ipToNumber(ip) {
  const parts = ip.split(".").map((part) => Number(part));
  if (parts.length !== 4 || parts.some((part) => !Number.isInteger(part) || part < 0 || part > 255)) {
    throw new Error(`Invalid IPv4 address: ${ip}`);
  }
  return (((parts[0] << 24) >>> 0) + (parts[1] << 16) + (parts[2] << 8) + parts[3]) >>> 0;
}

function numberToIp(value) {
  return [
    (value >>> 24) & 255,
    (value >>> 16) & 255,
    (value >>> 8) & 255,
    value & 255,
  ].join(".");
}

function expandCidr(cidr) {
  const [ip, bitsText] = cidr.split("/");
  const bits = bitsText === undefined ? 24 : Number(bitsText);
  if (!Number.isInteger(bits) || bits < 24 || bits > 32) {
    throw new Error("Scanner supports /24 through /32 ranges");
  }
  const base = ipToNumber(ip);
  const mask = bits === 0 ? 0 : (0xffffffff << (32 - bits)) >>> 0;
  const network = base & mask;
  const count = 2 ** (32 - bits);
  if (count > 256) throw new Error("Scanner range is too large; use /24 or smaller");
  const first = count <= 2 ? network : network + 1;
  const last = count <= 2 ? network + count - 1 : network + count - 2;
  const hosts = [];
  for (let value = first; value <= last; value++) hosts.push(numberToIp(value >>> 0));
  return hosts;
}

function probeEsp(ip, timeoutMs) {
  return new Promise((resolve) => {
    const started = Date.now();
    const req = http.request(`http://${ip}/mode`, { method: "GET", timeout: timeoutMs }, (res) => {
      let body = "";
      res.on("data", (chunk) => (body += chunk));
      res.on("end", () => {
        try {
          const json = JSON.parse(body);
          resolve({
            ip,
            espUrl: `http://${ip}`,
            sshHost: ip,
            sshPort: json.ssh_port || 2222,
            ok: true,
            mode: json.mode,
            usb_active: json.usb_active,
            uc_host: json.uc_host,
            latency_ms: Date.now() - started,
          });
        } catch {
          resolve(null);
        }
      });
    });
    req.on("timeout", () => req.destroy());
    req.on("error", () => resolve(null));
    req.end();
  });
}

async function scanForEsp(cidr, timeoutMs = 700) {
  const hosts = expandCidr(cidr);
  const results = [];
  const concurrency = 32;
  let index = 0;
  async function worker() {
    while (index < hosts.length) {
      const host = hosts[index++];
      const result = await probeEsp(host, timeoutMs);
      if (result) results.push(result);
    }
  }
  await Promise.all(Array.from({ length: concurrency }, worker));
  results.sort((a, b) => ipToNumber(a.ip) - ipToNumber(b.ip));
  appendAudit("scan", { cidr, count: results.length, results });
  return results;
}

function runCommand(command, args, timeoutMs = 15000) {
  return new Promise((resolve) => {
    const child = spawn(command, args, { windowsHide: true });
    let stdout = "";
    let stderr = "";
    const timer = setTimeout(() => child.kill(), timeoutMs);
    child.stdout.on("data", (chunk) => (stdout += chunk.toString("utf8")));
    child.stderr.on("data", (chunk) => (stderr += chunk.toString("utf8")));
    child.on("error", (err) => {
      clearTimeout(timer);
      resolve({ status: -1, stdout, stderr: err.message });
    });
    child.on("close", (status) => {
      clearTimeout(timer);
      resolve({ status, stdout, stderr });
    });
  });
}

function parseArpTable(text) {
  const macByIp = new Map();
  for (const line of text.split(/\r?\n/)) {
    const match = line.match(/(\d+\.\d+\.\d+\.\d+)\s+([0-9a-fA-F-]{17})\s+/);
    if (match) {
      macByIp.set(match[1], match[2].replace(/-/g, ":").toUpperCase());
    }
  }
  return macByIp;
}

function ouiHint(mac) {
  if (!mac) return "";
  const prefix = mac.slice(0, 8).toUpperCase();
  const hints = {
    "30:ED:A0": "Espressif",
    "24:0A:C4": "Espressif",
    "7C:DF:A1": "Espressif",
    "AC:67:B2": "Espressif",
    "C8:F0:9E": "Espressif",
    "E8:DB:84": "Espressif",
  };
  return hints[prefix] || "";
}

function pingHost(ip, timeoutMs) {
  return new Promise((resolve) => {
    const args = process.platform === "win32"
      ? ["-n", "1", "-w", String(timeoutMs), ip]
      : ["-c", "1", "-W", String(Math.ceil(timeoutMs / 1000)), ip];
    const child = spawn("ping", args, { windowsHide: true });
    let output = "";
    const timer = setTimeout(() => child.kill(), timeoutMs + 500);
    child.stdout.on("data", (chunk) => (output += chunk.toString("utf8")));
    child.on("error", () => {
      clearTimeout(timer);
      resolve(false);
    });
    child.on("close", (status) => {
      clearTimeout(timer);
      resolve(status === 0 || /TTL=/i.test(output));
    });
  });
}

function tcpProbe(ip, portNumber, timeoutMs) {
  return new Promise((resolve) => {
    const socket = new net.Socket();
    let done = false;
    function finish(open) {
      if (done) return;
      done = true;
      socket.destroy();
      resolve(open);
    }
    socket.setTimeout(timeoutMs);
    socket.once("connect", () => finish(true));
    socket.once("timeout", () => finish(false));
    socket.once("error", () => finish(false));
    socket.connect(portNumber, ip);
  });
}

function serviceName(portNumber) {
  const names = {
    22: "ssh",
    80: "http",
    443: "https",
    502: "modbus",
    2222: "esp-ssh-proxy",
    8080: "http-alt",
    9100: "printer",
  };
  return names[portNumber] || `tcp/${portNumber}`;
}

async function enrichEsp(ip, timeoutMs) {
  const result = await probeEsp(ip, timeoutMs);
  return result || null;
}

function classifyNetworkHost(hostInfo) {
  if (hostInfo.esp) return "ESP32 UC proxy";
  if (hostInfo.openPorts.includes(2222)) return "ESP32 candidate";
  if (hostInfo.openPorts.includes(22) && hostInfo.ip.endsWith(".100")) return "UC candidate";
  if (hostInfo.openPorts.includes(9100)) return "printer/peripheral";
  if (hostInfo.openPorts.includes(502)) return "industrial device";
  if (hostInfo.openPorts.includes(80) || hostInfo.openPorts.includes(443) || hostInfo.openPorts.includes(8080)) return "web device";
  if (hostInfo.alive) return "network host";
  return "unknown";
}

function parseNmapXml(xml) {
  const hosts = [];
  const hostBlocks = xml.match(/<host\b[\s\S]*?<\/host>/g) || [];
  for (const block of hostBlocks) {
    if (!/<status state="up"/.test(block)) continue;
    const ip = block.match(/<address addr="([^"]+)" addrtype="ipv4"/)?.[1];
    if (!ip) continue;
    const mac = block.match(/<address addr="([^"]+)" addrtype="mac"/)?.[1] || "";
    const openPorts = [];
    for (const portMatch of block.matchAll(/<port protocol="tcp" portid="(\d+)">[\s\S]*?<state state="open"/g)) {
      openPorts.push(Number(portMatch[1]));
    }
    hosts.push({ ip, mac, alive: true, openPorts, services: openPorts.map(serviceName), scanner: "nmap" });
  }
  return hosts;
}

async function scanWithNmap(cidr, ports) {
  const nmap = commandExists("nmap.exe") ? "nmap.exe" : commandExists("nmap") ? "nmap" : null;
  if (!nmap) return null;
  const result = await runCommand(nmap, ["-oX", "-", "-T4", "-p", ports.join(","), "--open", cidr], 45000);
  if (result.status !== 0 && !result.stdout) {
    throw new Error(`nmap failed: ${result.stderr || result.status}`);
  }
  return parseNmapXml(result.stdout);
}

async function scanWithFallback(cidr, ports, timeoutMs) {
  const hosts = expandCidr(cidr);
  const results = [];
  const concurrency = 48;
  let index = 0;

  async function inspectHost(ip) {
    const openPorts = [];
    const tcpChecks = ports.map(async (portNumber) => {
      if (await tcpProbe(ip, portNumber, Math.min(timeoutMs, 600))) {
        openPorts.push(portNumber);
      }
    });
    const pingPromise = pingHost(ip, timeoutMs);
    await Promise.all(tcpChecks);
    const alive = openPorts.length > 0 || await pingPromise;
    if (!alive) return null;
    return { ip, mac: "", alive, openPorts: openPorts.sort((a, b) => a - b), services: openPorts.map(serviceName), scanner: "fallback" };
  }

  async function worker() {
    while (index < hosts.length) {
      const hostIp = hosts[index++];
      const result = await inspectHost(hostIp);
      if (result) results.push(result);
    }
  }

  await Promise.all(Array.from({ length: concurrency }, worker));
  const arp = await runCommand("arp", ["-a"], 5000);
  const macByIp = parseArpTable(arp.stdout);
  for (const result of results) {
    result.mac = macByIp.get(result.ip) || "";
  }
  return results;
}

async function scanNetwork(cidr, options = {}) {
  const ports = options.ports || [22, 80, 443, 502, 2222, 8080, 9100];
  const timeoutMs = Number(options.timeoutMs || 800);
  let hosts = await scanWithNmap(cidr, ports);
  let scanner = "nmap";
  if (!hosts) {
    scanner = "fallback";
    hosts = await scanWithFallback(cidr, ports, timeoutMs);
  }

  await Promise.all(hosts.map(async (hostInfo) => {
    hostInfo.vendor = ouiHint(hostInfo.mac);
    hostInfo.esp = await enrichEsp(hostInfo.ip, timeoutMs);
    if (hostInfo.esp && !hostInfo.openPorts.includes(hostInfo.esp.sshPort)) {
      hostInfo.openPorts.push(hostInfo.esp.sshPort);
      hostInfo.openPorts.sort((a, b) => a - b);
      hostInfo.services = hostInfo.openPorts.map(serviceName);
    }
    hostInfo.kind = classifyNetworkHost(hostInfo);
  }));

  hosts.sort((a, b) => ipToNumber(a.ip) - ipToNumber(b.ip));
  const result = {
    scannedAt: new Date().toISOString(),
    cidr,
    scanner,
    count: hosts.length,
    hosts,
  };
  appendAudit("network_scan", result);
  return result;
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
    const simMatch = url.pathname.match(/^\/sim\/([^/]+)\/mode(?:\/(off|m0|m1))?$/);
    if (simMatch) {
      const [, simId, nextMode] = simMatch;
      if (!simulatedEspModes.has(simId)) {
        simulatedEspModes.set(simId, { mode: "OFF", usb_active: false, ssh_port: 2222, uc_host: "192.168.1.100" });
      }
      const state = simulatedEspModes.get(simId);
      if (req.method === "POST" && nextMode) {
        state.mode = nextMode.toUpperCase();
        state.usb_active = nextMode !== "off";
        res.writeHead(200, { "content-type": "text/plain" });
        res.end(`Simulated ESP ${simId} saved USB mode ${state.mode}. Rebooting...\n`);
        return;
      }
      if (req.method === "GET" && !nextMode) {
        res.writeHead(200, { "content-type": "application/json" });
        res.end(JSON.stringify(state) + "\n");
        return;
      }
      res.writeHead(405);
      res.end("Method not allowed");
      return;
    }

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

    if (req.method === "GET" && url.pathname === "/api/audit") {
      res.writeHead(200, { "content-type": "text/plain; charset=utf-8" });
      fs.createReadStream(auditFile).on("error", () => res.end("")).pipe(res);
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
        const result = { id, ...(await setEspMode(lane, body.mode)) };
        appendOutput(lane, "local", Buffer.from(`[local] ESP mode ${body.mode.toUpperCase()} request sent: ${result.body || result.statusCode}\r\n`));
        results.push(result);
      }
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ ok: true, results }));
      return;
    }

    if (req.method === "POST" && url.pathname === "/api/esp-status") {
      const body = await readBody(req);
      const results = [];
      for (const id of body.laneIds || []) {
        const lane = lanes.get(id);
        if (!lane) throw new Error(`Unknown lane ${id}`);
        const result = { id, ...(await getEspMode(lane)) };
        appendOutput(lane, "local", Buffer.from(`[local] ESP status: ${result.body || result.statusCode}\r\n`));
        results.push(result);
      }
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ ok: true, results }));
      return;
    }

    if (req.method === "POST" && url.pathname === "/api/scan") {
      const body = await readBody(req);
      const results = await scanForEsp(body.cidr || "192.168.11.0/24", Number(body.timeoutMs || 700));
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ ok: true, results, auditFile }));
      return;
    }

    if (req.method === "POST" && url.pathname === "/api/network-scan") {
      const body = await readBody(req);
      const result = await scanNetwork(body.cidr || "192.168.11.0/24", {
        timeoutMs: Number(body.timeoutMs || 800),
      });
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ ok: true, ...result, auditFile }));
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
  if (req.url === "/events" || req.url.startsWith("/api/") || req.url.startsWith("/sim/")) {
    handleApi(req, res);
  } else {
    serveStatic(req, res);
  }
});

server.listen(port, host, () => {
  console.log(`UC cockpit running at http://${host}:${port}`);
});

process.on("SIGINT", () => {
  for (const lane of lanes.values()) {
    if (lane.process) lane.process.kill();
  }
  process.exit(0);
});

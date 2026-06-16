const lanesEl = document.querySelector("#lanes");
const template = document.querySelector("#laneTemplate");
const passwordEl = document.querySelector("#password");
const commandEl = document.querySelector("#command");
const readySummaryEl = document.querySelector("#readySummary");
const scanCidrEl = document.querySelector("#scanCidr");
const scanStatusEl = document.querySelector("#scanStatus");
const networkSummaryEl = document.querySelector("#networkSummary");
const networkRowsEl = document.querySelector("#networkRows");

const laneViews = new Map();
let lanes = [];
let lastScanResults = [];
let repeatScanTimer = null;
let repeatScanEnabled = false;

const savedPassword = sessionStorage.getItem("ucCockpitPassword");
if (savedPassword) passwordEl.value = savedPassword;
passwordEl.addEventListener("input", () => {
  sessionStorage.setItem("ucCockpitPassword", passwordEl.value);
});

function api(path, body) {
  return fetch(path, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(body || {}),
  }).then(async (res) => {
    const data = await res.json().catch(() => ({}));
    if (!res.ok || data.ok === false) throw new Error(data.error || res.statusText);
    return data;
  });
}

function stateClass(state) {
  if (["root_menu", "system_menu", "usb_install", "pfe_menu", "device_menu", "version_check"].includes(state)) return "ready";
  if (["connecting", "login", "update_running", "unknown"].includes(state)) return "wait";
  if (["error", "disconnected"].includes(state)) return "error";
  return "";
}

function createTerminal(container) {
  const terminal = new Terminal({
    cursorBlink: true,
    convertEol: true,
    fontFamily: 'Consolas, "Cascadia Mono", monospace',
    fontSize: 13,
    lineHeight: 1.15,
    scrollback: 3000,
    theme: {
      background: "#050606",
      foreground: "#d6f5d6",
      cursor: "#f7faf8",
      selectionBackground: "#3a5f55",
      black: "#050606",
      red: "#e06c75",
      green: "#98c379",
      yellow: "#d19a66",
      blue: "#61afef",
      magenta: "#c678dd",
      cyan: "#56b6c2",
      white: "#dcdfe4",
      brightBlack: "#5c6370",
      brightRed: "#e06c75",
      brightGreen: "#98c379",
      brightYellow: "#e5c07b",
      brightBlue: "#61afef",
      brightMagenta: "#c678dd",
      brightCyan: "#56b6c2",
      brightWhite: "#ffffff",
    },
  });
  const fitAddon = new FitAddon.FitAddon();
  terminal.loadAddon(fitAddon);
  terminal.open(container);
  requestAnimationFrame(() => fitAddon.fit());
  return { terminal, fitAddon };
}

function writeLocal(view, text) {
  view.terminal.writeln(`\x1b[2m${text}\x1b[0m`);
}

function renderLane(lane) {
  let view = laneViews.get(lane.id);
  if (!view) {
    const node = template.content.firstElementChild.cloneNode(true);
    const terminalEl = node.querySelector(".terminal");
    const xterm = createTerminal(terminalEl);
    view = {
      node,
      name: node.querySelector(".lane-name"),
      state: node.querySelector(".lane-state"),
      ready: node.querySelector(".lane-ready"),
      host: node.querySelector(".lane-host"),
      port: node.querySelector(".lane-port"),
      login: node.querySelector(".lane-login"),
      esp: node.querySelector(".lane-esp"),
      terminalEl,
      terminal: xterm.terminal,
      fitAddon: xterm.fitAddon,
      connect: node.querySelector(".connect-one"),
      send: node.querySelector(".send-one"),
      clear: node.querySelector(".clear-one"),
      off: node.querySelector(".mode-off"),
      m1: node.querySelector(".mode-m1"),
      m0: node.querySelector(".mode-m0"),
      check: node.querySelector(".mode-check"),
    };
    view.connect.addEventListener("click", () => connectLanes([lane.id]));
    view.send.addEventListener("click", () => sendLine([lane.id]));
    view.clear.addEventListener("click", () => clearViews([lane.id]));
    view.off.addEventListener("click", () => setMode([lane.id], "off"));
    view.m1.addEventListener("click", () => setMode([lane.id], "m1"));
    view.m0.addEventListener("click", () => setMode([lane.id], "m0"));
    view.check.addEventListener("click", () => checkEsp([lane.id]));
    view.terminal.onData((data) => sendRawInput(lane.id, data));
    view.ready.addEventListener("change", updateReadySummary);
    laneViews.set(lane.id, view);
    lanesEl.appendChild(node);
    requestAnimationFrame(() => view.fitAddon.fit());
  }

  view.name.textContent = lane.name;
  view.state.textContent = lane.state || "idle";
  view.state.className = `lane-state ${stateClass(lane.state)}`;
  if (document.activeElement !== view.host) view.host.value = lane.host || "";
  if (document.activeElement !== view.port) view.port.value = lane.port || "";
  if (document.activeElement !== view.login) view.login.value = lane.login || "sdk_system";
  if (document.activeElement !== view.esp) view.esp.value = lane.espUrl || "";
  view.connect.textContent = lane.connected ? "Reconnect" : "Connect";
  view.off.disabled = lane.kind !== "esp";
  view.m0.disabled = lane.kind !== "esp";
  view.m1.disabled = lane.kind !== "esp";
  view.check.disabled = lane.kind !== "esp";
}

function updateLanes(nextLanes) {
  lanes = nextLanes;
  for (const lane of lanes) renderLane(lane);
  updateReadySummary();
  fitAll();
}

function laneConfigFromUi() {
  return lanes.map((lane) => {
    const view = laneViews.get(lane.id);
    return {
      id: lane.id,
      name: lane.name,
      kind: lane.kind,
      host: view?.host.value.trim() || lane.host,
      port: Number(view?.port.value || lane.port),
      login: view?.login.value.trim() || lane.login || "sdk_system",
      espUrl: view?.esp.value.trim() || lane.espUrl || "",
    };
  });
}

function selectedReadyLaneIds() {
  return lanes.filter((lane) => laneViews.get(lane.id)?.ready.checked).map((lane) => lane.id);
}

function updateReadySummary() {
  const ready = selectedReadyLaneIds().length;
  readySummaryEl.textContent = `${ready} ready`;
}

function fitAll() {
  for (const view of laneViews.values()) {
    try {
      view.fitAddon.fit();
    } catch {
    }
  }
}

function clearViews(ids) {
  for (const id of ids) {
    const view = laneViews.get(id);
    if (view) {
      view.terminal.clear();
      writeLocal(view, "[local] view cleared; UC session state was not changed");
    }
  }
  commandEl.focus();
}

async function saveConfig() {
  await api("/api/config", { lanes: laneConfigFromUi() });
}

async function connectLanes(ids) {
  await saveConfig();
  await api("/api/connect", { laneIds: ids, password: passwordEl.value });
}

async function disconnectAll() {
  await api("/api/disconnect", { laneIds: lanes.map((lane) => lane.id) });
}

async function sendLine(ids) {
  const line = commandEl.value;
  if (!line) return;
  await api("/api/send", { laneIds: ids, line });
  commandEl.select();
  commandEl.focus();
}

async function sendRawInput(id, data) {
  try {
    await api("/api/input", { laneId: id, data });
  } catch (err) {
    const view = laneViews.get(id);
    if (view) writeLocal(view, `[local] ${err.message}`);
  }
}

async function setMode(ids, mode) {
  if (mode !== "off" && !confirm(`Expose ${mode.toUpperCase()} over USB for ${ids.join(", ")}?`)) return;
  if (mode === "off" && !confirm(`Switch USB exposure OFF for ${ids.join(", ")}?`)) return;
  await saveConfig();
  const result = await api("/api/esp-mode", { laneIds: ids, mode });
  for (const item of result.results || []) {
    const view = laneViews.get(item.id);
    if (view) writeLocal(view, `[local] ${item.body?.trim() || `ESP replied ${item.statusCode}`}`);
  }
}

async function checkEsp(ids) {
  await saveConfig();
  const result = await api("/api/esp-status", { laneIds: ids });
  for (const item of result.results || []) {
    const view = laneViews.get(item.id);
    if (view) writeLocal(view, `[local] ESP status ${item.body?.trim() || item.statusCode}`);
  }
}

function loadSimulatorLanes() {
  const origin = window.location.origin;
  for (const lane of lanes) {
    const view = laneViews.get(lane.id);
    if (!view || lane.kind !== "esp") continue;
    view.esp.value = `${origin}/sim/${lane.id}`;
  }
  saveConfig();
}

async function scanEspDevices() {
  scanStatusEl.textContent = "Scanning network...";
  try {
    const result = await api("/api/network-scan", { cidr: scanCidrEl.value.trim(), timeoutMs: 800 });
    lastScanResults = (result.hosts || []).filter((host) => host.esp);
    renderNetworkRows(result);
    if (lastScanResults.length === 0) {
      scanStatusEl.textContent = `${result.count || 0} hosts, no ESP proxies`;
      return;
    }
    scanStatusEl.textContent = `${result.count} hosts, ${lastScanResults.length} ESP proxy candidate(s), scanner: ${result.scanner}`;
  } catch (err) {
    scanStatusEl.textContent = err.message;
  }
}

function renderNetworkRows(result) {
  networkSummaryEl.textContent = `${result.count || 0} host(s), ${result.scanner || "unknown"} scan, ${result.scannedAt || ""}`;
  const hosts = result.hosts || [];
  if (hosts.length === 0) {
    networkRowsEl.innerHTML = "<tr><td colspan=\"6\">No equipment found.</td></tr>";
    return;
  }
  networkRowsEl.innerHTML = "";
  for (const host of hosts) {
    const row = document.createElement("tr");
    const espStatus = host.esp
      ? `${host.esp.mode || "?"}${host.esp.usb_active ? " / USB active" : ""}`
      : "";
    row.innerHTML = `
      <td><code>${host.ip}</code></td>
      <td>${host.kind || "unknown"}</td>
      <td>${host.mac || ""}${host.vendor ? `<br><span>${host.vendor}</span>` : ""}</td>
      <td>${(host.services || []).join(", ")}</td>
      <td>${espStatus}</td>
      <td></td>
    `;
    const actionCell = row.lastElementChild;
    if (host.esp) {
      const button = document.createElement("button");
      button.textContent = "Use ESP";
      button.addEventListener("click", () => applyHostToNextEspLane(host));
      actionCell.appendChild(button);
    }
    networkRowsEl.appendChild(row);
  }
}

function applyScanResults() {
  const espLanes = lanes.filter((lane) => lane.kind === "esp");
  for (let i = 0; i < espLanes.length && i < lastScanResults.length; i++) {
    const lane = espLanes[i];
    const found = lastScanResults[i];
    const view = laneViews.get(lane.id);
    if (!view) continue;
    applyHostToLaneView(view, found);
  }
  saveConfig();
}

function applyHostToLaneView(view, host) {
  const esp = host.esp || host;
  view.host.value = esp.sshHost || host.ip;
  view.port.value = esp.sshPort || 2222;
  view.esp.value = esp.espUrl || `http://${host.ip}`;
}

function applyHostToNextEspLane(host) {
  const espLanes = lanes.filter((lane) => lane.kind === "esp");
  const targetLane = espLanes.find((lane) => {
    const view = laneViews.get(lane.id);
    return view && (!view.esp.value || view.esp.value.includes("192.168.11.18"));
  }) || espLanes[0];
  const view = laneViews.get(targetLane.id);
  if (view) {
    applyHostToLaneView(view, host);
    saveConfig();
  }
}

function toggleRepeatScan() {
  repeatScanEnabled = !repeatScanEnabled;
  document.querySelector("#repeatScan").textContent = repeatScanEnabled ? "Repeat On" : "Repeat Off";
  if (repeatScanTimer) {
    clearInterval(repeatScanTimer);
    repeatScanTimer = null;
  }
  if (repeatScanEnabled) {
    scanEspDevices();
    repeatScanTimer = setInterval(scanEspDevices, 15000);
  }
}

document.querySelector("#saveConfig").addEventListener("click", saveConfig);
document.querySelector("#connectAll").addEventListener("click", () => connectLanes(lanes.map((lane) => lane.id)));
document.querySelector("#disconnectAll").addEventListener("click", disconnectAll);
document.querySelector("#clearAll").addEventListener("click", () => clearViews(lanes.map((lane) => lane.id)));
document.querySelector("#loadSim").addEventListener("click", loadSimulatorLanes);
document.querySelector("#scanNetwork").addEventListener("click", scanEspDevices);
document.querySelector("#repeatScan").addEventListener("click", toggleRepeatScan);
document.querySelector("#applyScan").addEventListener("click", applyScanResults);
document.querySelector("#sendAll").addEventListener("click", () => sendLine(lanes.map((lane) => lane.id)));
document.querySelector("#sendReady").addEventListener("click", () => sendLine(selectedReadyLaneIds()));
commandEl.addEventListener("keydown", (event) => {
  if (event.key === "Enter") {
    event.preventDefault();
    sendLine(selectedReadyLaneIds());
  }
});
window.addEventListener("resize", fitAll);

const events = new EventSource("/events");
events.addEventListener("lanes", (event) => updateLanes(JSON.parse(event.data)));
events.addEventListener("output", (event) => {
  const msg = JSON.parse(event.data);
  const view = laneViews.get(msg.laneId);
  if (!view) return;
  if (msg.clear) view.terminal.clear();
  if (msg.stream === "stderr" && msg.data.startsWith("Using username")) {
    writeLocal(view, msg.data.trimEnd());
  } else {
    view.terminal.write(msg.data);
  }
});

fetch("/api/lanes").then((res) => res.json()).then(updateLanes);

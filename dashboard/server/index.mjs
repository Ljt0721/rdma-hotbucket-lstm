import { spawn } from "node:child_process";
import { createReadStream, existsSync, mkdirSync } from "node:fs";
import { createServer } from "node:http";
import path from "node:path";
import { fileURLToPath } from "node:url";

const dashboardRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const projectRoot = path.resolve(dashboardRoot, "..");
const distRoot = path.join(dashboardRoot, "dist");
const port = Number(process.env.DASHBOARD_PORT || 8787);

const executableNames = process.platform === "win32"
  ? ["hotbucket_sim.exe"]
  : ["hotbucket_sim"];
const executableCandidates = [
  process.env.SIMULATOR_PATH,
  ...executableNames.map((name) => path.join(projectRoot, "build", name)),
  ...executableNames.map((name) => path.join(projectRoot, "build-release", name)),
].filter(Boolean);

function simulatorPath() {
  return executableCandidates.find((candidate) => existsSync(candidate));
}

function integerParam(params, name, fallback, minimum, maximum) {
  const parsed = Number.parseInt(params.get(name) ?? "", 10);
  if (!Number.isFinite(parsed)) return fallback;
  return Math.min(maximum, Math.max(minimum, parsed));
}

function decimalParam(params, name, fallback, minimum, maximum) {
  const parsed = Number.parseFloat(params.get(name) ?? "");
  if (!Number.isFinite(parsed)) return fallback;
  return Math.min(maximum, Math.max(minimum, parsed));
}

function sendEvent(response, payload) {
  response.write(`data: ${JSON.stringify(payload)}\n\n`);
}

function startSimulation(request, response, url) {
  const executable = simulatorPath();
  response.writeHead(200, {
    "Content-Type": "text/event-stream",
    "Cache-Control": "no-cache, no-transform",
    Connection: "keep-alive",
    "X-Accel-Buffering": "no",
  });

  if (!executable) {
    sendEvent(response, {
      type: "error",
      message: "Simulator executable was not found. Build it before starting the dashboard.",
    });
    response.end();
    return;
  }

  const params = url.searchParams;
  const allowedPolicies = new Set(["no-action", "reactive", "recent-window"]);
  const requestedPolicy = params.get("policy") || "recent-window";
  const policy = allowedPolicies.has(requestedPolicy) ? requestedPolicy : "recent-window";
  const nodes = integerParam(params, "nodes", 4, 2, 16);
  const buckets = integerParam(params, "buckets", 64, 8, 512);
  const windows = integerParam(params, "windows", 40, 5, 200);
  const requests = integerParam(params, "requests", 4000, 100, 1000000);
  const nodeCapacity = integerParam(params, "nodeCapacity", 1800, 100, 1000000);
  const hotspotDuration = integerParam(params, "hotspotDuration", 5, 1, 100);
  const threshold = integerParam(params, "threshold", 1200, 1, 1000000);
  const delay = integerParam(params, "delay", 250, 0, 3000);
  const seed = integerParam(params, "seed", 42, 0, 4294967295);
  const readRatio = decimalParam(params, "readRatio", 0.9, 0, 1);
  const hotspotShare = decimalParam(params, "hotspotShare", 0.65, 0.01, 1);

  const resultDirectory = path.join(projectRoot, "results", "dashboard");
  mkdirSync(resultDirectory, { recursive: true });
  const outputPath = path.join(
    resultDirectory,
    `${policy}-${Date.now()}-seed-${seed}.csv`,
  );
  const relativeOutputPath = path.relative(projectRoot, outputPath);

  const args = [
    "--policy", policy,
    "--nodes", String(nodes),
    "--buckets", String(buckets),
    "--windows", String(windows),
    "--requests", String(requests),
    "--node-capacity", String(nodeCapacity),
    "--hotspot-duration", String(hotspotDuration),
    "--threshold", String(threshold),
    "--read-ratio", String(readRatio),
    "--hotspot-share", String(hotspotShare),
    "--seed", String(seed),
    "--window-delay-ms", String(delay),
    "--stream-json",
    "--output", relativeOutputPath,
  ];

  const child = spawn(executable, args, {
    cwd: projectRoot,
    windowsHide: true,
    stdio: ["ignore", "pipe", "pipe"],
  });
  let stdoutBuffer = "";
  let stderrBuffer = "";
  let completed = false;

  sendEvent(response, {
    type: "started",
    policy,
    nodes,
    buckets,
    windows,
    output: relativeOutputPath,
  });

  child.stdout.setEncoding("utf8");
  child.stdout.on("data", (chunk) => {
    stdoutBuffer += chunk;
    const lines = stdoutBuffer.split(/\r?\n/);
    stdoutBuffer = lines.pop() || "";
    for (const line of lines) {
      if (!line.trim()) continue;
      try {
        const payload = JSON.parse(line);
        if (payload.type === "complete") completed = true;
        sendEvent(response, payload);
      } catch {
        sendEvent(response, { type: "log", message: line });
      }
    }
  });

  child.stderr.setEncoding("utf8");
  child.stderr.on("data", (chunk) => {
    stderrBuffer += chunk;
  });

  child.on("error", (error) => {
    sendEvent(response, { type: "error", message: error.message });
    response.end();
  });

  child.on("close", (code) => {
    if (code !== 0) {
      sendEvent(response, {
        type: "error",
        message: stderrBuffer.trim() || `Simulator exited with code ${code}.`,
      });
    } else if (!completed) {
      sendEvent(response, { type: "complete", policy, windows });
    }
    response.end();
  });

  request.on("close", () => {
    if (child.exitCode === null) child.kill();
  });
}

const mimeTypes = {
  ".css": "text/css; charset=utf-8",
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".json": "application/json; charset=utf-8",
  ".svg": "image/svg+xml",
};

function serveStatic(response, pathname) {
  if (!existsSync(distRoot)) {
    response.writeHead(404, { "Content-Type": "application/json" });
    response.end(JSON.stringify({ error: "Dashboard build not found. Run npm run build." }));
    return;
  }

  const relativePath = pathname === "/" ? "index.html" : pathname.replace(/^\/+/, "");
  let filePath = path.resolve(distRoot, relativePath);
  if (!filePath.startsWith(distRoot) || !existsSync(filePath)) {
    filePath = path.join(distRoot, "index.html");
  }
  response.writeHead(200, {
    "Content-Type": mimeTypes[path.extname(filePath)] || "application/octet-stream",
  });
  createReadStream(filePath).pipe(response);
}

const server = createServer((request, response) => {
  const url = new URL(request.url || "/", `http://${request.headers.host || "localhost"}`);
  if (request.method === "GET" && url.pathname === "/api/health") {
    response.writeHead(200, { "Content-Type": "application/json" });
    response.end(JSON.stringify({ ok: true, simulator: simulatorPath() || null }));
    return;
  }
  if (request.method === "GET" && url.pathname === "/api/simulate") {
    startSimulation(request, response, url);
    return;
  }
  serveStatic(response, url.pathname);
});

server.listen(port, "127.0.0.1", () => {
  console.log(`Simulation API: http://127.0.0.1:${port}`);
});

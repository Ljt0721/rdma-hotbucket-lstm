import { execFile, execFileSync, spawn } from "node:child_process";
import { randomUUID } from "node:crypto";
import {
  appendFileSync,
  createReadStream,
  existsSync,
  mkdirSync,
  readFileSync,
  renameSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import { createServer } from "node:http";
import path from "node:path";
import { fileURLToPath } from "node:url";

const dashboardRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const projectRoot = path.resolve(dashboardRoot, "..");
const distRoot = path.join(dashboardRoot, "dist");
const port = Number(process.env.DASHBOARD_PORT || 8787);
const logDirectory = path.join(projectRoot, "results", "dashboard", "logs");
const logPath = path.join(logDirectory, "dashboard-server.jsonl");
const entityStorePath = process.env.ENTITY_STORE_PATH
  ? path.resolve(projectRoot, process.env.ENTITY_STORE_PATH)
  : path.join(projectRoot, "data", "entity-store", "entities.hbkv");
const entityStoreInitialCount = Math.min(
  100000,
  Math.max(2, Number.parseInt(process.env.ENTITY_STORE_INITIAL_COUNT || "10", 10) || 10),
);
mkdirSync(logDirectory, { recursive: true });

const executableNames = process.platform === "win32"
  ? ["hotbucket_sim.exe"]
  : ["hotbucket_sim"];
const executableCandidates = [
  process.env.SIMULATOR_PATH,
  ...executableNames.map((name) => path.join(projectRoot, "build", name)),
  ...executableNames.map((name) => path.join(projectRoot, "build-release", name)),
].filter(Boolean);

const sshExecutable = process.env.SSH_PATH || "ssh";
const scpExecutable = process.env.SCP_PATH || "scp";
const pythonExecutable = process.env.PYTHON_PATH || "python";
const lstmConfig = {
  checkpointPath: process.env.LSTM_CHECKPOINT_PATH
    || path.join(projectRoot, "models", "lstm", "conditional-hotspot", "hotbucket_lstm.pt"),
  minimumProbability: Number(process.env.LSTM_MIN_PROBABILITY || 0.50),
  minimumPredictedGetRatio: Number(process.env.LSTM_MIN_PREDICTED_GET_RATIO || 0.0),
  inferenceCostMs: Number(process.env.LSTM_INFERENCE_COST_MS || 0.9567),
};
const rdmaConfig = {
  memoryHost: process.env.RDMA_MEMORY_HOST || "rdma-memory",
  clientHost: process.env.RDMA_CLIENT_HOST || "rdma-client",
  memoryIp: process.env.RDMA_MEMORY_IP || "192.168.64.128",
  remoteRoot: process.env.RDMA_REMOTE_ROOT || "/home/ljt/rdma-hotbucket-lab",
  buildDirectory: process.env.RDMA_REMOTE_BUILD || "build-test",
  basePort: Number(process.env.RDMA_BASE_PORT || 7600),
  calibrationPath: process.env.RDMA_CALIBRATION_PATH
    || path.join(projectRoot, "results", "rdma-calibration", "summary.json"),
  calibrationPercentile: process.env.RDMA_CALIBRATION_PERCENTILE || "p50",
};

function remoteBinary(name) {
  return `${rdmaConfig.remoteRoot}/${rdmaConfig.buildDirectory}/${name}`;
}

function inspectRdmaCalibration() {
  try {
    const report = JSON.parse(readFileSync(rdmaConfig.calibrationPath, "utf8"));
    const modelName = `${rdmaConfig.calibrationPercentile}_end_to_end`;
    const model = report.cost_models?.[modelName];
    const bandwidth = Number(model?.copy_bandwidth_mib_per_ms);
    const fixedCost = Number(model?.fixed_cost_ms);
    if (!(bandwidth > 0) || !(fixedCost >= 0)) {
      throw new Error(`No usable ${modelName} model.`);
    }
    return {
      ok: true,
      percentile: rdmaConfig.calibrationPercentile,
      copyBandwidthMibPerMs: bandwidth,
      fixedCostMs: fixedCost,
      source: path.relative(projectRoot, rdmaConfig.calibrationPath),
    };
  } catch (error) {
    return {
      ok: false,
      error: `${rdmaConfig.calibrationPath}: ${error.message}`,
    };
  }
}

function sshArgs(host, command) {
  return [
    "-o", "BatchMode=yes",
    "-o", "ConnectTimeout=5",
    "-o", "ServerAliveInterval=5",
    "-o", "ServerAliveCountMax=2",
    host,
    command,
  ];
}

function runCommand(executable, args, options = {}) {
  return new Promise((resolve, reject) => {
    execFile(executable, args, {
      windowsHide: true,
      timeout: 10000,
      maxBuffer: 1024 * 1024,
      ...options,
    }, (error, stdout, stderr) => {
      if (error) {
        reject(new Error(stderr.trim() || stdout.trim() || error.message));
        return;
      }
      resolve({ stdout: stdout.trim(), stderr: stderr.trim() });
    });
  });
}

async function inspectRdmaHost(role, host, requiredBinary) {
  const command = [
    `test -x ${requiredBinary}`,
    "command -v rdma >/dev/null",
    "rdma link show",
  ].join(" && ");
  try {
    const result = await runCommand(sshExecutable, sshArgs(host, command));
    return {
      role,
      host,
      ok: true,
      provider: result.stdout || "RDMA link available",
    };
  } catch (error) {
    return { role, host, ok: false, error: error.message };
  }
}

async function inspectRdmaEnvironment() {
  const [memory, client] = await Promise.all([
    inspectRdmaHost(
      "memory",
      rdmaConfig.memoryHost,
      remoteBinary("hotbucket_rdma_replica"),
    ),
    inspectRdmaHost(
      "compute",
      rdmaConfig.clientHost,
      remoteBinary("hotbucket_rdma_sim"),
    ),
  ]);
  const calibration = inspectRdmaCalibration();
  return {
    ok: memory.ok && client.ok && calibration.ok,
    memory,
    client,
    calibration,
    memoryIp: rdmaConfig.memoryIp,
    basePort: rdmaConfig.basePort,
    buildDirectory: rdmaConfig.buildDirectory,
  };
}

function simulatorPath() {
  return executableCandidates.find((candidate) => existsSync(candidate));
}

function ensureEntityStore() {
  if (existsSync(entityStorePath)) return;
  const executable = simulatorPath();
  if (!executable) {
    throw new Error("Cannot initialize the entity store before the simulator is built.");
  }
  mkdirSync(path.dirname(entityStorePath), { recursive: true });
  execFileSync(executable, [
    "--entities", String(entityStoreInitialCount),
    "--initialize-entity-store", entityStorePath,
  ], { cwd: projectRoot, windowsHide: true });
}

function normalizeEntity(input, forcedId = null) {
  const id = forcedId ?? Number(input?.id ?? input?.entityId);
  if (!Number.isSafeInteger(id) || id <= 0) {
    throw new Error("EntityID must be a positive safe integer.");
  }
  const callsign = String(input?.callsign ?? "").trim();
  const entityType = String(input?.entityType ?? input?.type ?? "").trim();
  if (!callsign || !entityType || callsign.length > 128 || entityType.length > 128) {
    throw new Error("Callsign and entity type are required and limited to 128 characters.");
  }

  const rawAttributes = Array.isArray(input?.attributes)
    ? input.attributes
    : Object.entries(input?.attributes || {}).map(([name, attribute]) => ({
        name,
        valueType: attribute?.valueType ?? attribute?.type ?? "String",
        value: attribute?.value ?? attribute,
      }));
  if (!rawAttributes.length || rawAttributes.length > 128) {
    throw new Error("An entity requires between 1 and 128 attributes.");
  }
  const names = new Set();
  const attributes = rawAttributes.map((attribute) => {
    const name = String(attribute?.name ?? "").trim();
    const valueType = String(attribute?.valueType ?? attribute?.type ?? "String").trim();
    const value = String(attribute?.value ?? "");
    if (!name || !valueType || name.length > 128 || valueType.length > 64 || value.length > 65535) {
      throw new Error("Attribute keys, types, and values must be non-empty and within limits.");
    }
    if (names.has(name)) throw new Error(`Duplicate attribute key: ${name}`);
    names.add(name);
    return { name, valueType, value };
  });
  return { id, callsign, entityType, attributes };
}

function encodeLengthString(value) {
  const text = Buffer.from(value, "utf8");
  const length = Buffer.allocUnsafe(4);
  length.writeUInt32LE(text.length);
  return Buffer.concat([length, text]);
}

function encodeEntity(entity) {
  const id = Buffer.allocUnsafe(8);
  id.writeBigUInt64LE(BigInt(entity.id));
  const attributeCount = Buffer.allocUnsafe(4);
  attributeCount.writeUInt32LE(entity.attributes.length);
  const fields = [
    id,
    encodeLengthString(entity.callsign),
    encodeLengthString(entity.entityType),
    attributeCount,
  ];
  for (const attribute of entity.attributes) {
    fields.push(
      encodeLengthString(attribute.name),
      encodeLengthString(attribute.valueType),
      encodeLengthString(attribute.value),
    );
  }
  return Buffer.concat(fields);
}

function readUInt32(buffer, state) {
  if (state.offset + 4 > buffer.length) throw new Error("HBKV record ends inside uint32.");
  const value = buffer.readUInt32LE(state.offset);
  state.offset += 4;
  return value;
}

function readUInt64(buffer, state) {
  if (state.offset + 8 > buffer.length) throw new Error("HBKV record ends inside uint64.");
  const value = buffer.readBigUInt64LE(state.offset);
  state.offset += 8;
  if (value > BigInt(Number.MAX_SAFE_INTEGER)) throw new Error("EntityID exceeds JavaScript's safe range.");
  return Number(value);
}

function readLengthString(buffer, state) {
  const length = readUInt32(buffer, state);
  if (state.offset + length > buffer.length) throw new Error("HBKV string exceeds its record.");
  const value = buffer.toString("utf8", state.offset, state.offset + length);
  state.offset += length;
  return value;
}

function decodeEntity(payload) {
  const state = { offset: 0 };
  const entity = {
    id: readUInt64(payload, state),
    callsign: readLengthString(payload, state),
    entityType: readLengthString(payload, state),
    attributes: [],
  };
  const attributeCount = readUInt32(payload, state);
  for (let index = 0; index < attributeCount; index += 1) {
    const name = readLengthString(payload, state);
    const valueType = readLengthString(payload, state);
    const value = readLengthString(payload, state);
    entity.attributes.push({
      name,
      valueType,
      value,
      valueBytes: Buffer.byteLength(value, "utf8"),
    });
  }
  if (state.offset !== payload.length) throw new Error("HBKV entity contains trailing bytes.");
  return { ...entity, serializedBytes: payload.length };
}

function readEntityStore() {
  ensureEntityStore();
  const encoded = readFileSync(entityStorePath);
  if (encoded.length < 12 || encoded.toString("ascii", 0, 4) !== "HBKV") {
    throw new Error("Entity store has an invalid HBKV header.");
  }
  const state = { offset: 4 };
  const version = readUInt32(encoded, state);
  const count = readUInt32(encoded, state);
  if (version !== 1) throw new Error(`Unsupported HBKV version: ${version}`);
  const entities = [];
  const keys = new Set();
  for (let index = 0; index < count; index += 1) {
    const key = readUInt64(encoded, state);
    const length = readUInt32(encoded, state);
    if (state.offset + length > encoded.length) throw new Error("HBKV value exceeds the store file.");
    const entity = decodeEntity(encoded.subarray(state.offset, state.offset + length));
    state.offset += length;
    if (entity.id !== key || keys.has(key)) throw new Error("HBKV contains a mismatched or duplicate key.");
    keys.add(key);
    entities.push(entity);
  }
  if (state.offset !== encoded.length) throw new Error("HBKV store contains trailing bytes.");
  return entities.sort((left, right) => left.id - right.id);
}

function writeEntityStore(entities) {
  const normalized = entities.map((entity) => normalizeEntity(entity));
  const keys = new Set();
  const records = normalized.sort((left, right) => left.id - right.id).map((entity) => {
    if (keys.has(entity.id)) throw new Error(`Duplicate EntityID: ${entity.id}`);
    keys.add(entity.id);
    const value = encodeEntity(entity);
    const header = Buffer.allocUnsafe(12);
    header.writeBigUInt64LE(BigInt(entity.id), 0);
    header.writeUInt32LE(value.length, 8);
    return Buffer.concat([header, value]);
  });
  const header = Buffer.allocUnsafe(12);
  header.write("HBKV", 0, 4, "ascii");
  header.writeUInt32LE(1, 4);
  header.writeUInt32LE(records.length, 8);
  mkdirSync(path.dirname(entityStorePath), { recursive: true });
  const temporary = `${entityStorePath}.${randomUUID()}.tmp`;
  try {
    writeFileSync(temporary, Buffer.concat([header, ...records]));
    renameSync(temporary, entityStorePath);
  } finally {
    if (existsSync(temporary)) rmSync(temporary, { force: true });
  }
  return normalized;
}

function publicEntity(entity) {
  return {
    ...entity,
    bucketId: entity.id % 64,
    attributeMap: Object.fromEntries(
      entity.attributes.map((attribute) => [attribute.name, {
        type: attribute.valueType,
        value: attribute.value,
      }]),
    ),
  };
}

function readJsonBody(request, maximumBytes = 1024 * 1024) {
  return new Promise((resolve, reject) => {
    let body = "";
    request.setEncoding("utf8");
    request.on("data", (chunk) => {
      body += chunk;
      if (Buffer.byteLength(body, "utf8") > maximumBytes) {
        reject(new Error("Request body is too large."));
        request.destroy();
      }
    });
    request.on("end", () => {
      try {
        resolve(body ? JSON.parse(body) : {});
      } catch {
        reject(new Error("Request body must be valid JSON."));
      }
    });
    request.on("error", reject);
  });
}

function sendJson(response, status, payload) {
  response.writeHead(status, { "Content-Type": "application/json; charset=utf-8" });
  response.end(JSON.stringify(payload));
}

async function handleEntityApi(request, response, url) {
  const match = url.pathname.match(/^\/api\/kv\/entities(?:\/(\d+))?$/);
  if (!match) return false;
  try {
    const entities = readEntityStore();
    const requestedId = match[1] ? Number(match[1]) : null;
    if (match[1] && (!Number.isSafeInteger(requestedId) || requestedId <= 0)) {
      sendJson(response, 400, { error: "EntityID must be a positive safe integer." });
      return true;
    }

    if (request.method === "GET" && requestedId === null) {
      const offset = integerParam(url.searchParams, "offset", 0, 0, entities.length);
      const limit = integerParam(url.searchParams, "limit", 50, 1, 500);
      sendJson(response, 200, {
        durable: true,
        format: "HBKV v1",
        store: path.relative(projectRoot, entityStorePath),
        count: entities.length,
        offset,
        limit,
        hasMore: offset + limit < entities.length,
        entities: entities.slice(offset, offset + limit).map(publicEntity),
      });
      return true;
    }
    if (request.method === "GET") {
      const entity = entities.find((candidate) => candidate.id === requestedId);
      sendJson(response, entity ? 200 : 404, entity
        ? { durable: true, store: path.relative(projectRoot, entityStorePath), entity: publicEntity(entity) }
        : { error: `EntityID ${requestedId} was not found.` });
      return true;
    }
    if (request.method === "POST" && requestedId === null) {
      const entity = normalizeEntity(await readJsonBody(request));
      if (entities.some((candidate) => candidate.id === entity.id)) {
        sendJson(response, 409, { error: `EntityID ${entity.id} already exists.` });
        return true;
      }
      writeEntityStore([...entities, entity]);
      writeLog("INFO", "kv_entity_created", { entityId: entity.id, entityType: entity.entityType });
      sendJson(response, 201, { durable: true, entity: publicEntity({
        ...entity,
        serializedBytes: encodeEntity(entity).length,
      }) });
      return true;
    }
    if (request.method === "PUT" && requestedId !== null) {
      const entity = normalizeEntity(await readJsonBody(request), requestedId);
      const existingIndex = entities.findIndex((candidate) => candidate.id === requestedId);
      const updated = [...entities];
      if (existingIndex >= 0) updated[existingIndex] = entity;
      else updated.push(entity);
      writeEntityStore(updated);
      writeLog("INFO", existingIndex >= 0 ? "kv_entity_updated" : "kv_entity_created", {
        entityId: entity.id,
        entityType: entity.entityType,
      });
      sendJson(response, existingIndex >= 0 ? 200 : 201, { durable: true, entity: publicEntity({
        ...entity,
        serializedBytes: encodeEntity(entity).length,
      }) });
      return true;
    }
    if (request.method === "DELETE" && requestedId !== null) {
      if (!entities.some((candidate) => candidate.id === requestedId)) {
        sendJson(response, 404, { error: `EntityID ${requestedId} was not found.` });
        return true;
      }
      if (entities.length <= 2) {
        sendJson(response, 409, { error: "The simulator requires at least two stored entities." });
        return true;
      }
      writeEntityStore(entities.filter((candidate) => candidate.id !== requestedId));
      writeLog("INFO", "kv_entity_deleted", { entityId: requestedId });
      sendJson(response, 200, { durable: true, deletedEntityId: requestedId });
      return true;
    }
    sendJson(response, 405, { error: "Method not allowed for this entity-store endpoint." });
    return true;
  } catch (error) {
    writeLog("ERROR", "kv_entity_request_failed", { method: request.method, message: error.message });
    if (!response.writableEnded) sendJson(response, 400, { error: error.message });
    return true;
  }
}

function writeLog(level, event, details = {}) {
  const entry = {
    timestamp: new Date().toISOString(),
    level,
    event,
    ...details,
  };
  const line = JSON.stringify(entry);
  const output = level === "ERROR" ? console.error : level === "WARN" ? console.warn : console.log;
  output(line);
  try {
    appendFileSync(logPath, `${line}\n`, "utf8");
  } catch (error) {
    console.error(JSON.stringify({
      timestamp: new Date().toISOString(),
      level: "ERROR",
      event: "log_write_failed",
      message: error.message,
    }));
  }
  return entry;
}

function readRecentLogs(limit, runId) {
  if (!existsSync(logPath)) return [];
  const lines = readFileSync(logPath, "utf8").split(/\r?\n/).filter(Boolean);
  const selected = runId
    ? lines.filter((line) => {
        try {
          return JSON.parse(line).runId === runId;
        } catch {
          return false;
        }
      })
    : lines;
  return selected.slice(-limit).map((line) => {
    try {
      return JSON.parse(line);
    } catch {
      return { timestamp: null, level: "ERROR", event: "invalid_log_line", raw: line };
    }
  });
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
  if (!response.destroyed && !response.writableEnded) {
    response.write(`data: ${JSON.stringify(payload)}\n\n`);
  }
}

function placementSettings(url) {
  return {
    nodes: integerParam(url.searchParams, "nodes", 5, 2, 16),
    buckets: integerParam(url.searchParams, "buckets", 64, 8, 512),
    entities: integerParam(url.searchParams, "entities", 10, 2, 100000),
  };
}

function nodeMemoryBytes(nodes, entities) {
  return Math.max(2048, Math.ceil(entities / nodes) * 1024);
}

function simulationSettings(url) {
  const params = url.searchParams;
  const allowedPolicies = new Set(["no-action", "reactive", "recent-window", "lstm"]);
  const requestedPolicy = params.get("policy") || "recent-window";
  return {
    policy: allowedPolicies.has(requestedPolicy) ? requestedPolicy : "recent-window",
    nodes: integerParam(params, "nodes", 5, 2, 16),
    buckets: integerParam(params, "buckets", 64, 8, 512),
    entities: integerParam(params, "entities", 10, 2, 100000),
    windows: integerParam(params, "windows", 40, 5, 200),
    requests: integerParam(params, "requests", 4000, 100, 1000000),
    nodeCapacity: integerParam(params, "nodeCapacity", 1800, 100, 1000000),
    hotspotDuration: integerParam(params, "hotspotDuration", 5, 1, 100),
    threshold: integerParam(params, "threshold", 1200, 1, 1000000),
    delay: integerParam(params, "delay", 250, 0, 3000),
    seed: integerParam(params, "seed", 42, 0, 4294967295),
    readRatio: decimalParam(params, "readRatio", 0.9, 0, 1),
    hotspotShare: decimalParam(params, "hotspotShare", 0.65, 0.01, 1),
  };
}

async function prepareLstmPredictions(settings, runId, response) {
  const executable = simulatorPath();
  if (!executable) {
    throw new Error("The local simulator is required to prepare LSTM input windows.");
  }
  if (!existsSync(lstmConfig.checkpointPath)) {
    throw new Error(`LSTM checkpoint was not found: ${lstmConfig.checkpointPath}`);
  }
  ensureEntityStore();
  if (settings.windows < 7) {
    throw new Error("LSTM runs need at least seven windows: six observations and one target.");
  }

  const traceId = `dashboard-${runId}`;
  const runDirectory = path.join(projectRoot, "results", "dashboard", "lstm", runId);
  const rawDirectory = path.join(runDirectory, "raw");
  const tracePath = path.join(rawDirectory, `${traceId}.csv`);
  const predictionPath = path.join(runDirectory, "predictions.csv");
  const relativeTracePath = path.relative(projectRoot, tracePath);
  const relativePredictionPath = path.relative(projectRoot, predictionPath);
  mkdirSync(rawDirectory, { recursive: true });

  sendEvent(response, {
    type: "status",
    state: "predicting",
    message: "Building six-window LSTM input and predicting the next window",
  });
  await runCommand(executable, [
    "--trace-only",
    "--trace-id", traceId,
    "--entity-store", path.relative(projectRoot, entityStorePath),
    "--workload-pattern", "step",
    "--nodes", String(settings.nodes),
    "--buckets", String(settings.buckets),
    "--entities", String(settings.entities),
    "--node-memory-bytes", String(nodeMemoryBytes(settings.nodes, settings.entities)),
    "--windows", String(settings.windows),
    "--requests", String(settings.requests),
    "--node-capacity", String(settings.nodeCapacity),
    "--hotspot-duration", String(settings.hotspotDuration),
    "--threshold", String(settings.threshold),
    "--read-ratio", String(settings.readRatio),
    "--hotspot-share", String(settings.hotspotShare),
    "--seed", String(settings.seed),
    "--window-seconds", "30",
    "--output", relativeTracePath,
  ], { cwd: projectRoot, timeout: 60000, maxBuffer: 4 * 1024 * 1024 });

  await runCommand(pythonExecutable, [
    "-m", "ml.predict_raw_traces",
    "--checkpoint", lstmConfig.checkpointPath,
    "--raw-dir", rawDirectory,
    "--output", predictionPath,
    "--device", "cpu",
  ], { cwd: projectRoot, timeout: 60000, maxBuffer: 4 * 1024 * 1024 });

  writeLog("INFO", "lstm_predictions_prepared", {
    runId,
    traceId,
    checkpoint: path.relative(projectRoot, lstmConfig.checkpointPath),
    predictions: path.relative(projectRoot, predictionPath),
    sequenceLength: 6,
    minimumProbability: lstmConfig.minimumProbability,
  });
  return { traceId, predictionPath, relativePredictionPath };
}

function getPlacement(response, url) {
  const executable = simulatorPath();
  if (!executable) {
    response.writeHead(503, { "Content-Type": "application/json; charset=utf-8" });
    response.end(JSON.stringify({ error: "Simulator executable was not found." }));
    return;
  }
  try {
    ensureEntityStore();
  } catch (error) {
    sendJson(response, 503, { error: error.message });
    return;
  }

  const { nodes, buckets, entities } = placementSettings(url);
  const memoryBytes = nodeMemoryBytes(nodes, entities);
  const args = [
    "--nodes", String(nodes),
    "--buckets", String(buckets),
    "--entities", String(entities),
    "--entity-store", path.relative(projectRoot, entityStorePath),
    "--node-memory-bytes", String(memoryBytes),
    "--stream-json",
    "--placement-only",
  ];
  execFile(executable, args, {
    cwd: projectRoot,
    windowsHide: true,
    maxBuffer: 1024 * 1024,
  }, (error, stdout, stderr) => {
    if (error) {
      writeLog("ERROR", "placement_failed", {
        nodes,
        buckets,
        entities,
        message: stderr.trim() || error.message,
      });
      response.writeHead(500, { "Content-Type": "application/json; charset=utf-8" });
      response.end(JSON.stringify({ error: stderr.trim() || error.message }));
      return;
    }
    try {
      const placement = stdout.split(/\r?\n/)
        .filter(Boolean)
        .map((line) => JSON.parse(line))
        .find((payload) => payload.type === "placement");
      if (!placement) throw new Error("Simulator returned no placement snapshot.");
      response.writeHead(200, { "Content-Type": "application/json; charset=utf-8" });
      response.end(JSON.stringify(placement));
    } catch (parseError) {
      writeLog("ERROR", "placement_parse_failed", { message: parseError.message });
      response.writeHead(500, { "Content-Type": "application/json; charset=utf-8" });
      response.end(JSON.stringify({ error: parseError.message }));
    }
  });
}

function readLines(stream, onLine) {
  let buffer = "";
  stream.setEncoding("utf8");
  stream.on("data", (chunk) => {
    buffer += chunk;
    const lines = buffer.split(/\r?\n/);
    buffer = lines.pop() || "";
    for (const line of lines) {
      if (line.trim()) onLine(line.trim());
    }
  });
  stream.on("end", () => {
    if (buffer.trim()) onLine(buffer.trim());
  });
}

function startReplicaService(nodeId, runId, response) {
  const servicePort = rdmaConfig.basePort + nodeId;
  const command = [
    "exec",
    remoteBinary("hotbucket_rdma_replica"),
    "server",
    "--port", String(servicePort),
    "--node-id", String(nodeId),
    "--max-replications", "0",
  ].join(" ");
  const child = spawn(
    sshExecutable,
    sshArgs(rdmaConfig.memoryHost, command),
    { windowsHide: true, stdio: ["ignore", "pipe", "pipe"] },
  );

  const ready = new Promise((resolve, reject) => {
    let settled = false;
    const timeout = setTimeout(() => {
      if (settled) return;
      settled = true;
      reject(new Error(`Node ${nodeId} did not become ready on port ${servicePort}.`));
    }, 15000);

    readLines(child.stdout, (line) => {
      writeLog("INFO", "rdma_memory_service", { runId, nodeId, message: line });
      sendEvent(response, { type: "rdma-log", level: "INFO", nodeId, message: line });
      if (!settled && line.includes("replica_server_ready")) {
        settled = true;
        clearTimeout(timeout);
        resolve();
      }
    });
    readLines(child.stderr, (line) => {
      writeLog("WARN", "rdma_memory_service_stderr", { runId, nodeId, message: line });
      sendEvent(response, { type: "rdma-log", level: "WARN", nodeId, message: line });
    });
    child.on("error", (error) => {
      if (!settled) {
        settled = true;
        clearTimeout(timeout);
        reject(error);
      }
    });
    child.on("close", (code) => {
      if (!settled) {
        settled = true;
        clearTimeout(timeout);
        reject(new Error(`Node ${nodeId} service exited with code ${code}.`));
      }
    });
  });
  return { child, ready, nodeId, servicePort };
}

function requestRemoteServiceStop(nodeCount) {
  const commands = [];
  for (let node = 0; node < nodeCount; node += 1) {
    const servicePort = rdmaConfig.basePort + node;
    commands.push(
      `pkill -TERM -f '[h]otbucket_rdma_replica server --port ${servicePort}' || true`,
    );
  }
  return runCommand(
    sshExecutable,
    sshArgs(rdmaConfig.memoryHost, commands.join("; ")),
    { timeout: 5000 },
  ).catch(() => null);
}

async function startRdmaSimulation(request, response, url) {
  const runId = randomUUID();
  response.writeHead(200, {
    "Content-Type": "text/event-stream",
    "Cache-Control": "no-cache, no-transform",
    Connection: "keep-alive",
    "X-Accel-Buffering": "no",
  });

  const settings = simulationSettings(url);
  const memoryBytes = nodeMemoryBytes(settings.nodes, settings.entities);
  const services = [];
  let computeChild = null;
  let remotePredictionPath = null;
  let remoteEntityStorePath = null;
  let completed = false;
  let cleaned = false;

  const cleanup = () => {
    if (cleaned) return;
    cleaned = true;
    if (computeChild?.exitCode === null) computeChild.kill();
    for (const service of services) {
      if (service.child.exitCode === null) service.child.kill();
    }
    void requestRemoteServiceStop(settings.nodes);
    const remoteFiles = [remotePredictionPath, remoteEntityStorePath].filter(Boolean);
    if (remoteFiles.length) {
      void runCommand(
        sshExecutable,
        sshArgs(rdmaConfig.clientHost, `rm -f ${remoteFiles.join(" ")}`),
        { timeout: 5000 },
      ).catch(() => null);
    }
  };

  request.on("close", () => {
    if (!completed) {
      writeLog("INFO", "rdma_client_disconnected", { runId });
      cleanup();
    }
  });

  try {
    sendEvent(response, { type: "rdma-status", state: "checking", message: "Checking both VMs" });
    const health = await inspectRdmaEnvironment();
    if (!health.ok) {
      const failures = [health.memory, health.client]
        .filter((host) => !host.ok)
        .map((host) => `${host.host}: ${host.error}`)
        .concat(health.calibration.ok ? [] : [`calibration: ${health.calibration.error}`])
        .join("; ");
      throw new Error(`RDMA environment is not ready. ${failures}`);
    }

    ensureEntityStore();
    remoteEntityStorePath = `/tmp/hotbucket-dashboard-${runId}-entities.hbkv`;
    sendEvent(response, {
      type: "rdma-status",
      state: "uploading-entities",
      message: "Sending the durable entity KV snapshot to the compute VM",
    });
    await runCommand(scpExecutable, [
      "-o", "BatchMode=yes",
      "-o", "ConnectTimeout=5",
      entityStorePath,
      `${rdmaConfig.clientHost}:${remoteEntityStorePath}`,
    ], { timeout: 30000, maxBuffer: 4 * 1024 * 1024 });

    let lstmPrediction = null;
    if (settings.policy === "lstm") {
      lstmPrediction = await prepareLstmPredictions(settings, runId, response);
      remotePredictionPath = `/tmp/hotbucket-dashboard-${runId}-predictions.csv`;
      sendEvent(response, {
        type: "rdma-status",
        state: "uploading-prediction",
        message: "Sending frozen LSTM predictions to the compute VM",
      });
      await runCommand(scpExecutable, [
        "-o", "BatchMode=yes",
        "-o", "ConnectTimeout=5",
        lstmPrediction.predictionPath,
        `${rdmaConfig.clientHost}:${remotePredictionPath}`,
      ], { timeout: 30000, maxBuffer: 4 * 1024 * 1024 });
    }

    sendEvent(response, {
      type: "rdma-status",
      state: "starting",
      message: `Starting ${settings.nodes} memory-node services`,
      health,
    });
    await requestRemoteServiceStop(settings.nodes);
    for (let node = 0; node < settings.nodes; node += 1) {
      services.push(startReplicaService(node, runId, response));
    }
    await Promise.all(services.map((service) => service.ready));

    const remoteOutput = `/tmp/hotbucket-dashboard-${runId}.csv`;
    const calibration = health.calibration;
    const traceId = lstmPrediction?.traceId || `dashboard-${runId}`;
    const commandParts = [
      "exec",
      remoteBinary("hotbucket_rdma_sim"),
      "--policy", settings.policy,
      "--trace-id", traceId,
      "--nodes", String(settings.nodes),
      "--buckets", String(settings.buckets),
      "--entities", String(settings.entities),
      "--entity-store", remoteEntityStorePath,
      "--node-memory-bytes", String(memoryBytes),
      "--windows", String(settings.windows),
      "--requests", String(settings.requests),
      "--node-capacity", String(settings.nodeCapacity),
      "--hotspot-duration", String(settings.hotspotDuration),
      "--threshold", String(settings.threshold),
      "--read-ratio", String(settings.readRatio),
      "--hotspot-share", String(settings.hotspotShare),
      "--seed", String(settings.seed),
      "--window-delay-ms", String(settings.delay),
      "--bucket-size-mib", "0",
      "--copy-bandwidth-mib-per-ms", String(calibration.copyBandwidthMibPerMs),
      "--metadata-cost-ms", String(calibration.fixedCostMs),
      "--rdma-server", rdmaConfig.memoryIp,
      "--rdma-port-base", String(rdmaConfig.basePort),
      "--stream-json",
      "--output", remoteOutput,
    ];
    if (lstmPrediction) {
      commandParts.push(
        "--prediction-file", remotePredictionPath,
        "--lstm-min-probability", String(lstmConfig.minimumProbability),
        "--lstm-min-predicted-get-ratio", String(lstmConfig.minimumPredictedGetRatio),
        "--lstm-inference-cost-ms", String(lstmConfig.inferenceCostMs),
      );
    }
    const command = commandParts.join(" ");

    computeChild = spawn(
      sshExecutable,
      sshArgs(rdmaConfig.clientHost, command),
      { windowsHide: true, stdio: ["ignore", "pipe", "pipe"] },
    );

    writeLog("INFO", "rdma_run_started", {
      runId,
      childPid: computeChild.pid,
      policy: settings.policy,
      nodes: settings.nodes,
      entities: settings.entities,
      buckets: settings.buckets,
      windows: settings.windows,
      memoryHost: rdmaConfig.memoryHost,
      clientHost: rdmaConfig.clientHost,
      memoryIp: rdmaConfig.memoryIp,
      calibration,
      lstmPrediction: lstmPrediction
        ? {
            traceId,
            minimumProbability: lstmConfig.minimumProbability,
            sequenceLength: 6,
          }
        : null,
      output: `${rdmaConfig.clientHost}:${remoteOutput}`,
    });
    sendEvent(response, {
      type: "started",
      mode: "rdma",
      runId,
      policy: settings.policy,
      nodes: settings.nodes,
      buckets: settings.buckets,
      entities: settings.entities,
      windows: settings.windows,
      output: `${rdmaConfig.clientHost}:${remoteOutput}`,
      calibration,
      lstmPrediction: lstmPrediction
        ? {
            traceId,
            minimumProbability: lstmConfig.minimumProbability,
            sequenceLength: 6,
          }
        : null,
      logEndpoint: `/api/logs?runId=${encodeURIComponent(runId)}`,
    });

    let stderrBuffer = "";
    readLines(computeChild.stdout, (line) => {
      try {
        const payload = JSON.parse(line);
        if (payload.type === "complete") completed = true;
        if (payload.type === "placement") {
          writeLog("INFO", "rdma_entities_allocated", {
            runId,
            nodeCount: payload.nodeCount,
            entityCount: payload.entityCount,
            usedBytes: payload.totalUsedBytes,
          });
        } else if (payload.type === "window") {
          writeLog(payload.copyApplied ? "INFO" : "DEBUG", "rdma_window_completed", {
            runId,
            windowId: payload.windowId,
            hotBucket: payload.hotBucket,
            maxLoadRatio: payload.maxLoadRatio,
            copyApplied: payload.copyApplied,
            rdmaSequence: payload.rdmaSequence,
            rdmaWriteMs: payload.rdmaWriteMs,
            rdmaEndToEndMs: payload.rdmaEndToEndMs,
          });
        } else if (payload.type === "complete") {
          writeLog("INFO", "rdma_run_completed", {
            runId,
            copies: payload.copies,
            totalCompletionMs: payload.totalCompletionMs,
            remoteCsv: `${rdmaConfig.clientHost}:${remoteOutput}`,
          });
          payload.csv = `${rdmaConfig.clientHost}:${remoteOutput}`;
        }
        sendEvent(response, { ...payload, runId, mode: "rdma" });
      } catch {
        writeLog("WARN", "unparsed_rdma_output", { runId, message: line });
        sendEvent(response, { type: "rdma-log", level: "WARN", message: line });
      }
    });
    readLines(computeChild.stderr, (line) => {
      stderrBuffer += `${line}\n`;
      writeLog("WARN", "rdma_compute_stderr", { runId, message: line });
      sendEvent(response, { type: "rdma-log", level: "WARN", message: line });
    });
    computeChild.on("error", (error) => {
      writeLog("ERROR", "rdma_compute_spawn_failed", { runId, message: error.message });
      completed = true;
      if (!response.destroyed && !response.writableEnded) {
        sendEvent(response, { type: "error", runId, message: error.message });
        response.end();
      }
      cleanup();
    });
    computeChild.on("close", (code) => {
      if (code !== 0 && !completed) {
        const message = stderrBuffer.trim() || `Remote simulator exited with code ${code}.`;
        writeLog("ERROR", "rdma_compute_exited", { runId, exitCode: code, message });
        sendEvent(response, { type: "error", runId, message });
      }
      completed = true;
      cleanup();
      if (!response.destroyed && !response.writableEnded) response.end();
    });
  } catch (error) {
    writeLog("ERROR", "rdma_start_failed", { runId, message: error.message });
    completed = true;
    cleanup();
    if (!response.destroyed && !response.writableEnded) {
      sendEvent(response, { type: "error", runId, message: error.message });
      response.end();
    }
  }
}

async function startSimulation(request, response, url) {
  if (url.searchParams.get("mode") === "rdma") {
    void startRdmaSimulation(request, response, url);
    return;
  }
  const executable = simulatorPath();
  const runId = randomUUID();
  response.writeHead(200, {
    "Content-Type": "text/event-stream",
    "Cache-Control": "no-cache, no-transform",
    Connection: "keep-alive",
    "X-Accel-Buffering": "no",
  });

  if (!executable) {
    writeLog("ERROR", "simulator_not_found", { runId, candidates: executableCandidates });
    sendEvent(response, {
      type: "error",
      runId,
      message: "Simulator executable was not found. Build it before starting the dashboard.",
    });
    response.end();
    return;
  }
  try {
    ensureEntityStore();
  } catch (error) {
    writeLog("ERROR", "entity_store_unavailable", { runId, message: error.message });
    sendEvent(response, { type: "error", runId, message: error.message });
    response.end();
    return;
  }

  const {
    policy,
    nodes,
    buckets,
    entities,
    windows,
    requests,
    nodeCapacity,
    hotspotDuration,
    threshold,
    delay,
    seed,
    readRatio,
    hotspotShare,
  } = simulationSettings(url);
  const memoryBytes = nodeMemoryBytes(nodes, entities);

  const resultDirectory = path.join(projectRoot, "results", "dashboard");
  mkdirSync(resultDirectory, { recursive: true });
  const outputPath = path.join(
    resultDirectory,
    `${policy}-${Date.now()}-seed-${seed}.csv`,
  );
  const relativeOutputPath = path.relative(projectRoot, outputPath);
  let lstmPrediction = null;
  if (policy === "lstm") {
    try {
      lstmPrediction = await prepareLstmPredictions(
        simulationSettings(url), runId, response,
      );
    } catch (error) {
      writeLog("ERROR", "lstm_prediction_failed", { runId, message: error.message });
      sendEvent(response, { type: "error", runId, message: error.message });
      response.end();
      return;
    }
  }

  const args = [
    "--policy", policy,
    "--trace-id", lstmPrediction?.traceId || `dashboard-${runId}`,
    "--nodes", String(nodes),
    "--buckets", String(buckets),
    "--entities", String(entities),
    "--entity-store", path.relative(projectRoot, entityStorePath),
    "--node-memory-bytes", String(memoryBytes),
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
  if (lstmPrediction) {
    args.push(
      "--prediction-file", lstmPrediction.relativePredictionPath,
      "--lstm-min-probability", String(lstmConfig.minimumProbability),
      "--lstm-min-predicted-get-ratio", String(lstmConfig.minimumPredictedGetRatio),
      "--lstm-inference-cost-ms", String(lstmConfig.inferenceCostMs),
    );
  }

  const child = spawn(executable, args, {
    cwd: projectRoot,
    windowsHide: true,
    stdio: ["ignore", "pipe", "pipe"],
  });
  let stdoutBuffer = "";
  let stderrBuffer = "";
  let completed = false;

  writeLog("INFO", "run_started", {
    runId,
    childPid: child.pid,
    policy,
    nodes,
    entities,
    buckets,
    windows,
    requestsPerWindow: requests,
    threshold,
    seed,
    lstmPrediction: lstmPrediction
      ? {
          traceId: lstmPrediction.traceId,
          minimumProbability: lstmConfig.minimumProbability,
          sequenceLength: 6,
        }
      : null,
    output: relativeOutputPath,
  });

  sendEvent(response, {
    type: "started",
    mode: "local",
    runId,
    policy,
    nodes,
    buckets,
    entities,
    windows,
    lstmPrediction: lstmPrediction
      ? {
          traceId: lstmPrediction.traceId,
          minimumProbability: lstmConfig.minimumProbability,
          sequenceLength: 6,
        }
      : null,
    output: relativeOutputPath,
    logEndpoint: `/api/logs?runId=${encodeURIComponent(runId)}`,
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
        if (payload.type === "placement") {
          writeLog("INFO", "entities_allocated", {
            runId,
            nodeCount: payload.nodeCount,
            entityCount: payload.entityCount,
            usedBytes: payload.totalUsedBytes,
            capacityBytes: payload.totalCapacityBytes,
          });
        } else if (payload.type === "window") {
          const copyMatchedHotspot = payload.decision?.bucketId === payload.hotBucket;
          writeLog(
            payload.copyApplied ? (copyMatchedHotspot ? "INFO" : "WARN") : "DEBUG",
            "window_completed",
            {
            runId,
            windowId: payload.windowId,
            hotBucket: payload.hotBucket,
            maxLoadRatio: payload.maxLoadRatio,
            windowCompletionMs: payload.windowCompletionMs,
            copyApplied: payload.copyApplied,
            copyBucket: payload.decision?.bucketId ?? null,
            copyTargetNode: payload.decision?.targetNode ?? null,
            copyMatchedHotspot: payload.copyApplied ? copyMatchedHotspot : null,
          });
        } else if (payload.type === "complete") {
          writeLog("INFO", "simulation_completed", {
            runId,
            policy: payload.policy,
            windows: payload.windows,
            totalCompletionMs: payload.totalCompletionMs,
            maxLoadRatio: payload.maxLoadRatio,
            copies: payload.copies,
            csv: payload.csv,
          });
        }
        sendEvent(response, { ...payload, runId });
      } catch {
        writeLog("WARN", "unparsed_simulator_output", { runId, message: line });
        sendEvent(response, { type: "log", runId, message: line });
      }
    }
  });

  child.stderr.setEncoding("utf8");
  child.stderr.on("data", (chunk) => {
    stderrBuffer += chunk;
    writeLog("WARN", "simulator_stderr", { runId, message: chunk.trim() });
  });

  child.on("error", (error) => {
    writeLog("ERROR", "simulator_spawn_failed", { runId, message: error.message });
    sendEvent(response, { type: "error", runId, message: error.message });
    response.end();
  });

  child.on("close", (code) => {
    if (code !== 0) {
      writeLog("ERROR", "simulator_exited", {
        runId,
        exitCode: code,
        stderr: stderrBuffer.trim() || null,
      });
      sendEvent(response, {
        type: "error",
        runId,
        message: stderrBuffer.trim() || `Simulator exited with code ${code}.`,
      });
    } else if (!completed) {
      writeLog("WARN", "simulator_completed_without_summary", { runId, exitCode: code });
      sendEvent(response, { type: "complete", runId, policy, windows });
    }
    response.end();
  });

  request.on("close", () => {
    if (child.exitCode === null && !completed) {
      writeLog("INFO", "client_disconnected", { runId, childPid: child.pid });
      child.kill();
    }
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
  if (url.pathname.startsWith("/api/kv/entities")) {
    void handleEntityApi(request, response, url);
    return;
  }
  if (request.method === "GET" && url.pathname === "/api/health") {
    response.writeHead(200, { "Content-Type": "application/json" });
    response.end(JSON.stringify({
      ok: true,
      simulator: simulatorPath() || null,
      log: path.relative(projectRoot, logPath),
    }));
    return;
  }
  if (request.method === "GET" && url.pathname === "/api/logs") {
    const limit = integerParam(url.searchParams, "limit", 200, 1, 2000);
    const runId = url.searchParams.get("runId") || null;
    response.writeHead(200, { "Content-Type": "application/json; charset=utf-8" });
    response.end(JSON.stringify({
      log: path.relative(projectRoot, logPath),
      runId,
      entries: readRecentLogs(limit, runId),
    }));
    return;
  }
  if (request.method === "GET" && url.pathname === "/api/rdma/health") {
    void inspectRdmaEnvironment()
      .then((health) => {
        response.writeHead(health.ok ? 200 : 503, {
          "Content-Type": "application/json; charset=utf-8",
        });
        response.end(JSON.stringify(health));
      })
      .catch((error) => {
        response.writeHead(503, { "Content-Type": "application/json; charset=utf-8" });
        response.end(JSON.stringify({ ok: false, error: error.message }));
      });
    return;
  }
  if (request.method === "GET" && url.pathname === "/api/placement") {
    getPlacement(response, url);
    return;
  }
  if (request.method === "GET" && url.pathname === "/api/simulate") {
    void startSimulation(request, response, url);
    return;
  }
  serveStatic(response, url.pathname);
});

server.listen(port, "127.0.0.1", () => {
  writeLog("INFO", "server_started", {
    port,
    simulator: simulatorPath() || null,
    log: path.relative(projectRoot, logPath),
  });
});

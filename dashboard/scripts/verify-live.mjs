import assert from "node:assert/strict";

const baseUrl = process.env.DASHBOARD_URL || "http://127.0.0.1:5173";

async function requestJson(path, options = {}) {
  const response = await fetch(`${baseUrl}${path}`, options);
  const body = await response.json();
  assert.equal(
    response.ok,
    true,
    `${options.method || "GET"} ${path} returned ${response.status}: ${body.error || "unknown error"}`,
  );
  return body;
}

const params = new URLSearchParams({
  policy: "recent-window",
  nodes: "5",
  buckets: "64",
  entities: "10",
  windows: "12",
  requests: "4000",
  nodeCapacity: "1800",
  hotspotDuration: "5",
  threshold: "1200",
  delay: "0",
  seed: "42",
  readRatio: "0.9",
  hotspotShare: "0.65",
});

const response = await fetch(`${baseUrl}/api/simulate?${params}`);
assert.equal(response.ok, true, `simulation endpoint returned ${response.status}`);

const body = await response.text();
const events = body
  .split(/\r?\n\r?\n/)
  .filter((block) => block.startsWith("data: "))
  .map((block) => JSON.parse(block.slice(6)));
const placement = events.find((event) => event.type === "placement");
const windows = events.filter((event) => event.type === "window");
const memory = events.filter((event) => event.type === "memory");
const complete = events.find((event) => event.type === "complete");

assert.equal(placement.primaryCount, 10, "initial placement should contain 10 primaries");
assert.equal(windows.length, 12, "simulation should emit 12 window events");
assert.equal(memory.length, 12, "each window should emit a matching memory snapshot");
assert.ok(memory.some((event) => event.replicaCount > 0), "a replica should appear in memory");
assert.ok(
  memory.some((event) => event.totalUsedBytes > placement.totalUsedBytes),
  "physical memory usage should increase after a copy",
);
assert.ok(
  memory.some((event) => event.expiredReplicas.length > 0),
  "a TTL expiration event should be visible",
);
assert.ok(complete?.copies > 0, "run should complete with at least one physical copy");

console.log(JSON.stringify({
  windows: windows.length,
  copies: complete.copies,
  initialUsedBytes: placement.totalUsedBytes,
  peakUsedBytes: Math.max(...memory.map((event) => event.totalUsedBytes)),
  peakReplicas: Math.max(...memory.map((event) => event.replicaCount)),
  expirationEvents: memory.filter((event) => event.expiredReplicas.length > 0).length,
}, null, 2));

const lstmParams = new URLSearchParams(params);
lstmParams.set("policy", "lstm");
const lstmResponse = await fetch(`${baseUrl}/api/simulate?${lstmParams}`);
assert.equal(lstmResponse.ok, true, `LSTM endpoint returned ${lstmResponse.status}`);

const lstmEvents = (await lstmResponse.text())
  .split(/\r?\n\r?\n/)
  .filter((block) => block.startsWith("data: "))
  .map((block) => JSON.parse(block.slice(6)));
const predictionStatus = lstmEvents.find(
  (event) => event.type === "status" && event.state === "predicting",
);
const lstmStarted = lstmEvents.find((event) => event.type === "started");
const lstmWindows = lstmEvents.filter((event) => event.type === "window");
const lstmComplete = lstmEvents.find((event) => event.type === "complete");
const lstmError = lstmEvents.find((event) => event.type === "error");

assert.equal(lstmError, undefined, lstmError?.message || "LSTM run should not fail");
assert.ok(predictionStatus, "LSTM run should report prediction preparation");
assert.equal(lstmStarted?.policy, "lstm", "LSTM policy should start");
assert.equal(lstmStarted?.lstmPrediction?.sequenceLength, 6, "LSTM should use six windows");
assert.equal(lstmWindows.length, 12, "LSTM run should emit 12 window events");
assert.ok(lstmComplete?.copies > 0, "LSTM run should complete with a physical copy");

console.log(JSON.stringify({
  policy: lstmStarted.policy,
  sequenceLength: lstmStarted.lstmPrediction.sequenceLength,
  windows: lstmWindows.length,
  copies: lstmComplete.copies,
}, null, 2));

const temporaryEntityId = 900000000 + process.pid;
const originalStore = await requestJson("/api/kv/entities");
assert.equal(originalStore.durable, true, "entity store should be durable");
assert.ok(originalStore.count >= 2, "entity store should contain simulation records");

try {
  const created = await requestJson("/api/kv/entities", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      entityId: temporaryEntityId,
      callsign: "VERIFY-TANK",
      entityType: "Tank",
      attributes: {
        position: { valueType: "Vector3", value: "120.0,42.0,0.0" },
        velocity: { valueType: "Vector3", value: "4.5,0.0,0.0" },
        health: { valueType: "Float", value: "1.0" },
        ammunition: { valueType: "Integer", value: "30" },
      },
    }),
  });
  assert.equal(created.entity.id, temporaryEntityId, "created EntityID should match the KV key");
  assert.equal(created.entity.entityType, "Tank", "created record should retain its entity type");

  const fetched = await requestJson(`/api/kv/entities/${temporaryEntityId}`);
  assert.equal(fetched.entity.attributeMap.health.value, "1.0", "GET should decode stored attributes");

  const updated = await requestJson(`/api/kv/entities/${temporaryEntityId}`, {
    method: "PUT",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      callsign: "VERIFY-TANK",
      entityType: "Tank",
      attributes: {
        position: { valueType: "Vector3", value: "121.0,42.0,0.0" },
        velocity: { valueType: "Vector3", value: "4.5,0.0,0.0" },
        health: { valueType: "Float", value: "0.73" },
        ammunition: { valueType: "Integer", value: "28" },
      },
    }),
  });
  assert.equal(updated.entity.attributeMap.health.value, "0.73", "PUT should persist changes");

  const placementAfterCreate = await requestJson(
    `/api/placement?nodes=5&buckets=64&entities=${originalStore.count + 1}`,
  );
  const allocatedIds = placementAfterCreate.nodes
    .flatMap((node) => node.entities)
    .map((entity) => entity.id);
  assert.ok(
    allocatedIds.includes(temporaryEntityId),
    "a durable entity should be allocated into the simulated memory cluster",
  );
} finally {
  const maybeStored = await fetch(`${baseUrl}/api/kv/entities/${temporaryEntityId}`);
  if (maybeStored.ok) {
    await requestJson(`/api/kv/entities/${temporaryEntityId}`, { method: "DELETE" });
  }
}

const restoredStore = await requestJson("/api/kv/entities");
assert.equal(restoredStore.count, originalStore.count, "KV verification should restore the store");

console.log(JSON.stringify({
  kvStore: originalStore.store,
  records: restoredStore.count,
  verifiedOperations: ["CREATE", "GET", "UPDATE", "ALLOCATE", "DELETE"],
}, null, 2));

import assert from "node:assert/strict";

const baseUrl = process.env.DASHBOARD_URL || "http://127.0.0.1:5173";
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

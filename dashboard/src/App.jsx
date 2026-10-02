import { useEffect, useMemo, useRef, useState } from "react";
import {
  Activity,
  Boxes,
  Clock3,
  Copy,
  Database,
  Flame,
  Gauge,
  MemoryStick,
  Play,
  Server,
  Square,
  TerminalSquare,
} from "lucide-react";

const initialSettings = {
  policy: "recent-window",
  nodes: 5,
  buckets: 64,
  entities: 10,
  windows: 40,
  requests: 4000,
  nodeCapacity: 1800,
  hotspotDuration: 5,
  threshold: 1200,
  delay: 250,
  seed: 42,
  readRatio: 0.9,
  hotspotShare: 0.65,
};

const policyLabels = {
  "no-action": "No action",
  reactive: "Reactive",
  "recent-window": "Recent window",
};

function formatNumber(value, digits = 0) {
  if (!Number.isFinite(value)) return "-";
  return new Intl.NumberFormat("en-US", {
    maximumFractionDigits: digits,
    minimumFractionDigits: digits,
  }).format(value);
}

function formatBytes(value) {
  if (!Number.isFinite(value)) return "-";
  if (value >= 1024) return `${formatNumber(value / 1024, 2)} KiB`;
  return `${formatNumber(value)} B`;
}

function allocationKey(entity) {
  return `${entity.id}:${entity.nodeId}:${entity.isReplica ? "replica" : "primary"}`;
}

function Metric({ icon: Icon, label, value, detail, tone = "blue" }) {
  return (
    <div className="metric">
      <span className={`metric-icon ${tone}`}><Icon size={18} /></span>
      <div>
        <div className="metric-label">{label}</div>
        <div className="metric-value">{value}</div>
        <div className="metric-detail">{detail}</div>
      </div>
    </div>
  );
}

function LineChart({ history, valueKey, color, label, unit }) {
  const values = history.map((entry) => Number(entry[valueKey]) || 0);
  const width = 640;
  const height = 154;
  const left = 38;
  const right = 10;
  const top = 12;
  const bottom = 24;
  const max = Math.max(...values, 1);
  const min = Math.min(...values, 0);
  const range = Math.max(max - min, 0.001);
  const points = values.map((value, index) => {
    const x = left + (index / Math.max(values.length - 1, 1)) * (width - left - right);
    const y = top + ((max - value) / range) * (height - top - bottom);
    return `${x},${y}`;
  }).join(" ");

  return (
    <div className="chart-wrap">
      <div className="chart-heading">
        <span>{label}</span>
        <strong>{values.length ? `${formatNumber(values.at(-1), 2)}${unit}` : "Waiting"}</strong>
      </div>
      <svg className="line-chart" viewBox={`0 0 ${width} ${height}`} role="img" aria-label={label}>
        {[0, 0.5, 1].map((position) => {
          const y = top + position * (height - top - bottom);
          return <line key={position} x1={left} x2={width - right} y1={y} y2={y} className="grid-line" />;
        })}
        <line x1={left} x2={left} y1={top} y2={height - bottom} className="axis-line" />
        <line x1={left} x2={width - right} y1={height - bottom} y2={height - bottom} className="axis-line" />
        <text x={left - 6} y={top + 4} textAnchor="end" className="axis-label">{formatNumber(max, 1)}</text>
        <text x={left - 6} y={height - bottom + 4} textAnchor="end" className="axis-label">{formatNumber(min, 1)}</text>
        {points && <polyline points={points} fill="none" stroke={color} strokeWidth="2.5" vectorEffect="non-scaling-stroke" />}
        {values.length > 0 && (() => {
          const [x, y] = points.split(" ").at(-1).split(",");
          return <circle cx={x} cy={y} r="4" fill={color} />;
        })()}
        <text x={left} y={height - 5} className="axis-label">W1</text>
        <text x={width - right} y={height - 5} textAnchor="end" className="axis-label">
          W{Math.max(history.length, 1)}
        </text>
      </svg>
    </div>
  );
}

function NodeLoads({ nodes = [] }) {
  return (
    <div className="node-list">
      {nodes.map((node) => {
        const percent = node.loadRatio * 100;
        const level = percent >= 100 ? "danger" : percent >= 75 ? "warning" : "healthy";
        return (
          <div className="node-row" key={node.id}>
            <div className="node-label">
              <span>Node {node.id}</span>
              <span>{formatNumber(node.operations)} ops</span>
            </div>
            <div className="load-track" aria-label={`Node ${node.id} load ${formatNumber(percent, 1)} percent`}>
              <div className={`load-fill ${level}`} style={{ width: `${Math.min(percent, 100)}%` }} />
              {percent > 100 && <span className="overload-mark">+{formatNumber(percent - 100, 0)}%</span>}
            </div>
            <span className={`load-value ${level}`}>{formatNumber(percent, 1)}%</span>
          </div>
        );
      })}
      {!nodes.length && <div className="empty-state">No node measurements</div>}
    </div>
  );
}

function BucketMap({ buckets = [], hotBucket }) {
  const maxGets = Math.max(...buckets.map((bucket) => bucket.gets), 1);
  return (
    <div className="bucket-map" style={{ "--bucket-columns": Math.min(16, Math.max(8, Math.ceil(Math.sqrt(buckets.length * 2)))) }}>
      {buckets.map((bucket) => {
        const intensity = bucket.gets / maxGets;
        const level = intensity > 0.66 ? "high" : intensity > 0.25 ? "medium" : "low";
        const className = [
          "bucket-cell",
          level,
          bucket.id === hotBucket ? "current-hot" : "",
          bucket.replicas > 0 ? "replicated" : "",
        ].filter(Boolean).join(" ");
        return (
          <div
            key={bucket.id}
            className={className}
            title={`Bucket ${bucket.id}: ${bucket.gets} GET, ${bucket.puts} PUT, ${bucket.replicas} replicas`}
          >
            <span>{bucket.id}</span>
            {bucket.replicas > 0 && <i aria-label="replicated" />}
          </div>
        );
      })}
      {!buckets.length && <div className="empty-state bucket-empty">Run the simulation to populate buckets</div>}
    </div>
  );
}

function EntityMemoryAllocation({ placement, selectedAllocationKey, onSelect, hotBucket }) {
  const entities = placement?.nodes?.flatMap((node) => node.entities) || [];
  const selected = entities.find((entity) => allocationKey(entity) === selectedAllocationKey)
    || entities.find((entity) => !entity.isReplica)
    || entities[0];

  return (
    <section className="panel memory-panel">
      <div className="section-heading">
        <div><MemoryStick size={18} /><h2>Entity memory allocation</h2></div>
        <span>
          {placement
            ? `${placement.primaryCount} primary entities · ${placement.replicaCount} temporary copies`
            : "Loading placement"}
        </span>
      </div>

      {placement ? (
        <>
          <div className="memory-summary">
            <div><span>Total pool</span><strong>{formatBytes(placement.totalCapacityBytes)}</strong></div>
            <div><span>Allocated</span><strong>{formatBytes(placement.totalUsedBytes)}</strong></div>
            <div><span>Utilization</span><strong>{formatNumber((placement.totalUsedBytes / placement.totalCapacityBytes) * 100, 1)}%</strong></div>
            <div><span>Temporary copies</span><strong>{formatNumber(placement.replicaCount)}</strong></div>
          </div>

          <div className="memory-node-grid">
            {placement.nodes.map((node) => {
              const utilization = (node.usedBytes / node.capacityBytes) * 100;
              const primaryCount = node.entities.filter((entity) => !entity.isReplica).length;
              const replicaCount = node.entities.length - primaryCount;
              return (
                <article className="memory-node" key={node.id}>
                  <header>
                    <div><Server size={15} /><strong>Node {node.id}</strong></div>
                    <span>{primaryCount} primary · {replicaCount} copies</span>
                  </header>
                  <div className="node-memory-stats">
                    <span>{formatBytes(node.usedBytes)} used</span>
                    <span>{formatNumber(utilization, 1)}%</span>
                  </div>
                  <div className="memory-addresses"><span>0 B</span><span>{formatBytes(node.capacityBytes)}</span></div>
                  <div className="memory-pool-track" aria-label={`Node ${node.id} memory layout`}>
                    {node.entities.map((entity) => {
                      const left = (entity.offsetBytes / node.capacityBytes) * 100;
                      const width = (entity.allocatedBytes / node.capacityBytes) * 100;
                      const isHot = entity.bucketId === hotBucket;
                      const justCopied = entity.isReplica
                        && placement.copiedBucket === entity.bucketId
                        && placement.copiedToNode === entity.nodeId;
                      return (
                        <button
                          key={allocationKey(entity)}
                          type="button"
                          className={`memory-segment type-${entity.id % 6} ${entity.isReplica ? "replica" : "primary"} ${isHot ? "hot" : ""} ${justCopied ? "just-copied" : ""}`}
                          style={{ left: `${left}%`, width: `${width}%` }}
                          onClick={() => onSelect(allocationKey(entity))}
                          title={`${entity.callsign} ${entity.isReplica ? "replica" : "primary"}: offset ${entity.offsetBytes} B, allocation ${entity.allocatedBytes} B`}
                          aria-label={`Select ${entity.callsign}`}
                        />
                      );
                    })}
                  </div>
                  <div className="node-entity-list">
                    {node.entities.map((entity) => (
                      <button
                        key={allocationKey(entity)}
                        type="button"
                        className={`${selected && allocationKey(selected) === allocationKey(entity) ? "selected" : ""} ${entity.bucketId === hotBucket ? "hot" : ""} ${entity.isReplica ? "replica" : "primary"}`}
                        onClick={() => onSelect(allocationKey(entity))}
                      >
                        <span className={`entity-color type-${entity.id % 6}`} />
                        <span className="entity-name">
                          <strong>{entity.callsign}</strong>
                          <small>{entity.isReplica ? `Replica · expires after W${entity.expiresAfterWindow + 1}` : entity.entityType}</small>
                        </span>
                        <span className="entity-size">{entity.isReplica ? <Copy size={13} /> : null}{formatBytes(entity.allocatedBytes)}</span>
                      </button>
                    ))}
                  </div>
                </article>
              );
            })}
          </div>

          {selected && (
            <div className="entity-inspector">
              <div className="entity-inspector-heading">
                <div className={`entity-avatar type-${selected.id % 6}`}><Boxes size={18} /></div>
                <div>
                  <strong>{selected.callsign}</strong>
                  <span>{selected.entityType} · EntityID {selected.id} · {selected.isReplica ? "Temporary replica" : "Primary copy"}</span>
                </div>
              </div>
              <dl className="entity-mapping">
                <div><dt>KV key</dt><dd>{selected.id}</dd></div>
                <div><dt>Bucket</dt><dd>#{selected.bucketId}</dd></div>
                <div><dt>Memory node</dt><dd>Node {selected.nodeId}</dd></div>
                <div><dt>Home node</dt><dd>Node {selected.homeNode}</dd></div>
                <div><dt>Offset</dt><dd>{formatBytes(selected.offsetBytes)}</dd></div>
                <div><dt>Serialized</dt><dd>{formatBytes(selected.serializedBytes)}</dd></div>
                <div><dt>Allocated</dt><dd>{formatBytes(selected.allocatedBytes)}</dd></div>
              </dl>
              <div className="attribute-table-wrap">
                <table className="attribute-table">
                  <thead><tr><th>Attribute key</th><th>Type</th><th>Value</th><th>Size</th></tr></thead>
                  <tbody>
                    {selected.attributes.map((attribute) => (
                      <tr key={attribute.name}>
                        <td>{attribute.name}</td>
                        <td>{attribute.valueType}</td>
                        <td title={attribute.value}>{attribute.value}</td>
                        <td>{formatBytes(attribute.valueBytes)}</td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
            </div>
          )}
        </>
      ) : (
        <div className="empty-state memory-empty">Waiting for the C++ memory placement snapshot</div>
      )}
    </section>
  );
}

function NumberField({ label, name, value, min, max, step = 1, onChange, disabled }) {
  return (
    <label className="field">
      <span>{label}</span>
      <input
        type="number"
        name={name}
        value={value}
        min={min}
        max={max}
        step={step}
        onChange={onChange}
        disabled={disabled}
      />
    </label>
  );
}

export default function App() {
  const [settings, setSettings] = useState(initialSettings);
  const [status, setStatus] = useState("idle");
  const [history, setHistory] = useState([]);
  const [latest, setLatest] = useState(null);
  const [placement, setPlacement] = useState(null);
  const [selectedAllocationKey, setSelectedAllocationKey] = useState(null);
  const [message, setMessage] = useState("Ready");
  const [debugLogs, setDebugLogs] = useState([]);
  const [logEndpoint, setLogEndpoint] = useState("/api/logs");
  const sourceRef = useRef(null);

  useEffect(() => {
    const controller = new AbortController();
    fetch(`/api/placement?nodes=${initialSettings.nodes}&buckets=${initialSettings.buckets}&entities=${initialSettings.entities}`, {
      signal: controller.signal,
    })
      .then((response) => {
        if (!response.ok) throw new Error(`Placement request failed (${response.status})`);
        return response.json();
      })
      .then((payload) => {
        setPlacement(payload);
        const first = payload.nodes?.flatMap((node) => node.entities)?.[0];
        setSelectedAllocationKey(first ? allocationKey(first) : null);
      })
      .catch((error) => {
        if (error.name !== "AbortError") appendDebug("ERROR", error.message);
      });
    return () => {
      controller.abort();
      sourceRef.current?.close();
    };
  }, []);

  const hotBucket = latest?.buckets?.find((bucket) => bucket.id === latest.hotBucket);
  const progress = latest ? ((latest.windowId + 1) / latest.windowCount) * 100 : 0;
  const processedRequests = latest ? (latest.windowId + 1) * settings.requests : 0;
  const throughput = latest?.totalCompletionMs > 0
    ? processedRequests / (latest.totalCompletionMs / 1000)
    : 0;

  const eventRows = useMemo(() => history.slice(-7).reverse(), [history]);

  function appendDebug(level, text) {
    setDebugLogs((current) => [
      ...current,
      { time: new Date().toLocaleTimeString("en-GB"), level, text },
    ].slice(-80));
  }

  function changeSetting(event) {
    const { name, value } = event.target;
    setSettings((current) => ({ ...current, [name]: Number(value) }));
  }

  function selectPolicy(policy) {
    setSettings((current) => ({ ...current, policy }));
  }

  function stopRun() {
    sourceRef.current?.close();
    sourceRef.current = null;
    setStatus((current) => current === "running" ? "stopped" : current);
    setMessage("Stopped");
  }

  function startRun() {
    sourceRef.current?.close();
    setHistory([]);
    setLatest(null);
    setDebugLogs([]);
    setStatus("running");
    setMessage("Starting simulator");

    const params = new URLSearchParams(
      Object.entries(settings).map(([key, value]) => [key, String(value)]),
    );
    const source = new EventSource(`/api/simulate?${params.toString()}`);
    sourceRef.current = source;

    source.onmessage = (event) => {
      const payload = JSON.parse(event.data);
      if (payload.type === "started") {
        setLogEndpoint(payload.logEndpoint || "/api/logs");
        appendDebug("INFO", `Run ${payload.runId} started with child simulator`);
        appendDebug("INFO", `${payload.policy}, ${payload.nodes} nodes, ${payload.entities} entities`);
        setMessage(`${policyLabels[payload.policy]} · ${payload.nodes} nodes · ${payload.entities} entities`);
      } else if (payload.type === "placement") {
        setPlacement(payload);
        const allocated = payload.nodes?.flatMap((node) => node.entities) || [];
        setSelectedAllocationKey((current) => (
          allocated.some((entity) => allocationKey(entity) === current)
            ? current
            : allocated[0] ? allocationKey(allocated[0]) : null
        ));
        appendDebug(
          "INFO",
          `Allocated ${payload.entityCount} entities across ${payload.nodeCount} memory nodes (${formatBytes(payload.totalUsedBytes)})`,
        );
      } else if (payload.type === "window") {
        setLatest(payload);
        setHistory((current) => [...current, payload]);
        const copiedBucket = payload.decision?.bucketId;
        const copyMatchedHotspot = copiedBucket === payload.hotBucket;
        appendDebug(
          payload.copyApplied ? (copyMatchedHotspot ? "INFO" : "WARN") : "DEBUG",
          `W${payload.windowId + 1}: hot bucket ${payload.hotBucket}, load ${formatNumber(payload.maxLoadRatio * 100, 1)}%${payload.copyApplied ? `; copied bucket ${copiedBucket} to node ${payload.decision?.targetNode}${copyMatchedHotspot ? "" : " (stale prediction)"}` : ""}`,
        );
        setMessage(`Window ${payload.windowId + 1} of ${payload.windowCount}`);
      } else if (payload.type === "memory") {
        setPlacement(payload);
        const allocated = payload.nodes?.flatMap((node) => node.entities) || [];
        setSelectedAllocationKey((current) => {
          if (allocated.some((entity) => allocationKey(entity) === current)) return current;
          const firstPrimary = allocated.find((entity) => !entity.isReplica);
          return firstPrimary ? allocationKey(firstPrimary) : null;
        });
        if (payload.expiredReplicas?.length) {
          const locations = payload.expiredReplicas
            .map((replica) => `bucket ${replica.bucketId} on node ${replica.nodeId}`)
            .join(", ");
          appendDebug("DEBUG", `W${payload.windowId + 1}: expired ${locations}`);
        }
      } else if (payload.type === "complete") {
        appendDebug("INFO", `Completed with ${payload.copies ?? 0} copies in ${formatNumber(payload.totalCompletionMs, 2)} ms`);
        setStatus("complete");
        setMessage(`Complete · CSV saved to ${payload.csv || "results/dashboard"}`);
        source.close();
        sourceRef.current = null;
      } else if (payload.type === "error") {
        appendDebug("ERROR", payload.message);
        setStatus("error");
        setMessage(payload.message);
        source.close();
        sourceRef.current = null;
      }
    };

    source.onerror = () => {
      if (sourceRef.current === source && source.readyState === EventSource.CLOSED) {
        setStatus("error");
        setMessage("Connection to the simulation service was closed");
      }
    };
  }

  return (
    <div className="app-shell">
      <header className="topbar">
        <div className="brand-block">
          <Database size={22} />
          <div>
            <h1>Hot-Bucket Replication Monitor</h1>
            <span>Cost-aware KV workload simulator</span>
          </div>
        </div>
        <div className="run-state">
          <span className={`state-dot ${status}`} />
          <div>
            <strong>{status === "running" ? "RUNNING" : status.toUpperCase()}</strong>
            <span>{message}</span>
          </div>
          <span className="simulation-tag">SIMULATION</span>
        </div>
      </header>

      <div className="workspace">
        <aside className="control-panel">
          <div className="panel-title">
            <h2>Run configuration</h2>
            <span>Seed {settings.seed}</span>
          </div>

          <div className="control-group">
            <label className="group-label">Policy</label>
            <div className="segment-control">
              {Object.entries(policyLabels).map(([policy, label]) => (
                <button
                  key={policy}
                  type="button"
                  className={settings.policy === policy ? "active" : ""}
                  onClick={() => selectPolicy(policy)}
                  disabled={status === "running"}
                >
                  {label}
                </button>
              ))}
            </div>
          </div>

          <div className="field-grid">
            <NumberField label="Nodes" name="nodes" value={settings.nodes} min={2} max={16} onChange={changeSetting} disabled={status === "running"} />
            <NumberField label="Buckets" name="buckets" value={settings.buckets} min={8} max={512} onChange={changeSetting} disabled={status === "running"} />
            <NumberField label="Entities" name="entities" value={settings.entities} min={2} max={100} onChange={changeSetting} disabled={status === "running"} />
            <NumberField label="Windows" name="windows" value={settings.windows} min={5} max={200} onChange={changeSetting} disabled={status === "running"} />
            <NumberField label="Requests / window" name="requests" value={settings.requests} min={100} max={1000000} step={100} onChange={changeSetting} disabled={status === "running"} />
            <NumberField label="Node capacity" name="nodeCapacity" value={settings.nodeCapacity} min={100} max={1000000} step={100} onChange={changeSetting} disabled={status === "running"} />
            <NumberField label="Hotspot duration" name="hotspotDuration" value={settings.hotspotDuration} min={1} max={100} onChange={changeSetting} disabled={status === "running"} />
            <NumberField label="Severe threshold" name="threshold" value={settings.threshold} min={1} max={1000000} step={100} onChange={changeSetting} disabled={status === "running"} />
            <NumberField label="Seed" name="seed" value={settings.seed} min={0} max={4294967295} onChange={changeSetting} disabled={status === "running"} />
          </div>

          <div className="control-group range-group">
            <div className="range-label">
              <label htmlFor="delay">Playback delay</label>
              <span>{settings.delay} ms</span>
            </div>
            <input
              id="delay"
              name="delay"
              type="range"
              min="0"
              max="1000"
              step="50"
              value={settings.delay}
              onChange={changeSetting}
              disabled={status === "running"}
            />
          </div>

          {status === "running" ? (
            <button type="button" className="run-button stop" onClick={stopRun}>
              <Square size={17} fill="currentColor" /> Stop run
            </button>
          ) : (
            <button type="button" className="run-button" onClick={startRun}>
              <Play size={18} fill="currentColor" /> Start simulation
            </button>
          )}
        </aside>

        <main className="dashboard-main">
          <div className="progress-track"><div style={{ width: `${progress}%` }} /></div>

          <section className="metrics-row" aria-label="Current simulation metrics">
            <Metric icon={Flame} label="Current hot bucket" value={latest ? `#${latest.hotBucket}` : "-"} detail={hotBucket ? `${formatNumber(hotBucket.gets)} GET · Node ${hotBucket.homeNode}` : "No active window"} tone="red" />
            <Metric icon={Gauge} label="Maximum node load" value={latest ? `${formatNumber(latest.maxLoadRatio * 100, 1)}%` : "-"} detail={latest?.maxLoadRatio > 1 ? "Capacity exceeded" : "Within capacity"} tone={latest?.maxLoadRatio > 1 ? "red" : "green"} />
            <Metric icon={Activity} label="Simulated throughput" value={latest ? `${formatNumber(throughput / 1000, 1)}K` : "-"} detail="logical ops / second" tone="blue" />
            <Metric icon={Copy} label="Copies applied" value={latest ? formatNumber(latest.copies) : "-"} detail={latest ? `${formatNumber(latest.copyCostMs, 2)} ms current cost` : "No decisions"} tone="amber" />
          </section>

          <EntityMemoryAllocation
            placement={placement}
            selectedAllocationKey={selectedAllocationKey}
            onSelect={setSelectedAllocationKey}
            hotBucket={latest?.hotBucket}
          />

          <div className="dashboard-grid">
            <section className="panel node-panel">
              <div className="section-heading">
                <div><Server size={18} /><h2>Node load</h2></div>
                <span>Window {latest ? latest.windowId + 1 : 0}</span>
              </div>
              <NodeLoads nodes={latest?.nodes} />
            </section>

            <section className="panel decision-panel">
              <div className="section-heading">
                <div><Copy size={18} /><h2>Replication decision</h2></div>
                <span className={latest?.copyApplied ? "decision-applied" : "decision-idle"}>
                  {latest?.copyApplied ? "APPLIED" : "NO COPY"}
                </span>
              </div>
              {latest?.decision ? (
                <div className="decision-content">
                  <div className="decision-route">
                    <div><span>Bucket</span><strong>#{latest.decision.bucketId}</strong></div>
                    <span className="route-arrow">→</span>
                    <div><span>Target</span><strong>Node {latest.decision.targetNode}</strong></div>
                  </div>
                  <dl>
                    <div><dt>Predicted GET</dt><dd>{formatNumber(latest.decision.predictedGets)}</dd></div>
                    <div><dt>Expected benefit</dt><dd>{formatNumber(latest.decision.expectedBenefitMs, 2)} ms</dd></div>
                    <div><dt>Expected cost</dt><dd>{formatNumber(latest.decision.expectedCostMs, 2)} ms</dd></div>
                  </dl>
                  <p>{latest.decision.reason}</p>
                </div>
              ) : (
                <div className="empty-state decision-empty">The policy made no replication decision in this window</div>
              )}
            </section>

            <section className="panel chart-panel">
              <LineChart history={history} valueKey="maxLoadRatio" color="#d33d3d" label="Maximum node load" unit="×" />
            </section>

            <section className="panel chart-panel">
              <LineChart history={history} valueKey="windowCompletionMs" color="#1f6f8b" label="Window completion time" unit=" ms" />
            </section>

            <section className="panel bucket-panel">
              <div className="section-heading">
                <div><Database size={18} /><h2>Bucket activity</h2></div>
                <div className="legend">
                  <span><i className="legend-low" />Low</span>
                  <span><i className="legend-high" />High</span>
                  <span><i className="legend-hot" />Hot</span>
                </div>
              </div>
              <BucketMap buckets={latest?.buckets} hotBucket={latest?.hotBucket} />
            </section>

            <section className="panel event-panel">
              <div className="section-heading">
                <div><Clock3 size={18} /><h2>Recent windows</h2></div>
                <span>{history.length} received</span>
              </div>
              <div className="event-table-wrap">
                <table>
                  <thead><tr><th>Window</th><th>Hot bucket</th><th>Load</th><th>Copy</th><th>Time</th></tr></thead>
                  <tbody>
                    {eventRows.map((entry) => (
                      <tr key={entry.windowId}>
                        <td>W{entry.windowId + 1}</td>
                        <td>#{entry.hotBucket}</td>
                        <td className={entry.maxLoadRatio > 1 ? "danger-text" : ""}>{formatNumber(entry.maxLoadRatio * 100, 1)}%</td>
                        <td>{entry.copyApplied ? `→ N${entry.decision?.targetNode}` : "-"}</td>
                        <td>{formatNumber(entry.windowCompletionMs, 2)} ms</td>
                      </tr>
                    ))}
                    {!eventRows.length && <tr><td colSpan="5" className="table-empty">No windows received</td></tr>}
                  </tbody>
                </table>
              </div>
            </section>

            <section className="panel debug-panel">
              <div className="section-heading">
                <div><TerminalSquare size={18} /><h2>Debug log</h2></div>
                <a href={logEndpoint} target="_blank" rel="noreferrer">Open backend JSON</a>
              </div>
              <div className="debug-console" role="log" aria-live="polite">
                {debugLogs.map((entry, index) => (
                  <div className="debug-line" key={`${entry.time}-${index}`}>
                    <span className="log-time">{entry.time}</span>
                    <span className={`log-level ${entry.level.toLowerCase()}`}>{entry.level}</span>
                    <span>{entry.text}</span>
                  </div>
                ))}
                {!debugLogs.length && <div className="debug-empty">No backend events</div>}
              </div>
            </section>
          </div>
        </main>
      </div>
    </div>
  );
}

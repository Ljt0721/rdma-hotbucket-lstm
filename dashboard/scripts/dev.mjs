import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import path from "node:path";

const dashboardRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const serverPath = path.join(dashboardRoot, "server", "index.mjs");
const vitePath = path.join(dashboardRoot, "node_modules", "vite", "bin", "vite.js");

const children = [
  spawn(process.execPath, [serverPath], { cwd: dashboardRoot, stdio: "inherit" }),
  spawn(process.execPath, [vitePath, "--host", "127.0.0.1"], {
    cwd: dashboardRoot,
    stdio: "inherit",
  }),
];

let stopping = false;
function stop(exitCode = 0) {
  if (stopping) return;
  stopping = true;
  for (const child of children) {
    if (!child.killed) child.kill();
  }
  setTimeout(() => process.exit(exitCode), 100);
}

for (const child of children) {
  child.on("exit", (code) => {
    if (!stopping && code !== null && code !== 0) stop(code);
  });
  child.on("error", (error) => {
    console.error(error.message);
    stop(1);
  });
}

process.on("SIGINT", () => stop(0));
process.on("SIGTERM", () => stop(0));

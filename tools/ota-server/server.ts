#!/usr/bin/env bun
/**
 * Moki OTA-Server — serviert die zuletzt gebaute firmware.bin auf
 * deinem LAN für Remote-Updates auf Production-Mokis.
 *
 * Benutzt Bun. Läuft minimal:
 *   bun run server.ts
 *
 * Endpoints:
 *   GET /version.json   → { version, size, sha256, build_time }
 *   GET /firmware.bin   → die rohe Firmware
 *   GET /map           → moki.map (vorerst statisch aus firmware/data/)
 *
 * Das Moki ruft dann z.B.:
 *   serial> wifi_set MyHomeWifi MeinPasswort
 *   serial> ota http://192.168.1.42:8080/firmware.bin
 *
 * Server rebuildet NICHT — er reicht nur das aktuell auf Disk liegende
 * Build durch. Du baust manuell mit `pio run` und der Server greift dann
 * automatisch auf die neue Datei zu.
 */

import { createHash } from "node:crypto";
import { statSync } from "node:fs";
import { join } from "node:path";

const ROOT = join(import.meta.dir, "..", "..");
const FIRMWARE_PATH = join(ROOT, "firmware", ".pio", "build", "t5_s3_pro", "firmware.bin");
const MAP_PATH = join(ROOT, "firmware", "data", "moki.map");
const PORT = Number(process.env.PORT ?? 8080);

function getLanIPs(): string[] {
  // os.networkInterfaces() — pick non-internal IPv4 entries
  const os = require("node:os");
  const nets = os.networkInterfaces();
  const ips: string[] = [];
  for (const name of Object.keys(nets)) {
    for (const net of nets[name] ?? []) {
      if (net.family === "IPv4" && !net.internal) ips.push(net.address);
    }
  }
  return ips;
}

async function buildVersionInfo() {
  const file = Bun.file(FIRMWARE_PATH);
  if (!(await file.exists())) {
    return null;
  }
  const stat = statSync(FIRMWARE_PATH);
  const buf = new Uint8Array(await file.arrayBuffer());
  const sha = createHash("sha256").update(buf).digest("hex");
  return {
    size: stat.size,
    sha256: sha,
    build_time: stat.mtime.toISOString(),
    path: FIRMWARE_PATH,
  };
}

const server = Bun.serve({
  port: PORT,
  hostname: "0.0.0.0",
  async fetch(req) {
    const url = new URL(req.url);
    const path = url.pathname;
    const remote = req.headers.get("x-forwarded-for") ?? "?";

    if (path === "/" || path === "/health") {
      return Response.json({ ok: true, service: "moki-ota" });
    }

    if (path === "/version.json") {
      const info = await buildVersionInfo();
      if (!info) {
        return new Response("firmware.bin not found — run `pio run` first", { status: 404 });
      }
      console.log(`[ota] /version.json → ${info.size}b sha=${info.sha256.slice(0,16)} (${remote})`);
      return Response.json({
        size: info.size,
        sha256: info.sha256,
        build_time: info.build_time,
      });
    }

    if (path === "/firmware.bin") {
      const file = Bun.file(FIRMWARE_PATH);
      if (!(await file.exists())) {
        return new Response("firmware.bin not found — run `pio run` first", { status: 404 });
      }
      const stat = statSync(FIRMWARE_PATH);
      console.log(`[ota] /firmware.bin → ${stat.size} bytes (${remote})`);
      return new Response(file, {
        headers: {
          "Content-Type": "application/octet-stream",
          "Content-Length": String(stat.size),
        },
      });
    }

    if (path === "/map") {
      const file = Bun.file(MAP_PATH);
      if (!(await file.exists())) {
        return new Response("moki.map not found", { status: 404 });
      }
      const stat = statSync(MAP_PATH);
      console.log(`[ota] /map → ${stat.size} bytes (${remote})`);
      return new Response(file, {
        headers: {
          "Content-Type": "application/octet-stream",
          "Content-Length": String(stat.size),
        },
      });
    }

    return new Response("not found", { status: 404 });
  },
});

console.log(`\n┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓`);
console.log(`┃  Moki OTA-Server  ·  port ${PORT}                            ┃`);
console.log(`┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛`);
console.log(`\nFirmware: ${FIRMWARE_PATH}`);
const info = await buildVersionInfo();
if (info) {
  console.log(`  ${info.size} bytes · sha=${info.sha256.slice(0, 16)}... · ${info.build_time}`);
} else {
  console.log(`  ⚠️  firmware.bin not yet built — run \`pio run\` in firmware/`);
}

console.log(`\nServing on:`);
const ips = getLanIPs();
for (const ip of ips) {
  console.log(`  http://${ip}:${PORT}`);
}
console.log(`\nOn Moki, type via Serial:`);
console.log(`  wifi_set <YOUR_SSID> <YOUR_PSK>`);
console.log(`  wifi_status               # wait for IP`);
if (ips.length) {
  console.log(`  ota http://${ips[0]}:${PORT}/firmware.bin`);
}
console.log(``);

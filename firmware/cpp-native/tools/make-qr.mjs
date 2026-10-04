#!/usr/bin/env node
/*
 * Spectr sensor setup label / QR generator.
 *
 * Produces a printable label (HTML with two QR codes) for a sensor:
 *   1. Join code  — Wi-Fi QR that joins the `Spectr-XXXX` setup hotspot.
 *   2. Claim code — `spectr://claim?device_id=...` for the app's add-sensor flow.
 *
 * Usage:
 *   node tools/make-qr.mjs --device-id a1b2c3d4e5f60718 [--ap-password K7MQP4XR9T]
 *                          [--name "Living room"] [--out labels]
 *
 * The hotspot password is generated on the device at first boot and printed on
 * the serial console (`Setup mode: hotspot 'Spectr-XXXX' password '...'`).
 * Pass it here to print a matching label, or omit it to plan with a placeholder.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
import { mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";

const ALPHABET = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";

function parseArgs(argv) {
  const args = { out: "labels" };
  for (let i = 0; i < argv.length; i += 1) {
    const key = argv[i];
    if (!key.startsWith("--")) continue;
    const name = key.slice(2);
    const value = argv[i + 1] && !argv[i + 1].startsWith("--") ? argv[++i] : "true";
    args[name] = value;
  }
  return args;
}

function randomPassword() {
  let out = "";
  for (let i = 0; i < 10; i += 1) {
    out += ALPHABET[Math.floor(Math.random() * ALPHABET.length)];
  }
  return out;
}

function apSsid(deviceId) {
  return `Spectr-${deviceId.slice(-4).toUpperCase()}`;
}

function escapeWifi(value) {
  return String(value).replace(/([\\;,:"])/g, "\\$1");
}

function escapeHtml(value) {
  return String(value).replace(/[&<>"']/g, (c) => (
    { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]
  ));
}

async function qrSvg(text) {
  let qrcode;
  try {
    qrcode = await import("qrcode");
  } catch {
    console.error(
      "This tool needs the 'qrcode' package.\n" +
        "  cd firmware/spectr-node/tools && npm install qrcode\n" +
        "Then run it again."
    );
    process.exit(1);
  }
  return qrcode.toString(text, { type: "svg", margin: 1, width: 220, errorCorrectionLevel: "M" });
}

const args = parseArgs(process.argv.slice(2));
const deviceId = String(args["device-id"] || "").trim().toLowerCase();
if (!/^[0-9a-f]{16}$/.test(deviceId)) {
  console.error("--device-id must be 16 lowercase hex characters (from the device serial log)");
  process.exit(1);
}

const password = String(args["ap-password"] || "").trim() || randomPassword();
const generated = !args["ap-password"];
const name = String(args.name || "").trim();
const ssid = apSsid(deviceId);
const joinPayload = `WIFI:T:WPA;S:${escapeWifi(ssid)};P:${escapeWifi(password)};;`;
const claimPayload = `spectr://claim?device_id=${deviceId}`;

const [joinQr, claimQr] = await Promise.all([qrSvg(joinPayload), qrSvg(claimPayload)]);

mkdirSync(args.out, { recursive: true });
const base = `spectr-${deviceId}`;
writeFileSync(join(args.out, `${base}-join.svg`), joinQr);
writeFileSync(join(args.out, `${base}-claim.svg`), claimQr);

const label = `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8" />
<title>Spectr label ${escapeHtml(deviceId)}</title>
<style>
  body { font-family: ui-sans-serif, system-ui, "Segoe UI", sans-serif; background: #f6f5f3; margin: 0; padding: 24px; }
  .label { width: 720px; background: #fff; border: 1px solid #e6e3df; border-radius: 14px; padding: 28px; margin: 0 auto; }
  h1 { font-size: 20px; margin: 0 0 4px; color: #17181a; }
  p.sub { margin: 0 0 20px; color: #8a8f98; font-size: 13px; }
  .row { display: flex; gap: 28px; }
  .col { flex: 1; }
  .col h2 { font-size: 11px; text-transform: uppercase; letter-spacing: 0.14em; color: #8a8f98; margin: 0 0 8px; }
  .qr { width: 220px; height: 220px; }
  .kv { font-size: 13px; color: #4d5158; margin: 4px 0; }
  code { font-family: ui-monospace, Menlo, monospace; font-size: 13px; color: #17181a; }
  ol { font-size: 13px; color: #4d5158; padding-left: 18px; margin: 16px 0 0; }
  .note { margin-top: 18px; font-size: 12px; color: #8a8f98; }
  .signal { width: 22px; height: 22px; }
</style>
</head>
<body>
  <div class="label">
    <h1>Spectr sensor</h1>
    <p class="sub">${name ? escapeHtml(name) + " · " : ""}Device ID <code>${escapeHtml(deviceId)}</code></p>
    <div class="row">
      <div class="col">
        <h2>1 · Join the sensor hotspot</h2>
        <div class="qr">${joinQr}</div>
        <p class="kv">Hotspot: <code>${escapeHtml(ssid)}</code></p>
        <p class="kv">Password: <code>${escapeHtml(password)}</code>${generated ? " (placeholder)" : ""}</p>
      </div>
      <div class="col">
        <h2>2 · Claim it in the app</h2>
        <div class="qr">${claimQr}</div>
        <p class="kv">Scan in the Spectr app → Add sensor.</p>
        <p class="kv">Or enter the device ID manually.</p>
      </div>
    </div>
    <ol>
      <li>Plug in the sensor and wait for the <code>${escapeHtml(ssid)}</code> hotspot (about 20 seconds).</li>
      <li>Join the hotspot, open <code>http://192.168.4.1/</code>, choose your Wi-Fi and enter its password.</li>
      <li>The sensor restarts, connects and starts sensing. Claim it in the app.</li>
    </ol>
    <p class="note">The hotspot password is generated on the device at first boot; the serial console prints the real value. Keep this label out of sight of the hotspot itself if the password matters to you.</p>
  </div>
</body>
</html>
`;
writeFileSync(join(args.out, `${base}-label.html`), label);

console.log(`Device ID : ${deviceId}`);
console.log(`Hotspot   : ${ssid}`);
console.log(`Password  : ${password}${generated ? " (generated placeholder — real value is on the device serial)" : ""}`);
console.log(`Join QR   : ${joinPayload}`);
console.log(`Claim QR  : ${claimPayload}`);
console.log(`Written to: ${args.out}/${base}-join.svg, ${base}-claim.svg, ${base}-label.html`);

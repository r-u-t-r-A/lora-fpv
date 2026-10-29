// SPDX-License-Identifier: GPL-3.0-or-later
// GUI entry: connect to every VRX, merge their packets in one reassembler
// (first good copy of each shard wins), decode, draw video + OSD + stats.
//
// URL parameters (all optional):
//   vrx=ws://host:port/rx,...   VRX list (default: probe 10.55.1.1 .. 10.55.8.1)
//   bind=PHRASE                 bind phrase (default: from the first VRX's config)
//   fps=N                       frame rate for timestamps (default: from the mode)
import { HevcDecoder } from "./decoder.ts";
import { drawOsd } from "./osd.ts";
import { linkId, parseOta, OTA_TYPE_VIDEO, type RxPacket } from "./proto.ts";
import { PUSH_DUP, PUSH_LATE, PUSH_NEW, Reassembler } from "./reasm.ts";
import { discover, VrxLink, type VrxConfig } from "./vrx.ts";

const params = new URLSearchParams(location.search);
const canvas = document.querySelector<HTMLCanvasElement>("#video")!;
const ctx = canvas.getContext("2d")!;
const statsEl = document.querySelector<HTMLDivElement>("#stats")!;
const logEl = document.querySelector<HTMLPreElement>("#log")!;

const MODE_FPS: Record<string, number> = { lr520: 30, lr260: 30, hq2600: 60, hq1300: 30 };

function log(msg: string): void {
  const line = `${new Date().toLocaleTimeString()} ${msg}\n`;
  logEl.textContent = (line + (logEl.textContent ?? "")).slice(0, 8000);
}

function nowUs(): number {
  return Math.round(performance.now() * 1000);
}

async function vrxConfig(wsUrl: string): Promise<VrxConfig | null> {
  try {
    const u = new URL(wsUrl);
    u.protocol = u.protocol === "wss:" ? "https:" : "http:";
    u.pathname = "/api/config";
    const r = await fetch(u);
    return r.ok ? ((await r.json()) as VrxConfig) : null;
  } catch {
    return null;
  }
}

async function main(): Promise<void> {
  if (!window.isSecureContext)
    log("not a secure context: WebCodecs is unavailable. Use localhost, or add this origin under chrome://flags/#unsafely-treat-insecure-origin-as-secure");
  if (!HevcDecoder.supported()) log("WebCodecs VideoDecoder not available in this browser");

  let urls = (params.get("vrx") ?? "").split(",").map((s) => s.trim()).filter(Boolean);
  let config: VrxConfig | null = null;
  if (urls.length === 0) {
    log("probing 10.55.1.1 .. 10.55.8.1");
    const found = await discover();
    urls = found.map((f) => f.wsUrl);
    config = found[0]?.config ?? null;
  } else {
    config = await vrxConfig(urls[0]!);
  }
  if (urls.length === 0) {
    log("no VRX found. Plug one in, or pass ?vrx=ws://127.0.0.1:8081/rx (see sim/fake_vrx.sh)");
  }
  const bind = params.get("bind") ?? config?.bind ?? "open-lr-video";
  const fps = Number(params.get("fps")) || MODE_FPS[config?.mode ?? ""] || 30;
  log(`bind "${bind}" (link id ${linkId(bind)}), ${fps} fps, ${urls.length} VRX`);

  const reasm = await Reassembler.load(new URL("./olv.wasm", location.href).toString());
  reasm.init(linkId(bind), 2, 100_000);

  let decodedThisSecond = 0;
  let fpsShown = 0;
  let framesIn = 0;
  let framesComplete = 0;
  const decoder = new HevcDecoder(
    () => fps,
    (f) => {
      if (canvas.width !== f.displayWidth || canvas.height !== f.displayHeight) {
        canvas.width = f.displayWidth;
        canvas.height = f.displayHeight;
      }
      ctx.drawImage(f, 0, 0);
      f.close();
      decodedThisSecond++;
      osd();
    },
  );

  // Decoder feeding must stay in order; configure() is async.
  let chain: Promise<void> = Promise.resolve();
  const drain = (): void => {
    for (const f of reasm.frames()) {
      framesIn++;
      if (f.complete) framesComplete++;
      chain = chain.then(() => decoder.feed(f));
    }
  };

  const onPacket = (link: VrxLink, pkt: RxPacket): void => {
    const h = parseOta(pkt.ota);
    if (!h || h.type !== OTA_TYPE_VIDEO) return;
    const r = reasm.push(pkt.ota, nowUs());
    if (r === PUSH_NEW) link.stats.firstCopies++;
    else if (r === PUSH_DUP || r === PUSH_LATE) link.stats.dups++;
    drain();
  };
  const links = urls.map((u) => new VrxLink(u, onPacket, (l, t) => log(`${l.label}: ${t}`)));

  const osd = (): void => {
    const total = links.reduce((a, l) => a + l.stats.firstCopies, 0) || 1;
    drawOsd(ctx, canvas.width, canvas.height, {
      fps: fpsShown,
      completePct: framesIn ? (100 * framesComplete) / framesIn : 0,
      vrx: links.map((l) => ({ label: l.label, rssi: l.stats.rssiAvg, share: l.stats.firstCopies / total, up: l.connected })),
      message: links.some((l) => l.connected) ? (decoder.stats.waitingForKey ? "waiting for key frame" : null) : "no VRX connected",
    });
  };

  setInterval(() => {
    reasm.tick(nowUs());
    drain();
  }, 20);
  setInterval(() => {
    fpsShown = decodedThisSecond;
    decodedThisSecond = 0;
    if (decoder.stats.decoded === 0) {
      ctx.fillStyle = "#111";
      ctx.fillRect(0, 0, canvas.width, canvas.height);
      osd();
    }
  }, 1000);
  setInterval(() => renderStats(links, reasm, decoder), 500);
}

const SYNC = ["parked", "locked", "coasting"];

function renderStats(links: VrxLink[], reasm: Reassembler, dec: HevcDecoder): void {
  const s = reasm.stats();
  const total = links.reduce((a, l) => a + l.stats.firstCopies, 0) || 1;
  const rows = links
    .map((l) => {
      const st = l.status;
      return `<tr><td>${l.label}</td><td>${l.connected ? "up" : "down"}</td><td>${st ? SYNC[st.syncState] ?? "?" : "-"}</td>
        <td>${l.stats.pkts}</td><td>${((100 * l.stats.firstCopies) / total).toFixed(0)}%</td><td>${l.stats.dups}</td>
        <td>${l.stats.rssiAvg?.toFixed(0) ?? "-"}</td><td>${l.stats.snrAvg?.toFixed(1) ?? "-"}</td>
        <td>${st ? st.pktsCrc : "-"}</td><td>${st ? st.pktsDrop : "-"}</td></tr>`;
    })
    .join("");
  const frames = s.framesComplete + s.framesPartial + s.framesLost || 1;
  statsEl.innerHTML = `
    <table><thead><tr><th>VRX</th><th>link</th><th>sync</th><th>pkts</th><th>first copy</th><th>dups</th>
      <th>RSSI</th><th>SNR</th><th>CRC err</th><th>WS drop</th></tr></thead><tbody>${rows}</tbody></table>
    <p>merged: frames ${(100 * s.framesComplete / frames).toFixed(1)}% complete, ${(100 * s.framesPartial / frames).toFixed(1)}% partial,
      ${(100 * s.framesLost / frames).toFixed(1)}% lost; blocks ${s.blocksOk} ok / ${s.blocksLost} lost; late ${s.late}, bad ${s.bad}</p>
    <p>decoder: ${dec.stats.codec ?? "not configured"}, ${dec.stats.decoded} frames, ${dec.stats.errors} errors
      ${dec.stats.lastError ? `(${dec.stats.lastError.replace(/</g, "&lt;")})` : ""}</p>`;
}

main().catch((e: unknown) => log(`fatal: ${e instanceof Error ? e.message : String(e)}`));

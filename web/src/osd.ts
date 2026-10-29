// SPDX-License-Identifier: GPL-3.0-or-later
// On-screen display drawn over the video. Link quality now; MAVLink summary
// telemetry (type 1 packets) will be added here.

export interface OsdState {
  fps: number;
  completePct: number;
  vrx: { label: string; rssi: number | null; share: number; up: boolean }[];
  message: string | null;
}

export function drawOsd(ctx: CanvasRenderingContext2D, w: number, h: number, s: OsdState): void {
  const scale = Math.max(1, h / 480);
  ctx.save();
  ctx.font = `${Math.round(14 * scale)}px ui-monospace, monospace`;
  ctx.textBaseline = "top";
  const lines = [
    `${s.fps.toFixed(0)} fps  ${s.completePct.toFixed(0)}% complete`,
    ...s.vrx.map((v) => `${v.label}  ${v.up ? (v.rssi === null ? "--" : `${v.rssi.toFixed(0)} dBm`) : "down"}  ${(v.share * 100).toFixed(0)}%`),
  ];
  const pad = 6 * scale;
  const lh = 18 * scale;
  const boxW = Math.max(...lines.map((l) => ctx.measureText(l).width)) + 2 * pad;
  ctx.fillStyle = "rgba(0,0,0,0.45)";
  ctx.fillRect(pad, pad, boxW, lines.length * lh + pad);
  ctx.fillStyle = "#fff";
  lines.forEach((l, i) => ctx.fillText(l, 2 * pad, pad * 1.5 + i * lh));
  if (s.message) {
    ctx.font = `${Math.round(20 * scale)}px system-ui, sans-serif`;
    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    ctx.fillStyle = "rgba(0,0,0,0.5)";
    const mw = ctx.measureText(s.message).width + 4 * pad;
    ctx.fillRect(w / 2 - mw / 2, h / 2 - 20 * scale, mw, 40 * scale);
    ctx.fillStyle = "#fff";
    ctx.fillText(s.message, w / 2, h / 2);
  }
  ctx.restore();
}

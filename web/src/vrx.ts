// SPDX-License-Identifier: GPL-3.0-or-later
// One WebSocket connection to one VRX, with reconnect and per-VRX stats.
import { parseMessage, type RxPacket, type VrxStatus } from "./proto.ts";

export interface VrxStats {
  msgs: number;
  pkts: number;
  firstCopies: number; // shards this VRX delivered before any other
  dups: number;
  rssiAvg: number | null;
  snrAvg: number | null;
}

export class VrxLink {
  ws: WebSocket | null = null;
  connected = false;
  status: VrxStatus | null = null;
  readonly stats: VrxStats = { msgs: 0, pkts: 0, firstCopies: 0, dups: 0, rssiAvg: null, snrAvg: null };
  private retryMs = 500;
  private closed = false;

  constructor(
    readonly url: string,
    private readonly onPacket: (link: VrxLink, pkt: RxPacket) => void,
    private readonly onLog: (link: VrxLink, text: string) => void,
  ) {
    this.open();
  }

  get label(): string {
    return this.url.replace(/^wss?:\/\//, "").replace(/\/rx$/, "");
  }

  close(): void {
    this.closed = true;
    this.ws?.close();
  }

  private open(): void {
    const ws = new WebSocket(this.url);
    ws.binaryType = "arraybuffer";
    this.ws = ws;
    ws.onopen = () => {
      this.connected = true;
      this.retryMs = 500;
      this.onLog(this, "connected");
    };
    ws.onclose = () => {
      const was = this.connected;
      this.connected = false;
      if (was) this.onLog(this, "disconnected");
      if (!this.closed) {
        setTimeout(() => this.open(), this.retryMs);
        this.retryMs = Math.min(this.retryMs * 2, 5000);
      }
    };
    ws.onmessage = (ev: MessageEvent<ArrayBuffer>) => {
      this.stats.msgs++;
      for (const rec of parseMessage(new Uint8Array(ev.data))) {
        if (rec.kind === "rx") {
          this.stats.pkts++;
          const a = 0.05;
          this.stats.rssiAvg = this.stats.rssiAvg === null ? rec.pkt.rssi : this.stats.rssiAvg + a * (rec.pkt.rssi - this.stats.rssiAvg);
          this.stats.snrAvg = this.stats.snrAvg === null ? rec.pkt.snr : this.stats.snrAvg + a * (rec.pkt.snr - this.stats.snrAvg);
          this.onPacket(this, rec.pkt);
        } else if (rec.kind === "status") {
          this.status = rec.status;
        } else {
          this.onLog(this, rec.text);
        }
      }
    };
  }
}

export interface VrxConfig {
  id: number;
  band: string; // "auto" | "868" | "2400": bands the VRX scans
  mode: string; // "auto" or a mode name
  active_mode?: string; // mode the VRX is receiving (or trying) now
  bind: string;
  link_id: number;
}

// Probes http://10.55.N.1:8080/api/config for N = 1..8 in parallel.
export async function discover(timeoutMs = 800): Promise<{ wsUrl: string; config: VrxConfig }[]> {
  const probes = Array.from({ length: 8 }, async (_, i) => {
    const base = `10.55.${i + 1}.1:8080`;
    const ctl = new AbortController();
    const t = setTimeout(() => ctl.abort(), timeoutMs);
    try {
      const r = await fetch(`http://${base}/api/config`, { signal: ctl.signal });
      if (!r.ok) return null;
      const config = (await r.json()) as VrxConfig;
      return { wsUrl: `ws://${base}/rx`, config };
    } catch {
      return null;
    } finally {
      clearTimeout(t);
    }
  });
  return (await Promise.all(probes)).filter((x): x is { wsUrl: string; config: VrxConfig } => x !== null);
}

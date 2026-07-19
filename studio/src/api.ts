// The aero-api client — the ONLY path from the Studio to the runtime (013 T2, 015 U4). Every runtime
// action and all device contact goes through here; the browser never touches a device directly.
// Endpoints mirror include/aero/api/rest_api.hpp.

import type { Application } from "./application";

export interface StatusSnapshot {
  deployed: boolean;
  name?: string;
  version?: string;
  actor_key?: number;
  frames_processed?: number;
  events_published?: number;
  last_output?: number;
  has_driver?: boolean;
  [k: string]: unknown;
}

export interface ApiResult {
  ok: boolean;
  status: number;
  body: unknown;
}

export type Fetcher = typeof fetch;

export class AeroApi {
  // Default to a wrapper arrow, NOT the bare `fetch`: native fetch must be invoked with `this` ===
  // the global (window). Storing it as a member and calling `this.fetcher(...)` would rebind `this`
  // to the AeroApi instance → "Illegal invocation". Tests inject a plain mock (no `this` need).
  constructor(private base = "/api", private fetcher: Fetcher = (input, init) => fetch(input, init)) {}

  private async req(method: string, path: string, body?: unknown): Promise<ApiResult> {
    const res = await this.fetcher(`${this.base}${path}`, {
      method,
      headers: body !== undefined ? { "Content-Type": "application/json" } : undefined,
      body: body !== undefined ? JSON.stringify(body) : undefined,
    });
    let parsed: unknown = null;
    const text = await res.text();
    try {
      parsed = text ? JSON.parse(text) : null;
    } catch {
      parsed = text;
    }
    return { ok: res.ok, status: res.status, body: parsed };
  }

  health(): Promise<ApiResult> { return this.req("GET", "/health"); }
  deploy(app: Application): Promise<ApiResult> { return this.req("POST", "/apps", app); }
  status(): Promise<ApiResult> { return this.req("GET", "/status"); }
  listApps(): Promise<ApiResult> { return this.req("GET", "/apps"); }
  undeploy(name: string): Promise<ApiResult> { return this.req("DELETE", `/apps/${encodeURIComponent(name)}`); }
  reload(name: string, app: Application): Promise<ApiResult> {
    return this.req("PUT", `/apps/${encodeURIComponent(name)}`, app);
  }
  rollback(name: string): Promise<ApiResult> {
    return this.req("POST", `/apps/${encodeURIComponent(name)}/rollback`);
  }

  // Live metrics via SSE (013 §5). Returns an unsubscribe fn. Uses EventSource if present.
  subscribeMetrics(onSnapshot: (s: StatusSnapshot) => void): () => void {
    if (typeof EventSource === "undefined") return () => {};
    const es = new EventSource(`${this.base}/metrics/stream`);
    es.onmessage = (ev) => {
      try { onSnapshot(JSON.parse(ev.data)); } catch { /* ignore malformed frame */ }
    };
    return () => es.close();
  }

  // Runtime-assisted discovery (015 §5): browsing an OPC UA address space / testing an MQTT
  // connection needs the runtime + a live device — which only the edge node can reach. The browser
  // NEVER dials a device. Offline there is no connected runtime+device, so this is an honest gate,
  // not a fake browse. When wired, it POSTs to aero-api which forwards to the driver's browse/test.
  async discover(_driverTypeId: string): Promise<{ available: false; notice: string }> {
    return {
      available: false,
      notice:
        "Runtime-assisted discovery requires a connected runtime + device. The Studio never dials a " +
        "device directly (015 §5, U4); wire aero-api → runtime → driver.browse()/test_connect().",
    };
  }
}

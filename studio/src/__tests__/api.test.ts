// The API client builds correct aero-api requests and routes ALL runtime contact through itself
// (013 T2). Uses a mock fetch — no live daemon. Also asserts the discovery gate (015 §5 / U4).
import { describe, it, expect, vi } from "vitest";
import { AeroApi } from "../api";
import type { Application } from "../application";

const app: Application = {
  name: "demo", version: "1.0.0", actor: { kind: "edge", key: 7 },
  flow: [{ type_id: "aero.source.decode" }, { type_id: "aero.output.sum" }],
};

function mockFetch(status = 200, body: unknown = { ok: true }) {
  return vi.fn(async (_url: string, _init?: RequestInit) =>
    new Response(JSON.stringify(body), { status }));
}

describe("AeroApi request building", () => {
  it("POSTs /apps with the Application body", async () => {
    const f = mockFetch();
    const r = await new AeroApi("/api", f as unknown as typeof fetch).deploy(app);
    expect(r.ok).toBe(true);
    const [url, init] = f.mock.calls[0];
    expect(url).toBe("/api/apps");
    expect(init!.method).toBe("POST");
    expect(JSON.parse(init!.body as string)).toEqual(app);
  });

  it("uses the right verbs + urls for status/reload/rollback/undeploy", async () => {
    const f = mockFetch();
    const api = new AeroApi("/api", f as unknown as typeof fetch);
    await api.status();
    await api.reload("demo", app);
    await api.rollback("demo");
    await api.undeploy("demo");
    const calls = f.mock.calls.map((c) => [c[1]?.method, c[0]]);
    expect(calls).toEqual([
      ["GET", "/api/status"],
      ["PUT", "/api/apps/demo"],
      ["POST", "/api/apps/demo/rollback"],
      ["DELETE", "/api/apps/demo"],
    ]);
  });

  it("surfaces non-2xx as ok:false with the parsed body", async () => {
    const f = mockFetch(400, { error: "invalid expression" });
    const r = await new AeroApi("/api", f as unknown as typeof fetch).deploy(app);
    expect(r.ok).toBe(false);
    expect(r.status).toBe(400);
    expect(r.body).toEqual({ error: "invalid expression" });
  });

  it("gates runtime-assisted discovery (browser never dials a device)", async () => {
    const r = await new AeroApi("/api", mockFetch() as unknown as typeof fetch).discover("aero.driver.generator");
    expect(r.available).toBe(false);
    expect(r.notice).toMatch(/runtime \+ device/i);
  });
});

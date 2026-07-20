// FlowsPage (016 §1 correction): Runtime hosts one Application, so Deploy must route to POST /apps
// the first time and PUT /apps/{name} (reload) afterward -- picking up "already deployed" both from
// an initial GET /status on mount AND from a successful Deploy click. Rollback/Undeploy POST/DELETE
// the right routes and are disabled until something is deployed.
import { describe, it, expect, vi, afterEach } from "vitest";
import { render, screen, waitFor, fireEvent } from "@testing-library/react";
import { FlowsPage } from "../pages/FlowsPage";

function mockFetch(statusBody: unknown) {
  const calls: { url: string; method: string; body?: unknown }[] = [];
  const g = globalThis as unknown as { fetch: unknown };
  g.fetch = vi.fn(async (url: string, init?: RequestInit) => {
    const method = init?.method ?? "GET";
    calls.push({ url, method, body: init?.body ? JSON.parse(init.body as string) : undefined });
    if (url === "/api/status" && method === "GET") {
      return new Response(JSON.stringify(statusBody), { status: 200 });
    }
    return new Response(JSON.stringify({ deployed: true, name: "hello_flow" }), { status: 200 });
  });
  return calls;
}

describe("FlowsPage", () => {
  afterEach(() => vi.restoreAllMocks());

  it("first Deploy click POSTs /apps when nothing is deployed yet", async () => {
    const calls = mockFetch({ deployed: false });
    render(<FlowsPage />);
    await waitFor(() => expect(screen.getByRole("button", { name: "Deploy" })).toBeTruthy());

    fireEvent.click(screen.getByRole("button", { name: "Deploy" }));
    await waitFor(() => expect(calls.some((c) => c.method === "POST" && c.url === "/api/apps")).toBe(true));
  });

  it("picks up an already-deployed app from GET /status and reloads (PUT) instead of POSTing again", async () => {
    const calls = mockFetch({ deployed: true, name: "hello_flow", version: "0.1.0" });
    render(<FlowsPage />);
    await waitFor(() => expect(screen.getByRole("button", { name: /Redeploy/ })).toBeTruthy());

    fireEvent.click(screen.getByRole("button", { name: /Redeploy/ }));
    await waitFor(() =>
      expect(calls.some((c) => c.method === "PUT" && c.url === "/api/apps/hello_flow")).toBe(true));
    expect(calls.some((c) => c.method === "POST" && c.url === "/api/apps")).toBe(false);
  });

  it("Rollback/Undeploy are disabled until something is deployed, then call the right routes", async () => {
    const calls = mockFetch({ deployed: false });
    render(<FlowsPage />);
    await waitFor(() => {
      const rollback = screen.getByRole("button", { name: "Rollback" }) as HTMLButtonElement;
      expect(rollback.disabled).toBe(true);
    });

    fireEvent.click(screen.getByRole("button", { name: "Deploy" }));
    await waitFor(() => {
      const rollback = screen.getByRole("button", { name: "Rollback" }) as HTMLButtonElement;
      expect(rollback.disabled).toBe(false);
    });

    fireEvent.click(screen.getByRole("button", { name: "Rollback" }));
    await waitFor(() =>
      expect(calls.some((c) => c.method === "POST" && c.url === "/api/apps/hello_flow/rollback")).toBe(true));

    fireEvent.click(screen.getByRole("button", { name: "Undeploy" }));
    await waitFor(() =>
      expect(calls.some((c) => c.method === "DELETE" && c.url === "/api/apps/hello_flow")).toBe(true));
    await waitFor(() => {
      const rollback = screen.getByRole("button", { name: "Rollback" }) as HTMLButtonElement;
      expect(rollback.disabled).toBe(true);
    });
  });
});

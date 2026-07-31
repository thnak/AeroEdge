// BrokerPage (017 §1/§2/M4): renders the unconfigured hint honestly, and renders the listen port
// once GET /broker/status returns real data. Mocks global fetch (AeroApi's default fetcher) — no
// live daemon.
import { describe, it, expect, vi, afterEach } from "vitest";
import { render, screen, waitFor } from "@testing-library/react";
import { BrokerPage } from "../pages/BrokerPage";

function mockFetchOnce(body: unknown, status = 200) {
  const g = globalThis as unknown as { fetch: unknown };
  g.fetch = vi.fn(async () => new Response(JSON.stringify(body), { status }));
}

describe("BrokerPage", () => {
  afterEach(() => vi.restoreAllMocks());

  it("shows the unconfigured hint honestly, no crash", async () => {
    mockFetchOnce({ configured: false });
    render(<BrokerPage />);
    await waitFor(() => expect(screen.getByText(/No native broker configured/)).toBeTruthy());
    expect(screen.getByText(/--broker-port/)).toBeTruthy();
  });

  it("renders the listen port once configured", async () => {
    mockFetchOnce({ configured: true, listen_port: 1883 });
    render(<BrokerPage />);
    await waitFor(() => expect(screen.getByText("1883")).toBeTruthy());
    expect(screen.getByText("Listen port")).toBeTruthy();
  });
});

// FleetPage (016 §2.1): renders the unconfigured hint honestly, and renders nodes/devices/placement
// once GET /fleet returns real data. Mocks global fetch (AeroApi's default fetcher) — no live daemon.
import { describe, it, expect, vi, afterEach } from "vitest";
import { render, screen, waitFor } from "@testing-library/react";
import { FleetPage } from "../pages/FleetPage";

function mockFetchOnce(body: unknown, status = 200) {
  const g = globalThis as unknown as { fetch: unknown };
  g.fetch = vi.fn(async () => new Response(JSON.stringify(body), { status }));
}

describe("FleetPage", () => {
  afterEach(() => vi.restoreAllMocks());

  it("shows the unconfigured hint honestly, no crash", async () => {
    mockFetchOnce({ configured: false });
    render(<FleetPage />);
    await waitFor(() => expect(screen.getByText(/No fleet configured/)).toBeTruthy());
    expect(screen.getByText(/--fleet-device/)).toBeTruthy();
  });

  it("renders nodes, devices, and placement once configured", async () => {
    mockFetchOnce({
      configured: true,
      nodes: [{ id: 1, flags: ["opcua-gateway"] }],
      devices: [
        { id: "d0", eligible: true, node: 1 },
        { id: "d1", eligible: false },
      ],
    });
    render(<FleetPage />);
    await waitFor(() => expect(screen.getByText("d0")).toBeTruthy());
    expect(screen.getByText("opcua-gateway")).toBeTruthy();
    expect(screen.getByText("node 1")).toBeTruthy();
    expect(screen.getByText("unplaceable")).toBeTruthy();
    // Stat row: 1 node, 2 devices, 1 unplaceable.
    expect(screen.getByText("Nodes", { selector: ".stat-label" }).previousSibling?.textContent).toBe("1");
    expect(screen.getByText("Devices", { selector: ".stat-label" }).previousSibling?.textContent).toBe("2");
  });
});

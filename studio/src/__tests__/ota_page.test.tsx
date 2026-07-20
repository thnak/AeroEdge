// OtaPage (016 §2.2): unconfigured hint, wave table rendering, and starting a rollout POSTs the
// right body (never a trust key — 012 M5 posture) and refreshes status. Mocks global fetch.
import { describe, it, expect, vi, afterEach } from "vitest";
import { render, screen, waitFor, fireEvent } from "@testing-library/react";
import { OtaPage } from "../pages/OtaPage";

function mockFetchSequence(bodies: unknown[]) {
  let i = 0;
  const g = globalThis as unknown as { fetch: unknown };
  g.fetch = vi.fn(async () => new Response(JSON.stringify(bodies[Math.min(i++, bodies.length - 1)]), { status: 200 }));
  return () => (g.fetch as ReturnType<typeof vi.fn>);
}

describe("OtaPage", () => {
  afterEach(() => vi.restoreAllMocks());

  it("shows the unconfigured hint honestly, no crash", async () => {
    mockFetchSequence([{ configured: false }]);
    render(<OtaPage />);
    await waitFor(() => expect(screen.getByText(/No fleet configured/)).toBeTruthy());
  });

  it("renders wave results and state once configured", async () => {
    mockFetchSequence([{
      configured: true,
      state: "Completed",
      devices_updated: 3,
      devices_rolled_back: 0,
      waves: [{ name: "canary", attempted: 1, succeeded: 1, success_rate: 1.0, passed: true }],
    }]);
    render(<OtaPage />);
    await waitFor(() => expect(screen.getByText("canary")).toBeTruthy());
    expect(screen.getByText("Completed")).toBeTruthy();
    expect(screen.getByText("100%")).toBeTruthy();
    expect(screen.getByText("passed")).toBeTruthy();
  });

  it("starting a rollout POSTs {version, bytes} only, then refreshes", async () => {
    const getFetch = mockFetchSequence([
      { configured: true, state: "Idle", devices_updated: 0, devices_rolled_back: 0, waves: [] },
      { configured: true },
      { configured: true, state: "Completed", devices_updated: 2, devices_rolled_back: 0, waves: [] },
    ]);
    render(<OtaPage />);
    await waitFor(() => expect(screen.getByText("Idle")).toBeTruthy());

    fireEvent.click(screen.getByRole("button", { name: /Start rollout/ }));
    await waitFor(() => expect(screen.getByText("Completed")).toBeTruthy());

    const f = getFetch();
    const postCall = f.mock.calls.find((c: unknown[]) => (c[1] as RequestInit | undefined)?.method === "POST");
    expect(postCall).toBeTruthy();
    expect(postCall![0]).toBe("/api/ota/rollouts");
    expect(JSON.parse((postCall![1] as RequestInit).body as string)).toEqual({
      version: "2.0",
      bytes: "firmware-payload",
    });
  });
});

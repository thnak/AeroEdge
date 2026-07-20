// MesPage (016 §2.3): unconfigured hint, stat rendering, and draining POSTs the drain route then
// refreshes. Mocks global fetch.
import { describe, it, expect, vi, afterEach } from "vitest";
import { render, screen, waitFor, fireEvent } from "@testing-library/react";
import { MesPage } from "../pages/MesPage";

function mockFetchSequence(bodies: unknown[]) {
  let i = 0;
  const g = globalThis as unknown as { fetch: unknown };
  g.fetch = vi.fn(async () => new Response(JSON.stringify(bodies[Math.min(i++, bodies.length - 1)]), { status: 200 }));
  return () => (g.fetch as ReturnType<typeof vi.fn>);
}

describe("MesPage", () => {
  afterEach(() => vi.restoreAllMocks());

  it("shows the unconfigured hint honestly, no crash", async () => {
    mockFetchSequence([{ configured: false }]);
    render(<MesPage />);
    await waitFor(() => expect(screen.getByText(/No MES gateway configured/)).toBeTruthy());
  });

  it("renders staged/pending/delivered once configured", async () => {
    mockFetchSequence([{ configured: true, staged: 5, pending: 2, delivered: 3 }]);
    render(<MesPage />);
    await waitFor(() => expect(screen.getByText("5")).toBeTruthy());
    expect(screen.getByText("2")).toBeTruthy();
    expect(screen.getByText("3")).toBeTruthy();
  });

  it("drain button disabled at pending=0, enabled otherwise", async () => {
    mockFetchSequence([{ configured: true, staged: 5, pending: 0, delivered: 5 }]);
    render(<MesPage />);
    await waitFor(() => expect(screen.getByRole("button", { name: /Drain outbox/ })).toBeTruthy());
    const btn = screen.getByRole("button", { name: /Drain outbox/ }) as HTMLButtonElement;
    expect(btn.disabled).toBe(true);
  });

  it("draining POSTs the drain route and refreshes", async () => {
    const getFetch = mockFetchSequence([
      { configured: true, staged: 5, pending: 2, delivered: 3 },
      { configured: true },
      { configured: true, staged: 5, pending: 0, delivered: 5 },
    ]);
    render(<MesPage />);
    await waitFor(() => {
      const btn = screen.getByRole("button", { name: /Drain outbox/ }) as HTMLButtonElement;
      expect(btn.disabled).toBe(false);
    });

    fireEvent.click(screen.getByRole("button", { name: /Drain outbox/ }));
    await waitFor(() => {
      const btn = screen.getByRole("button", { name: /Drain outbox/ }) as HTMLButtonElement;
      expect(btn.disabled).toBe(true);
    });

    const postCall = getFetch().mock.calls.find((c: unknown[]) => (c[1] as RequestInit | undefined)?.method === "POST");
    expect(postCall).toBeTruthy();
    expect(postCall![0]).toBe("/api/mes/outbox/drain");
  });
});

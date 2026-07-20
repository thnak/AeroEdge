// The AppShell + routing (016 §1): the sidebar renders all four nav links, "/" redirects to
// "/flows", and each nav link actually routes to its page. FlowsPage itself doesn't fetch on mount
// (only on Deploy/Refresh/Go-live), but Fleet/OTA/MES DO fetch their status on mount (Phase
// 11.4-11.6) — mock global fetch so navigating there doesn't leak a real request. No extra test
// deps: getByText/getByRole throw if the element is missing, which is assertion enough here.
import { describe, it, expect, vi, afterEach } from "vitest";
import { render, screen, fireEvent, waitFor } from "@testing-library/react";
import { MemoryRouter } from "react-router-dom";
import { App } from "../App";

function stubFetch() {
  const g = globalThis as unknown as { fetch: unknown };
  g.fetch = vi.fn(async () => new Response(JSON.stringify({ configured: false }), { status: 200 }));
}

describe("Studio app shell", () => {
  afterEach(() => vi.restoreAllMocks());

  it("redirects / to /flows and shows the Flow Designer", () => {
    render(<MemoryRouter initialEntries={["/"]}><App /></MemoryRouter>);
    expect(screen.getByText("AeroEdge Studio")).toBeTruthy();
    expect(screen.getByRole("heading", { name: "Flow" })).toBeTruthy();
  });

  it("renders all four nav links", () => {
    render(<MemoryRouter initialEntries={["/flows"]}><App /></MemoryRouter>);
    for (const label of ["Flows", "Fleet", "OTA", "MES"]) {
      expect(screen.getByRole("link", { name: label })).toBeTruthy();
    }
  });

  it("navigates to the Fleet page on click", async () => {
    stubFetch();
    render(<MemoryRouter initialEntries={["/flows"]}><App /></MemoryRouter>);
    fireEvent.click(screen.getByRole("link", { name: "Fleet" }));
    await waitFor(() => expect(screen.getAllByText("Fleet").length).toBeGreaterThan(0));
  });

  it("navigates to the OTA and MES pages directly", async () => {
    stubFetch();
    render(<MemoryRouter initialEntries={["/ota"]}><App /></MemoryRouter>);
    await waitFor(() => expect(screen.getAllByText("OTA").length).toBeGreaterThan(0));

    render(<MemoryRouter initialEntries={["/mes"]}><App /></MemoryRouter>);
    await waitFor(() => expect(screen.getAllByText("MES").length).toBeGreaterThan(0));
  });
});

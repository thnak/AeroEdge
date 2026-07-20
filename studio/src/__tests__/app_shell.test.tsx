// The AppShell + routing (016 §1): the sidebar renders all four nav links, "/" redirects to
// "/flows", and each nav link actually routes to its page. Every page fetches its status on mount as
// of Phase 11.7 (FlowsPage now checks GET /status too, to know Deploy-vs-reload), so fetch is
// stubbed for every test. No extra test deps: getByText/getByRole throw if the element is missing,
// which is assertion enough here.
import { describe, it, expect, vi, beforeEach, afterEach } from "vitest";
import { render, screen, fireEvent, waitFor } from "@testing-library/react";
import { MemoryRouter } from "react-router-dom";
import { App } from "../App";

function stubFetch() {
  const g = globalThis as unknown as { fetch: unknown };
  g.fetch = vi.fn(async () => new Response(JSON.stringify({ deployed: false, configured: false }), { status: 200 }));
}

describe("Studio app shell", () => {
  beforeEach(() => stubFetch());
  afterEach(() => vi.restoreAllMocks());

  it("redirects / to /flows and shows the Flow Designer", async () => {
    render(<MemoryRouter initialEntries={["/"]}><App /></MemoryRouter>);
    expect(screen.getByText("AeroEdge Studio")).toBeTruthy();
    await waitFor(() => expect(screen.getByRole("heading", { name: "Flow" })).toBeTruthy());
  });

  it("renders all four nav links", async () => {
    render(<MemoryRouter initialEntries={["/flows"]}><App /></MemoryRouter>);
    for (const label of ["Flows", "Fleet", "OTA", "MES"]) {
      expect(screen.getByRole("link", { name: label })).toBeTruthy();
    }
    await waitFor(() => expect(screen.getByRole("heading", { name: "Flow" })).toBeTruthy());
  });

  it("navigates to the Fleet page on click", async () => {
    render(<MemoryRouter initialEntries={["/flows"]}><App /></MemoryRouter>);
    fireEvent.click(screen.getByRole("link", { name: "Fleet" }));
    await waitFor(() => expect(screen.getAllByText("Fleet").length).toBeGreaterThan(0));
  });

  it("navigates to the OTA and MES pages directly", async () => {
    render(<MemoryRouter initialEntries={["/ota"]}><App /></MemoryRouter>);
    await waitFor(() => expect(screen.getAllByText("OTA").length).toBeGreaterThan(0));

    render(<MemoryRouter initialEntries={["/mes"]}><App /></MemoryRouter>);
    await waitFor(() => expect(screen.getAllByText("MES").length).toBeGreaterThan(0));
  });
});

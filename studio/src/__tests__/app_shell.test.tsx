// The AppShell + routing (016 §1): the sidebar renders all four nav links, "/" redirects to
// "/flows", and each nav link actually routes to its page. No live daemon — FlowsPage doesn't fetch
// on mount (only on Deploy/Refresh/Go-live), so no fetch mock is needed here. No extra test deps:
// getByText/getByRole throw if the element is missing, which is assertion enough here.
import { describe, it, expect } from "vitest";
import { render, screen, fireEvent } from "@testing-library/react";
import { MemoryRouter } from "react-router-dom";
import { App } from "../App";

describe("Studio app shell", () => {
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

  it("navigates to the Fleet page on click", () => {
    render(<MemoryRouter initialEntries={["/flows"]}><App /></MemoryRouter>);
    fireEvent.click(screen.getByRole("link", { name: "Fleet" }));
    expect(screen.getByText(/Fleet view/)).toBeTruthy();
  });

  it("navigates to the OTA and MES pages directly", () => {
    render(<MemoryRouter initialEntries={["/ota"]}><App /></MemoryRouter>);
    expect(screen.getByText(/OTA rollout view/)).toBeTruthy();

    render(<MemoryRouter initialEntries={["/mes"]}><App /></MemoryRouter>);
    expect(screen.getByText(/MES outbox view/)).toBeTruthy();
  });
});

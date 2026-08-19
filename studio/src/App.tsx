// The Studio shell (016 §1): AppShell sidebar nav + routed content, replacing the old single page.
import { useEffect, useState } from "react";
import { Routes, Route, Navigate } from "react-router-dom";
import { AppShell, NavLink } from "./components";
import { FlowsPage } from "./pages/FlowsPage";
import { FleetPage } from "./pages/FleetPage";
import { OtaPage } from "./pages/OtaPage";
import { MesPage } from "./pages/MesPage";
import { BrokerPage } from "./pages/BrokerPage";
import { AeroApi } from "./api";
import { setCatalog, type RawCatalog } from "./catalog";

export function App() {
  // Load the node/driver catalog once at startup (015 U1, 019 slice) before anything reads it — the
  // Flow Designer's palette/dropdowns are empty until this resolves. A fetch failure (daemon not
  // running yet) degrades to an empty catalog rather than blocking the whole app — status/fleet/OTA/
  // MES/broker pages don't depend on it.
  const [catalogReady, setCatalogReady] = useState(false);
  useEffect(() => {
    const api = new AeroApi();
    void api.fetchCatalog().then((r) => {
      if (r.ok) setCatalog(r.body as RawCatalog);
      setCatalogReady(true);
    });
  }, []);

  return (
    <AppShell
      nav={
        <>
          <NavLink to="/flows">Flows</NavLink>
          <NavLink to="/fleet">Fleet</NavLink>
          <NavLink to="/ota">OTA</NavLink>
          <NavLink to="/mes">MES</NavLink>
          <NavLink to="/broker">Broker</NavLink>
        </>
      }
    >
      <Routes>
        <Route path="/" element={<Navigate to="/flows" replace />} />
        <Route path="/flows" element={catalogReady ? <FlowsPage /> : <p className="muted">Loading catalog…</p>} />
        <Route path="/fleet" element={<FleetPage />} />
        <Route path="/ota" element={<OtaPage />} />
        <Route path="/mes" element={<MesPage />} />
        <Route path="/broker" element={<BrokerPage />} />
      </Routes>
    </AppShell>
  );
}

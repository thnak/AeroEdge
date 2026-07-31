// The Studio shell (016 §1): AppShell sidebar nav + routed content, replacing the old single page.
import { Routes, Route, Navigate } from "react-router-dom";
import { AppShell, NavLink } from "./components";
import { FlowsPage } from "./pages/FlowsPage";
import { FleetPage } from "./pages/FleetPage";
import { OtaPage } from "./pages/OtaPage";
import { MesPage } from "./pages/MesPage";
import { BrokerPage } from "./pages/BrokerPage";

export function App() {
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
        <Route path="/flows" element={<FlowsPage />} />
        <Route path="/fleet" element={<FleetPage />} />
        <Route path="/ota" element={<OtaPage />} />
        <Route path="/mes" element={<MesPage />} />
        <Route path="/broker" element={<BrokerPage />} />
      </Routes>
    </AppShell>
  );
}

// aero-studio-sdk (mini) — the shared design-system components a plugin UI builds on so every
// protocol UI looks consistent (015 §4). Kept intentionally tiny; grown in 016 §4 with the
// primitives an AppShell + status-heavy Fleet/OTA/MES views need, still no new dependency beyond
// react-router (015 U7 — the host stays small).
import type { ReactNode } from "react";
import { NavLink as RouterNavLink } from "react-router-dom";

export function Panel({ title, children, actions }: { title: string; children: ReactNode; actions?: ReactNode }) {
  return (
    <section className="panel">
      <header className="panel-h">
        <h2>{title}</h2>
        {actions}
      </header>
      <div className="panel-b">{children}</div>
    </section>
  );
}

export function Button({ onClick, children, variant = "default", disabled }: {
  onClick?: () => void; children: ReactNode; variant?: "default" | "primary" | "danger"; disabled?: boolean;
}) {
  return (
    <button className={`btn btn-${variant}`} onClick={onClick} disabled={disabled}>
      {children}
    </button>
  );
}

export function Field({ label, error, children, help }: {
  label: string; error?: string; children: ReactNode; help?: string;
}) {
  return (
    <label className="field">
      <span className="field-label">{label}</span>
      {children}
      {help && <span className="field-help">{help}</span>}
      {error && <span className="field-error">{error}</span>}
    </label>
  );
}

// The Studio shell (016 §1): a sidebar nav + content area, replacing the old single <header>.
export function AppShell({ nav, children }: { nav: ReactNode; children: ReactNode }) {
  return (
    <div className="shell">
      <aside className="shell-nav">
        <div className="shell-brand">AeroEdge Studio</div>
        <nav>{nav}</nav>
      </aside>
      <main className="shell-main">{children}</main>
    </div>
  );
}

// Route-aware sidebar link (react-router NavLink styled to this design system's active state).
export function NavLink({ to, children }: { to: string; children: ReactNode }) {
  return (
    <RouterNavLink to={to} className={({ isActive }) => `nav-link${isActive ? " active" : ""}`}>
      {children}
    </RouterNavLink>
  );
}

// A plain data table (device lists, wave results, outbox entries). `columns` maps a header label to
// a cell renderer over each row; `rowKey` avoids forcing every caller to shape data with an `id`.
export function Table<T>({ columns, rows, rowKey, empty }: {
  columns: { header: string; render: (row: T) => ReactNode }[];
  rows: T[];
  rowKey: (row: T, index: number) => string | number;
  empty?: string;
}) {
  if (rows.length === 0) {
    return <p className="muted">{empty ?? "Nothing to show."}</p>;
  }
  return (
    <table className="table">
      <thead>
        <tr>{columns.map((c) => <th key={c.header}>{c.header}</th>)}</tr>
      </thead>
      <tbody>
        {rows.map((row, i) => (
          <tr key={rowKey(row, i)}>
            {columns.map((c) => <td key={c.header}>{c.render(row)}</td>)}
          </tr>
        ))}
      </tbody>
    </table>
  );
}

// A labelled metric tile (node count, pending count, success rate).
export function StatCard({ label, value, tone }: { label: string; value: ReactNode; tone?: "ok" | "warn" | "error" }) {
  return (
    <div className={`stat-card${tone ? ` stat-${tone}` : ""}`}>
      <div className="stat-value">{value}</div>
      <div className="stat-label">{label}</div>
    </div>
  );
}

// ok/warn/error status coloring, generalizing the old one-off .badge/.live-badge classes.
export function StatusPill({ tone, children }: { tone: "ok" | "warn" | "error" | "neutral"; children: ReactNode }) {
  return <span className={`pill pill-${tone}`}>{children}</span>;
}

export function Tabs({ tabs, active, onChange }: { tabs: string[]; active: string; onChange: (t: string) => void }) {
  return (
    <div className="tabs">
      {tabs.map((t) => (
        <button key={t} className={`tab${t === active ? " active" : ""}`} onClick={() => onChange(t)}>
          {t}
        </button>
      ))}
    </div>
  );
}

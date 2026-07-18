// aero-studio-sdk (mini) — the shared design-system components a plugin UI builds on so every
// protocol UI looks consistent (015 §4). Kept intentionally tiny for Phase 9.
import type { ReactNode } from "react";

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

// The node/driver catalog — the Studio-side reflection of each plugin's config schema (015 U1). Was
// hardcoded here and had drifted from what the runtime actually accepts (4 of 5 real drivers were
// invisible; several nodes were missing fields — see 019-Flow-Graph-Model-and-Studio-Canvas-API.md's
// ground-truth table). Now sourced from `GET /catalog` (api.ts's fetchCatalog) via setCatalog() —
// call it once at app start (App.tsx) before anything reads catalogEntry/sourceIds/outputIds.
//
// The server schema doesn't carry a human display label per node/driver type (only per config field —
// that IS on the wire, see FieldSpec below) — labels shown in the palette/dropdowns are derived
// client-side from the type_id (humanize()). Every other call site (ConfigForm.tsx, FlowCanvasNode.tsx,
// FlowDesigner.tsx, FlowsPage.tsx, application.ts's tests) keeps calling catalogEntry/sourceIds/
// outputIds/validateConfig exactly as before — only the data backing them changed.

export type FieldType = "number" | "int" | "string" | "boolean" | "enum" | "string_array" | "object";

export interface FieldSpec {
  key: string;
  label: string;
  type: FieldType;
  required?: boolean;
  default?: number | string | boolean;
  min?: number;
  help?: string;
  // A field whose editor is a Tier-2 custom micro-frontend rather than a plain input (015 §3) — e.g.
  // "modbus-register-map", "opcua-security", "http-headers". Free-form string from the server.
  tier2?: string;
  options?: string[];  // Enum only
}

export type Category = "Source" | "Transform" | "Rule" | "Output";

export interface CatalogEntry {
  type_id: string;
  label: string;
  category: Category;
  fields: FieldSpec[];
  // 020 §4.3: true for a node type that must be the flow's LAST step (a Cap shape — nothing may follow
  // it — vs. the default Stack shape). Optional/undefined for driver entries, which don't carry it.
  terminal?: boolean;
}

export interface DriverCatalogEntry extends CatalogEntry {
  writable: boolean;
  pollDriven: boolean;
}

export interface Catalog {
  nodes: CatalogEntry[];
  drivers: DriverCatalogEntry[];
}

// The raw `GET /catalog` response shape (runtime.hpp's build_catalog()).
export interface RawCatalog {
  nodes: { type_id: string; category: Category; terminal?: boolean; fields: FieldSpec[] }[];
  drivers: { type_id: string; writable: boolean; poll_driven: boolean; fields: FieldSpec[] }[];
}

function humanize(type_id: string): string {
  const last = type_id.split(".").pop() ?? type_id;
  return last.split("_").map((w) => (w ? w[0].toUpperCase() + w.slice(1) : w)).join(" ");
}

let current: Catalog = { nodes: [], drivers: [] };

// Populate the catalog from a `GET /catalog` response (App.tsx calls this once at startup, and tests
// call it with a fixture). Replaces whatever was there before — the server is always the whole truth.
// Defensive against a malformed/unexpected body (e.g. a test's stubbed fetch returning an unrelated
// JSON shape for every URL, including /catalog): ignored, leaving whatever catalog was already loaded
// in place, rather than wiping it — a malformed response is not evidence the real catalog is empty.
export function setCatalog(raw: RawCatalog): void {
  if (!Array.isArray(raw?.nodes) || !Array.isArray(raw?.drivers)) {
    return;
  }
  current = {
    nodes: raw.nodes.map((n) => ({ ...n, label: humanize(n.type_id) })),
    drivers: raw.drivers.map((d) => ({
      type_id: d.type_id,
      label: humanize(d.type_id),
      category: "Source",  // drivers aren't node-categorized server-side; palette groups them separately
      fields: d.fields,
      writable: d.writable,
      pollDriven: d.poll_driven,
    })),
  };
}

export function getCatalog(): Catalog {
  return current;
}

export function catalogEntry(type_id: string): CatalogEntry | undefined {
  return current.nodes.find((e) => e.type_id === type_id) ??
         current.drivers.find((e) => e.type_id === type_id);
}

export function sourceIds(): Set<string> {
  return new Set(current.nodes.filter((e) => e.category === "Source").map((e) => e.type_id));
}
export function outputIds(): Set<string> {
  return new Set(current.nodes.filter((e) => e.category === "Output").map((e) => e.type_id));
}

// Validate a config object against a catalog entry's field specs — the Tier-1 client-side check
// (015 §7). Returns per-field error messages ({} == valid). The runtime configure() remains the
// authority (U1); this is instant feedback, not the source of truth.
export function validateConfig(entry: CatalogEntry, config: Record<string, unknown>): Record<string, string> {
  const errs: Record<string, string> = {};
  for (const f of entry.fields) {
    const v = config[f.key];
    if (v === undefined || v === "") {
      if (f.required) errs[f.key] = `${f.label} is required`;
      continue;
    }
    if (f.type === "number" || f.type === "int") {
      const n = Number(v);
      if (Number.isNaN(n)) errs[f.key] = `${f.label} must be a number`;
      else if (f.type === "int" && !Number.isInteger(n)) errs[f.key] = `${f.label} must be an integer`;
      else if (f.min !== undefined && n < f.min) errs[f.key] = `${f.label} must be ≥ ${f.min}`;
    }
  }
  return errs;
}

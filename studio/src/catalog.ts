// The node/driver catalog — the Studio-side reflection of each plugin's config schema (015 U1). In
// the full design the runtime serves this (a future GET /catalog so the Studio can't drift from what
// the runtime accepts); Phase-9 hardcodes the built-in set registered in
// include/aero/runtime/runtime.hpp. Each entry drives the Tier-1 schema-driven config form (015 §3).

export type FieldType = "number" | "int" | "string" | "boolean";

export interface FieldSpec {
  key: string;
  label: string;
  type: FieldType;
  required?: boolean;
  default?: number | string | boolean;
  min?: number;
  help?: string;
  // A field whose editor is a Tier-2 custom micro-frontend rather than a plain input (015 §3).
  tier2?: "modbus-register-map";
}

export type Category = "Source" | "Transform" | "Rule" | "Output";

export interface CatalogEntry {
  type_id: string;
  label: string;
  category: Category;
  fields: FieldSpec[];
}

export const NODE_CATALOG: CatalogEntry[] = [
  { type_id: "aero.source.decode", label: "Decode (scalar)", category: "Source", fields: [] },
  { type_id: "aero.source.json", label: "JSON Parse", category: "Source", fields: [] },
  { type_id: "aero.source.modbus", label: "Modbus Decode", category: "Source",
    fields: [{ key: "map", label: "Register map", type: "string", tier2: "modbus-register-map",
               help: "Rich editor: address / type / endianness / scale per register." }] },
  { type_id: "aero.transform.scale", label: "Scale", category: "Transform",
    fields: [{ key: "factor", label: "Factor", type: "number", required: true, default: 1,
               help: "Multiply every tag by this factor." }] },
  { type_id: "aero.transform.moving_average", label: "Moving Average", category: "Transform",
    fields: [{ key: "window", label: "Window (samples)", type: "int", required: true, default: 8, min: 1 }] },
  { type_id: "aero.transform.mean", label: "Mean", category: "Transform", fields: [] },
  { type_id: "aero.transform.minmax", label: "Min/Max", category: "Transform", fields: [] },
  { type_id: "aero.transform.sum", label: "Sum (tags)", category: "Transform", fields: [] },
  { type_id: "aero.transform.crc", label: "CRC-16", category: "Transform", fields: [] },
  { type_id: "aero.rule.expr", label: "Expression Rule", category: "Rule",
    fields: [{ key: "expr", label: "Expression", type: "string", required: true, default: "raw > 100",
               help: "Non-Turing DSL: compare / boolean / arithmetic over tags. On match: alarm + stop." }] },
  { type_id: "aero.output.sum", label: "Sum Output", category: "Output", fields: [] },
  { type_id: "aero.output.mes", label: "MES Report", category: "Output",
    fields: [{ key: "line", label: "Line/device id", type: "string", required: true, default: "line-1" }] },
];

export const DRIVER_CATALOG: CatalogEntry[] = [
  { type_id: "aero.driver.generator", label: "Generator (synthetic)", category: "Source",
    fields: [{ key: "frame_count", label: "Frame count", type: "int", default: 100, min: 0,
               help: "0 = run until stopped." }] },
];

const byId = new Map<string, CatalogEntry>();
for (const e of [...NODE_CATALOG, ...DRIVER_CATALOG]) byId.set(e.type_id, e);

export function catalogEntry(type_id: string): CatalogEntry | undefined {
  return byId.get(type_id);
}

export function sourceIds(): Set<string> {
  return new Set(NODE_CATALOG.filter((e) => e.category === "Source").map((e) => e.type_id));
}
export function outputIds(): Set<string> {
  return new Set(NODE_CATALOG.filter((e) => e.category === "Output").map((e) => e.type_id));
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

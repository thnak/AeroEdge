// Tier-1 schema-driven config form (015 §3, U2/U3): renders a node's config FROM its catalog field
// specs — no per-node hardcoded form. A field marked tier2 mounts a custom micro-frontend instead.
import type { CatalogEntry, FieldSpec } from "./catalog";
import { validateConfig } from "./catalog";
import type { NodeConfig } from "./application";
import { Field } from "./components";
import { ModbusRegisterMap } from "./tier2/ModbusRegisterMap";

export function ConfigForm({ entry, config, onChange }: {
  entry: CatalogEntry;
  config: NodeConfig;
  onChange: (c: NodeConfig) => void;
}) {
  const errors = validateConfig(entry, config);

  if (entry.fields.length === 0) {
    return <p className="muted">No configuration for {entry.label}.</p>;
  }

  const set = (f: FieldSpec, raw: string | boolean) => {
    let v: number | string | boolean = raw;
    if (f.type === "number" || f.type === "int") v = raw === "" ? "" : Number(raw);
    onChange({ ...config, [f.key]: v as number | string | boolean });
  };

  return (
    <div className="config-form">
      {entry.fields.map((f) => {
        if (f.tier2 === "modbus-register-map") {
          return (
            <Field key={f.key} label={f.label} help={f.help} error={errors[f.key]}>
              <ModbusRegisterMap
                value={typeof config[f.key] === "string" ? (config[f.key] as string) : ""}
                onChange={(json) => onChange({ ...config, [f.key]: json })}
              />
            </Field>
          );
        }
        const val = config[f.key];
        return (
          <Field key={f.key} label={f.label} help={f.help} error={errors[f.key]}>
            {f.type === "boolean" ? (
              <input type="checkbox" checked={Boolean(val)} onChange={(e) => set(f, e.target.checked)} />
            ) : (
              <input
                type={f.type === "string" ? "text" : "number"}
                value={val === undefined ? "" : String(val)}
                step={f.type === "int" ? 1 : "any"}
                onChange={(e) => set(f, e.target.value)}
              />
            )}
          </Field>
        );
      })}
    </div>
  );
}

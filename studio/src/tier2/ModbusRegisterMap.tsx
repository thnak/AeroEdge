// Tier-2 custom UI (015 §3): a Modbus register-map editor — the kind of rich, protocol-specific
// config a plain schema form can't express (grid of address / type / endianness / scale rows, with
// add/remove/import/export). Pure UI: it edits a register map; LIVE device discovery is a separate,
// runtime-assisted, gated action (015 §5). The value is serialized to a JSON string the runtime's
// Modbus node consumes — so the Tier-1 form treats it as one opaque config field.
import { useMemo } from "react";
import { Button } from "../components";

export type RegType = "uint16" | "int16" | "uint32" | "int32" | "float32";
export type Endian = "big" | "little";

export interface RegisterRow {
  name: string;
  address: number;
  type: RegType;
  endian: Endian;
  scale: number;
}

export function parseMap(json: string): RegisterRow[] {
  if (!json) return [];
  try {
    const v = JSON.parse(json);
    return Array.isArray(v) ? (v as RegisterRow[]) : [];
  } catch {
    return [];
  }
}

export function serializeMap(rows: RegisterRow[]): string {
  return JSON.stringify(rows);
}

const REG_TYPES: RegType[] = ["uint16", "int16", "uint32", "int32", "float32"];

export function ModbusRegisterMap({ value, onChange }: { value: string; onChange: (json: string) => void }) {
  const rows = useMemo(() => parseMap(value), [value]);

  const update = (next: RegisterRow[]) => onChange(serializeMap(next));
  const addRow = () =>
    update([...rows, { name: `reg${rows.length}`, address: rows.length, type: "uint16", endian: "big", scale: 1 }]);
  const removeRow = (i: number) => update(rows.filter((_, idx) => idx !== i));
  const patch = (i: number, p: Partial<RegisterRow>) =>
    update(rows.map((r, idx) => (idx === i ? { ...r, ...p } : r)));

  return (
    <div className="regmap">
      <table>
        <thead>
          <tr><th>Name</th><th>Addr</th><th>Type</th><th>Endian</th><th>Scale</th><th></th></tr>
        </thead>
        <tbody>
          {rows.map((r, i) => (
            <tr key={i}>
              <td><input value={r.name} onChange={(e) => patch(i, { name: e.target.value })} /></td>
              <td><input type="number" value={r.address} onChange={(e) => patch(i, { address: Number(e.target.value) })} /></td>
              <td>
                <select value={r.type} onChange={(e) => patch(i, { type: e.target.value as RegType })}>
                  {REG_TYPES.map((t) => <option key={t} value={t}>{t}</option>)}
                </select>
              </td>
              <td>
                <select value={r.endian} onChange={(e) => patch(i, { endian: e.target.value as Endian })}>
                  <option value="big">big</option>
                  <option value="little">little</option>
                </select>
              </td>
              <td><input type="number" value={r.scale} step="any" onChange={(e) => patch(i, { scale: Number(e.target.value) })} /></td>
              <td><Button variant="danger" onClick={() => removeRow(i)}>✕</Button></td>
            </tr>
          ))}
          {rows.length === 0 && <tr><td colSpan={6} className="muted">No registers. Add one, or discover from a connected device.</td></tr>}
        </tbody>
      </table>
      <Button onClick={addRow}>+ Register</Button>
    </div>
  );
}

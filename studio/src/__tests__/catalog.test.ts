// Tier-1 schema-driven config: the catalog descriptors drive validation (015 §3/§7).
import { describe, it, expect } from "vitest";
import { catalogEntry, validateConfig } from "../catalog";
import { parseMap, serializeMap, type RegisterRow } from "../tier2/ModbusRegisterMap";

describe("config validation from catalog", () => {
  it("requires and type-checks scale.factor", () => {
    const scale = catalogEntry("aero.transform.scale")!;
    expect(validateConfig(scale, { factor: 2 })).toEqual({});
    expect(validateConfig(scale, {})).toHaveProperty("factor");
    expect(validateConfig(scale, { factor: "abc" })).toHaveProperty("factor");
  });

  it("enforces integer + min on moving_average.window", () => {
    const ma = catalogEntry("aero.transform.moving_average")!;
    expect(validateConfig(ma, { window: 8 })).toEqual({});
    expect(validateConfig(ma, { window: 1.5 })).toHaveProperty("window");
    expect(validateConfig(ma, { window: 0 })).toHaveProperty("window");
  });

  it("nodes with no fields validate trivially", () => {
    expect(validateConfig(catalogEntry("aero.output.sum")!, {})).toEqual({});
  });
});

describe("Tier-2 Modbus register map", () => {
  it("serializes and parses register rows round-trip", () => {
    const rows: RegisterRow[] = [
      { name: "temp", address: 0, type: "float32", endian: "big", scale: 0.1 },
      { name: "state", address: 2, type: "uint16", endian: "little", scale: 1 },
    ];
    expect(parseMap(serializeMap(rows))).toEqual(rows);
    expect(parseMap("")).toEqual([]);
    expect(parseMap("not json")).toEqual([]);
  });
});

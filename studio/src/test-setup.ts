// jsdom has no ResizeObserver (@xyflow/react's canvas needs one to size itself) or real layout, so
// jsdom can never fire the "measured" callback that flips a node from `visibility: hidden` to
// visible — a real-browser-only transient. A no-op stub is enough to stop the ReferenceError; tests
// that need to find node contents by role query with `{ hidden: true }` to see past that CSS state
// (getByText already ignores it, which is why it needs no such flag).
class ResizeObserverStub {
  observe() {}
  unobserve() {}
  disconnect() {}
}
(globalThis as unknown as { ResizeObserver: unknown }).ResizeObserver = ResizeObserverStub;

// catalog.ts (019 slice) is now populated from `GET /catalog` at app startup (App.tsx), not hardcoded
// literals — no component under test does that fetch, so every test needs a catalog pre-loaded. This
// fixture mirrors the real backend catalog (runtime.hpp's register_builtins()/build_catalog()) field
// for field, so any test exercising a real type_id sees the same shape production does.
import { setCatalog } from "./catalog";

setCatalog({
  nodes: [
    { type_id: "aero.source.decode", category: "Source", fields: [] },
    { type_id: "aero.source.json", category: "Source", fields: [] },
    { type_id: "aero.source.modbus", category: "Source", fields: [] },
    { type_id: "aero.source.modbus_bits", category: "Source", fields: [] },
    { type_id: "aero.source.mes_order", category: "Source",
      fields: [{ key: "order_qty", label: "Order quantity", type: "number" }] },
    { type_id: "aero.transform.scale", category: "Transform",
      fields: [{ key: "factor", label: "Factor", type: "number", required: true, default: 1,
                 help: "Multiply every tag by this factor." }] },
    { type_id: "aero.transform.moving_average", category: "Transform",
      fields: [{ key: "window", label: "Window (samples)", type: "int", default: 1, min: 1 }] },
    { type_id: "aero.transform.mean", category: "Transform", fields: [] },
    { type_id: "aero.transform.minmax", category: "Transform", fields: [] },
    { type_id: "aero.transform.sum", category: "Transform", fields: [] },
    { type_id: "aero.transform.crc", category: "Transform", fields: [] },
    { type_id: "aero.rule.expr", category: "Rule",
      fields: [
        { key: "expr", label: "Expression", type: "string", required: true,
          help: "Non-Turing DSL: compare / boolean / arithmetic over tags. On match: alarm + stop." },
        { key: "alarm", label: "Alarm event", type: "string", default: "AlarmRaised" },
      ] },
    { type_id: "aero.output.sum", category: "Output", fields: [] },
    { type_id: "aero.output.mes", category: "Output",
      fields: [
        { key: "line", label: "Line/device id", type: "string", required: true, default: "line-1" },
        { key: "label", label: "Label", type: "string", default: "produced" },
        { key: "kind", label: "Report kind", type: "enum", default: "production",
          options: ["production", "alarm", "tag_sample"] },
      ] },
    { type_id: "aero.output.http", category: "Output",
      fields: [
        { key: "url", label: "URL", type: "string", required: true },
        { key: "method", label: "Method", type: "enum", default: "POST",
          options: ["GET", "POST", "PUT", "PATCH", "DELETE"] },
        { key: "headers", label: "Headers", type: "object", tier2: "http-headers" },
        { key: "timeout_ms", label: "Timeout (ms)", type: "int", default: 2000, min: 1 },
      ] },
  ],
  drivers: [
    { type_id: "aero.driver.generator", writable: false, poll_driven: false,
      fields: [
        { key: "frame_count", label: "Frame count", type: "int", default: 0, min: 0,
          help: "0 = run until stopped." },
        { key: "rate_hz", label: "Rate (Hz)", type: "int", default: 0, min: 0 },
      ] },
    { type_id: "aero.driver.modbus_tcp", writable: true, poll_driven: true,
      fields: [
        { key: "host", label: "Host", type: "string", required: true },
        { key: "port", label: "Port", type: "int", default: 502, min: 1 },
        { key: "unit_id", label: "Unit id", type: "int", default: 1, min: 0 },
        { key: "start_address", label: "Start address", type: "int", min: 0 },
        { key: "register_count", label: "Register count", type: "int", default: 8, min: 1 },
        { key: "register_type", label: "Register type", type: "enum", default: "holding",
          options: ["holding", "input", "coils", "discrete_inputs"] },
      ] },
    { type_id: "aero.driver.modbus_rtu", writable: true, poll_driven: true,
      fields: [
        { key: "port", label: "Serial port", type: "string", required: true },
        { key: "baud_rate", label: "Baud rate", type: "int", default: 9600, min: 1 },
        { key: "slave_address", label: "Slave address", type: "int", default: 1, min: 0 },
        { key: "start_address", label: "Start address", type: "int", min: 0 },
        { key: "register_count", label: "Register count", type: "int", default: 8, min: 1 },
        { key: "register_type", label: "Register type", type: "enum", default: "holding",
          options: ["holding", "input", "coils", "discrete_inputs"] },
        { key: "parity", label: "Parity", type: "enum", default: "N", options: ["N", "E", "O"] },
        { key: "stop_bits", label: "Stop bits", type: "int", default: 1, min: 1 },
      ] },
    { type_id: "aero.driver.opcua", writable: true, poll_driven: true,
      fields: [
        { key: "endpoint", label: "Endpoint URL", type: "string", required: true },
        { key: "node_ids", label: "Node IDs", type: "string_array" },
        { key: "browse_root", label: "Browse root NodeId", type: "string" },
        { key: "security", label: "Security", type: "object", tier2: "opcua-security" },
      ] },
    { type_id: "aero.driver.opcua_subscribe", writable: false, poll_driven: false,
      fields: [
        { key: "endpoint", label: "Endpoint URL", type: "string", required: true },
        { key: "node_ids", label: "Node IDs", type: "string_array" },
        { key: "security", label: "Security", type: "object", tier2: "opcua-security" },
      ] },
  ],
});

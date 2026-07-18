// The AeroEdge Application contract — the Studio side of the canonical schema (013 T3, 015 U1).
// This MUST match include/aero/schema/application.hpp and examples/hello_flow.json exactly: the
// runtime is the authority, so what the Flow Designer emits here has to deploy unchanged. The
// round-trip test (src/__tests__/application.test.ts) locks that alignment.

export type NodeConfig = Record<string, number | string | boolean>;

export interface FlowNode {
  type_id: string;
  config?: NodeConfig;
}

export interface DriverSpec {
  type_id: string;
  config?: NodeConfig;
}

export interface Application {
  name: string;
  version: string;
  actor: { kind: string; key: number };
  flow: FlowNode[];
  driver?: DriverSpec;
  persistence?: { model: string; mode: string };
}

// The editable model the Flow Designer manipulates. Kept separate from the wire Application so the
// UI can hold in-progress state; `toApplication` projects it to the canonical shape.
export interface FlowModel {
  name: string;
  version: string;
  actorKind: string;
  actorKey: number;
  nodes: FlowNode[];
  driver?: DriverSpec;
  persistence?: { model: string; mode: string };
}

// Drop empty config objects so the emitted JSON matches the runtime's minimal shape (a node with no
// config omits the key entirely, as hello_flow.json's decode/sum nodes do).
function cleanNode(n: FlowNode): FlowNode {
  if (n.config && Object.keys(n.config).length > 0) return { type_id: n.type_id, config: n.config };
  return { type_id: n.type_id };
}

export function toApplication(m: FlowModel): Application {
  const app: Application = {
    name: m.name,
    version: m.version,
    actor: { kind: m.actorKind, key: m.actorKey },
    flow: m.nodes.map(cleanNode),
  };
  if (m.driver) {
    app.driver =
      m.driver.config && Object.keys(m.driver.config).length > 0
        ? { type_id: m.driver.type_id, config: m.driver.config }
        : { type_id: m.driver.type_id };
  }
  if (m.persistence) app.persistence = m.persistence;
  return app;
}

// Parse a canonical Application (e.g. an existing deployment, or examples/hello_flow.json) back into
// the editable model — the load side of the round-trip.
export function fromApplication(app: Application): FlowModel {
  return {
    name: app.name,
    version: app.version,
    actorKind: app.actor.kind,
    actorKey: app.actor.key,
    nodes: app.flow.map((n) => ({ type_id: n.type_id, config: n.config })),
    driver: app.driver,
    persistence: app.persistence,
  };
}

// Structural validation mirroring the runtime's deploy-time checks (009 §3): a valid Application has
// a name, an actor, at least one node, a Source first and an Output present. Full type/DAG validation
// is the runtime's authority (U1) — this is the fast client-side pre-check (015 §7).
export function validateApplication(app: Application, sourceIds: Set<string>, outputIds: Set<string>): string[] {
  const errs: string[] = [];
  if (!app.name) errs.push("name is required");
  if (!app.version) errs.push("version is required");
  if (!app.actor || !app.actor.kind) errs.push("actor.kind is required");
  if (!app.flow || app.flow.length === 0) {
    errs.push("flow must have at least one node");
    return errs;
  }
  if (!sourceIds.has(app.flow[0].type_id)) errs.push("flow must start with a Source node");
  if (!app.flow.some((n) => outputIds.has(n.type_id))) errs.push("flow must contain an Output node");
  return errs;
}

// The AeroEdge Application contract — the Studio side of the canonical schema (013 T3, 015 U1, 019).
// This MUST match include/aero/schema/application.hpp and examples/hello_flow.json exactly: the
// runtime is the authority, so what the Flow Designer emits here has to deploy unchanged. The
// round-trip test (src/__tests__/application.test.ts) locks that alignment.

export type NodeConfig = Record<string, number | string | boolean>;

// `id` is optional on the WIRE type (019 §1: the runtime synthesizes "n<index>" when absent) — kept
// optional here too so every existing literal `Application`/`FlowNode` (HELLO in FlowsPage.tsx, test
// fixtures) that never mentions `id` keeps type-checking unchanged. The client model always populates
// it in practice (see `nodeId` below); the wire type just doesn't require it.
export interface FlowNode {
  id?: string;
  type_id: string;
  config?: NodeConfig;
}

// One edge of the flow graph (019 §1/§2), matching include/aero/schema/application.hpp's EdgeSpec
// wire shape exactly (snake_case `from_port`).
export interface EdgeSpec {
  from: string;
  from_port?: string;
  to: string;
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
  edges?: EdgeSpec[];
  driver?: DriverSpec;
  persistence?: { model: string; mode: string };
}

// A canvas-drawn edge, addressed by `FlowNode.id` (not the wire `EdgeSpec` — that one has no stable
// per-edge identity, which the canvas needs for selection/deletion). `id` is a client-only React/
// deletion key, never serialized.
export interface GraphEdge {
  id: string;
  from: string;
  fromPort?: string;
  to: string;
}

// The editable model the Flow Designer manipulates. Kept separate from the wire Application so the
// UI can hold in-progress state; `toApplication` projects it to the canonical shape.
export interface FlowModel {
  name: string;
  version: string;
  actorKind: string;
  actorKey: number;
  nodes: FlowNode[];
  // Empty = legacy/linear mode: array order IS the DAG, exactly today's v0.1 behavior, and
  // `toApplication` emits the historical minimal shape (no `id`, no `edges` key) — this is what keeps
  // the hello_flow round-trip byte-identical. Non-empty = the user has drawn/deleted a connection on
  // the canvas at least once; `toApplication` then emits explicit node `id`s + `edges` (019 §6 G6,
  // mirrored client-side: "edges[] is the new, optional way to express anything beyond linear").
  edges: GraphEdge[];
  driver?: DriverSpec;
  persistence?: { model: string; mode: string };
}

// A node's stable graph identity: its explicit `id` if set, else the position-based synthesis the
// runtime loader itself uses (application.hpp:130-137) — kept in sync so an edge referencing a
// synthesized id resolves identically on both sides.
export function nodeId(n: FlowNode, index: number): string {
  return n.id ?? `n${index}`;
}

// Drop empty config objects (and, in legacy mode, `id`) so the emitted JSON matches the runtime's
// minimal shape (a node with no config omits the key entirely, as hello_flow.json's decode/sum nodes
// do).
function cleanNode(n: FlowNode, index: number, includeId: boolean): FlowNode {
  const out: FlowNode = { type_id: n.type_id };
  if (includeId) out.id = nodeId(n, index);
  if (n.config && Object.keys(n.config).length > 0) out.config = n.config;
  return out;
}

function cleanEdge(e: GraphEdge): EdgeSpec {
  return e.fromPort ? { from: e.from, from_port: e.fromPort, to: e.to } : { from: e.from, to: e.to };
}

export function toApplication(m: FlowModel): Application {
  const graphMode = m.edges.length > 0;
  const app: Application = {
    name: m.name,
    version: m.version,
    actor: { kind: m.actorKind, key: m.actorKey },
    flow: m.nodes.map((n, i) => cleanNode(n, i, graphMode)),
  };
  if (graphMode) app.edges = m.edges.map(cleanEdge);
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
// the editable model — the load side of the round-trip. A legacy (`edges`-empty/absent) Application
// loads back into legacy mode untouched — no implicit-chain synthesis into `model.edges` here, so it
// stays in legacy mode (and keeps emitting the minimal shape) until the user actually edits the graph.
export function fromApplication(app: Application): FlowModel {
  return {
    name: app.name,
    version: app.version,
    actorKind: app.actor.kind,
    actorKey: app.actor.key,
    nodes: app.flow.map((n, i) => ({ id: n.id ?? `n${i}`, type_id: n.type_id, config: n.config })),
    edges: (app.edges ?? []).map((e, i) => ({ id: `e${i}`, from: e.from, fromPort: e.from_port, to: e.to })),
    driver: app.driver,
    persistence: app.persistence,
  };
}

// The `n[i] -> n[i+1]` chain implied by array order — today's v0.1 behavior (004), and the seed used
// to materialize `model.edges` the first time the user edits the graph (see `withEdge`/`withoutEdge`).
export function implicitEdges(nodes: FlowNode[]): GraphEdge[] {
  const edges: GraphEdge[] = [];
  for (let i = 0; i < nodes.length - 1; i++) {
    edges.push({ id: `implicit-${i}`, from: nodeId(nodes[i], i), to: nodeId(nodes[i + 1], i + 1) });
  }
  return edges;
}

function materializedEdges(m: FlowModel): GraphEdge[] {
  return m.edges.length > 0 ? m.edges : implicitEdges(m.nodes);
}

// Add a manually-drawn connection. First call on a legacy-mode model materializes the implicit chain
// into `model.edges` before appending — nothing already-connected is lost by opting into graph mode.
export function withEdge(m: FlowModel, edge: GraphEdge): FlowModel {
  return { ...m, edges: [...materializedEdges(m), edge] };
}

// Remove one drawn connection (canvas delete gesture). Same materialize-first rule as `withEdge` — a
// deletion is itself the graph edit that opts a legacy flow into graph mode.
export function withoutEdge(m: FlowModel, edgeId: string): FlowModel {
  return { ...m, edges: materializedEdges(m).filter((e) => e.id !== edgeId) };
}

// Drop a node and any edge referencing it. In legacy mode this stays legacy (array-order removal
// alone is enough, exactly today's behavior/round-trip); in graph mode the dangling edges are pruned.
export function removeNodeAndEdges(m: FlowModel, index: number): FlowModel {
  const removedId = nodeId(m.nodes[index], index);
  const nodes = m.nodes.filter((_, i) => i !== index);
  if (m.edges.length === 0) return { ...m, nodes };
  const edges = m.edges.filter((e) => e.from !== removedId && e.to !== removedId);
  return { ...m, nodes, edges };
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
  // "flow[0] is the Source" only holds in legacy (array-order-is-the-DAG) mode — once `edges[]` is
  // present the graph's actual root can be anywhere in the array; `validateGraph` below owns the
  // real root/reachability checks for that case.
  if ((!app.edges || app.edges.length === 0) && !sourceIds.has(app.flow[0].type_id)) {
    errs.push("flow must start with a Source node");
  }
  if (!app.flow.some((n) => outputIds.has(n.type_id))) errs.push("flow must contain an Output node");
  return errs;
}

// Tier-1 instant-feedback graph checks (015 §7 posture, extended to §2/§3 of 019) mirroring
// runtime/flow_compiler.hpp's `order_flow_graph` — the runtime stays the authority (Runtime::deploy
// re-validates independently); this only short-circuits obviously-broken graphs before Deploy is even
// clickable. No-op (returns []) when the Application has no `edges` (legacy mode has nothing to check
// here — `validateApplication` already covers it).
export function validateGraph(app: Application): string[] {
  const edges = app.edges ?? [];
  if (edges.length === 0) return [];

  const errs: string[] = [];
  const ids = new Set(app.flow.map((n, i) => nodeId(n, i)));

  for (const e of edges) {
    if (!ids.has(e.from)) errs.push(`edge references unknown node '${e.from}'`);
    if (!ids.has(e.to)) errs.push(`edge references unknown node '${e.to}'`);
  }
  if (errs.length > 0) return errs; // topo-sort below assumes every endpoint resolves

  // Kahn's algorithm: topo-sort + cycle detection in one pass, same approach as the C++ compiler.
  const outgoing = new Map<string, string[]>();
  const indeg = new Map<string, number>();
  for (const id of ids) { outgoing.set(id, []); indeg.set(id, 0); }
  for (const e of edges) {
    outgoing.get(e.from)!.push(e.to);
    indeg.set(e.to, (indeg.get(e.to) ?? 0) + 1);
  }
  const queue = [...ids].filter((id) => indeg.get(id) === 0);
  let visited = 0;
  const indegWork = new Map(indeg);
  while (queue.length > 0) {
    const id = queue.shift()!;
    visited++;
    for (const to of outgoing.get(id) ?? []) {
      const remaining = (indegWork.get(to) ?? 0) - 1;
      indegWork.set(to, remaining);
      if (remaining === 0) queue.push(to);
    }
  }
  if (visited !== ids.size) errs.push("flow graph contains a cycle");

  // A node reached by edges with more than one distinct branch label (including the empty
  // "unconditional" label) can't be scheduled consistently — same rule as order_flow_graph.
  const labelsInto = new Map<string, Set<string>>();
  for (const e of edges) {
    if (!labelsInto.has(e.to)) labelsInto.set(e.to, new Set());
    labelsInto.get(e.to)!.add(e.from_port ?? "");
  }
  for (const [id, labels] of labelsInto) {
    if (labels.size > 1) errs.push(`node '${id}' is reached by edges with conflicting branch labels`);
  }

  // ctx.active_branch is a single field, not a stack (019 §10) — only one branch-producing node per
  // flow is supported.
  const branchSources = new Set(edges.filter((e) => e.from_port).map((e) => e.from));
  if (branchSources.size > 1) {
    errs.push(
      `flow has more than one branch-producing node (${[...branchSources].join(", ")}) — only one ` +
        "active switch point per flow is supported",
    );
  }

  return errs;
}

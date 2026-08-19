// A custom edge that arcs around a node when source === target (a loop-back/self-referencing edge),
// following react-flow's own documented pattern (reactflow.dev/examples/edges/custom-edges). Originally
// written when the runtime could not execute any cycle at all (I3: a flow compiled once into a flat
// array walked in a single zero-alloc pass, no loop-iteration semantics) — `validateGraph`'s cycle
// check rejected every self-edge at the errors banner, and this component existed purely so a user
// could still draw one and see it clearly instead of a zero-length/invisible edge (UI-completeness,
// not execution capability).
//
// 020 §8 changed that for exactly one case: a `from_port === "loop_back"` self-edge on an
// `aero.flow.loop_back` node (the empty-body loop shape, closing directly on itself) is now a REAL,
// runtime-executable construct — `validateGraph` excludes it from the cycle check entirely (mirroring
// order_flow_graph's own exclusion), and loopCavity.ts's computeLoops recognizes it as a closed pair.
// This component's arc rendering is unchanged either way; only the meaning of what it's drawing split
// in two: a genuine validation error for any OTHER self-edge, or a legitimate empty loop body for this
// one specific from_port.
import { BaseEdge, getBezierPath, type EdgeProps } from "@xyflow/react";

export function SelfConnectingEdge(props: EdgeProps) {
  const { id, sourceX, sourceY, targetX, targetY, sourcePosition, targetPosition, source, target, markerEnd, style, label } = props;

  if (source !== target) {
    const [edgePath, labelX, labelY] = getBezierPath({
      sourceX, sourceY, sourcePosition, targetX, targetY, targetPosition,
    });
    return <BaseEdge id={id} path={edgePath} markerEnd={markerEnd} style={style} label={label} labelX={labelX} labelY={labelY} />;
  }

  // Self-loop: arc out from the node's bottom and back into its top, radius scaled to the node so it
  // reads as "loops back to itself" rather than overlapping the card.
  const radiusX = 60;
  const radiusY = 50;
  const edgePath = `M ${sourceX - 5} ${sourceY} A ${radiusX} ${radiusY} 0 1 0 ${targetX + 5} ${targetY}`;
  return <BaseEdge id={id} path={edgePath} markerEnd={markerEnd} style={style} label={label} labelX={sourceX + radiusX} labelY={(sourceY + targetY) / 2} />;
}

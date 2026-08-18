// A custom edge that arcs around a node when source === target (a loop-back/self-referencing edge),
// following react-flow's own documented pattern (reactflow.dev/examples/edges/custom-edges). The
// runtime cannot execute a cycle (I3: a flow compiles once into a flat array walked in a single
// zero-alloc pass — no loop-iteration semantics exist), so `validateGraph`'s cycle check already
// rejects one at the errors banner. This component is purely so a user CAN draw one and see it
// clearly instead of a zero-length/invisible edge — UI-completeness, not new execution capability.
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

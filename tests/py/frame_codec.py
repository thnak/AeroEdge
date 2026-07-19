# Python mirror of include/aero/transport/frame_codec.hpp — the SAME little-endian MessageFrame wire
# layout, so the gRPC echo server can decode/verify/re-encode real AeroEdge frames. Keep in lockstep with
# the C++ header (any field add/reorder must change both). Layout:
#   u64 from | u64 to | u64 target.type | u64 target.key | u64 msg_type
#   u8 mode  | u8 kind | i64 deadline_ns | u64 trace_id
#   u64 principal.subject | u64 principal.rights
#   u32 payload_len | payload bytes
import struct

_HEADER = struct.Struct("<QQQQQ B B q Q Q Q I")  # matches the fixed 78-byte header above
HEADER_BYTES = _HEADER.size


def decode_frame(buf: bytes):
    if len(buf) < HEADER_BYTES:
        return None
    (frm, to, ttype, tkey, mtype, mode, kind, deadline, trace, subj, rights, plen) = _HEADER.unpack_from(buf, 0)
    if len(buf) < HEADER_BYTES + plen:
        return None
    payload = buf[HEADER_BYTES:HEADER_BYTES + plen]
    return {
        "from": frm, "to": to, "target_type": ttype, "target_key": tkey, "msg_type": mtype,
        "mode": mode, "kind": kind, "deadline_ns": deadline, "trace_id": trace,
        "subject": subj, "rights": rights, "payload": payload,
    }


def encode_frame(f: dict) -> bytes:
    payload = f.get("payload", b"")
    head = _HEADER.pack(
        f["from"], f["to"], f["target_type"], f["target_key"], f["msg_type"],
        f["mode"], f["kind"], f["deadline_ns"], f["trace_id"],
        f["subject"], f["rights"], len(payload),
    )
    return head + payload

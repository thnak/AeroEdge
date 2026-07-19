#!/usr/bin/env python3
# A real gRPC server for the AeroEdge GrpcClientTransport gate (014 §6). Launched by the C++ test via
# `uv run --with grpcio tests/py/grpc_echo_server.py <port>`. It exposes ONE bidirectional-streaming
# method, /aero.Transport/Exchange, over real HTTP/2 (grpcio's C-core) — the exact wire the hand-written
# C++ HTTP/2 client must speak. No .proto/protoc: a generic handler with identity (bytes) codecs.
#
# Behaviour: it DECODES each inbound MessageFrame with AeroEdge's own frame codec (reimplemented in
# frame_codec.py) to prove the C++ encoding is real & correct, then ECHOES a frame back with from/to
# swapped and trace_id preserved — so the C++ client's on_receive observes a genuine HTTP/2 round-trip.
import struct
import sys
from concurrent import futures

import grpc

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from frame_codec import decode_frame, encode_frame  # noqa: E402


def exchange(request_iterator, context):
    # Each `req` is the raw gRPC message bytes == one encoded MessageFrame (no seq prefix: a single
    # HTTP/2 stream is order-preserving, so gRPC needs no resequencer — 014 §6).
    for req in request_iterator:
        f = decode_frame(req)
        if f is None:
            continue
        # Echo back with endpoints swapped (a plausible "reply" frame); keep trace_id as the order key.
        f["from"], f["to"] = f["to"], f["from"]
        yield encode_frame(f)


def main(port: int) -> None:
    handlers = {
        "Exchange": grpc.stream_stream_rpc_method_handler(
            exchange,
            request_deserializer=lambda b: b,  # identity: the message IS the frame bytes
            response_serializer=lambda b: b,
        )
    }
    generic = grpc.method_handlers_generic_handler("aero.Transport", handlers)
    server = grpc.server(futures.ThreadPoolExecutor(max_workers=4))
    server.add_generic_rpc_handlers((generic,))
    server.add_insecure_port(f"127.0.0.1:{port}")
    server.start()
    print(f"GRPC_READY {port}", flush=True)
    server.wait_for_termination()


if __name__ == "__main__":
    main(int(sys.argv[1]))

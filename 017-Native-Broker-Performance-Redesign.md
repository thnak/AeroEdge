# 017 Follow-on: Native Broker I/O & Fan-out Architecture Redesign (WIP)

Status: research/design phase — no implementation started. This document is the living
record of that phase, per the working agreement: full feature inventory and validated
technical claims come first; no new production code lands until both are done.

## Background

`bench/broker/broker_bench.cpp` (merged in `e5481d7`) measured two real, unaddressed
bottlenecks in `NativeBroker` beyond the already-fixed linear session scan
(`route_publish()`'s topic index, same commit):

1. **Unbuffered, byte-at-a-time packet reads** — `mqtt_codec.hpp`'s `read_packet()` does
   3+ separate `recv()` syscalls per packet (1 for the fixed header, 1+ for the
   remaining-length varint, 1+ for the body), plus `NativeBroker::session_loop()` adds a
   4th (`wait_readable` poll) before every read. Measured ceiling: ~23K msg/s (QoS 1,
   round-trip-bound) / ~63K msg/s (QoS 0, backlog-bound) for a single publisher→single
   subscriber pair, no fan-out.
2. **Synchronous, serial, in-thread fan-out** — `route_publish()` writes to each matching
   session one at a time, inline, on the publishing session's own thread. Measured: 1→10
   subscribers dropped throughput ~7x (62K → 8.9K msg/s at QoS 0).

A first attempt at (2) — a small fixed-size worker pool + `std::latch` barrier — was
built, passed the test suite on early runs, then produced an intermittent severe stall
under repeated stress testing. Root cause was not conclusively isolated (evidence points
at Windows thread-creation-cost variability rather than a logic bug, but this was not
proven) and the change was reverted rather than shipped unverified. See conversation
history / commit `e5481d7` for the full trace.

**Working agreement for this round:** no more piecemeal patches. Full feature inventory
first, then a validated design (claims checked against real, isolated tests/benchmarks
by sub-agents), then implementation — in that order.

## Phase 1 — Feature inventory

(filled in by research pass — see below)

## Phase 2 — Design candidates + validated claims

(not started)

## Phase 3 — Implementation

(not started)

// AeroEdge Phase-8 LOAD-BEARING gate (spec 010 §3): FENCED MIGRATION — the no-dual-drive guarantee.
//
// The hard industrial-safety requirement: when a device actor migrates from node A to node B, the same
// physical device is NEVER driven by two activations at once. AeroEdge does not build this — it rides
// Quark 012's fence TOKEN (acquired inside `DurableState::recover`, 007 §7). This test PROVES the four
// migration properties on the real Quark `InMemoryStore`:
//
//   (i)   B RECOVERS A's committed state    — the migrated actor resumes from A's last durable snapshot.
//   (ii)  A's STALE write is REJECTED       — A's post-migration checkpoint (old fence f1 < owner f2) is
//                                             fenced out by the store; the device state it tried to write
//                                             does NOT land (no dual-drive).
//   (iii) B's write is ACCEPTED             — B's checkpoint (fresh fence f2 == owner) succeeds.
//   (iv)  the DRIVER is RE-OPENED on B       — re-activation re-dials the device link (010 §3 rule),
//                                             observed by an open() call count.
//
// Single-threaded + deterministic ⇒ TSan-clean. Exit code 0 = OK; prints "FAIL" on any mismatch.
#include <cstdint>
#include <cstdio>

#include "aero/cluster/migration.hpp"
#include "aero/drivers/generator_driver.hpp"

#include "quark/core/persistence.hpp"
#include "quark/core/serialize.hpp"
#include "quark/core/snapshot.hpp"

using namespace quark;

// --- Durable device state (007 §1): the actuator setpoint the actor commits + a write counter. ------
struct DeviceState {
    std::uint64_t setpoint = 0;
    std::uint64_t writes = 0;
};
QUARK_SERIALIZE(DeviceState, (1, setpoint), (2, writes))

namespace {

// A driver with an OBSERVABLE open() count — proves the actor re-dials its device on re-activation
// (010 §3). Otherwise a no-op stand-in for a device link (the real socket driver is gated on 019).
class CountingDriver final : public aero::IDriver {
public:
    aero::DriverStatus open(const aero::DriverConfig&) noexcept override {
        ++opens_;
        return aero::DriverStatus::Ok;
    }
    aero::DriverStatus run(aero::StreamSink<aero::Frame>, aero::StopToken) noexcept override {
        return aero::DriverStatus::Ok;
    }
    void close() noexcept override {}
    const aero::DriverDescriptor& descriptor() const noexcept override { return kDesc; }
    [[nodiscard]] std::uint64_t opens() const noexcept { return opens_; }
    static constexpr aero::DriverDescriptor kDesc{"aero.driver.counting", /*writable*/ true};

private:
    std::uint64_t opens_ = 0;
};

}  // namespace

int main() {
    bool ok = true;

    // ONE store + ONE ActorId shared across the two activations — exactly what a real migration sees:
    // the durable record + the fence owner live in the store, node-independent.
    InMemoryStore store;
    const ActorId id{TypeKey{0xDE71CE}, 42};
    const aero::DriverConfig cfg{"opc.tcp://plc-line3:4840", /*frame_count*/ 0, /*rate_hz*/ 0};

    // ===== Activation on node A =====================================================================
    CountingDriver drvA;
    aero::cluster::EdgeActivation<DeviceState, InMemoryStore> a(store, id, drvA, cfg);
    const auto ra = a.activate(DeviceState{});
    ok &= (!ra.recovered);                                   // cold start on A (no prior snapshot)
    ok &= (ra.driver == aero::DriverStatus::Ok && drvA.opens() == 1);  // A opened the device once
    const FenceToken f1 = ra.fence;

    // A commits device state (setpoint 42) — durable under fence f1.
    a.durable().state().setpoint = 42;
    a.durable().state().writes = 1;
    ok &= a.durable().checkpoint();                          // accepted: f1 == current owner
    ok &= (store.current_owner(id) == f1);

    // ===== Migrate: re-activate on node B (fresh EdgeActivation over the SAME store + id) ===========
    CountingDriver drvB;
    aero::cluster::EdgeActivation<DeviceState, InMemoryStore> b(store, id, drvB, cfg);
    const auto rb = b.activate(DeviceState{});
    const FenceToken f2 = rb.fence;

    // (iv) DRIVER RE-OPENED on the new node.
    ok &= (rb.driver == aero::DriverStatus::Ok && drvB.opens() == 1);
    // (i) B RECOVERED A's committed state.
    ok &= (rb.recovered && b.durable().state().setpoint == 42 && b.durable().state().writes == 1);
    // Fresh fence strictly greater than A's — A is now superseded as the owner.
    ok &= (f2.value > f1.value && store.current_owner(id) == f2);

    // ===== (ii) A's STALE write is REJECTED (no dual-drive) ========================================
    // A is partitioned/superseded but still "alive"; it tries to push a setpoint of 999. Its fence f1
    // is now older than the owner f2, so the store MUST reject the write — the device state stays A's
    // last committed 42, never the stale 999.
    a.durable().state().setpoint = 999;
    const bool stale_ok = a.durable().checkpoint();
    ok &= (!stale_ok);                                       // REJECTED (fenced out)
    {
        auto snap = load_snapshot<DeviceState>(store, id);
        const bool held = snap && snap->has_value() && (*snap)->state.setpoint == 42;
        ok &= held;                                          // store still holds A's committed 42, not 999
    }

    // ===== (iii) B's write is ACCEPTED =============================================================
    b.durable().state().setpoint = 100;
    b.durable().state().writes = 2;
    const bool b_ok = b.durable().checkpoint();
    ok &= b_ok;                                              // accepted: f2 == current owner
    {
        auto snap = load_snapshot<DeviceState>(store, id);
        const bool wrote = snap && snap->has_value() && (*snap)->state.setpoint == 100;
        ok &= wrote;                                         // B is the sole legitimate writer now
    }

    std::printf("[fence] f1=%llu f2=%llu (f2>f1: %s) | B.recovered=%d setpoint=42 | "
                "A.stale_write=%s B.write=%s | drvA.opens=%llu drvB.opens=%llu\n",
                (unsigned long long)f1.value, (unsigned long long)f2.value,
                f2.value > f1.value ? "yes" : "NO", rb.recovered,
                stale_ok ? "ACCEPTED(BUG)" : "REJECTED", b_ok ? "ACCEPTED" : "REJECTED(BUG)",
                (unsigned long long)drvA.opens(), (unsigned long long)drvB.opens());
    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

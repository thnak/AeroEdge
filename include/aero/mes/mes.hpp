// AeroEdge MES integration hook — the canonical DTOs + the IMesAdapter seam (spec 012 §3).
//
// AeroEdge DEFINES this hook; an MES vendor/site IMPLEMENTS an adapter behind it (012 §1). Flows and
// actors only ever speak these AeroEdge-canonical DTOs (M1) — swapping the MES swaps only the adapter,
// never a flow. The MesGateway actor owns the adapter instance and is the ONLY caller, so the adapter
// may block on I/O on the gateway's lane; a flow NEVER blocks on the MES (M2). This header is pure
// contract (no httplib, no runtime) so aero-core/aero-nodes could depend on it without pulling HTTP —
// the concrete RestMesAdapter (httplib) lives in rest_mes_adapter.hpp, fenced to aero-mes (R1).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace aero::mes {

// ---- Outbound: AeroEdge → MES (report) -----------------------------------------------------------
// The canonical report an edge event maps to (012 §2.1). The adapter translates it to the MES's native
// schema. `idempotency_key` is the at-least-once dedup key (M3): a retried delivery carries the SAME
// key so the MES counts it once. `seq` is the outbox's monotonic sequence (ordering / key derivation).
struct MesReport {
    enum class Kind : std::uint8_t { Production, Alarm, TagSample };
    Kind kind = Kind::Production;
    std::string line;             // production line / device id
    std::string label;            // metric name / alarm code
    double value = 0.0;           // produced count / measurement / severity
    std::uint64_t seq = 0;        // outbox monotonic sequence
    std::string idempotency_key;  // MES-side dedup key (M3) — stable across retries

    [[nodiscard]] const char* kind_name() const noexcept {
        switch (kind) {
            case Kind::Production: return "production";
            case Kind::Alarm:      return "alarm";
            case Kind::TagSample:  return "tag_sample";
        }
        return "production";
    }
};

// ---- Inbound: MES → AeroEdge (command) -----------------------------------------------------------
// The canonical command the MES drives production with (012 §2.2). Delivered into AeroEdge as an
// ordinary Command via the adapter's command sink, then it is a normal Command-triggered Flow (M4).
struct MesCommand {
    enum class Kind : std::uint8_t { Order, Recipe, StartLine, StopLine };
    Kind kind = Kind::Order;
    std::string line;      // target line / device
    std::string ref;       // order number / recipe id
    double value = 0.0;    // quantity / setpoint
};

// The sink the adapter invokes to deliver an inbound MES command (poll / webhook / subscription — the
// adapter's choice, 012 §2.2). AeroEdge wires this to a `tell` into the target actor (M4).
using MesCommandSink = std::function<void(const MesCommand&)>;

// ---- Lifecycle + status ---------------------------------------------------------------------------
enum class MesStatus : std::uint8_t { Ok, Unreachable, AuthFailed, Error };
enum class MesResult : std::uint8_t { Delivered, Retry, Rejected };

// Connection knobs handed to connect() (012 §5). Credentials are a Quark 020 secret handle in a real
// deployment (M5) — NEVER embedded in a flow/node JSON; Phase-10 keeps the minimal REST target set.
struct MesConfig {
    std::string endpoint;   // MES base URL / host
    int port = 0;           // explicit port (REST adapter)
    std::string report_path = "/production";  // outbound report route
    std::string token;      // opaque auth token (a Quark 020 secret in production, M5)
};

struct MesAdapterDescriptor {
    std::string_view type_id;   // e.g. "aero.mes.rest"
    bool outbound = true;       // supports report()
    bool inbound = false;       // supports a command sink
};

// The seam AeroEdge defines (012 §3). An MES adapter implements it; the MesGateway actor owns the
// instance and is the only caller. report() may block (gateway lane); the gateway wraps it in the
// durable-outbox retry (M3). Never called from a flow (M2).
class IMesAdapter {
public:
    virtual ~IMesAdapter() = default;

    // Outbound: report an edge event. `Delivered` → the gateway drops it from the outbox; `Retry` →
    // it STAYS durably in the outbox to be re-sent (an MES outage delays, never drops — M3);
    // `Rejected` → a permanent 4xx (bad request), also dropped so a poison report can't wedge the drain.
    virtual MesResult report(const MesReport& r) = 0;

    // Inbound: register the sink the adapter delivers MES-originated commands to (012 §2.2). Default:
    // an outbound-only adapter ignores it.
    virtual void set_command_sink(MesCommandSink /*sink*/) {}

    // Connect/authenticate to the MES; called on gateway activation (012 §3).
    virtual MesStatus connect(const MesConfig& cfg) = 0;

    virtual const MesAdapterDescriptor& descriptor() const noexcept = 0;
};

}  // namespace aero::mes

// AeroEdge SDK — the IDriver contract (spec 006).
//
// A Driver bridges a device protocol into frames and feeds them into an actor's inbound stream. It
// runs on an I/O / blocking lane, never inside a flow (D6). This is the Phase-1 declaration of the
// seam; the streaming producer path (StreamSink over Quark 024) and discovery/test hooks (015 §5)
// are wired in Phase 2. Kept here so the SDK contract is frozen early (013 T1).
#pragma once

#include <string_view>

namespace aero {

enum class DriverStatus {
    Ok,
    Error,
    Unsupported,  // capability not offered by this driver (e.g. write on a read-only driver)
};

struct DriverDescriptor {
    std::string_view type_id;   // e.g. "aero.driver.tcp"
    bool writable = false;      // supports device writes (actuators / OTA transfer, 006 §7)
};

// Full contract (open/run/poll/write/close) lands in Phase 2 with the streaming producer path.
class IDriver {
public:
    virtual ~IDriver() = default;
    virtual const DriverDescriptor& descriptor() const noexcept = 0;
};

}  // namespace aero

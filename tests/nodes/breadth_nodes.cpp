// AeroEdge Phase-10 breadth gate (spec 005 §2): the new pure compute nodes each transform correctly,
// and malformed input is handled WITHOUT a crash (a clean NodeResult::Error, never UB). No engine, no
// socket — each node is driven directly over a ProcessingContext (like flow_zero_alloc), so the test is
// a deterministic unit. Exit code 0 = OK; prints "FAIL" on any mismatch.
#include <cstdint>
#include <cstdio>
#include <string>

#include "aero/nodes/compute_nodes.hpp"
#include "aero/sdk/processing_context.hpp"

using namespace aero;

static ProcessingContext make_ctx() {
    ProcessingContext ctx;
    ctx.reset(nullptr);
    return ctx;
}

int main() {
    bool ok = true;

    // --- MeanNode: windowless mean over tags -----------------------------------------------------
    {
        nodes::MeanNode n;
        auto ctx = make_ctx();
        ctx.tags = {{"a", 2.0}, {"b", 4.0}, {"c", 6.0}};
        const auto r = n.process(ctx);
        const bool pass = r == NodeResult::Continue && !ctx.output.empty() && ctx.output.back() == 4.0;
        ok &= pass;
        std::printf("[mean]   mean(2,4,6)=%.1f (expect 4.0) %s\n", ctx.output.back(), pass ? "ok" : "FAIL");
    }

    // --- MinMaxNode: [min, max] over tags --------------------------------------------------------
    {
        nodes::MinMaxNode n;
        auto ctx = make_ctx();
        ctx.tags = {{"a", 5.0}, {"b", -1.0}, {"c", 3.0}};
        n.process(ctx);
        const bool pass = ctx.output.size() == 2 && ctx.output[0] == -1.0 && ctx.output[1] == 5.0;
        ok &= pass;
        std::printf("[minmax] min=%.1f max=%.1f (expect -1,5) %s\n", ctx.output[0], ctx.output[1],
                    pass ? "ok" : "FAIL");
    }

    // --- SumNode: total over tags ----------------------------------------------------------------
    {
        nodes::SumNode n;
        auto ctx = make_ctx();
        ctx.tags = {{"a", 1.0}, {"b", 2.0}, {"c", 3.0}};
        n.process(ctx);
        const bool pass = !ctx.output.empty() && ctx.output.back() == 6.0;
        ok &= pass;
        std::printf("[sum]    sum(1,2,3)=%.1f (expect 6.0) %s\n", ctx.output.back(), pass ? "ok" : "FAIL");
    }

    // --- CrcNode: CRC-16/CCITT-FALSE over payload bytes. Known check vector: "123456789" => 0x29B1 --
    {
        nodes::CrcNode n;
        auto ctx = make_ctx();
        ctx.payload = "123456789";
        n.process(ctx);
        const auto crc = static_cast<std::uint16_t>(ctx.output.back());
        const bool pass = crc == 0x29B1;
        ok &= pass;
        std::printf("[crc]    crc16(\"123456789\")=0x%04X (expect 0x29B1) %s\n", crc, pass ? "ok" : "FAIL");
    }

    // --- ModbusDecodeNode: big-endian 16-bit registers into tags ---------------------------------
    {
        nodes::ModbusDecodeNode n;
        auto ctx = make_ctx();
        // regs: 0x000A=10, 0x0100=256
        ctx.payload = std::string({0x00, 0x0A, 0x01, 0x00});
        const auto r = n.process(ctx);
        const bool pass = r == NodeResult::Continue && ctx.tags.size() == 2 &&
                          ctx.tags[0].value == 10.0 && ctx.tags[1].value == 256.0 &&
                          ctx.tags[0].name == "reg0" && ctx.tags[1].name == "reg1";
        ok &= pass;
        std::printf("[modbus] reg0=%.0f reg1=%.0f (expect 10,256) %s\n",
                    ctx.tags.size() > 0 ? ctx.tags[0].value : -1.0,
                    ctx.tags.size() > 1 ? ctx.tags[1].value : -1.0, pass ? "ok" : "FAIL");
    }
    {
        // Malformed: an odd-length (torn) Modbus frame => clean Error, no crash.
        nodes::ModbusDecodeNode n;
        auto ctx = make_ctx();
        ctx.payload = std::string({0x00, 0x0A, 0x01});  // 3 bytes — not a whole register
        const auto r = n.process(ctx);
        const bool pass = r == NodeResult::Error;
        ok &= pass;
        std::printf("[modbus] odd-length frame => %s (expect Error) %s\n",
                    r == NodeResult::Error ? "Error" : "Continue", pass ? "ok" : "FAIL");
    }

    // --- JsonParseNode: parse {name:number} into tags; malformed => Error --------------------------
    {
        nodes::JsonParseNode n;
        auto ctx = make_ctx();
        ctx.payload = R"({"temp": 21.5, "rpm": 1500, "name": "ignored"})";
        const auto r = n.process(ctx);
        double temp = -1, rpm = -1;
        for (const auto& t : ctx.tags) {
            if (t.name == "temp") temp = t.value;
            if (t.name == "rpm") rpm = t.value;
        }
        const bool pass = r == NodeResult::Continue && temp == 21.5 && rpm == 1500.0 && ctx.tags.size() == 2;
        ok &= pass;
        std::printf("[json]   temp=%.1f rpm=%.0f tags=%zu (non-number skipped) %s\n", temp, rpm,
                    ctx.tags.size(), pass ? "ok" : "FAIL");
    }
    {
        // Malformed JSON => clean Error, no crash, no throw (exceptions OFF).
        nodes::JsonParseNode n;
        auto ctx = make_ctx();
        ctx.payload = "{ this is not json";
        const auto r = n.process(ctx);
        const bool pass = r == NodeResult::Error;
        ok &= pass;
        std::printf("[json]   malformed => %s (expect Error) %s\n",
                    r == NodeResult::Error ? "Error" : "Continue", pass ? "ok" : "FAIL");
    }

    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

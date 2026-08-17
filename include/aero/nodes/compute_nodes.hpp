// AeroEdge Phase-10 compute nodes (spec 005 §2 node breadth). Pure, socket-free, fully testable
// transforms/sources over the ProcessingContext — no I/O, no driver, no sockets (so each is a
// deterministic unit under breadth_nodes.cpp). They obey N1 on the steady compute path (write only the
// reused ctx buffers) EXCEPT the parsing Source (JsonParseNode), which is inherently allocating because
// parsing a wire frame builds a DOM — the 0-alloc invariant is a *compute*-path property (005 §7); a
// decode/parse Source that touches the frame arena is the documented exception (the 0-alloc gate,
// flow_zero_alloc.cpp, runs only the non-parsing nodes and stays green).
//
// Real socket protocol drivers (MQTT / Modbus-TCP / OPC UA) are GATED — they need the Quark PAL (019)
// sockets that are not available offline (see generator_driver.hpp TcpDriver, transport gate). What IS
// testable offline is a Modbus register-map DECODER over bytes that already arrived: ModbusDecodeNode
// decodes a holding-register payload with NO socket, so it ships here; the socket transport stays gated.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

#include "aero/sdk/node.hpp"
#include "nlohmann/json.hpp"

namespace aero::nodes {

// Transform: the windowless arithmetic mean over the working-set tags (005 §2). Distinct from
// MovingAverageNode<K> (a K-sample sliding window) — this reduces the CURRENT frame's tags to one mean.
// 0-alloc: reads ctx.tags, writes only the reused output/events buffers (N1). Empty tags → mean 0.
class MeanNode final : public INode {
public:
    NodeResult process(ProcessingContext& ctx) noexcept override {
        double sum = 0.0;
        for (const auto& t : ctx.tags) sum += t.value;
        const double mean = ctx.tags.empty() ? 0.0 : sum / static_cast<double>(ctx.tags.size());
        ctx.output.push_back(mean);
        ctx.events.push_back(Event{"Mean", mean});
        return NodeResult::Continue;
    }
    const NodeDescriptor& descriptor() const noexcept override { return kDesc; }
    static constexpr NodeDescriptor kDesc{NodeCategory::Transform, "aero.transform.mean"};
};

// Transform: min and max over the working-set tags, staged as two outputs [min, max] (005 §2). A
// SPC/limit building block. 0-alloc (writes only the reused buffers). Empty tags → [0, 0].
class MinMaxNode final : public INode {
public:
    NodeResult process(ProcessingContext& ctx) noexcept override {
        double lo = 0.0, hi = 0.0;
        bool first = true;
        for (const auto& t : ctx.tags) {
            if (first) { lo = hi = t.value; first = false; }
            else { lo = std::min(lo, t.value); hi = std::max(hi, t.value); }
        }
        ctx.output.push_back(lo);
        ctx.output.push_back(hi);
        ctx.events.push_back(Event{"MinMax", hi});
        return NodeResult::Continue;
    }
    const NodeDescriptor& descriptor() const noexcept override { return kDesc; }
    static constexpr NodeDescriptor kDesc{NodeCategory::Transform, "aero.transform.minmax"};
};

// Transform: the running sum of the working-set tags, staged as one output (005 §2). A totalizer
// building block distinct from the SumOutputNode (which is an Output-category egress node). 0-alloc.
class SumNode final : public INode {
public:
    NodeResult process(ProcessingContext& ctx) noexcept override {
        double sum = 0.0;
        for (const auto& t : ctx.tags) sum += t.value;
        ctx.output.push_back(sum);
        ctx.events.push_back(Event{"Sum", sum});
        return NodeResult::Continue;
    }
    const NodeDescriptor& descriptor() const noexcept override { return kDesc; }
    static constexpr NodeDescriptor kDesc{NodeCategory::Transform, "aero.transform.sum"};
};

// Transform: CRC-16/CCITT-FALSE over the raw frame bytes (ctx.payload), staged as a tag + output
// (006 §wire integrity). A frame-integrity building block: a Rule node can then compare it to a
// trailing checksum tag. 0-alloc: the table-free bitwise CRC touches only locals + the reused buffers.
class CrcNode final : public INode {
public:
    NodeResult process(ProcessingContext& ctx) noexcept override {
        std::uint16_t crc = 0xFFFF;  // CCITT-FALSE init
        for (const unsigned char b : ctx.payload) {
            crc ^= static_cast<std::uint16_t>(static_cast<std::uint16_t>(b) << 8);
            for (int i = 0; i < 8; ++i) {
                crc = (crc & 0x8000) ? static_cast<std::uint16_t>((crc << 1) ^ 0x1021)
                                     : static_cast<std::uint16_t>(crc << 1);
            }
        }
        ctx.tags.push_back(Tag{"crc16", static_cast<double>(crc)});
        ctx.output.push_back(static_cast<double>(crc));
        return NodeResult::Continue;
    }
    const NodeDescriptor& descriptor() const noexcept override { return kDesc; }
    static constexpr NodeDescriptor kDesc{NodeCategory::Transform, "aero.transform.crc"};
};

// Source: decode a Modbus holding-register payload (big-endian 16-bit registers) in ctx.payload into
// tags named "reg0","reg1",… (006 §protocol breadth). This is the register-map DECODE over bytes that
// ALREADY ARRIVED — NO socket, so it is fully testable while the Modbus-TCP socket transport stays
// gated on the Quark PAL (019). An odd-length payload (a torn frame) is a clean Error, never a crash.
class ModbusDecodeNode final : public INode {
public:
    NodeResult process(ProcessingContext& ctx) noexcept override {
        const std::size_t n = ctx.payload.size();
        if (n % 2 != 0) return NodeResult::Error;  // registers are 16-bit — an odd length is a torn frame
        const auto* p = reinterpret_cast<const unsigned char*>(ctx.payload.data());
        // reg_names_ is a STABLE, monotonically-grown cache of tag-name strings (Tag::name views it).
        // Reserve to this frame's width up front so no push_back below reallocates mid-frame — a realloc
        // would move the std::string objects and dangle the views already pushed into ctx.tags.
        reg_names_.reserve(n / 2);
        for (std::size_t i = 0; i + 1 < n; i += 2) {
            const std::uint16_t reg = static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[i]) << 8) |
                                                                 static_cast<std::uint16_t>(p[i + 1]));
            const std::size_t idx = i / 2;
            if (idx >= reg_names_.size()) reg_names_.push_back("reg" + std::to_string(idx));
            ctx.tags.push_back(Tag{reg_names_[idx], static_cast<double>(reg)});
        }
        return NodeResult::Continue;
    }
    const NodeDescriptor& descriptor() const noexcept override { return kDesc; }
    static constexpr NodeDescriptor kDesc{NodeCategory::Source, "aero.source.modbus"};

private:
    // Stable backing storage for the tag-name string_views (Tag::name is a view). Amortized: grown once
    // as frames widen, then reused — the steady path reads the same strings (config-time growth, N3).
    std::vector<std::string> reg_names_;
};

// Source: decode a Modbus coil/discrete-input payload (bit-packed, LSB-first per byte — Modbus's own
// wire order) in ctx.payload into tags named "bit0","bit1",… (018 §8, M9.1's FC01/FC02 counterpart to
// ModbusDecodeNode's word-based decode; a separate node because bit-packed and word-packed responses
// need different unpacking, not just a different tag count).
//
// A byte-packed response can carry MORE bits than were actually requested (padding fills out the last
// byte) — with no side channel, this node would have no way to tell real bits from padding. It reads
// the exact requested count from `ctx.frame->raw`: ModbusTcpDriver stashes the coil/discrete-input
// count there for bit-packed reads (Frame::raw is otherwise unused by any Modbus decode path). Falls
// back to decoding every available bit (payload.size()*8, no trimming) when ctx.frame is null (this
// node built/driven directly in a test, no real Frame behind it) or raw is unset/out of range — the
// same offline-testable posture ModbusDecodeNode already has.
class ModbusBitsDecodeNode final : public INode {
public:
    NodeResult process(ProcessingContext& ctx) noexcept override {
        const std::size_t n = ctx.payload.size();
        const std::size_t max_bits = n * 8;
        std::size_t bit_count = max_bits;
        if (ctx.frame != nullptr && ctx.frame->raw > 0 &&
            static_cast<std::uint64_t>(ctx.frame->raw) <= max_bits) {
            bit_count = static_cast<std::size_t>(ctx.frame->raw);
        }

        const auto* p = reinterpret_cast<const unsigned char*>(ctx.payload.data());
        // bit_names_ — same stable-cache reasoning as ModbusDecodeNode::reg_names_ (Tag::name is a view).
        bit_names_.reserve(bit_count);
        for (std::size_t i = 0; i < bit_count; ++i) {
            const bool bit = ((p[i / 8] >> (i % 8)) & 0x1) != 0;
            if (i >= bit_names_.size()) bit_names_.push_back("bit" + std::to_string(i));
            ctx.tags.push_back(Tag{bit_names_[i], bit ? 1.0 : 0.0});
        }
        return NodeResult::Continue;
    }
    const NodeDescriptor& descriptor() const noexcept override { return kDesc; }
    static constexpr NodeDescriptor kDesc{NodeCategory::Source, "aero.source.modbus_bits"};

private:
    std::vector<std::string> bit_names_;
};

// Source: parse a JSON object payload (ctx.payload) of {"name": number, …} into working-set tags
// (005 §2). Uses nlohmann with exceptions OFF so a malformed payload is a clean Error (no throw, no
// crash) — NOT a 0-alloc node: parsing a wire frame builds a DOM and is inherently allocating, so it
// is excluded from the 0-alloc compute gate (see header note). Non-object / non-number members are
// skipped; a discarded parse is an Error.
class JsonParseNode final : public INode {
public:
    NodeResult process(ProcessingContext& ctx) noexcept override {
        auto j = nlohmann::json::parse(ctx.payload, /*cb=*/nullptr, /*allow_exceptions=*/false);
        if (j.is_discarded() || !j.is_object()) return NodeResult::Error;
        key_store_.clear();
        key_store_.reserve(j.size());  // no realloc during the loop → the Tag::name views stay valid
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (!it.value().is_number()) continue;
            key_store_.push_back(it.key());  // own the key text so Tag::name (a view) stays valid
            ctx.tags.push_back(Tag{key_store_.back(), it.value().get<double>()});
        }
        return NodeResult::Continue;
    }
    const NodeDescriptor& descriptor() const noexcept override { return kDesc; }
    static constexpr NodeDescriptor kDesc{NodeCategory::Source, "aero.source.json"};

private:
    std::vector<std::string> key_store_;  // stable backing for parsed key names (Tag::name views it)
};

}  // namespace aero::nodes

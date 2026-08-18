// AeroEdge — NativeBroker load/latency benchmark (not a ctest gate; run manually).
//
// Purpose: give a real number before trading the broker's current "simple, auditable" design
// (fine-grained locks, snapshot-then-unlock, linear-scan topic/ACL matching, thread-per-connection —
// see include/aero/broker/native_broker.hpp's own comments) for higher-throughput techniques (memory
// pooling, zero-copy buffers, write batching, a topic trie, epoll/io_uring). Drives a real NativeBroker
// over real loopback sockets with the SAME hand-rolled MQTT client primitives tests/broker/native_broker.cpp
// uses (mqtt_codec.hpp) — no external MQTT client/broker dependency.
//
// Measures, for a configurable fan-out shape:
//   - publish-side throughput (messages/sec, MB/sec) — bounded by QoS 1's per-message PUBACK round trip
//     when --qos 1, or unbounded fire-and-forget when --qos 0.
//   - end-to-end delivery latency (min/p50/p95/p99/max), timestamped in each payload's first 8 bytes.
//   - the cost of `route_publish()`'s linear scan over ALL sessions (not just matching ones) via
//     --idle-sessions: extra connected+subscribed clients on topics that never match, present in
//     `sessions_` and scanned on every publish, but never fanned out to. This is the one knob that
//     speaks directly to "would a topic trie/index help" — if p99 latency and throughput barely move as
//     --idle-sessions grows, the linear scan (acl.hpp's documented tradeoff) isn't the bottleneck.
//
// Usage: broker_bench [--subscribers N] [--idle-sessions N] [--publishers N] [--messages N]
//                      [--payload-bytes N] [--qos 0|1] [--timeout-s N]
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "aero/broker/native_broker.hpp"

namespace mqtt = aero::transport::mqtt;
using aero::broker::Config;
using aero::broker::NativeBroker;

namespace {

// ===== CLI ==============================================================================================
struct Options {
    int subscribers = 20;
    int idle_sessions = 0;
    int publishers = 4;
    int messages = 2000;      // per publisher
    int payload_bytes = 64;   // clamped to >= 8 (leading 8 bytes carry the send timestamp)
    int qos = 1;
    int timeout_s = 30;       // max wait for delivery to finish once publishing is done
    int external_port = 0;    // 0 = spin up an in-process NativeBroker (default); nonzero = dial an
                               // already-running MQTT 3.1.1 broker on 127.0.0.1:external_port instead, for
                               // apples-to-oranges comparisons against other brokers using the exact same
                               // client-side protocol code and measurement methodology.
};

[[noreturn]] void usage_and_exit(const char* prog) {
    std::fprintf(stderr,
                  "usage: %s [--subscribers N] [--idle-sessions N] [--publishers N] [--messages N]\n"
                  "          [--payload-bytes N] [--qos 0|1] [--timeout-s N] [--external-port N]\n",
                  prog);
    std::exit(2);
}

Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        if (i + 1 >= argc) usage_and_exit(argv[0]);
        const int val = std::atoi(argv[++i]);
        if (flag == "--subscribers") o.subscribers = val;
        else if (flag == "--idle-sessions") o.idle_sessions = val;
        else if (flag == "--publishers") o.publishers = val;
        else if (flag == "--messages") o.messages = val;
        else if (flag == "--payload-bytes") o.payload_bytes = val;
        else if (flag == "--qos") o.qos = val;
        else if (flag == "--timeout-s") o.timeout_s = val;
        else if (flag == "--external-port") o.external_port = val;
        else usage_and_exit(argv[0]);
    }
    if (o.payload_bytes < 8) o.payload_bytes = 8;  // leading 8 bytes are the send timestamp
    if (o.qos != 0 && o.qos != 1) usage_and_exit(argv[0]);
    return o;
}

// ===== shared socket-level helpers (mirrors tests/broker/native_broker.cpp's TestClient) ===============
quark::pal::fd_t dial_loopback(std::uint16_t port) {
    auto fd = quark::pal::tcp_connect(quark::pal::ipv4_loopback, port);
    if (!fd) return quark::pal::invalid_fd;
    const auto writable = aero::pal::wait_writable(*fd, 2000);
    if (!writable || !*writable || !quark::pal::connect_result(*fd)) {
        quark::pal::close_fd(*fd);
        return quark::pal::invalid_fd;
    }
    return *fd;
}

std::vector<std::byte> build_connect(const std::string& client_id, std::uint16_t keep_alive_s = 300) {
    std::vector<std::byte> vh;
    mqtt::put_str(vh, "MQTT");
    vh.push_back(std::byte{0x04});  // MQTT 3.1.1
    vh.push_back(std::byte{0x02});  // clean session, no will/user/pass
    mqtt::put_u16_be(vh, keep_alive_s);
    mqtt::put_str(vh, client_id);
    return vh;
}

// ===== SubClient: a connected, subscribed client with a live reader thread ==============================
// Handles CONNACK/SUBACK via a small setup inbox (polled by connect()/subscribe()); once past setup, the
// SAME reader thread inline-processes inbound PUBLISH — PUBACKs it if QoS 1, records delivery latency
// from the 8-byte send timestamp embedded at the start of the payload. Used for both the real subscribers
// (whose latency/throughput we report) and the --idle-sessions (which never match anything, existing
// purely to inflate `sessions_` and be scanned-and-skipped by every route_publish() call).
class SubClient {
public:
    ~SubClient() { close(); }

    bool connect_and_subscribe(std::uint16_t port, const std::string& client_id, const std::string& filter,
                                std::uint8_t qos) {
        fd_ = dial_loopback(port);
        if (fd_ == quark::pal::invalid_fd) return false;
        running_.store(true, std::memory_order_release);
        reader_ = std::thread([this] { reader_loop(); });

        if (!mqtt::write_packet(fd_, std::byte{0x10}, build_connect(client_id))) return false;
        auto connack = wait_setup(0x20, 5000);
        if (!connack || connack->body.size() < 2 || std::to_integer<std::uint8_t>(connack->body[1]) != 0)
            return false;

        std::vector<std::byte> vh;
        mqtt::put_u16_be(vh, next_id());
        mqtt::put_str(vh, filter);
        vh.push_back(static_cast<std::byte>(qos));
        if (!mqtt::write_packet(fd_, std::byte{0x82}, vh)) return false;
        auto suback = wait_setup(0x90, 5000);
        return suback.has_value();
    }

    void close() {
        running_.store(false, std::memory_order_release);
        if (reader_.joinable()) reader_.join();
        if (fd_ != quark::pal::invalid_fd) {
            quark::pal::close_fd(fd_);
            fd_ = quark::pal::invalid_fd;
        }
    }

    [[nodiscard]] std::uint64_t received() const noexcept { return received_.load(std::memory_order_relaxed); }
    // Only the reader thread ever appends; safe to read once that thread has been joined via close().
    [[nodiscard]] const std::vector<std::int64_t>& latencies_ns() const noexcept { return latencies_ns_; }

private:
    std::uint16_t next_id() {
        const std::uint16_t id = ++packet_id_;
        return id == 0 ? ++packet_id_ : id;
    }

    std::optional<mqtt::Packet> wait_setup(std::uint8_t type_high_nibble, int timeout_ms) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            {
                std::lock_guard<std::mutex> g(setup_mu_);
                for (auto it = setup_inbox_.begin(); it != setup_inbox_.end(); ++it) {
                    if ((it->type_flags & 0xF0) == type_high_nibble) {
                        mqtt::Packet p = std::move(*it);
                        setup_inbox_.erase(it);
                        return p;
                    }
                }
            }
            if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    void reader_loop() {
        while (running_.load(std::memory_order_acquire)) {
            auto pkt = mqtt::read_packet(fd_, running_);
            if (!pkt) break;
            const std::uint8_t type = pkt->type_flags & 0xF0;
            if (type == 0x30) {
                handle_publish(*pkt);
            } else {
                std::lock_guard<std::mutex> g(setup_mu_);
                setup_inbox_.push_back(std::move(*pkt));
            }
        }
    }

    void handle_publish(const mqtt::Packet& pkt) {
        const auto recv_ts = std::chrono::steady_clock::now();
        const std::uint8_t qos = (pkt.type_flags >> 1) & 0x03;
        const std::vector<std::byte>& b = pkt.body;
        if (b.size() < 2) return;
        const std::uint16_t tlen =
            (std::to_integer<std::uint8_t>(b[0]) << 8) | std::to_integer<std::uint8_t>(b[1]);
        std::size_t pos = 2 + tlen;
        if (pos > b.size()) return;
        if (qos > 0) {
            if (pos + 2 > b.size()) return;
            const std::uint16_t pid =
                (std::to_integer<std::uint8_t>(b[pos]) << 8) | std::to_integer<std::uint8_t>(b[pos + 1]);
            pos += 2;
            std::vector<std::byte> ack;
            mqtt::put_u16_be(ack, pid);
            (void)mqtt::write_packet(fd_, std::byte{0x40}, ack);  // sole writer on this fd besides setup
        }
        if (pos + 8 > b.size()) return;  // payload too short to carry the send timestamp — skip
        std::int64_t sent_ns = 0;
        std::memcpy(&sent_ns, b.data() + pos, sizeof(sent_ns));
        const auto sent = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(sent_ns));
        latencies_ns_.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(recv_ts - sent).count());
        received_.fetch_add(1, std::memory_order_relaxed);
    }

    quark::pal::fd_t fd_ = quark::pal::invalid_fd;
    std::atomic<bool> running_{false};
    std::thread reader_;
    std::mutex setup_mu_;
    std::deque<mqtt::Packet> setup_inbox_;
    std::uint16_t packet_id_ = 0;
    std::atomic<std::uint64_t> received_{0};
    std::vector<std::int64_t> latencies_ns_;
};

// ===== publisher: single-threaded connect + sequential publish loop =====================================
// QoS 1 waits for each PUBACK before sending the next PUBLISH (single in-flight, like a real constrained
// device) — this is what bounds publish-side throughput by the broker's per-message round-trip cost.
struct PublishResult {
    std::uint64_t sent = 0;
    std::uint64_t errors = 0;
};

PublishResult publish_worker(std::uint16_t port, const std::string& client_id, const std::string& topic,
                              int messages, int payload_bytes, std::uint8_t qos) {
    PublishResult r;
    quark::pal::fd_t fd = dial_loopback(port);
    if (fd == quark::pal::invalid_fd) {
        r.errors = static_cast<std::uint64_t>(messages);
        return r;
    }
    std::atomic<bool> running{true};

    if (!mqtt::write_packet(fd, std::byte{0x10}, build_connect(client_id))) {
        quark::pal::close_fd(fd);
        r.errors = static_cast<std::uint64_t>(messages);
        return r;
    }
    auto connack = mqtt::read_packet(fd, running);
    if (!connack || connack->body.size() < 2 || std::to_integer<std::uint8_t>(connack->body[1]) != 0) {
        quark::pal::close_fd(fd);
        r.errors = static_cast<std::uint64_t>(messages);
        return r;
    }

    std::vector<std::byte> payload(static_cast<std::size_t>(payload_bytes), std::byte{0x5A});
    std::uint16_t packet_id = 0;
    for (int i = 0; i < messages; ++i) {
        const std::int64_t now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
        std::memcpy(payload.data(), &now_ns, sizeof(now_ns));

        std::vector<std::byte> vh;
        mqtt::put_str(vh, topic);
        if (qos > 0) {
            packet_id = packet_id == 0xFFFF ? 1 : static_cast<std::uint16_t>(packet_id + 1);
            mqtt::put_u16_be(vh, packet_id);
        }
        vh.insert(vh.end(), payload.begin(), payload.end());
        const std::uint8_t flags = static_cast<std::uint8_t>(qos << 1);
        if (!mqtt::write_packet(fd, static_cast<std::byte>(0x30 | flags), vh)) {
            ++r.errors;
            continue;
        }
        if (qos > 0) {
            auto puback = mqtt::read_packet(fd, running);
            if (!puback || (puback->type_flags & 0xF0) != 0x40) {
                ++r.errors;
                continue;
            }
        }
        ++r.sent;
    }

    std::vector<std::byte> disc;
    (void)mqtt::write_packet(fd, std::byte{0xE0}, disc);
    quark::pal::close_fd(fd);
    return r;
}

// ===== reporting =========================================================================================
struct LatencyStats {
    double min_ms = 0, p50_ms = 0, p95_ms = 0, p99_ms = 0, max_ms = 0, mean_ms = 0;
    std::size_t count = 0;
};

LatencyStats summarize(std::vector<std::int64_t> ns) {
    LatencyStats s;
    s.count = ns.size();
    if (ns.empty()) return s;
    std::sort(ns.begin(), ns.end());
    auto at = [&](double frac) {
        std::size_t idx = static_cast<std::size_t>(frac * static_cast<double>(ns.size() - 1));
        return static_cast<double>(ns[idx]) / 1e6;
    };
    double sum = 0;
    for (auto v : ns) sum += static_cast<double>(v);
    s.min_ms = static_cast<double>(ns.front()) / 1e6;
    s.max_ms = static_cast<double>(ns.back()) / 1e6;
    s.mean_ms = sum / static_cast<double>(ns.size()) / 1e6;
    s.p50_ms = at(0.50);
    s.p95_ms = at(0.95);
    s.p99_ms = at(0.99);
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    const Options o = parse_args(argc, argv);
    const std::string topic = "bench/topic";

    std::printf("broker_bench: subscribers=%d idle-sessions=%d publishers=%d messages/publisher=%d "
                "payload-bytes=%d qos=%d\n",
                o.subscribers, o.idle_sessions, o.publishers, o.messages, o.payload_bytes, o.qos);

    std::optional<NativeBroker> broker;
    std::uint16_t port = 0;
    if (o.external_port != 0) {
        port = static_cast<std::uint16_t>(o.external_port);
        std::printf("driving external broker at 127.0.0.1:%u (no in-process NativeBroker started)\n", port);
    } else {
        Config cfg;
        cfg.bind_host = "127.0.0.1";
        cfg.listen_port = 0;  // ephemeral
        cfg.backlog = 256;
        broker.emplace(std::move(cfg));
        auto started = broker->start();
        if (!started) {
            std::fprintf(stderr, "broker start failed: %s\n", started.error().c_str());
            return 1;
        }
        port = broker->listen_port();
    }

    // Idle sessions: connected + subscribed to topics that never match `topic` — pure sessions_ bloat for
    // route_publish()'s linear scan-and-skip. Never counted in delivery stats. Progress is printed every
    // batch (with a per-batch and cumulative connect rate) because setup rate is itself a signal: if it
    // holds roughly steady as the session count climbs, thread-per-connection onboarding isn't the
    // bottleneck; if it visibly decays, connection setup itself (not just route_publish's scan) degrades
    // with session count.
    std::vector<SubClient> idle(static_cast<std::size_t>(o.idle_sessions));
    const auto idle_setup_start = std::chrono::steady_clock::now();
    auto batch_start = idle_setup_start;
    const int progress_every = std::max(1, o.idle_sessions / 20);
    for (int i = 0; i < o.idle_sessions; ++i) {
        const std::string cid = "idle-" + std::to_string(i);
        const std::string filter = "idle/" + std::to_string(i);
        if (!idle[static_cast<std::size_t>(i)].connect_and_subscribe(port, cid, filter,
                                                                       static_cast<std::uint8_t>(o.qos))) {
            std::fprintf(stderr, "idle session %d failed to connect/subscribe (after %.1fs)\n", i,
                         std::chrono::duration<double>(std::chrono::steady_clock::now() - idle_setup_start)
                             .count());
            return 1;
        }
        if ((i + 1) % progress_every == 0 || i + 1 == o.idle_sessions) {
            const auto now = std::chrono::steady_clock::now();
            const double batch_secs = std::chrono::duration<double>(now - batch_start).count();
            const double total_secs = std::chrono::duration<double>(now - idle_setup_start).count();
            std::printf("  idle sessions: %d/%d connected — last %d in %.2fs (%.0f/s), cumulative %.0f/s\n",
                        i + 1, o.idle_sessions, progress_every, batch_secs,
                        batch_secs > 0 ? progress_every / batch_secs : 0.0,
                        total_secs > 0 ? (i + 1) / total_secs : 0.0);
            std::fflush(stdout);
            batch_start = now;
        }
    }

    std::vector<SubClient> subs(static_cast<std::size_t>(o.subscribers));
    for (int i = 0; i < o.subscribers; ++i) {
        const std::string cid = "sub-" + std::to_string(i);
        if (!subs[static_cast<std::size_t>(i)].connect_and_subscribe(port, cid, topic,
                                                                       static_cast<std::uint8_t>(o.qos))) {
            std::fprintf(stderr, "subscriber %d failed to connect/subscribe\n", i);
            return 1;
        }
    }

    std::printf("setup done: %d idle session(s), %d subscriber(s) connected — publishing...\n",
                o.idle_sessions, o.subscribers);

    const auto t_pub_start = std::chrono::steady_clock::now();
    std::vector<std::thread> pub_threads;
    std::vector<PublishResult> pub_results(static_cast<std::size_t>(o.publishers));
    for (int i = 0; i < o.publishers; ++i) {
        pub_threads.emplace_back([&, i] {
            pub_results[static_cast<std::size_t>(i)] =
                publish_worker(port, "pub-" + std::to_string(i), topic, o.messages, o.payload_bytes,
                                static_cast<std::uint8_t>(o.qos));
        });
    }
    for (auto& t : pub_threads) t.join();
    const auto t_pub_end = std::chrono::steady_clock::now();

    std::uint64_t total_sent = 0, total_errors = 0;
    for (const auto& r : pub_results) {
        total_sent += r.sent;
        total_errors += r.errors;
    }

    const std::uint64_t expected_per_sub = total_sent;  // every subscriber sees every successful publish
    const std::uint64_t expected_total =
        expected_per_sub * static_cast<std::uint64_t>(std::max(o.subscribers, 0));
    const auto deliver_deadline =
        t_pub_end + std::chrono::seconds(o.timeout_s);
    for (;;) {
        std::uint64_t total_received = 0;
        for (const auto& s : subs) total_received += s.received();
        if (total_received >= expected_total || std::chrono::steady_clock::now() >= deliver_deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const auto t_deliver_end = std::chrono::steady_clock::now();

    for (auto& s : subs) s.close();
    for (auto& s : idle) s.close();
    if (broker) broker->stop();

    std::uint64_t total_received = 0;
    std::vector<std::int64_t> all_latencies;
    for (const auto& s : subs) {
        total_received += s.received();
        all_latencies.insert(all_latencies.end(), s.latencies_ns().begin(), s.latencies_ns().end());
    }

    const double pub_secs =
        std::chrono::duration<double>(t_pub_end - t_pub_start).count();
    const double deliver_secs =
        std::chrono::duration<double>(t_deliver_end - t_pub_start).count();
    const double pub_mb = static_cast<double>(total_sent) * static_cast<double>(o.payload_bytes) / 1e6;
    const LatencyStats lat = summarize(std::move(all_latencies));

    std::printf("\n--- publish side --------------------------------------------------\n");
    std::printf("sent=%llu errors=%llu wall=%.3fs throughput=%.0f msg/s (%.2f MB/s)\n",
                static_cast<unsigned long long>(total_sent), static_cast<unsigned long long>(total_errors),
                pub_secs, pub_secs > 0 ? static_cast<double>(total_sent) / pub_secs : 0.0,
                pub_secs > 0 ? pub_mb / pub_secs : 0.0);
    std::printf("\n--- delivery (fan-out to %d subscriber(s)) --------------------------\n", o.subscribers);
    std::printf("expected=%llu received=%llu (%.1f%%) wall-from-publish-start=%.3fs\n",
                static_cast<unsigned long long>(expected_total),
                static_cast<unsigned long long>(total_received),
                expected_total > 0 ? 100.0 * static_cast<double>(total_received) /
                                          static_cast<double>(expected_total)
                                    : 100.0,
                deliver_secs);
    if (lat.count > 0) {
        std::printf("latency (ms): min=%.3f p50=%.3f p95=%.3f p99=%.3f max=%.3f mean=%.3f (n=%zu)\n",
                    lat.min_ms, lat.p50_ms, lat.p95_ms, lat.p99_ms, lat.max_ms, lat.mean_ms, lat.count);
    }

    return 0;  // informational tool, not a pass/fail gate — the printed numbers are the point
}

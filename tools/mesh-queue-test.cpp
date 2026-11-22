#include "mesh_incoming_queue.hpp"

#include <cassert>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using p2p::MeshIncomingQueue;

namespace {

    MeshIncomingQueue::Item item(std::string raw, std::string source, std::string peer = {}) {
        return {std::move(raw), std::move(peer), std::move(source)};
    }

    void testPerSourceLimitAndConnections() {
        MeshIncomingQueue queue;
        for (int i = 0; i < 8; ++i) {
            assert(queue.push(item("peer-" + std::to_string(i), "192.0.2.10",
                                   "connection-" + std::to_string(i))));
        }
        // A new TCP connection from the same IP shares the same source quota.
        assert(!queue.push(item("peer-overflow", "192.0.2.10", "another-connection")));
        assert(queue.push(item("other-source", "192.0.2.11")));
        assert(queue.size() == 9);
    }

    void testDhtReservationAndGlobalCap() {
        MeshIncomingQueue queue;
        for (int source = 0; source < 12; ++source) {
            for (int i = 0; i < 8; ++i) {
                assert(queue.push(item("peer-" + std::to_string(source) + "-" + std::to_string(i),
                                       "198.51.100." + std::to_string(source + 1))));
            }
        }
        for (int i = 0; i < 32; ++i) {
            assert(queue.push(item("dht-" + std::to_string(i), "")));
        }
        assert(queue.size() == MeshIncomingQueue::kMaxIncoming);
        assert(!queue.push(item("global-overflow", "203.0.113.99")));
    }

    void testDeduplication() {
        MeshIncomingQueue queue;
        assert(queue.push(item("same-envelope", "192.0.2.1")));
        assert(!queue.push(item("same-envelope", "192.0.2.2")));
        auto detached = queue.take();
        assert(detached.size() == 1);
        assert(queue.size() == 0);
        // Deduplication only covers the queued lifetime, not a later retry.
        assert(queue.push(item("same-envelope", "192.0.2.2")));
    }

    void testRoundRobinDrain() {
        MeshIncomingQueue queue;
        assert(queue.push(item("a0", "a")));
        assert(queue.push(item("a1", "a")));
        assert(queue.push(item("a2", "a")));
        assert(queue.push(item("a3", "a")));
        assert(queue.push(item("b0", "b")));
        assert(queue.push(item("b1", "b")));
        assert(queue.push(item("c0", "c")));

        auto ordered = MeshIncomingQueue::roundRobin(queue.take());
        const std::vector<std::string> expected{"a0", "b0", "c0", "a1", "b1", "a2", "a3"};
        assert(ordered.size() == expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            assert(ordered[i].raw == expected[i]);
        }
    }

} // namespace

int main() {
    testPerSourceLimitAndConnections();
    testDhtReservationAndGlobalCap();
    testDeduplication();
    testRoundRobinDrain();
    std::cout << "PASS MediaMesh incoming queue limits, deduplication and round-robin\n";
    return 0;
}

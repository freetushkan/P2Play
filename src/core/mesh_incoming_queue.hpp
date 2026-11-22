#pragma once

#include <algorithm>
#include <cstddef>
#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace p2p {

// The Core protects this queue with m_meshMutex. Keep admission cheap and bounded
// on libtorrent's callback thread; reorder only after the worker has swapped a batch.
class MeshIncomingQueue {
public:
    struct Item {
        std::string raw;
        std::string peer;
        std::string source;
    };

    static constexpr std::size_t kMaxIncoming = 128;
    static constexpr std::size_t kMaxIncomingPerSource = 8;
    static constexpr std::size_t kMaxIncomingFromDht = 32;

    bool push(Item item) {
        if (m_items.size() >= kMaxIncoming ||
            std::any_of(m_items.begin(), m_items.end(),
                        [&](const Item &queued) { return queued.raw == item.raw; })) {
            return false;
        }

        const std::string key = sourceKey(item);
        const auto count = static_cast<std::size_t>(std::count_if(
            m_items.begin(), m_items.end(),
            [&](const Item &queued) { return sourceKey(queued) == key; }));
        const std::size_t limit = key == "dht" ? kMaxIncomingFromDht : kMaxIncomingPerSource;
        if (count >= limit) {
            return false;
        }

        m_items.push_back(std::move(item));
        return true;
    }

    std::vector<Item> take() {
        std::vector<Item> items;
        items.swap(m_items);
        return items;
    }

    static std::vector<Item> roundRobin(std::vector<Item> items) {
        std::map<std::string, std::deque<Item>> sources;
        for (auto &item : items) {
            sources[sourceKey(item)].push_back(std::move(item));
        }

        std::vector<Item> ordered;
        ordered.reserve(items.size());
        while (!sources.empty()) {
            for (auto it = sources.begin(); it != sources.end();) {
                ordered.push_back(std::move(it->second.front()));
                it->second.pop_front();
                if (it->second.empty()) {
                    it = sources.erase(it);
                } else {
                    ++it;
                }
            }
        }
        return ordered;
    }

    std::size_t size() const { return m_items.size(); }

private:
    static std::string sourceKey(const Item &item) {
        return item.source.empty() ? (item.peer.empty() ? "dht" : item.peer) : item.source;
    }

    std::vector<Item> m_items;
};

} // namespace p2p

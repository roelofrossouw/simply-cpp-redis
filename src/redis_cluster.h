#pragma once

// Redis Cluster routing, used by redis.cpp and by the unit tests (not installed): the hash slot of a
// key, which argument of a command is its key, the slot map from CLUSTER SHARDS or CLUSTER SLOTS,
// and SHA-1 for EVALSHA.

#include <hiredis/hiredis.h>
#include <sc/net/ip_endpoints.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sc::redis::cluster {
    constexpr int slot_count = 16384;

    // INFO's (or CLUSTER INFO's) text as its fields: "used_memory:1024" lines, the "# Memory"
    // headings and empty lines left out (in redis.cpp).
    [[nodiscard]] std::map<std::string, std::string> parse_info(std::string_view text);

    // CRC16-CCITT (XMODEM: polynomial 0x1021, initial 0), as Redis Cluster hashes keys.
    inline std::uint16_t crc16(const std::string_view data) {
        std::uint16_t crc = 0;
        for (const unsigned char byte: data) {
            crc ^= static_cast<std::uint16_t>(byte) << 8;
            for (int bit = 0; bit < 8; ++bit) crc = crc & 0x8000 ? static_cast<std::uint16_t>((crc << 1) ^ 0x1021) : static_cast<std::uint16_t>(crc << 1);
        }
        return crc;
    }

    // The hash slot of key: of the part between the first { and the } after it when that part
    // isn't empty (a hash tag), else of the whole key.
    inline int key_slot(std::string_view key) {
        if (const auto open = key.find('{'); open != std::string_view::npos) {
            if (const auto close = key.find('}', open + 1); close != std::string_view::npos && close > open + 1)
                key = key.substr(open + 1, close - open - 1);
        }
        return crc16(key) % slot_count;
    }

    // The key a command is routed by: the first argument, or for EVAL/EVALSHA the first of its keys.
    // None for commands without a key (PING, CLUSTER, INFO, SCAN, SCRIPT ...) or a script without keys.
    inline std::optional<std::string_view> routing_key(const std::vector<std::string> &command) {
        if (command.size() < 2) return std::nullopt;
        std::string name = command.front();
        std::ranges::transform(name, name.begin(), [](const unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (name == "EVAL" || name == "EVALSHA" || name == "EVAL_RO" || name == "EVALSHA_RO") {
            int keys = 0;
            const auto &count = command[2 < command.size() ? 2 : 0];
            if (command.size() < 4 || std::from_chars(count.data(), count.data() + count.size(), keys).ec != std::errc{} || keys < 1)
                return std::nullopt;
            return command[3];
        }
        static constexpr std::array keyless{"PING", "CLUSTER", "INFO", "SCAN", "SCRIPT", "ASKING", "AUTH", "SELECT", "HELLO", "DBSIZE",
                                            "FLUSHALL", "FLUSHDB", "KEYS", "RANDOMKEY", "TIME", "CONFIG", "CLIENT", "READONLY", "READWRITE"};
        if (std::ranges::find(keyless, name) != keyless.end()) return std::nullopt;
        return command[1];
    }

    // The slot of a MOVED redirect ("MOVED 3999 10.0.100.46:6379"); none for anything else.
    inline std::optional<int> moved_slot(const std::string_view message) {
        if (!message.starts_with("MOVED ")) return std::nullopt;
        const auto number = message.substr(6, message.find(' ', 6) - 6);
        int slot = -1;
        if (std::from_chars(number.data(), number.data() + number.size(), slot).ec != std::errc{} || slot < 0 || slot >= slot_count)
            return std::nullopt;
        return slot;
    }

    // A range of slots and the master that serves it.
    struct slot_range {
        int first = 0, last = 0;
        ip_endpoint master;
    };

    namespace detail {
        inline std::string text(const redisReply &reply) { return reply.str ? std::string(reply.str, reply.len) : std::string{}; }

        inline bool is_text(const redisReply &reply) {
            bool yes = reply.type == REDIS_REPLY_STRING || reply.type == REDIS_REPLY_STATUS;
#ifdef REDIS_REPLY_VERB
            yes = yes || reply.type == REDIS_REPLY_VERB;
#endif
            return yes;
        }

        // A list of key/value pairs: RESP2's flat array or RESP3's map.
        inline bool is_pairs(const redisReply &reply) {
            bool yes = reply.type == REDIS_REPLY_ARRAY;
#ifdef REDIS_REPLY_MAP
            yes = yes || reply.type == REDIS_REPLY_MAP;
#endif
            return yes && reply.elements % 2 == 0;
        }

        inline const redisReply *value_of(const redisReply &pairs, const std::string_view key) {
            for (std::size_t i = 0; i + 1 < pairs.elements; i += 2)
                if (is_text(*pairs.element[i]) && text(*pairs.element[i]) == key) return pairs.element[i + 1];
            return nullptr;
        }

        // A node's address: its IP, else the endpoint it advertises; an empty or "?" one is the
        // node that was asked (fallback_host).
        inline std::optional<ip_endpoint> address(std::string host, const long long port, const std::string &fallback_host) {
            if (host.empty()) host = fallback_host;
            if (host == "?" || host.empty() || port <= 0 || port > 65535) return std::nullopt;
            return ip_endpoint{host, static_cast<int>(port)};
        }

        inline bool in_range(const long long first, const long long last) {
            return first >= 0 && last >= first && last < slot_count;
        }
    }

    // CLUSTER SHARDS (Redis 7+, Valkey): each shard's slot ranges with its master's address (role
    // "master" or "primary", health not "failed"). A shard without such a master is left out.
    // Throws std::runtime_error when the reply isn't CLUSTER SHARDS's shape.
    inline std::vector<slot_range> parse_cluster_shards(const redisReply &reply, const std::string &fallback_host) {
        using namespace detail;
        if (reply.type != REDIS_REPLY_ARRAY) throw std::runtime_error("Redis CLUSTER SHARDS returned an unexpected reply");
        std::vector<slot_range> ranges;
        for (std::size_t s = 0; s < reply.elements; ++s) {
            const auto &shard = *reply.element[s];
            if (!is_pairs(shard)) throw std::runtime_error("Redis CLUSTER SHARDS returned an unexpected shard");
            const auto *slots = value_of(shard, "slots");
            const auto *nodes = value_of(shard, "nodes");
            if (!slots || !nodes || slots->type != REDIS_REPLY_ARRAY || nodes->type != REDIS_REPLY_ARRAY || slots->elements % 2 != 0)
                throw std::runtime_error("Redis CLUSTER SHARDS returned a shard without slots or nodes");
            std::optional<ip_endpoint> master;
            for (std::size_t n = 0; n < nodes->elements && !master; ++n) {
                const auto &node = *nodes->element[n];
                if (!is_pairs(node)) continue;
                const auto *role = value_of(node, "role");
                const auto *health = value_of(node, "health");
                if (!role || (text(*role) != "master" && text(*role) != "primary")) continue;
                if (health && (text(*health) == "failed" || text(*health) == "fail")) continue;
                const auto *port = value_of(node, "port");
                if (!port || port->type != REDIS_REPLY_INTEGER) continue;
                std::string host;
                if (const auto *ip = value_of(node, "ip"); ip && is_text(*ip)) host = text(*ip);
                if ((host.empty() || host == "?") && value_of(node, "endpoint")) host = text(*value_of(node, "endpoint"));
                master = address(host, port->integer, fallback_host);
            }
            if (!master) continue;
            for (std::size_t i = 0; i + 1 < slots->elements; i += 2) {
                const auto &first = *slots->element[i], &last = *slots->element[i + 1];
                if (first.type != REDIS_REPLY_INTEGER || last.type != REDIS_REPLY_INTEGER || !in_range(first.integer, last.integer))
                    throw std::runtime_error("Redis CLUSTER SHARDS returned an unexpected slot range");
                ranges.push_back({static_cast<int>(first.integer), static_cast<int>(last.integer), *master});
            }
        }
        return ranges;
    }

    // CLUSTER SLOTS (older Redis): [first, last, [ip, port, id ...], replicas ...] per range; the
    // first node is the master. Throws std::runtime_error when the reply isn't that shape.
    inline std::vector<slot_range> parse_cluster_slots(const redisReply &reply, const std::string &fallback_host) {
        using namespace detail;
        if (reply.type != REDIS_REPLY_ARRAY) throw std::runtime_error("Redis CLUSTER SLOTS returned an unexpected reply");
        std::vector<slot_range> ranges;
        for (std::size_t r = 0; r < reply.elements; ++r) {
            const auto &range = *reply.element[r];
            if (range.type != REDIS_REPLY_ARRAY || range.elements < 3 || range.element[0]->type != REDIS_REPLY_INTEGER ||
                range.element[1]->type != REDIS_REPLY_INTEGER || range.element[2]->type != REDIS_REPLY_ARRAY ||
                !in_range(range.element[0]->integer, range.element[1]->integer))
                throw std::runtime_error("Redis CLUSTER SLOTS returned an unexpected range");
            const auto &node = *range.element[2];
            if (node.elements < 2 || !is_text(*node.element[0]) || node.element[1]->type != REDIS_REPLY_INTEGER)
                throw std::runtime_error("Redis CLUSTER SLOTS returned an unexpected node");
            if (const auto master = address(text(*node.element[0]), node.element[1]->integer, fallback_host))
                ranges.push_back({static_cast<int>(range.element[0]->integer), static_cast<int>(range.element[1]->integer), *master});
        }
        return ranges;
    }

    // SHA-1 of data as 40 lower-case hex digits: a Lua script's name for EVALSHA.
    inline std::string sha1_hex(const std::string_view data) {
        std::uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
        const auto rotate = [](const std::uint32_t value, const int bits) { return (value << bits) | (value >> (32 - bits)); };
        std::string message{data};
        const std::uint64_t length_bits = static_cast<std::uint64_t>(data.size()) * 8;
        message.push_back(static_cast<char>(0x80));
        while (message.size() % 64 != 56) message.push_back('\0');
        for (int i = 7; i >= 0; --i) message.push_back(static_cast<char>((length_bits >> (i * 8)) & 0xFF));
        for (std::size_t chunk = 0; chunk < message.size(); chunk += 64) {
            std::uint32_t w[80];
            for (int i = 0; i < 16; ++i) {
                const auto *b = reinterpret_cast<const unsigned char *>(message.data() + chunk + i * 4);
                w[i] = static_cast<std::uint32_t>(b[0]) << 24 | static_cast<std::uint32_t>(b[1]) << 16 | static_cast<std::uint32_t>(b[2]) << 8 | b[3];
            }
            for (int i = 16; i < 80; ++i) w[i] = rotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
            std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
            for (int i = 0; i < 80; ++i) {
                std::uint32_t f, k;
                if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
                else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
                else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
                else f = b ^ c ^ d, k = 0xCA62C1D6;
                const std::uint32_t t = rotate(a, 5) + f + e + k + w[i];
                e = d, d = c, c = rotate(b, 30), b = a, a = t;
            }
            h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
        }
        static constexpr char digits[] = "0123456789abcdef";
        std::string out;
        for (const auto word: h)
            for (int shift = 28; shift >= 0; shift -= 4) out.push_back(digits[(word >> shift) & 0xF]);
        return out;
    }
}

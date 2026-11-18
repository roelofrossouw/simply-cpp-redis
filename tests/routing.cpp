// Redis Cluster routing without a server: hash slots, which argument is the key, MOVED's slot, the
// slot map from CLUSTER SHARDS and CLUSTER SLOTS replies (RESP2 and RESP3, read by hiredis's own
// reader from the bytes a server sends), and SHA-1 for EVALSHA.
#include "../src/redis_cluster.h"

#include <sc/redis/redis.h>
#include <hiredis/hiredis.h>
#include <sc_test.h>

#include <memory>
#include <string>
#include <vector>

namespace {
    using reply_ptr = std::unique_ptr<redisReply, decltype(&freeReplyObject)>;

    // A reply as hiredis reads it from what a server would send.
    reply_ptr read_reply(const std::string &resp) {
        std::unique_ptr<redisReader, decltype(&redisReaderFree)> reader{redisReaderCreate(), redisReaderFree};
        redisReaderFeed(reader.get(), resp.data(), resp.size());
        void *reply = nullptr;
        if (redisReaderGetReply(reader.get(), &reply) != REDIS_OK || !reply) return {nullptr, freeReplyObject};
        return {static_cast<redisReply *>(reply), freeReplyObject};
    }

    std::string bulk(const std::string &text) { return "$" + std::to_string(text.size()) + "\r\n" + text + "\r\n"; }
    std::string number(const long long value) { return ":" + std::to_string(value) + "\r\n"; }

    // One CLUSTER SHARDS shard: its slot ranges and nodes (each "role", "ip", "port", "health").
    struct shard_node {
        std::string role, ip;
        int port;
        std::string health = "online";
    };

    std::string shard(const std::vector<std::pair<int, int>> &slots, const std::vector<shard_node> &nodes, const bool resp3) {
        const auto pairs = [resp3](const std::size_t count) { return (resp3 ? "%" + std::to_string(count / 2) : "*" + std::to_string(count)) + "\r\n"; };
        std::string out = pairs(4) + bulk("slots") + "*" + std::to_string(slots.size() * 2) + "\r\n";
        for (const auto &[first, last]: slots) out += number(first) + number(last);
        out += bulk("nodes") + "*" + std::to_string(nodes.size()) + "\r\n";
        for (const auto &node: nodes) {
            out += pairs(12) + bulk("id") + bulk(std::string(40, 'a')) + bulk("port") + number(node.port) + bulk("ip") + bulk(node.ip) +
                   bulk("endpoint") + bulk(node.ip) + bulk("role") + bulk(node.role) + bulk("health") + bulk(node.health);
        }
        return out;
    }
}

int main() {
    using namespace sc::redis::cluster;

    SECTION("Hash slots: CRC16/XMODEM of the key or its hash tag");
    CHECK_EQ(crc16("123456789"), std::uint16_t{0x31C3}); // the reference value in the Redis Cluster spec
    CHECK_EQ(key_slot("123456789"), 0x31C3 % 16384);
    CHECK_EQ(key_slot("foo"), 12182);                      // CLUSTER KEYSLOT foo
    CHECK_EQ(key_slot("hello"), 866);                      // CLUSTER KEYSLOT hello
    CHECK_EQ(key_slot("{user1000}.following"), key_slot("{user1000}.followers"));
    CHECK_EQ(key_slot("{user1000}.following"), key_slot("user1000"));
    CHECK_EQ(key_slot("foo{}{bar}"), key_slot("foo{}{bar}"));   // an empty tag: the whole key
    CHECK(key_slot("foo{}{bar}") != key_slot("bar"));
    CHECK_EQ(key_slot("foo{{bar}}zap"), key_slot("{bar"));      // from the first { to the first } after it
    CHECK_EQ(key_slot("foo{bar}{zap}"), key_slot("bar"));
    CHECK_EQ(key_slot(""), 0);

    SECTION("The key a command goes by");
    using command = std::vector<std::string>;
    const auto key_of = [](const command &c) { return std::string{routing_key(c).value_or("-")}; }; // the view points into c
    CHECK_EQ(key_of({"GET", "a"}), std::string{"a"});
    CHECK_EQ(key_of({"hset", "h", "f", "v"}), std::string{"h"});
    CHECK_EQ(key_of({"EVAL", "return 1", "2", "k1", "k2", "arg"}), std::string{"k1"});
    CHECK_EQ(key_of({"EVALSHA", "abc", "1", "k"}), std::string{"k"});
    CHECK(!routing_key(command{"EVAL", "return 1", "0", "arg"}).has_value()); // no keys: any master
    CHECK(!routing_key(command{"EVAL", "return 1", "x"}).has_value());
    CHECK(!routing_key(command{"PING"}).has_value());
    CHECK(!routing_key(command{"CLUSTER", "NODES"}).has_value());
    CHECK(!routing_key(command{"SCAN", "0", "MATCH", "x*"}).has_value());
    CHECK(!routing_key(command{"INFO", "server"}).has_value());

    SECTION("MOVED's slot");
    CHECK_EQ(moved_slot("MOVED 3999 10.0.100.46:6379").value_or(-1), 3999);
    CHECK_EQ(moved_slot("MOVED 0 :6380").value_or(-1), 0);
    CHECK(!moved_slot("ASK 3999 10.0.100.46:6379").has_value());
    CHECK(!moved_slot("MOVED 16384 10.0.100.46:6379").has_value());
    CHECK(!moved_slot("WRONGTYPE Operation").has_value());

    SECTION("The slot map from CLUSTER SHARDS (RESP2 and RESP3)");
    for (const bool resp3: {false, true}) {
#if HIREDIS_MAJOR < 1
        if (resp3) continue; // hiredis before 1.0 (Ubuntu jammy's 0.14) reads no RESP3; sc-redis never asks for it (no HELLO 3)
#endif
        const auto text = "*3\r\n" +
                          shard({{0, 5460}}, {{"master", "10.0.100.45", 6379}, {"replica", "10.0.100.46", 6380}}, resp3) +
                          shard({{5461, 10922}}, {{"replica", "10.0.100.47", 6380}, {"primary", "10.0.100.46", 6379}}, resp3) +
                          shard({{10923, 12000}, {12001, 16383}}, {{"master", "", 6379}}, resp3);
        const auto reply = read_reply(text);
        CHECK(reply != nullptr);
        if (!reply) continue;
        const auto ranges = parse_cluster_shards(*reply, "seed.example.com");
        CHECK_EQ(ranges.size(), std::size_t{4});
        if (ranges.size() != 4) continue;
        CHECK_EQ(ranges[0].first, 0);
        CHECK_EQ(ranges[0].last, 5460);
        CHECK_EQ(ranges[0].master.to_string(), std::string{"10.0.100.45:6379"});
        CHECK_EQ(ranges[1].master.to_string(), std::string{"10.0.100.46:6379"}); // "primary", after its replica
        CHECK_EQ(ranges[2].master.to_string(), std::string{"seed.example.com:6379"}); // no address: the node asked
        CHECK_EQ(ranges[3].first, 12001);
        CHECK_EQ(ranges[3].last, 16383);
    }
    {
        // A failed master's shard is left out (its slots go by the seeds until it's back or replaced)
        const auto reply = read_reply("*1\r\n" + shard({{0, 16383}}, {{"master", "10.0.100.45", 6379, "failed"}}, false));
        CHECK(reply && parse_cluster_shards(*reply, "x").empty());
        const auto not_shards = read_reply(bulk("no"));
        CHECK(not_shards != nullptr);
        if (not_shards) CHECK_THROWS_AS((void) parse_cluster_shards(*not_shards, "x"), std::runtime_error);
    }

    SECTION("The slot map from CLUSTER SLOTS");
    {
        const auto node = [](const std::string &ip, const int port) { return "*3\r\n" + bulk(ip) + number(port) + bulk(std::string(40, 'b')); };
        const auto text = "*3\r\n"
                          "*4\r\n" + number(0) + number(5460) + node("10.0.100.45", 6379) + node("10.0.100.46", 6380) +
                          "*3\r\n" + number(5461) + number(10922) + node("", 6379) +
                          "*3\r\n" + number(10923) + number(16383) + node("?", 6379);
        const auto reply = read_reply(text);
        CHECK(reply != nullptr);
        if (reply) {
            const auto ranges = parse_cluster_slots(*reply, "seed.example.com");
            CHECK_EQ(ranges.size(), std::size_t{2}); // "?": no usable address, left out
            if (ranges.size() == 2) {
                CHECK_EQ(ranges[0].master.to_string(), std::string{"10.0.100.45:6379"});
                CHECK_EQ(ranges[1].first, 5461);
                CHECK_EQ(ranges[1].master.to_string(), std::string{"seed.example.com:6379"});
            }
        }
        const auto bad = read_reply("*1\r\n*2\r\n" + number(0) + number(1));
        CHECK(bad != nullptr);
        if (bad) CHECK_THROWS_AS((void) parse_cluster_slots(*bad, "x"), std::runtime_error);
    }

    SECTION("SHA-1 for EVALSHA");
    CHECK_EQ(sha1_hex(""), std::string{"da39a3ee5e6b4b0d3255bfef95601890afd80709"});
    CHECK_EQ(sha1_hex("abc"), std::string{"a9993e364706816aba3e25717850c26c9cd0d89d"});
    CHECK_EQ(sha1_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"), std::string{"84983e441c3bd26ebaae4aa1f95129e5e54670f1"});
    CHECK_EQ(sha1_hex(std::string(1000, 'a')), std::string{"291e9a6c66994949b57ba5e650361e98fc36b1ba"});
    CHECK_EQ(sha1_hex("return 1"), std::string{"e0e1f9fabfc9d4800c877a703b823ac0578ff8db"}); // SCRIPT LOAD "return 1"

    TEST_SUMMARY();
}

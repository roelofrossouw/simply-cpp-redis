#include <redis.h>
#include <sc_test.h>

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

namespace {
    class key_cleanup {
    public:
        key_cleanup(sc::redis &client, std::vector<std::string> keys) : client_(client), keys_(std::move(keys)) {
        }

        ~key_cleanup() {
            for (const auto &key : keys_) {
                try {
                    client_.erase(key);
                } catch (const std::exception &error) {
                    std::cerr << "Unable to remove temporary Redis key " << key << ": " << error.what() << '\n';
                }
            }
        }

    private:
        sc::redis &client_;
        std::vector<std::string> keys_;
    };
}

int main() {
    const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::string prefix = "sc-tmp:simply-cpp-redis:cluster:" + suffix;
    const auto first_key = prefix + ":first";
    const auto second_key = prefix + ":second";
    const auto hash_key = prefix + ":hash";

    SECTION("Redis Cluster string and hash values");
    sc::redis client{std::vector<sc::ip_endpoint>{
        {"redis1.1web.co.za", 6379},
        {"redis2.1web.co.za", 6379},
    }};
    key_cleanup cleanup{client, {first_key, second_key, hash_key}};

    client.set(first_key, "first-value");
    client.set(second_key, "second-value");
    CHECK_EQ(client.get(first_key), std::optional<std::string>{"first-value"});
    CHECK_EQ(client.get(second_key), std::optional<std::string>{"second-value"});

    client.hset(hash_key, "name", "simply-cpp");
    client.hset(hash_key, "kind", "integration-test");
    CHECK_EQ(client.hget(hash_key, "name"), std::optional<std::string>{"simply-cpp"});
    CHECK_EQ(client.hget(hash_key, "kind"), std::optional<std::string>{"integration-test"});

    SECTION("Several hash fields at once");
    const auto values = client.hmget(hash_key, {"kind", "missing", "name"});
    CHECK_EQ(values.size(), std::size_t{3});
    if (values.size() == 3) {
        CHECK_EQ(values[0], std::optional<std::string>{"integration-test"});
        CHECK(!values[1].has_value());
        CHECK_EQ(values[2], std::optional<std::string>{"simply-cpp"});
    }
    CHECK(client.hmget(hash_key, {}).empty());
    CHECK_EQ(client.hmget(prefix + ":no-such-hash", {"name"}).size(), std::size_t{1});

    const auto all = client.hgetall(hash_key);
    CHECK((all == std::map<std::string, std::string>{{"kind", "integration-test"}, {"name", "simply-cpp"}}));
    CHECK(client.hgetall(prefix + ":no-such-hash").empty());

    TEST_SUMMARY();
}

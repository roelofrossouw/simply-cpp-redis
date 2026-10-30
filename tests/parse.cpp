// CLUSTER NODES and INFO read into their parts, without a server.
#include <redis.h>
#include <sc_test.h>

#include <string>

int main() {
    SECTION("CLUSTER NODES: masters, replicas, slot ranges, flags, a node without its address");
    {
        const std::string text =
            "07c37dfeb235213a872192d90877d0cd55635b91 10.0.100.46:6380@16380 myself,master - 0 1426238317239 4 connected 0-5460\n"
            "67ed2db8d677e59ec4a4cefb06858cf2a1a89fa1 10.0.100.46:6379@16379,redis2.example.com master - 0 1426238316232 2 connected 5461-10922 12000\n"
            "292f8b365bb7edb5e285caf0b7e6ddc7265d2f4f 10.0.100.47:6379@16379 master - 0 1426238318243 3 connected 10923-16383 [12001->-e7d1eecce10fd6bb5eb35b9f99a514335d9ba9ca]\n"
            "6ec23923021cf3ffec47632106199cb7f496ce01 10.0.100.45:6380@16380 slave 07c37dfeb235213a872192d90877d0cd55635b91 0 1426238316232 4 connected\r\n"
            "824fe116063bc5fcf9f4ffd895bc17aee7731ac3 10.0.100.45:6379@16379 slave,fail? 67ed2db8d677e59ec4a4cefb06858cf2a1a89fa1 1426238317741 1426238317239 2 disconnected\n"
            "e7d1eecce10fd6bb5eb35b9f99a514335d9ba9ca :0@0 master,noaddr - 0 0 0 disconnected\n";
        const auto nodes = sc::parse_cluster_nodes(text);
        CHECK_EQ(nodes.size(), std::size_t{6});
        if (nodes.size() == 6) {
            CHECK_EQ(nodes[0].address.to_string(), std::string{"10.0.100.46:6380"});
            CHECK_EQ(nodes[0].bus_port, 16380);
            CHECK(nodes[0].master() && nodes[0].has("myself") && !nodes[0].replica());
            CHECK_EQ(nodes[0].slot_ranges(), (std::vector<std::string>{"0-5460"}));
            CHECK_EQ(nodes[0].config_epoch, std::int64_t{4});
            CHECK_EQ(nodes[1].address.to_string(), std::string{"10.0.100.46:6379"}); // ",hostname" left out
            CHECK_EQ(nodes[1].slot_ranges(), (std::vector<std::string>{"5461-10922", "12000"}));
            CHECK_EQ(nodes[2].migrating.size(), std::size_t{1});
            CHECK_EQ(nodes[3].master_id, nodes[0].id);
            CHECK(nodes[3].replica() && nodes[3].link == "connected" && !nodes[3].failing());
            CHECK(nodes[4].failing() && nodes[4].link == "disconnected");
            CHECK_EQ(nodes[4].ping_sent, std::int64_t{1426238317741});
            CHECK(nodes[5].address.host.empty() && nodes[5].has("noaddr"));
        }
        CHECK_THROWS_AS((void) sc::parse_cluster_nodes("not a node line\n"), std::runtime_error);
        CHECK(sc::parse_cluster_nodes("").empty());
    }

    SECTION("INFO: fields, headings and empty lines left out");
    {
        const auto info = sc::parse_info("# Server\r\nredis_version:7.2.4\r\nuptime_in_seconds:1234\r\n\r\n# Keyspace\r\ndb0:keys=12,expires=3,avg_ttl=0\r\n");
        CHECK_EQ(info.size(), std::size_t{3});
        CHECK_EQ(info.at("redis_version"), std::string{"7.2.4"});
        CHECK_EQ(info.at("db0"), std::string{"keys=12,expires=3,avg_ttl=0"});
    }
    TEST_SUMMARY();
}

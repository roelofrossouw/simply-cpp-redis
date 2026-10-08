// Writes and reads back a string and a hash, then removes them.
// Servers come from SC_REDIS_DEMO_SERVER: one server, or Redis Cluster seed nodes separated
// by ';' ("redis1:6379;redis2:6379"). Unset or invalid falls back to 127.0.0.1:6379.

#include <redis.h>
#include <timer.h>

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
    std::vector<sc::ip_endpoint> demo_servers(const char *variable, const int default_port) {
        const std::vector<sc::ip_endpoint> fallback{{"127.0.0.1", default_port}};
        const char *value = std::getenv(variable);
        if (!value || !*value) return fallback;

        std::vector<sc::ip_endpoint> servers;
        std::string_view rest{value};
        while (!rest.empty()) {
            const auto separator = rest.find(';');
            auto item = rest.substr(0, separator);
            rest = separator == std::string_view::npos ? std::string_view{} : rest.substr(separator + 1);

            const auto first = item.find_first_not_of(" \t");
            if (first == std::string_view::npos) continue;
            item = item.substr(first, item.find_last_not_of(" \t") - first + 1);
            try {
                servers.push_back(sc::ip_endpoint::parse(item, default_port));
            } catch (const std::invalid_argument &error) {
                std::cerr << variable << " ignored, " << error.what() << '\n';
                return fallback;
            }
        }
        return servers.empty() ? fallback : servers;
    }
}

int main() {
    const std::string key = "sc-tmp:sc-redis-demo:string";
    const std::string hash = "sc-tmp:sc-redis-demo:hash";

    try {
        const auto seeds = demo_servers("SC_REDIS_DEMO_SERVER", 6379);
        std::cout << "Redis servers:";
        for (const auto &seed : seeds) std::cout << ' ' << seed;
        std::cout << '\n';

        // [readme]
        sc::timer sw;
        sc::redis cache{seeds};
        std::cout << "Connected after " << sw << '\n';

        cache.set(key, "Hello World!");
        std::cout << key << " = " << cache.get(key).value() << '\n';

        cache.hset(hash, "name", "simply-cpp");
        cache.hset(hash, "kind", "demo");
        std::cout << hash << " name = " << cache.hget(hash, "name").value()
                << ", kind = " << cache.hget(hash, "kind").value() << '\n';

        cache.erase(key);
        cache.erase(hash);
        std::cout << "Done after " << sw << '\n';
        // [/readme]
    } catch (const std::exception &error) {
        std::cerr << "sc-redis-demo: " << error.what() << '\n';
        return 1;
    }
    return 0;
}

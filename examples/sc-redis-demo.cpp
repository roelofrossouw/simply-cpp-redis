// Stores and reads values in Redis: a string, a hash (one field, several, all of them), a set,
// then removes them. Each line shows a call, as written, and what it returned.
// The servers come from SC_REDIS_DEMO_SERVER: one server, or Redis Cluster seed nodes separated
// by ';' ("redis1:6379;redis2:6379"). Unset or empty means 127.0.0.1:6379.

#include <iostream>
#include <string>

#include <sc.h>

#include <redis.h>

int main() {
    try {
        const sc::ip_endpoints servers{sc::getenv("SC_REDIS_DEMO_SERVER", "127.0.0.1"), 6379};
        sc::console::title("simply-cpp redis: storing and reading values, each call with what it returned");
        sc::console::output() << "Redis servers: " << servers << "  (SC_REDIS_DEMO_SERVER, default 127.0.0.1)\n";
        sc::timer sw;

        // [readme]
        sc::console::heading("Connecting");
        sc::redis cache{servers};
        sc::console::show_text("sc::redis cache{servers};", "connected in " + std::string(sw));

        sc::console::heading("A string value");
        SC_STEP(cache.set("sc-tmp:demo:greeting", "Hello World!"));
        SC_SHOW(cache.get("sc-tmp:demo:greeting"));
        SC_SHOW(cache.get("sc-tmp:demo:missing")); // std::optional: no such key

        sc::console::heading("A hash, one field at a time and several at once");
        SC_STEP(cache.hset("sc-tmp:demo:user", "name", "simply-cpp"));
        SC_STEP(cache.hset("sc-tmp:demo:user", "kind", "library"));
        SC_SHOW(cache.hget("sc-tmp:demo:user", "name"));
        SC_SHOW(cache.hmget("sc-tmp:demo:user", {"name", "colour"}));
        SC_SHOW(cache.hgetall("sc-tmp:demo:user"));

        sc::console::heading("A set: members without order or repeats");
        SC_SHOW(cache.sadd("sc-tmp:demo:tags", {"cpp", "redis", "demo"})); // members added
        SC_SHOW(cache.sadd("sc-tmp:demo:tags", "cpp"));                   // already there
        SC_SHOW(cache.scard("sc-tmp:demo:tags"));
        SC_SHOW(cache.sismember("sc-tmp:demo:tags", "redis"));
        SC_SHOW(cache.srem("sc-tmp:demo:tags", "demo"));
        SC_SHOW(cache.smembers("sc-tmp:demo:tags"));

        sc::console::heading("Removing them");
        SC_SHOW(cache.erase("sc-tmp:demo:greeting")); // keys removed
        SC_SHOW(cache.erase("sc-tmp:demo:user"));
        SC_SHOW(cache.erase("sc-tmp:demo:tags"));
        SC_SHOW(cache.get("sc-tmp:demo:greeting"));
        // [/readme]

        sc::console::output() << "\nAll of that took " << sw << ".\n";
    } catch (const std::exception &error) {
        std::cerr << "sc-redis-demo: " << error.what() << '\n';
        return 1;
    }
    return 0;
}

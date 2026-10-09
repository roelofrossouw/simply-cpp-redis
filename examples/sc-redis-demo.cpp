// Stores and reads values in Redis: a string, a hash (one field, several, all of them), then
// removes them. Each line shows a call, as written, and what it returned.
// The servers come from SC_REDIS_DEMO_SERVER: one server, or Redis Cluster seed nodes separated
// by ';' ("redis1:6379;redis2:6379"). Unset or empty means 127.0.0.1:6379.

#include <core.h>
#include <ip_endpoints.h>
#include <redis.h>
#include <timer.h>

#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {
    // How a result is printed: strings quoted, nothing as (none), lists and maps in brackets.
    template<typename T>
    void print(std::ostream &out, const T &value) { out << value; }

    void print(std::ostream &out, const std::string &value) { out << '"' << value << '"'; }

    template<typename T>
    void print(std::ostream &out, const std::optional<T> &value) {
        if (value) print(out, *value);
        else out << "(none)";
    }

    template<typename T>
    void print(std::ostream &out, const std::vector<T> &values) {
        out << '[';
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i) out << ", ";
            print(out, values[i]);
        }
        out << ']';
    }

    template<typename K, typename V>
    void print(std::ostream &out, const std::map<K, V> &values) {
        out << '{';
        bool first = true;
        for (const auto &[key, value]: values) {
            out << (first ? "" : ", ");
            first = false;
            print(out, key);
            out << ": ";
            print(out, value);
        }
        out << '}';
    }

    // One step of the demo: the call, as written in the source, and what it returned.
    template<typename T>
    void show(const std::string_view call, const T &result) {
        std::ostringstream text;
        print(text, result);
        std::cout << "  " << call << "\n      -> " << text.str() << '\n';
    }

    void heading(const std::string_view title) { std::cout << '\n' << title << '\n'; }
}

#define SHOW(expression) show(#expression, expression)
#define STEP(statement) (std::cout << "  " << #statement << '\n', statement)

int main() {
    try {
        const sc::ip_endpoints servers{sc::getenv("SC_REDIS_DEMO_SERVER", "127.0.0.1"), 6379};
        std::cout << "simply-cpp redis: storing and reading values, each call with what it returned\n"
                  << "Redis servers: " << servers << "  (SC_REDIS_DEMO_SERVER, default 127.0.0.1)\n";
        sc::timer sw;

        // [readme]
        heading("Connecting");
        sc::redis cache{servers};
        std::cout << "  sc::redis cache{servers}\n      -> connected in " << sw << '\n';

        heading("A string value");
        STEP(cache.set("sc-tmp:demo:greeting", "Hello World!"));
        SHOW(cache.get("sc-tmp:demo:greeting"));
        SHOW(cache.get("sc-tmp:demo:missing")); // std::optional: no such key

        heading("A hash, one field at a time and several at once");
        STEP(cache.hset("sc-tmp:demo:user", "name", "simply-cpp"));
        STEP(cache.hset("sc-tmp:demo:user", "kind", "library"));
        SHOW(cache.hget("sc-tmp:demo:user", "name"));
        SHOW(cache.hmget("sc-tmp:demo:user", {"name", "colour"}));
        SHOW(cache.hgetall("sc-tmp:demo:user"));

        heading("Removing them");
        SHOW(cache.erase("sc-tmp:demo:greeting")); // keys removed
        SHOW(cache.erase("sc-tmp:demo:user"));
        SHOW(cache.get("sc-tmp:demo:greeting"));
        // [/readme]

        std::cout << "\nAll of that took " << sw << ".\n";
    } catch (const std::exception &error) {
        std::cerr << "sc-redis-demo: " << error.what() << '\n';
        return 1;
    }
    return 0;
}

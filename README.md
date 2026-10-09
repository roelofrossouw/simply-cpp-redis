# simply-cpp-redis

`sc-redis` provides a small Redis Cluster client based on hiredis. Supply a
single server or one or more Cluster seed nodes to the constructor as an
`sc::ip_endpoints`: a `;`-separated string, a `std::vector<sc::ip_endpoint>` or a
braced list. Entries without a port use 6379. It follows Redis Cluster `MOVED`
and `ASK` redirections automatically.

```cpp
sc::redis standalone_cache{"redis.example.com"};
sc::redis cache{"redis1.example.com;redis2.example.com:6380"};
sc::redis listed_cache({{"redis1.example.com", 6379}, {"redis2.example.com", 6380}});
sc::redis authenticated_cache(
    {"redis.example.com", 6380, 0, true, "configured-elsewhere"});
cache.set("key", "value");
const auto value = cache.get("key");

cache.hset("key", "field", "value");
const auto field = cache.hget("key", "field");
const auto fields = cache.hmget("key", {"field", "other"}); // std::vector<std::optional<std::string>>
const auto all = cache.hgetall("key");                     // std::map<std::string, std::string>

cache.sadd("tags", {"cpp", "redis"});         // members added: 2
cache.srem("tags", "redis");                  // members removed: 1
const auto count = cache.scard("tags");       // std::size_t
const auto tags = cache.smembers("tags");     // std::set<std::string>
const bool tagged = cache.sismember("tags", "cpp");
```

`redis_options` sets a password, database and timeouts for every node the client
connects to, including Cluster seeds given as a list:

```cpp
using namespace std::chrono_literals;
sc::redis cache{"redis1;redis2", {.password = "secret", .connect_timeout = 500ms, .command_timeout = 500ms}};
```

`connect_timeout` defaults to 2 seconds. `command_timeout` defaults to none: a
command waits for its reply however long it takes. Set it for anything that must
not hang when a server stops answering.

A client connects when it's constructed and keeps its connections open. If one
breaks while idle (Redis restarted, or a firewall dropped it), the next command
replaces it and runs again once, transparently. When Redis can't be reached
(no connection, or it broke or timed out again) the client throws
`sc::redis_unavailable`; Redis answering with an error (such as `WRONGTYPE`) is a
plain `std::runtime_error`, so an outage can be told apart from a bad command.

## Demo

`sc-redis-demo` stores and reads values (a string, then a hash one field, several
fields and all fields at a time) and removes them again, showing each call with
what it returned. It is installed with the runtime package (`simply-cpp-redis`),
so it also checks a machine can reach Redis without the `-dev` package:

```bash
sc-redis-demo                                                      # 127.0.0.1:6379
SC_REDIS_DEMO_SERVER="redis1.example.com;redis2.example.com:6380" sc-redis-demo
```

`SC_REDIS_DEMO_SERVER` holds one server or several Cluster seed nodes,
separated by `;` (quote the value in a shell). When it's unset or empty the
demo uses `127.0.0.1:6379`; an invalid value is an error. It is a demonstration,
not a test, so CTest doesn't run it.

Its source is `examples/sc-redis-demo.cpp`; the code below is copied from it at
configure time, so it always matches code that compiles:

<!-- sc-example: examples/sc-redis-demo.cpp -->
```cpp
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
```
<!-- /sc-example -->

## Building locally

Run `bash scripts/install.sh` to configure, build, test, package, and install.
If a Homebrew upgrade removes the cached hiredis library, reconfiguring
automatically locates the installed version; deleting the build directory is
not necessary.

## Tests

The `cluster` and `authenticated` CTests are small production integration
tests. They use unique `sc-tmp:simply-cpp-redis:*` keys and remove them on
exit. `authenticated` requires `SC_REDIS_TEST_PASSWORD` in the publishing
environment; it is never stored in the repository. On each build server,
declare it in the root-owned `/etc/simply-cpp/test.env` file. Redis deployment
sources that file immediately before running CTest, so it can also hold future
test-only environment variables.

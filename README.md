# simply-cpp-redis

`sc-redis` provides a small Redis Cluster client based on hiredis. Its default
constructor seeds discovery from `redis1.1web.co.za:6379` and
`redis2.1web.co.za:6379`, then follows Redis Cluster `MOVED` and `ASK`
redirections automatically.

```cpp
sc::redis cache;
cache.set("key", "value");
const auto value = cache.get("key");

cache.hset("key", "field", "value");
const auto field = cache.hget("key", "field");
```

The `redis` CTest is a small production integration test. It uses unique
`sc-tmp:simply-cpp-redis:*` keys and removes them on exit.

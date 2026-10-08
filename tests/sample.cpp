#include <iostream>
#include <redis.h>
#include <timer.h>

using namespace std;

int main() {
    sc::timer sw;
    sc::redis oneweb{
        std::vector<sc::ip_endpoint>{
            {"redis1.1web.co.za", 6379},
            {"redis2.1web.co.za", 6379},
        }
    };

    cout << "Connected after " << sw << endl;
    sw.reset();
    oneweb.set("tmp-key-a", "first-value");
    cout << "Set key " << sw << endl;
    sw.reset();
    const auto rslt1 = oneweb.get("tmp-key-a");
    cout << "Got key " << sw << endl;
    cout << rslt1.value() << endl;

    sw.reset();
    oneweb.hset("tmp-key-b", "name", "simply-cpp");
    oneweb.hset("tmp-key-b", "kind", "integration-test");
    cout << "Set hkeys " << sw << endl;
    sw.reset();
    const auto name = oneweb.hget("tmp-key-b", "name");
    const auto kind = oneweb.hget("tmp-key-b", "kind");
    cout << "Got hkeys " << sw << endl;

    cout << name.value() << endl;
    cout << kind.value() << endl;
    return 0;
}

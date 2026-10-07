#include "../vendor/plank-client/wacomidentity.h"

#include <cassert>
#include <cstring>

int main() {
    PlankWacomHidIdentity identity{};
    assert(plankParseWacomHidIdentity("0005:0000056A:00000360", &identity));
    assert(identity.bus == 5 && identity.product == 0x0360);
    assert(plankParseWacomHidIdentity("5:56a:315", &identity));
    assert(identity.product == 0x0315);

    const char* invalid[] = {
        "0003:0000056A:00000360", // USB must use its physical USB ancestor
        "0005:0000046D:00000360", // vendor is not Wacom
        "0005:0000056A:00010000", // product does not fit HID identity
        "0005:0000056A:", "0005:0000056A:00000360:1",
        "0005:0000056G:00000360", "0005:0000056A:000003600",
        "0005:0000056A", "", nullptr,
    };
    for (const char* hidId : invalid) {
        identity = {42, 42};
        assert(!plankParseWacomHidIdentity(hidId, &identity));
        assert(identity.bus == 42 && identity.product == 42);
    }
    assert(!plankParseWacomHidIdentity("0005:0000056A:00000360", nullptr));
}

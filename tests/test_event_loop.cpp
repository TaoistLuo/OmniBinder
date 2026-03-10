#include "core/event_loop.h"
#include "platform/platform.h"
#include <cstdio>
#include <cassert>

using namespace omnibinder;

int main() {
    printf("=== EventLoop Tests ===\n");
    
    printf("  TEST timer ... ");
    {
        EventLoop loop;
        int count = 0;
        loop.addTimer(50, [&count, &loop]() {
            count++;
            if (count >= 3) loop.stop();
        }, true);
        loop.run();
        assert(count >= 3);
    }
    printf("PASS\n");
    
    printf("  TEST post ... ");
    {
        EventLoop loop;
        bool called = false;
        loop.addTimer(10, [&loop, &called]() {
            loop.post([&called, &loop]() {
                called = true;
                loop.stop();
            });
        }, false);
        loop.run();
        assert(called);
    }
    printf("PASS\n");
    
    printf("\nAll event loop tests passed!\n");
    return 0;
}

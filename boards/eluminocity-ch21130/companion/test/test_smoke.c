/* Smoke test: proves the test harness compiles and runs. */
#include "test_harness.h"

int main(void)
{
    CHECK(1 + 1 == 2);
    CHECK_EQ(0xa10, 2576);
    TEST_MAIN_END();
}

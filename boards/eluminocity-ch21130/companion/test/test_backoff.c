#include "test_harness.h"
#include "backoff.h"

int main(void)
{
    /* doubles from 1s, capped at 60s */
    CHECK_EQ(backoff_next(0),  1);
    CHECK_EQ(backoff_next(1),  2);
    CHECK_EQ(backoff_next(2),  4);
    CHECK_EQ(backoff_next(32), 60);
    CHECK_EQ(backoff_next(60), 60);
    CHECK_EQ(backoff_next(100), 60);
    TEST_MAIN_END();
}

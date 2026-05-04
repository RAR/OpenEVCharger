#include "test_harness.h"

int  g_tests_run    = 0;
int  g_tests_failed = 0;
const char *g_current_case = NULL;

extern void suite_pingpong(void);
extern void suite_boot_config(void);

int main(void)
{
    suite_crc16();
    suite_crc32();
    suite_j1772();
    suite_fault();
    suite_tlv();
    suite_pingpong();
    suite_boot_config();

    fprintf(stdout, "%d cases run, %d failed\n", g_tests_run, g_tests_failed);
    return g_tests_failed ? 1 : 0;
}

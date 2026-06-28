/*
 * test_output.c - Simple BOF to test output capture
 */

#include <windows.h>
#include "beacon.h"

void go(char* args, int alen)
{
    BeaconOutput(CALLBACK_OUTPUT, "=== TEST OUTPUT START ===\n", 26);
    BeaconOutput(CALLBACK_OUTPUT, "BOF is running\n", 15);
    BeaconOutput(CALLBACK_OUTPUT, "Args length: ", 13);

    char num[16];
    int n = alen;
    int i = 0;
    if (n == 0) {
        num[i++] = '0';
    } else {
        char tmp[16];
        int j = 0;
        while (n > 0) {
            tmp[j++] = '0' + (n % 10);
            n /= 10;
        }
        while (j > 0) {
            num[i++] = tmp[--j];
        }
    }
    num[i] = '\0';

    BeaconOutput(CALLBACK_OUTPUT, num, i);
    BeaconOutput(CALLBACK_OUTPUT, "\n", 1);
    BeaconOutput(CALLBACK_OUTPUT, "=== TEST OUTPUT END ===\n", 24);
}

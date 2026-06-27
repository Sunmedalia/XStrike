/*
 * Minimal Beacon API declarations matching ruststrike-loader's stubs.
 *
 * This header is what a BOF #includes to call Beacon* APIs. The `datap`
 * struct layout MUST match the loader's (see crates/loader/src/beacon.rs) and
 * follows the Cobalt Strike 4.x layout:
 *   char *original;  // +0
 *   char *buffer;    // +8   current pointer
 *   int   length;    // +16  remaining length
 *   int   size;      // +20  total size
 */
#ifndef _BEACON_H
#define _BEACON_H

#include <windows.h>

#define CALLBACK_OUTPUT       0x0
#define CALLBACK_OUTPUT_OEM   0x1
#define CALLBACK_ERROR        0xd
#define CALLBACK_OUTPUT_UTF8  0x20

typedef struct {
    char *original;
    char *buffer;
    int   length;
    int   size;
} datap;

void  BeaconDataParse(datap *parser, char *buffer, int size);
int   BeaconDataInt(datap *parser);
short BeaconDataShort(datap *parser);
char *BeaconDataExtract(datap *parser, int *size);
void  BeaconOutput(int type, char *data, int len);
void  BeaconPrintf(int type, char *fmt, ...);
BOOL  BeaconIsAdmin(void);

#endif /* _BEacon_H */

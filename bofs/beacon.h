/*
 * beacon.h - Minimal Beacon API header for BOF development.
 * Matches the Cobalt Strike / rustbof Beacon API convention.
 */

#ifndef _BEACON_H_
#define _BEACON_H_

#include <windows.h>

/*
 * BOF convention for importing Windows APIs:
 *   DECLSPEC_IMPORT return_type WINAPI LIBRARY$Function(args...);
 *
 * The compiler generates __imp_LIBRARY$Function which the BOF loader
 * resolves via LoadLibraryA + GetProcAddress at runtime.
 */

/* ---- Beacon Output ---- */

#define CALLBACK_OUTPUT      0x00
#define CALLBACK_OUTPUT_OEM  0x1E
#define CALLBACK_OUTPUT_UTF8 0x20
#define CALLBACK_ERROR       0x0D

void BeaconOutput(int callbackType, const char* data, int len);

/* ---- Beacon Data API ---- */

typedef struct {
    char* original;
    char* buffer;
    int   length;
    int   size;
} datap;

void  BeaconDataParse(datap* parser, char* buffer, int size);
int   BeaconDataInt(datap* parser);
short BeaconDataShort(datap* parser);
int   BeaconDataLength(datap* parser);
char* BeaconDataExtract(datap* parser, int* size);

#endif /* _BEACON_H_ */

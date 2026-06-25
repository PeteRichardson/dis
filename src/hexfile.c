#include <string.h>
#include <stdlib.h>
#include "hexfile.h"
#include "memory.h"

/* ------------------------------------------------------------------
 * read the hex file
 */
uint16_t readHexFile(const char* hexFilename, uint16_t* out_end) {

    uint16_t runAddress = 0;
    uint16_t lastAddr = 0;
#ifndef HAVE_STRNCPY_S
#define strncpy_s(A, B, C, D) strncpy((A), (C), (D)); (A)[(D)] = 0
#endif

    /*
     * load the INTEL HEX file
     */

    FILE* hexFile = NULL;
#ifndef HAVE_FOPEN_S
    hexFile = fopen(hexFilename, "r");
#else
    fopen_s(&hexFile, hexFilename, "r");
#endif

    if (hexFile) {
        char lineBuffer[1024];
        char tmpBuffer[10];

        int totalBytesRead = 0;

        while (fgets(lineBuffer, sizeof(lineBuffer), hexFile)) {
            if (lineBuffer[0] != ':') continue;

            strncpy_s(tmpBuffer, sizeof(tmpBuffer), lineBuffer + 1, 2);
            int numBytes = (int)strtol(tmpBuffer, NULL, 16);
            totalBytesRead += numBytes;
            strncpy_s(tmpBuffer, sizeof(tmpBuffer), lineBuffer + 3, 4);
            int destAddr = (int)strtol(tmpBuffer, NULL, 16);

            strncpy_s(tmpBuffer, sizeof(tmpBuffer), lineBuffer + 7, 2);
            int recType = (int)strtol(tmpBuffer, NULL, 16);

            if (recType == 0) {
                if (runAddress == 0)
                    runAddress = (uint16_t)destAddr;
                for (int i = 0; i < numBytes; ++i) {
                    strncpy_s(tmpBuffer, sizeof(tmpBuffer), lineBuffer + 9 + (i * 2), 2);
                    uint8_t value = (uint8_t)strtol(tmpBuffer, NULL, 16);
                    MemWrite(destAddr + i, value);
                }
                uint16_t recordEnd = (uint16_t)(destAddr + numBytes - 1);
                if (recordEnd > lastAddr)
                    lastAddr = recordEnd;
            }
            else if (recType == 1) {
                break; // EOF record
            }
        }

        fclose(hexFile);

        if (totalBytesRead == 0) {
            printf("ERROR: Invalid Intel HEX file: %s\n", hexFilename);
            return 0;
        }
        else if (runAddress == 0) {
            printf("WARNING: Run address not set from Intel HEX file: %s\n", hexFilename);
            return 0;
        }
    }
    else {
        printf("ERROR: Unable to open HEX file: %s\n", hexFilename);
        return 0;
    }
    if (out_end) *out_end = lastAddr;
    return runAddress;
}

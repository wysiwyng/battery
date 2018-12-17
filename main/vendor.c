#include <string.h>

#include "vendor.h"
#include "oui.h"

extern char vendorstring[64];

int binSearchVendors(uint8_t* searchBytes, int lowerEnd, int upperEnd) {
    uint8_t listBytes[3];
    int     res;
    int     mid = (lowerEnd + upperEnd) / 2;

    while (lowerEnd <= upperEnd) {
        listBytes[0] = (*(const unsigned char *)(data_macs + mid * 5));
        listBytes[1] = (*(const unsigned char *)(data_macs + mid * 5 + 1));
        listBytes[2] = (*(const unsigned char *)(data_macs + mid * 5 + 2));

        res = memcmp(searchBytes, listBytes, 3);

        if (res == 0) {
            return mid;
        } else if (res < 0) {
            upperEnd = mid - 1;
            mid      = (lowerEnd + upperEnd) / 2;
        } else if (res > 0) {
            lowerEnd = mid + 1;
            mid      = (lowerEnd + upperEnd) / 2;
        }
    }

    return -1;
}

void searchVendor(uint8_t* mac) {
    int    pos        = binSearchVendors(mac, 0, sizeof(data_macs) / 5 - 1);
    int    realPos    = (*(const unsigned char *)(data_macs + pos * 5 + 3)) | (*(const unsigned char *)(data_macs + pos * 5 + 4)) << 8;
    int idx = 0;
    if (pos >= 0) {
        char tmp;

        for (int i = 0; i < 8; i++) {
            tmp = (char)(*(const unsigned char *)(data_vendors + realPos * 8 + i));

            if (tmp != 0) vendorstring[idx++] += tmp;
            tmp += ' ';
        }
    }
    vendorstring[idx] = 0;
}
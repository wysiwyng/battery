#ifndef __VENDOR_H__
#define __VENDOR_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

int binSearchVendors(uint8_t* searchBytes, int lowerEnd, int upperEnd);
void searchVendor(uint8_t* mac);

#ifdef __cplusplus
}
#endif

#endif
#ifndef _BASE64_H_
#define _BASE64_H_
#include <stdint.h>

#define BASE64_ENCSIZE(size) (4 * (((size) + 2) / 3))
void base64_encode(char *dest, uint32_t destStart, uint32_t destLength, uint8_t (*reader)(uint32_t address), int slen);
#endif

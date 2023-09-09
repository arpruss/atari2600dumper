/*
 * public domain
 * Written by Manuel Badzong. If you have suggestions or improvements, let me
 * know.
 */
 
#include "base64.h"

//#define TEST
#ifdef TEST
#include <stdio.h>
#include <string.h>
#endif

static const char encoder[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";


// put the [destStart:destStart+destLength) section of the base64 encoding into dest
void
base64_encode(char *dest, uint32_t destStart, uint32_t destLength, uint8_t (*reader)(uint32_t address), int slen)
{
	int dlen, i, j;
	uint32_t a, b, c, triple;
    uint32_t destEnd = destStart + destLength;

	for (i = 0, j = 0; i < slen;)
	{
        if (j >= destEnd) {
            return;
        }
        
        if (j+4 <= destStart) {
            i += 3;
            j += 4;
            continue; // ideally we'd do some math to avoid looping, but in our application it doesn't matter much
        }
        
		a = reader(i++);

		// b and c may be off limit
		b = i < slen ? reader(i++) : 0;
		c = i < slen ? reader(i++) : 0;

		triple = (a << 16) + (b << 8) + c;
        
        if (destStart <= j /* && j < destEnd */)
            dest[j-destStart] = encoder[(triple >> 18) & 0x3F];
        j++;
        if (destStart <= j && j < destEnd)
            dest[j-destStart] = encoder[(triple >> 12) & 0x3F];
        j++;
        if (destStart <= j && j < destEnd)
            dest[j-destStart] = encoder[(triple >> 6) & 0x3F];
        j++;
        if (destStart <= j && j < destEnd)
            dest[j-destStart] = encoder[(triple) & 0x3F];
        j++;
	}

	// Pad zeroes at the end
	switch (slen % 3)
	{
	case 1:
        if (destStart <= j-2 && j-2 < destEnd) 
            dest[j-2 - destStart] = '=';
	case 2:
        if (destStart <= j-1 && j-1 < destEnd) 
            dest[j-1 - destStart] = '=';
	}
}

#ifdef TEST
uint8_t input[123457];
char out[BASE64_ENCSIZE(sizeof(input))];
char out2[BASE64_ENCSIZE(sizeof(input))];

uint8_t read(unsigned address) {
    if (address >= sizeof(input)) {
        printf("Error: reading %u\n", address);
        return 0;
    }
    return input[address];
}

int main(int argc, char** argv) {
    for (unsigned i = 0 ; i<sizeof(input); i++)
        input[i] = (uint8_t)i;
    base64_encode(out, 0, sizeof(out), read, sizeof(input));
    memset(out2, '.', sizeof(out2));
    for (unsigned i=0; i<sizeof(out2); i++) 
        base64_encode(out2+i, i,1, read, sizeof(input));
    if (memcmp(out2, out, sizeof(out))) {
        puts("Error with chunks of size 1.");
        return 1;
    }
    memset(out2, '.', sizeof(out2));
    for (unsigned i=0; i<sizeof(out2); i+=7) 
        base64_encode(out2+i, i,(i+7)<=sizeof(out2) ? 7 : sizeof(out2)-i, read, sizeof(input));
    if (memcmp(out2, out, sizeof(out))) {
        puts("Error with chunks of size 7.");
        return 1;
    }
    memset(out2, '.', sizeof(out2));
    for (unsigned i=0; i<sizeof(out2); i+=3) 
        base64_encode(out2+i, i,(i+3)<=sizeof(out2) ? 3 : sizeof(out2)-i, read, sizeof(input));
    if (memcmp(out2, out, sizeof(out))) {
        puts("Error with chunks of size 3.");
        return 1;
    }
    memset(out2, '.', sizeof(out2));
    for (unsigned i=0; i<sizeof(out2); i+=4) 
        base64_encode(out2+i, i,(i+4)<=sizeof(out2) ? 4 : sizeof(out2)-i, read, sizeof(input));
    if (memcmp(out2, out, sizeof(out))) {
        puts("Error with chunks of size 4.");
        return 1;
    }
    puts("success");
    return 0;
}
#endif
#ifndef _FAT16Readonly_H_
#define _FAT16Readonly_H_

#include <stdint.h>

#define FAT16_NUM_SECTORS          16384 // big enough to force FAT16
#define FAT16_MAX_ROOT_DIR_ENTRIES 512
#define FAT16_SECTOR_SIZE          512
#define FAT16_NUM_ROOT_DIR_ENTRIES_FOR_LFN(length) (((length)+1+12)/13) // length should not include terminating nul
#define FAT16_DATE(y,m,d)          ( ((y)-1980)<<9 | (m)<<5 | (d) )
#define FAT16_TIME(h,m,s)         ( (h)<<11 | (m)<<5 | ((s)/2) )

typedef struct __attribute__ ((packed)) {
  char name[11]; // 0-10
  uint8_t attributes; // 11
  uint8_t reserved;   // 12
  uint8_t createdCentiseconds; // 13
  uint16_t createdTime; // 14
  uint16_t createdDate; // 16
  uint16_t accessedDate; // 18
  uint16_t reserved2; // 20
  uint16_t modifiedTime; // 22
  uint16_t modifiedDate; // 24
  uint16_t cluster; // 26
  uint32_t size; // 28
} FAT16RootDirEntry;

bool FAT16MemoryFileReader(uint8_t* out, const uint8_t* in, uint32_t inLength, uint32_t sector, uint32_t numSectors);
typedef bool (*FAT16FileReader)(uint8_t* buf, const char* name, uint32_t sector, uint32_t numSectors);
FAT16RootDirEntry* FAT16AddFile(const char* name, uint32_t size);
bool FAT16AddLFN(const char* shortName, const char* longName);
void FAT16SetRootDir(FAT16RootDirEntry* r, unsigned count, FAT16FileReader reader);
bool FAT16ReadSector(uint8_t *buf, uint32_t sector, uint16_t numSectors);

// TODO: names starting with E6

#endif

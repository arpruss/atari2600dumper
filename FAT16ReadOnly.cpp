#include "FAT16ReadOnly.h"
#include <string.h>
#include <ctype.h>

#define NUM_SECTORS         FAT16_NUM_SECTORS
#define MAX_ROOT_DIR_ENTRIES FAT16_MAX_ROOT_DIR_ENTRIES
#define CLUSTER_SIZE           2
#define SECTOR_SIZE          FAT16_SECTOR_SIZE // DO NOT CHANGE
#define MAX_CLUSTERS        ((NUM_SECTORS+CLUSTER_SIZE-1)/CLUSTER_SIZE)
#define SECTORS_PER_FAT     ((2*MAX_CLUSTERS+SECTOR_SIZE-1)/SECTOR_SIZE)
#define SECTORS_ROOT_DIR    ((MAX_ROOT_DIR_ENTRIES*32+SECTOR_SIZE-1)/SECTOR_SIZE)
#define NUM_FATS               2

#define BOOT_SECTOR         0
#define FATS_SECTOR         1
#define ROOT_DIR_SECTOR     (FATS_SECTOR+NUM_FATS*SECTORS_PER_FAT)
#define DATA_SECTOR         (ROOT_DIR_SECTOR+SECTORS_ROOT_DIR)

const struct {
  uint8_t  jumpCode[3];
  char     oemName[8];
  uint16_t bytesPerSector;
  uint8_t  sectorsPerCluster;
  uint16_t reservedSectors;
  uint8_t  copiesOfFAT;
  uint16_t maxRootDirEntries;
  uint16_t numSectors16;
  uint8_t  mediaDescriptor;
  uint16_t sectorsPerFAT;
  uint16_t sectorsPerTrack;
  uint16_t numHeads;
  uint32_t numHiddenSectors;
  uint32_t numSectors32;
  uint8_t  logicalDriveNumber;
  //uint8_t  reserved;
  //uint8_t  extendedSignature;
  //uint32_t serialNumber;
  //char     volumeName[11];
  //char     fatName[8];
} __packed FAT16BootRecord = {
  .jumpCode = {0xeb,0x3c,0x90},
  .oemName  = {'r','o','f','a','t'},
  .bytesPerSector = SECTOR_SIZE,
  .sectorsPerCluster = CLUSTER_SIZE,
  .reservedSectors = 1,
  .copiesOfFAT = 2,
  .maxRootDirEntries = MAX_ROOT_DIR_ENTRIES,
  .numSectors16 = NUM_SECTORS,
  .mediaDescriptor = 0xF8,
  .sectorsPerFAT = SECTORS_PER_FAT,
  .sectorsPerTrack = 0x20,
  .numHeads = 0x20,
  .numHiddenSectors = 0,
  .numSectors32 = 0,
  .logicalDriveNumber = 0x80,
};

static FAT16RootDirEntry* rootDir;
static uint16_t rootDirEntries;
static FAT16FileReader fileReader;


static char* from83Filename(const char* name11) {
  static char out[13];
  int i;
  char* toReturn;
  
  if (name11[8] == ' ') {
    out[8] = 0;
  }
  else {
    out[8] = '.';
    for (i = 0 ; i < 3 && name11[8+i] != ' '; i++)
      out[9+i] = name11[8+i];
    out[9+i] = 0;
  }
  for (i = 7; i >= 0 && name11[i] == ' ' ; i--);
  i++; // length of filename
  if (i <= 0)
    toReturn = out+8;
  else {
    memcpy(out+8-i, name11, i);
    toReturn = out+8-i;
  }
  if (toReturn[0] == 0x05)
    toReturn[0] = 0xE5;
  return toReturn;
}

static void to83Filename(char* out, const char* name) {
  int len = strlen(name);
  int i;
  
  if (len == 0) {
    memset(out, ' ', 11);
    return;
  }
  int ext;
  for (ext = len - 1; ext >= 0 && name[ext] != '.' ; ext--); 
  if (ext < 0) { 
    // no extension
    ext = len;
    memset(out+8, ' ', 3);
  }
  else {
    const char* extension = name + ext + 1;
    for(i=0; i<3 && extension[i]; i++)
      out[8+i] = toupper(extension[i]);
    for(; i<3; i++)
      out[8+i] = ' ';
  }
  for(i=0; i<8 && i<ext ; i++)
    out[i] = toupper(name[i]);
  for(; i<8; i++)
    out[i] = ' ';
  if (out[0] == 0xE5)
    out[0] = 0x05;
}


static inline bool hasData(int rootDirPos) {
  FAT16RootDirEntry* entry = rootDir + rootDirPos;
  return entry->name[0] != 0 && entry->name[0] != 0xE5 && entry->size != 0 && entry->cluster != 0;
}

static void updateClusters() {
  uint16_t cluster = 2;
  for (unsigned i=0; i<rootDirEntries && rootDir[i].name[0]; i++) {
    if (! hasData(i))
      continue;
    rootDir[i].cluster = cluster;
    cluster += (rootDir[i].size + CLUSTER_SIZE * SECTOR_SIZE - 1) / (CLUSTER_SIZE * SECTOR_SIZE);
  }
}

FAT16RootDirEntry* FAT16AddFile(const char* name, uint32_t size) {
  int i;
  for (i=0 ; i<rootDirEntries && rootDir[i].name[0] ; i++);
  if (i >= rootDirEntries)
    return NULL;
  to83Filename(rootDir[i].name, name);
  rootDir[i].size = size;
  rootDir[i].accessedDate = FAT16_DATE(2023,1,1);
  rootDir[i].createdDate = FAT16_DATE(2023,1,1);
  rootDir[i].modifiedDate = FAT16_DATE(2023,1,1);
  rootDir[i].createdTime = FAT16_TIME(12,0,0);
  rootDir[i].modifiedTime = FAT16_TIME(12,0,0);
  rootDir[i].cluster = 0xFFFF;
  updateClusters();
  return rootDir+i;
}

void FAT16AddLabel(const char* name) {
  int i;
  for (i=0 ; i<rootDirEntries && rootDir[i].name[0] ; i++);
  if (i >= rootDirEntries)
    return;
  int len = strlen(name);
  for (int j = 0 ; j < 11 ; j++) {
    if (j < len) 
      rootDir[i].name[j] = name[j];
    else
      rootDir[i].name[j] = ' ';
  }
  rootDir[i].size = 0;
  rootDir[i].accessedDate = FAT16_DATE(2023,1,1);
  rootDir[i].createdDate = FAT16_DATE(2023,1,1);
  rootDir[i].modifiedDate = FAT16_DATE(2023,1,1);
  rootDir[i].createdTime = FAT16_TIME(12,0,0);
  rootDir[i].modifiedTime = FAT16_TIME(12,0,0);
  rootDir[i].attributes = 8;
  rootDir[i].cluster = 0;
}

bool FAT16AddLFN(const char* shortName, const char* longName) {
  // todo: reverse order of parts
  // see: https://www.win.tue.nl/~aeb/linux/fs/fat/fat-1.html
  unsigned length = strlen(longName) + 1;
  int i;
  for (i=0 ; i<rootDirEntries && rootDir[i].name[0] ; i++);
  char shortName83[11];
  to83Filename(shortName83, shortName);
  uint8_t sum = 0;
  for (unsigned j = 0; j < 11 ; j++) {
    sum = (sum>>1) + ((sum&1)<<7);
    sum += (uint8_t)shortName83[j];
  }
  uint8_t sequence = 1;
  int index = i + (length + 12) / 13 - 1;
  if (length == 0 || index >= rootDirEntries)
    return false;
  unsigned start = 0; 
  while (length > 0) {
    int currentPart = length > 13 ? 13 : length;
    uint8_t* entry = (uint8_t*)(rootDir+index);
    memset(entry, 0, sizeof(FAT16RootDirEntry));
    index--;
    length -= currentPart;
    entry[0] = sequence++;
    entry[11] = 0xF;
    entry[13] = sum;
    if (length == 0)
      entry[0] |= 1<<6;
    uint8_t* outPos = entry+1;
    for (unsigned j = 0 ; j < 13 ; j++) {
      if (outPos == entry + 11)
        outPos = entry + 14;
      else if (outPos == entry + 26)
        outPos = entry + 28;
      if (j < currentPart) {
        *outPos++ = *longName;
        *outPos++ = 0;
        longName++;
      }
      else {
        *outPos++ = 0xFF;
        *outPos++ = 0xFF;
      }
    }
  }
  return true;
}

static bool readDataSector(uint8_t* buf, uint32_t dataSector) {
  uint32_t sector = 0;
  memset(buf, 0, SECTOR_SIZE);
  for (int i=0; i<rootDirEntries && rootDir[i].name[0] && sector <= dataSector; i++) {
    if (! hasData(i))
      continue;
    uint16_t size = (rootDir[i].size + CLUSTER_SIZE * SECTOR_SIZE - 1) / (CLUSTER_SIZE * SECTOR_SIZE) * CLUSTER_SIZE;
    if (dataSector < sector + size) {
      return fileReader(buf, from83Filename(rootDir[i].name), dataSector - sector, 1);
    }
    sector += size;
  }
  return true;
}

static void readBootSector(uint8_t* buf) {
  memset(buf, 0, SECTOR_SIZE);
  memcpy(buf, &FAT16BootRecord, sizeof(FAT16BootRecord));
  buf[0x1FE] = 0x55;
  buf[0x1FF] = 0xAA;
}



void FAT16SetRootDir(FAT16RootDirEntry* r, unsigned count, FAT16FileReader reader) {
  rootDirEntries = count;
  rootDir = r;
  fileReader = reader;
  memset(r, 0, sizeof(*r) * count);
}

static void readRootDirSector(uint8_t* buf, uint32_t sectorNumber) {
  memset(buf, 0, SECTOR_SIZE);
  uint32_t start = sectorNumber * SECTOR_SIZE / sizeof(FAT16RootDirEntry);
  uint32_t end = (sectorNumber + 1) * SECTOR_SIZE / sizeof(FAT16RootDirEntry);
  if (start >= rootDirEntries)
    return;
  if (end > rootDirEntries)
    end = rootDirEntries;
  memcpy(buf, rootDir+start, (end-start)*sizeof(FAT16RootDirEntry));
}

static void readFatSector(uint8_t* buf, uint32_t sectorNumber) {
  memset(buf, 0, SECTOR_SIZE);
  if (sectorNumber == 0) {
    buf[0] = FAT16BootRecord.mediaDescriptor;
    buf[1] = 0xFF;
    buf[2] = 0xFF;
    buf[3] = 0xFF;
  }
  uint16_t firstCluster = sectorNumber * SECTOR_SIZE / 2;
  uint16_t endCluster = firstCluster + SECTOR_SIZE / 2;
  
  uint16_t cluster = 2;
  for (int i=0; i<MAX_ROOT_DIR_ENTRIES && rootDir[i].name[0]; i++) {
    if (! hasData(i))
      continue;
    uint16_t last = cluster + (rootDir[i].size + CLUSTER_SIZE * SECTOR_SIZE - 1) / (CLUSTER_SIZE * SECTOR_SIZE);
    for (; cluster < last ; cluster++) {
      if (firstCluster <= cluster && cluster < endCluster) {
        uint8_t* entry = buf + (cluster - firstCluster) * 2;
        if (cluster + 1 < last) {
          uint16_t next = cluster+1;
          entry[0] = next & 0xFF;
          entry[1] = next >> 8;
        }
        else {
          entry[0] = 0xFF;
          entry[1] = 0xFF;
        }
      }
      else if (cluster >= endCluster)
        break;
    }
  }
}

static void readSector(uint8_t* buf, uint32_t sectorNumber) {
  if (sectorNumber == BOOT_SECTOR)
    readBootSector(buf);
  else if (sectorNumber < ROOT_DIR_SECTOR) 
    readFatSector(buf, (sectorNumber - FATS_SECTOR) % SECTORS_PER_FAT);
  else if (sectorNumber < DATA_SECTOR)
    readRootDirSector(buf, sectorNumber - ROOT_DIR_SECTOR);
  else
    readDataSector(buf, sectorNumber - DATA_SECTOR);
}

bool FAT16ReadSectors(uint8_t *buf, uint32_t sector, uint16_t numSectors) {
  for (int i=0; i<numSectors; i++)
    readSector(buf+i*SECTOR_SIZE, sector+i);
  
  return true;
}

/* 
 * This is a pretty complicated thing to explain. The idea is that we imagine a file, and we want to fill a destination buffer with a specified
 * range of sectors out of that file. However, the file as a whole isn't quite available. Instead, what we have is a source chunk, which 
 * represents the subset of the file starting at position srcChunkStartInFile with length srcChunkStartInFile+srcChunkLength. We want to 
 * know what copy operation needs to be performed from the source chunk to the destination buffer. If both the source chunk
 * and the destination buffer are uint8_t arrays, then after calling 
 *   length = FAT16GetChunkCopyRange(srcChunkStartInFile, srcChunkLength, requestedSectorStart, requestedNumberOfSectors, &destStart, &srcStart);
 * you need to do:
 *   if (length) memcpy(dest+destStart,srcChunk+srcStart,length);
 * If one or the other isn't a buffer, what you do with the information may be more complex.
 */
uint32_t FAT16GetChunkCopyRange(uint32_t srcChunkStartInFile, uint32_t srcChunkLength, uint32_t requestedSectorStart, uint32_t requestedNumberOfSectors,
        uint32_t* destStartP, uint32_t* srcStartP) {
    uint32_t requestStart = requestedSectorStart * FAT16_SECTOR_SIZE;
    uint32_t requestEnd = requestStart + requestedNumberOfSectors * FAT16_SECTOR_SIZE;
    if (requestEnd <= srcChunkStartInFile)
        return 0;
    uint32_t srcChunkEndInFile = srcChunkStartInFile + srcChunkLength;
    if (srcChunkEndInFile <= requestStart)
        return 0;
    /* at this point, we know that the requested region overlaps the available chunk */
    if (srcChunkStartInFile <= requestStart)
        *srcStartP = requestStart - srcChunkStartInFile;
    else
        *srcStartP = 0;
    uint32_t srcEnd;
    if (srcChunkEndInFile <= requestEnd)
        srcEnd = srcChunkLength;
    else
        srcEnd = requestEnd - srcChunkStartInFile;
    if (srcChunkStartInFile <= requestStart)
        *destStartP = 0;
    else
        *destStartP = srcChunkStartInFile - requestStart;
    return srcEnd - *srcStartP;
}

void FAT16MemoryFileReader(uint8_t* out, const uint8_t* in, uint32_t inLength, uint32_t sector, uint32_t numSectors) {
    uint32_t length;
    uint32_t src;
    uint32_t dest;
    length = FAT16GetChunkCopyRange(0, inLength, sector, numSectors, &dest, &src);
    if (length) memcpy(out+dest, in+src, length);
    /*
    uint32_t start = sector * SECTOR_SIZE;
    if (start >= inLength)
      return true;
    uint32_t end = start + numSectors * SECTOR_SIZE;
    if (end > inLength)
      end = inLength;
    memcpy(out, in+start, end-start); 
    */
}

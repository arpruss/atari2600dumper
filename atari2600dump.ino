
/*
 * You need 21 pins, 8 of them (data) 5V tolerant. The assignments below
 * are for an stm32f103c8t6 blue pill.
 *
 * View facing console:
 *             PB0  PB1  PB11 PB10 PC15 PB14 PB15 PA8  PA9  PA10
 * --------------------------------------------------------------
 * | GND  5V   A8   A9   A11  A10  A12  D8   D7   D6   D5   D4  |
 * |
 * | A7   A6   A5   A4   A3   A2   A1   A0   D1   D2   D3   GND |
 * --------------------------------------------------------------
 *   PA7  PA6  PA5  PA4  PA3  PA2  PA1  PA0  PB4  PB3  PA15
 */

#define MASS_STORAGE

#ifdef MASS_STORAGE
#include <USBComposite.h>
#include "FAT16ReadOnly.h"

USBMassStorage MassStorage;
USBCompositeSerial CompositeSerial;

#define PRODUCT_ID 0x29

FAT16RootDirEntry rootDir[3];

char info[FAT16_SECTOR_SIZE];
char index_htm[] = "<meta http-equiv=\"Refresh\" content=\"0; url='https://javatari.org/'\" />";
#endif

unsigned dataPins[8] = { PB4,PB3,PA15,PA10,PA9,PA8,PB15,PB14 };
unsigned addressPins[13] = { PA0,PA1,PA2,PA3,PA4,PA5,PA6,PA7,PB0,PB1,PB11,PB10,PC15 };
unsigned romSize = 4096;
uint32_t* hotspots = NULL;
uint32_t hotspots_F6[] = { 0xff6, 0xff7, 0xff8, 0xff9 };
uint32_t hotspots_F8[] = { 0xff8, 0xff9 };
uint32_t hotspots_FA[] = { 0xff8, 0xff9, 0xffa };
uint32_t numHotspots = 0;

// crc32 adapted from https://github.com/aeldidi/crc32/blob/master/src/crc32.c

uint32_t crc32_for_byte(uint32_t byte)
{
  const uint32_t polynomial = 0xEDB88320L;
  uint32_t       result     = byte;
  size_t         i          = 0;

  for (; i < 8; i++) {
    /* IMPLEMENTATION: the code below always shifts result right by
     * 1, but only XORs it by the polynomial if we're on the lowest
     * bit.
     *
     * This is because 1 in binary is 00000001, so ANDing the
     * result by 1 will always give 0 unless the lowest bit is set.
     * And since XOR by zero does nothing, the other half only
     * occurs when we're on the lowest bit.
     *
     * I didn't leave the above implementation in, despite being
     * faster on my machine since it is a more complex operation
     * which may be slower on less sophisticated processors. It can
     * be added in in place of the loop code below.
     */

    result = (result >> 1) ^ (result & 1) * polynomial;

    /* Here is the code I replaced with the branch I tried to
     * remove:
    if (result & 1) {
      result = (result >> 1) ^ polynomial;
      continue;
    }
    result >>= 1;
     */
  }
  return result;
}

uint32_t crc32(const void *input, size_t size, uint32_t start)
{
  const uint8_t *current = (const uint8_t*)input;
  uint32_t       result  = ~start;
  size_t         i       = 0;

  for (; i < size; i++) {
    result ^= current[i];
    result = crc32_for_byte(result);
  }

  return ~result;
}

uint32_t crcRange(uint32_t start, uint32_t count) {
  uint32_t c = 0;
  uint32_t i = start;
  uint32_t result = ~read(i);
  
  for (;  i < start + count ; i++) {
    result ^= read(i);
    result = crc32_for_byte(result);
  }

  return ~result;
}

uint32_t address = 0;
uint32_t crc = 0;

uint8_t read(uint32_t address) {
  digitalWrite(addressPins[12], 0);
  for (unsigned i = 0 ; i < 13 ; i++, address >>= 1) {
    digitalWrite(addressPins[i], address & 1);
  }
  delayMicroseconds(20);
  digitalWrite(addressPins[12], 1);
  uint8_t datum = 0;
  for (int i = 7 ; i >= 0 ; i--) {
    datum = (datum << 1) | digitalRead(dataPins[i]);
  }

  return datum;
}

uint8_t bankedRead(uint32_t address) {
  static int lastBank = -1;
  if (numHotspots == 0)
    return read(address);
  int bank = address / 4096;
  if (bank != lastBank) {
    read(hotspots[bank]);
    lastBank = bank;
  }
  return read(address % 4096);
}

bool diff(uint32_t hotspot1,uint32_t hotspot2) {
  read(hotspot1);
  uint32_t c = crcRange(256,500);
  read(hotspot2);
  return c != crcRange(256,500);
}

void detectHotspots() {
  const char* msg = NULL;
  if (diff(hotspots_F6[0],hotspots_F6[1])) {
    hotspots = hotspots_F6;
    numHotspots = sizeof(hotspots_F6)/sizeof(*hotspots_F6);
    msg = "Detected F6 bank switching.\n";
  }
  else if (diff(hotspots_FA[1],hotspots_FA[2])) {
    hotspots = hotspots_FA;
    numHotspots = sizeof(hotspots_FA)/sizeof(*hotspots_FA);
    msg = "Detected FA bank switching.\n";
  }
  else if (diff(hotspots_F8[0],hotspots_F8[1])) {
    hotspots = hotspots_F8;
    numHotspots = sizeof(hotspots_F8)/sizeof(*hotspots_F8);
    msg = "Detected F8 bank switching.\n";
  }
  else {
    msg = "No bank switching detected.\n";
  }
  romSize = numHotspots > 0 ? numHotspots * 4096 : 4096;
  strcpy(info, msg);
}

#ifdef MASS_STORAGE
bool write(const uint8_t *writebuff, uint32_t memoryOffset, uint16_t transferLength) {
  return false;
}

bool fileReader(uint8_t *buf, const char* name, uint32_t sector, uint32_t sectorCount) {
  if (!strcmp(name,"GAME.A26")) {
    uint32_t size = sectorCount * FAT16_SECTOR_SIZE;
    uint32_t start = sector * FAT16_SECTOR_SIZE;
    for (unsigned i = 0 ; i < size ; i++)
      buf[i] = bankedRead(start + i);
    return true;
  }
  else if (!strcmp(name,"INFO.TXT")) {
    strcpy((char*)buf, info); 
    return true;
  }
  else if (!strcmp(name,"INDEX.HTM")) {
    strcpy((char*)buf, index_htm);
    return true;
  }
  return false;
}
#endif

void setup() {
  for (unsigned i = 0 ; i < 8 ; i++)
    pinMode(dataPins[i], INPUT);
  for (unsigned i = 0 ; i < 13 ; i++)
    pinMode(addressPins[i], OUTPUT);
  detectHotspots();
#ifdef MASS_STORAGE
  FAT16SetRootDir(rootDir, sizeof(rootDir)/sizeof(*rootDir), fileReader);
  sprintf(info+strlen(info), "Length: %u bytes\n", romSize);
  FAT16AddFile("INDEX.HTM", strlen(index_htm));
  FAT16AddFile("INFO.TXT", strlen(info));
  FAT16AddFile("GAME.A26", romSize);

  USBComposite.setProductId(PRODUCT_ID);
  MassStorage.setDriveData(0, FAT16_NUM_SECTORS, FAT16ReadSector, write);
  MassStorage.registerComponent();

  USBComposite.begin();
  while(!USBComposite);  
#else
  Serial.begin();
  while (!Serial);
  delay(2000);
#endif  
}


void loop() {
#ifdef MASS_STORAGE
  MassStorage.loop();
#else  
  char s[9];
  uint8_t data16[16];
  if (address >= romSize) {
    Serial.print("Dumped: ");
    Serial.print(romSize);
    Serial.print(" bytes\nCRC-32: ");
    sprintf(s,"%08lx", crc);
    Serial.println(s);
    for(;;);
  }
  uint16_t sum = 0;
  sprintf(s, ">%05lx ", address);
  Serial.print(s);
  for (unsigned i = 0 ; i < 16 ; i++) {
    uint8_t datum = bankedRead(address);
    data16[i] = datum;
    sum += datum;
    address++;
    sprintf(s, "%02x", datum);
    Serial.print(s);
  }
  crc = crc32(data16, 16, crc);
  sprintf(s, " %04x", sum);
  Serial.println(s);    
#endif  
}


// ET = c3e930e6

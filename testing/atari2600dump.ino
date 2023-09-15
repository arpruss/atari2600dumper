
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

// cartridge types theoretically supported: 2K, 4K, F4, F6, F8, FA, FE, DPC, plus Super Chip variants
// tested: F8, DPC

#include <ctype.h>
#include "libmaple/bitband.h"
#include <USBComposite.h>
#include "FAT16ReadOnly.h"
#include "base64.h"

USBMassStorage MassStorage;
USBCompositeSerial CompositeSerial;

#define PRODUCT_ID 0x29

//#define DEBUG
#define USE_BITBAND

#ifdef DEBUG
FAT16RootDirEntry rootDir[4+2*FAT16_NUM_ROOT_DIR_ENTRIES_FOR_LFN(255)+8];
#else
FAT16RootDirEntry rootDir[4+2*FAT16_NUM_ROOT_DIR_ENTRIES_FOR_LFN(255)];
#endif

char filename[255];
char stellaFilename[255];
char stellaShortName[] = "GAME.XXX";
char info[FAT16_SECTOR_SIZE];
const char launch_htm_0[]="<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'><title>Javatari</title><meta name='description' content='Javatari - The online Atari 2600 emulator'></head>"
"<body><div id='javatari' style='text-align: center; margin: 20px auto 0; padding: 0 10px;'><div id='javatari-screen' style='box-shadow: 2px 2px 10px rgba(0, 0, 0, .7);'></div></div>"
"<script src='https://arpruss.github.io/javatari/javatari.js'></script><script>Javatari.CARTRIDGE_URL = 'data:application/octet-stream;base64,";
const char launch_htm_1[]="';Javatari.preLoadImagesAndStart();</script></body></html>\n";

#define CARTRIDGE_KEEP_TIME_MILLIS 2000 // cartridge must be kept in this long to register
#define LED    PC13
#define LED_ON 0

#include "roms.h"

int gameNumber = -1;

gpio_dev* dataPort[8];
uint8_t dataBit[8];
gpio_dev* addressPort[13];
uint8_t addressBit[13];

const unsigned dataPins[8] = { PB4,PB3,PA15,PA10,PA9,PA8,PB15,PB14 };
const unsigned addressPins[13] = { PA0,PA1,PA2,PA3,PA4,PA5,PA6,PA7,PB0,PB1,PB11,PB10,PC15 };
#ifdef USE_BITBAND
volatile uint32_t* addressWriteBitband[13];
volatile uint32_t* dataReadBitband[8];
volatile uint32_t* dataWriteBitband[8];
#endif

uint32_t romSize;
uint32_t portStart;
uint32_t portEnd;
uint32_t port2Start;
uint32_t port2End;
char stellaExtension[5];
const uint32_t* hotspots = NULL;
#define MAPPER_DPC 0xD0
uint8_t mapper;
const uint32_t hotspots_F6[] = { 0xff6, 0xff7, 0xff8, 0xff9 };
const uint32_t hotspots_F8[] = { 0xff8, 0xff9 };
const uint32_t hotspots_F4[] = { 0xff4, 0xff5, 0xff6, 0xff7, 0xff8, 0xff9, 0xffa, 0xffb };
const uint32_t hotspots_FA[] = { 0xff8, 0xff9, 0xffa }; 
uint32_t numHotspots = 0;
int lastBank = -1;
uint32_t crc;
uint8_t detectBuffer[128];
uint8_t blankValue;

#ifdef USE_BITBAND
volatile uint32_t* getWriteBitband(gpio_dev* port, uint8_t bitNumber) {
  return (volatile uint32_t*)bb_perip( &(port->regs->ODR), bitNumber);
}

volatile uint32_t* getReadBitband(gpio_dev* port, uint8_t bitNumber) {
  return (volatile uint32_t*)bb_perip( &(port->regs->IDR), bitNumber);
}

void setBitbands() {
  for (unsigned i = 0 ; i < 13 ; i++)
    addressWriteBitband[i] = getWriteBitband(addressPort[i], addressBit[i]);
  for (unsigned i = 0 ; i < 9 ; i++) {
    dataWriteBitband[i] = getWriteBitband(dataPort[i], dataBit[i]);
    dataReadBitband[i] = getReadBitband(dataPort[i], dataBit[i]);
  }
}
#endif

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
  uint32_t result = ~0;
  
  for (;  i < start + count ; i++) {
    result ^= read(i);
    result = crc32_for_byte(result);
  }

  return ~result;
}

#ifdef USE_BITBAND
uint8_t read(uint32_t address) {
  if (mapper == 0xFE) {
    // this is a kludge to prevent an accidental access to 0x01xx 
    *(addressWriteBitband[8]) = 0;
  }
  *(addressWriteBitband[12]) = 0;
  delayMicroseconds(2);
  for (unsigned i = 0 ; i < 12 ; i++, address >>= 1) {
    *(addressWriteBitband[i]) = address & 1;
  }
  *(addressWriteBitband[12]) = 1;
  delayMicroseconds(4); 
  uint32_t datum = 0;
  for (int i = 7 ; i >= 0 ; i--) {
    datum = (datum << 1) | *(dataReadBitband[i]);
  }

  return datum;
}

// write but don't zero A12
uint8_t write0(uint32_t address, uint8_t value) {
  for (unsigned i = 0 ; i < 8 ; i++, value>>=1) {
    gpio_set_mode(dataPort[i], dataBit[i], (value & 1) ? GPIO_INPUT_PU : GPIO_INPUT_PD);
  }
  for (unsigned i = 0 ; i < 13 ; i++, address >>= 1) {
    *(addressWriteBitband[i]) = address & 1;
  }
  delayMicroseconds(4);

  dataPinState(GPIO_INPUT_FLOATING);
}

uint8_t write(uint32_t address, uint8_t value) {
  *(addressWriteBitband[12]) = 0;
  delayMicroseconds(2);  
  write0(address, value);
}

#else

uint8_t read(uint32_t address) {
  if (mapper == 0xFE) {
    // this is a kludge to prevent an accidental access to 0x01xx 
    gpio_write_bit(addressPort[8], addressBit[8], 0);
  }
  gpio_write_bit(addressPort[12], addressBit[12], 0);
  delayMicroseconds(2);
  for (unsigned i = 0 ; i < 12 ; i++, address >>= 1) {
    gpio_write_bit(addressPort[i], addressBit[i], address & 1);
  }
  gpio_write_bit(addressPort[12], addressBit[12], 1);
  delayMicroseconds(4); 
  uint8_t datum = 0;
  for (int i = 7 ; i >= 0 ; i--) {
    datum = (datum << 1) | (gpio_read_bit(dataPort[i], dataBit[i]) ? 1:0);
  }

  return datum;
}

// write but don't zero A12
uint8_t write0(uint32_t address, uint8_t value) {
  for (unsigned i = 0 ; i < 8 ; i++, value>>=1) {
    gpio_set_mode(dataPort[i], dataBit[i], (value & 1) ? GPIO_INPUT_PU : GPIO_INPUT_PD);
  }
  for (unsigned i = 0 ; i < 13 ; i++, address >>= 1) {
    gpio_write_bit(addressPort[i], addressBit[i], address & 1);
  }
  delayMicroseconds(4);

  dataPinState(GPIO_INPUT_FLOATING);
}

uint8_t write(uint32_t address, uint8_t value) {
  gpio_write_bit(addressPort[12], addressBit[12], 0);  
  delayMicroseconds(2);  
  write0(address, value);
}
#endif

void switchBankFE(uint8_t bank) {
    write0(0x01FE, 0xF0^(bank<<5));
    write0(0x01FE, 0xF0^(bank<<5));
}

void switchBank(uint8_t bank) {
  if (mapper == 0xFE) {
    switchBankFE(bank);
  }
  else if (mapper == 0xFA) {
    gpio_set_mode(dataPort[0], dataBit[0], GPIO_INPUT_PU); // D0 must be high to switch banks on FA: https://patents.google.com/patent/US4485457A/en
    read(hotspots[bank]);
    gpio_set_mode(dataPort[0], dataBit[0], GPIO_INPUT_FLOATING);       
  }
  else if (numHotspots > 0) {
    read(hotspots[bank]);
  }
}

// in theory, we could do sequential reads and it would be faster, but why bother?
uint8_t readDPCGraphics(uint16_t address) {
  write(0x1050, address & 0xFF);
  write(0x1058, address >> 8);
  return read(0x1008);
}

uint8_t bankedRead(uint32_t address) {
  if (mapper == MAPPER_DPC && address >= romSize - 2048) {
    return readDPCGraphics(2047 - (address - (romSize-2048)));
  }
  uint32_t address4k = address % 4096;
  if (portStart <= address4k && address4k < portEnd)
    return blankValue;
  if (port2Start <= address4k && address4k < port2End)
    return blankValue;
  if (romSize <= 4096)
    return read(address);
  int bank = address / 4096;
  if (bank != lastBank) {
    switchBank(bank);
    lastBank = bank;
  }
  uint8_t value = read(address4k);
  // check if what we read is one of the hotspots, so that
  // we might have unintentionally swapped banks
  for (int i=0; i<numHotspots; i++) {
    if (hotspots[i] == address4k) {
     lastBank = i;
     break;
    }
  }
  return value;
}

uint32_t romCRC(unsigned start, unsigned count) {
  uint32_t c = 0;
  uint32_t result = ~0;
  unsigned i = start;

  for (;  i < count ; i++) {
    result ^= bankedRead(i);
    result = crc32_for_byte(result);
  }

  return ~result;
}

bool diff(uint8_t _mapper, const uint32_t* _hotspots, unsigned _numHotspots, uint8_t b1, uint8_t b2) {
  mapper = _mapper;
  hotspots = _hotspots;
  numHotspots = _numHotspots;
  // sample memory space every 37 bytes, skipping a potential RAM space at the beginning; this is nearly certain to pick up a difference 
  // between banks if there is a difference
  // detectBuffer is large enough for this sampling
  switchBank(b1);
  for (unsigned i=512,j=0;i<0x0FF0; (i+=37),j++)
    detectBuffer[j] = read(i);
  switchBank(b2);
  for (unsigned i=512,j=0;i<0x0FF0; (i+=37),j++)
    if (!(0x800<=i && i<0x880) && detectBuffer[j] != read(i)) // skip DPC port
      return true;
  return false;
}

bool detectFE() {
  mapper = 0xFE;
  switchBankFE(0);
  for (unsigned i=512,j=0;i<0x0FF0; (i+=37),j++)
    detectBuffer[j] = read(i);  
  switchBankFE(1);
  for (unsigned i=512,j=0;i<0x0FF0; (i+=37),j++)
    if (!(0x800<=i && i<0x880) && detectBuffer[j] != read(i)) // skip DPC port
      return true;
  return false;
}

/*
bool detectExtraRAM() {
  // if the first 128 bytes don't change between banks, they're probably RAM
  // TODO: smarter way to check!
  if (hotspots == hotspots_FA)
    return true;
  if (numHotspots == 0)
    return false;
  read(hotspots[0]);
  for (unsigned i=0; i<128; i++)
    detectBuffer[i] = read(i);
  for (unsigned j=1; j<numHotspots; j++) {
    read(hotspots[j]);
    for (unsigned i=0; i<128; i++)
      if (detectBuffer[i] != read(i))
        return false;
  }
}
*/

void dataPinState(gpio_pin_mode state) {
  for (unsigned i = 0 ; i < 8 ; i++)
    gpio_set_mode(dataPort[i], dataBit[i], state);  
}

bool detectWritePort(uint32_t address) { 
  uint8_t x;
  dataPinState(GPIO_INPUT_PU);
  x = read(address);
  dataPinState(GPIO_INPUT_FLOATING);
  if (x != 0xFF)
    return false;
  dataPinState(GPIO_INPUT_PD);
  x = read(address);
  dataPinState(GPIO_INPUT_FLOATING);
  return x == 0;
}

bool detectCartridge(void) {
  dataPinState(GPIO_INPUT_PU);
  uint16_t reset = read(0xFFC) | ((uint16_t)read(0xFFD)<<8);
  dataPinState(GPIO_INPUT_FLOATING);
  return reset != 0xFFFF;
}

bool check2k(void) {
  for (unsigned i = 0 ; i < 2048 ; i++)
    if (read(i) != read(i+2048))
      return false;
  return true;
}

uint8_t lfsr(uint8_t LFSR) {
  return ((LFSR << 1) | (~(((LFSR >> 7) ^ (LFSR >> 5)) ^ ((LFSR >> 4) ^ (LFSR >> 3))) & 1)) & 0xff;
}

bool detectDPC(void) {
  // detect the random number generator 
  read(0x70);
  uint8_t x = 0;
  for (int i=0; i<8; i++) {
    if (read(0) != x)
      return false;
    x = lfsr(x);
  }
  return true;
}

void identifyCartridge() {
  const char* msg = NULL;
  bool checkRAM = false;
  portStart = 0;
  portEnd = 0;
  port2Start = 0;
  port2End = 0;
  strcpy(info, "Cartridge type: ");
  
  if (detectDPC()) {
    mapper = MAPPER_DPC;
    strcpy(stellaExtension, ".dpc"); // check
    strcat(info + strlen(info), "F8+DPC");
    hotspots = hotspots_F8;
    numHotspots = sizeof(hotspots_F8)/sizeof(*hotspots_F8);
    portStart = 0;
    portEnd = 0x80;
    port2Start = 0x800;
    port2End = 0x880;
    romSize = 8192+2048;
  }
  else if (diff(0xF4,hotspots_F4,8,0,7)) {
    mapper = 0xF4;
    hotspots = hotspots_F4;
    numHotspots = 8;
    strcat(info, "F4");
    strcpy(stellaExtension, ".f4");
    romSize = numHotspots * 4096;
    checkRAM = true;
  }
  else if (diff(0xF6,hotspots_F6,4,0,1)) {
    mapper = 0xF6;
    hotspots = hotspots_F6;
    numHotspots = 4;
    strcpy(stellaExtension, ".f6");
    strcat(info, "F6");
    romSize = numHotspots * 4096;
    checkRAM = true;
  }
  else if (diff(0xFA,hotspots_FA,3,1,2)) {
    mapper = 0xFA;
    hotspots = hotspots_FA;
    numHotspots = 3;
    strcpy(stellaExtension, ".fa");
    strcat(info, "FA");
    romSize = numHotspots * 4096; 
    checkRAM = true;
  } 
  else if (diff(0xF8,hotspots_F8,2,0,1)) {
    mapper = 0xF8;
    hotspots = hotspots_F8;
    numHotspots = 2;
    strcpy(stellaExtension, ".f8");
    strcat(info, "F8");
    romSize = numHotspots * 4096;
    checkRAM = true;
  }
  else if (detectFE()) {
    mapper = 0xFE;
    hotspots = NULL;
    numHotspots = 0;
    strcpy(stellaExtension, ".fe");
    strcat(info, "FE");
    romSize = 8192;
    checkRAM = false;
  }
  else {
    mapper = 0;
    hotspots = NULL;
    numHotspots = 0;
    if (check2k()) {
      strcpy(stellaExtension, ".2k");
      strcat(info, "2K");
      romSize = 2048;      
    }
    else {
      strcpy(stellaExtension, ".4k");
      strcat(info, "4K");
      romSize = 4096;
    }
  }
  
  if (checkRAM) {
    if (detectWritePort(0)) {
      if (mapper == 0xFA) {
        portStart = 0;
        portEnd = 512;
      }
      else {
        // Super Chip
        portStart = 0;
        portEnd = 256;
        strcat(info, "SC");
        strcat(stellaExtension, "s");
      }
    }
  }
  
  lastBank = -1;
  gameNumber = -1;
  blankValue = 0;
  do 
  {
    crc = romCRC(0, romSize); 
    for (int i = 0 ; i < sizeof database / sizeof *database ; i++) {
      if (romSize == database[i].size && crc == database[i].crc) {
        gameNumber = i;
        break;
      }
    }

    if (gameNumber >= 0 || blankValue == 0xFF || portEnd <= portStart)
      break;
    blankValue = 0xFF;
  }
  while (1);
  sprintf(info+strlen(info), "\r\nSize: %u\nCRC-32: %08x\nGame: %s\n", 
    romSize,
    crc,
    gameNumber < 0 ? "unidentified" : database[gameNumber].name);
  if (gameNumber < 0) {
    strcpy(filename, "game.a26");
    strcpy(stellaFilename, "game");
  }
  else {
    strcpy(filename, database[gameNumber].name);
    strcat(filename, ".a26");
    strcpy(stellaFilename, database[gameNumber].name);
  }
  strcat(stellaFilename, stellaExtension);
  const char* p = stellaExtension;
  char* q = stellaShortName + 4;
  while (*p) {
    char c = *p++;
    *q++ = toupper(c);
  }
  *q = 0;
}

bool nullWrite(const uint8_t *writebuff, uint32_t memoryOffset, uint16_t transferLength) {
  return false;
}

void generateHTML(uint8_t* buf, uint32_t sector, uint32_t sectorCount) {
    uint32_t b64Size = BASE64_ENCSIZE(romSize);
    uint32_t destStart;
    uint32_t srcStart;
    uint32_t length;
    length = FAT16GetChunkCopyRange(0, sizeof(launch_htm_0)-1, sector, sectorCount, &destStart, &srcStart);
    if (length) memcpy(buf+destStart, launch_htm_0+srcStart, length);
    length = FAT16GetChunkCopyRange(sizeof(launch_htm_0)-1, b64Size, sector, sectorCount, &destStart, &srcStart);
    if (length) base64_encode((char*)buf+destStart, srcStart, length, bankedRead, romSize);
    length = FAT16GetChunkCopyRange(sizeof(launch_htm_0)-1+b64Size, sizeof(launch_htm_1)-1, sector, sectorCount, &destStart, &srcStart);
    if (length) memcpy(buf+destStart, launch_htm_1+srcStart, length);

}

bool fileReader(uint8_t *buf, const char* name, uint32_t sector, uint32_t sectorCount) {
  if (!strcmp(name,"GAME.A26") || !strcmp(name,stellaShortName)) {
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
  else if (!strcmp(name,"LAUNCH.HTM")) {
    generateHTML(buf, sector, sectorCount);
    return true;
  }
#ifdef DEBUG  
  else {
    unsigned hotspot;
    sscanf(name, "%x", &hotspot);
    read(hotspot);
    for (unsigned i = sector * FAT16_SECTOR_SIZE ; i < sector * FAT16_SECTOR_SIZE + FAT16_SECTOR_SIZE ; i++)
      buf[i - sector * FAT16_SECTOR_SIZE] = read(i);
    return true;
  }
#endif  
  return false;
}

void waitForCartridge() {
  while (true) {
    uint32_t start = millis();
    while (detectCartridge()) {
      if (millis() - start >= CARTRIDGE_KEEP_TIME_MILLIS)
        return;
      delay(2);
    }
  }
}

void setup() {
  for (unsigned i = 0 ; i < 13 ; i++) {
    addressPort[i] = PIN_MAP[addressPins[i]].gpio_device;
    addressBit[i] = PIN_MAP[addressPins[i]].gpio_bit;
    gpio_set_mode(addressPort[i], addressBit[i], GPIO_OUTPUT_PP);
  }
  for (unsigned i = 0 ; i < 8 ; i++) {
    dataPort[i] = PIN_MAP[dataPins[i]].gpio_device;
    dataBit[i] = PIN_MAP[dataPins[i]].gpio_bit;
  }
#ifdef USE_BITBAND  
  setBitbands();
#endif  
  dataPinState(GPIO_INPUT_FLOATING);
  pinMode(LED, OUTPUT);
  digitalWrite(LED,!LED_ON);
  waitForCartridge();
  identifyCartridge();
  digitalWrite(LED,LED_ON);

  FAT16SetRootDir(rootDir, sizeof(rootDir)/sizeof(*rootDir), fileReader);
  FAT16AddFile("LAUNCH.HTM", sizeof(launch_htm_0)-1+sizeof(launch_htm_1)-1+BASE64_ENCSIZE(romSize));
  FAT16AddFile("INFO.TXT", strlen(info)); // room for CRC-32 and crlf
  FAT16AddLFN("GAME.A26", filename);
  FAT16AddFile("GAME.A26", romSize);
  FAT16AddLFN(stellaShortName, stellaFilename);
  FAT16AddFile(stellaShortName, romSize);
#ifdef DEBUG
  for (unsigned i = 0 ; i < 8; i++) {
    char buf[5];
    sprintf(buf, "%04X", hotspots_F4[i]);
    FAT16AddFile(buf, 4096);
  }
#endif
  USBComposite.setProductId(PRODUCT_ID);
  MassStorage.setDriveData(0, FAT16_NUM_SECTORS, FAT16ReadSectors, nullWrite);
  MassStorage.registerComponent();

  USBComposite.begin();
  while(!USBComposite);  
}


void loop() {
  if (!detectCartridge())
    nvic_sys_reset();
  MassStorage.loop();
}



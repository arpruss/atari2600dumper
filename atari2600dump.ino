/*
 * You need 21 pins, 8 of them (data) 5V tolerant. The assignments below
 * are for an stm32f103c8t6 blue pill.
 *
 * View if you are facing the console:
 * View if you are facing the console:
 *             PB0  PB1  PB11 PB10 PC15 PB14 PB15 PA8  PA9  PA10
 * --------------------------------------------------------------
 * | GND  5V   A8   A9   A11  A10  A12  D7   D6   D5   D4   D3  |
 * |
 * | A7   A6   A5   A4   A3   A2   A1   A0   D0   D1   D2   GND |
 * --------------------------------------------------------------
 *   PA7  PA6  PA5  PA4  PA3  PA2  PA1  PA0  PB4  PB3  PA15
 */

// cartridge types theoretically supported: 2K, 4K, 3F, CV, E0, E7, F4, F6, F8, FA, FE, DPC, plus Super Chip variants
// classic types not supported: F0, UA
// tested: F8, DPC

#include <ctype.h>
#include <USBComposite.h>
#include "FAT16ReadOnly.h"
#include "base64.h"
#include "roms.h"
#include "dwt.h"

USBMassStorage MassStorage;
USBCompositeSerial CompositeSerial;

#define PRODUCT_ID 0x29

#define INPUTX INPUT

//#define DEBUG

const char inconsistent[] = "Inconsistent Read!";
char options[512];

#ifdef DEBUG
FAT16RootDirEntry rootDir[6+2*FAT16_NUM_ROOT_DIR_ENTRIES_FOR_LFN(LONGEST_FILENAME)+8+1];
#else
FAT16RootDirEntry rootDir[6+2*FAT16_NUM_ROOT_DIR_ENTRIES_FOR_LFN(LONGEST_FILENAME)+FAT16_NUM_ROOT_DIR_ENTRIES_FOR_LFN(sizeof(inconsistent)-1)];
#endif

#define NO_HOTPLUG 0
#define NO_STELLAEXT 0
bool hotplug = true;
bool stellaExt = true;
bool finicky = true;
uint32_t lastNoCartridgeTime; // last time we had no cartridge at all (not finicky about this)
char force[4] = "";
bool restart = false;
char filename[255];
char stellaFilename[255];
char stellaShortName[] = "GAME.XXX";
char info[FAT16_SECTOR_SIZE];
char label[12];
const char launch_htm_0[]="<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'><title>Javatari</title><meta name='description' content='Javatari - The online Atari 2600 emulator'></head>"
"<body><div id='javatari' style='text-align: center; margin: 20px auto 0; padding: 0 10px;'><div id='javatari-screen' style='box-shadow: 2px 2px 10px rgba(0, 0, 0, .7);'></div></div>"
"<script src='https://arpruss.github.io/javatari/javatari.js'></script><script>Javatari.CARTRIDGE_URL = 'data:application/octet-stream;base64,";
const char launch_htm_1[]="';Javatari.preLoadImagesAndStart();</script></body></html>\n";

#define CARTRIDGE_KEEP_TIME_MILLIS 2000 // cartridge must be kept in this long to register
#define FALLBACK_TO_NONFINICKY   10000 // after 10 sec, give up waiting for reliable reads and just assume it's an unreliable cartridge
#define LED    PC13
#define LED_ON 0

int gameNumber = -1;

const unsigned dataPins[8] = { PB3,PB4,PB10,PB11,PB12,PB13,PB14,PB15 };
const unsigned addressPins[13] = { PA0,PA1,PA2,PA3,PA4,PA5,PA6,PA7,PA8,PA9,PA10, PA15,PC15 };
//const unsigned dataBits[8] = {3,4,10,11,12,13,14,15};
gpio_reg_map* const mainAddressRegs = GPIOA_BASE;
const uint32_t mainAddressPortMask = 0b1000011111111111;
gpio_reg_map* const bit12AddressRegs = GPIOC_BASE;
const uint32_t bit12AddressPortMask = 0b1000000000000000;
gpio_reg_map* const dataRegs = GPIOB_BASE;
volatile uint32_t* const addressBit12Write = bb_perip( &(bit12AddressRegs->ODR), 15);
volatile uint32_t* const addressBit8Write = bb_perip( &(mainAddressRegs->ODR), 8);
const uint32_t dataCRLMask = 0xFFul << (3*4);
const uint32_t dataCRHMask = 0xFFFFFFul << 8;
const uint32_t dataPortMask = 0b1111110000011000;
#define DATA_CRL_MODE(mode) ( ((mode) << (3*4)) | ((mode) << (4*4)) )
#define DATA_CRH_MODE(mode) ( ((mode) << (2*4)) | ((mode) << (3*4)) | ((mode) << (4*4)) | ((mode) << (5*4)) | ((mode) << (6*4)) | ((mode) << (7*4)) )
#define SET_DATA_MODE(mode) ( ( dataRegs->CRL = (dataRegs->CRL & ~dataCRLMask) | DATA_CRL_MODE((mode)) ), \
                              ( dataRegs->CRH = (dataRegs->CRH & ~dataCRHMask) | DATA_CRH_MODE((mode)) ) )

uint32_t romSize;
uint32_t portStart;
uint32_t portEnd;
uint32_t port2Start;
uint32_t port2End;
char stellaExtension[5];
const uint32_t* hotspots = NULL;
#define MAPPER_DPC 0xD0
#define MAPPER_CV 0xC0

uint8_t mapper;
const uint32_t hotspots_F6[] = { 0xff6, 0xff7, 0xff8, 0xff9 };
const uint32_t hotspots_F8[] = { 0xff8, 0xff9 };
const uint32_t hotspots_F4[] = { 0xff4, 0xff5, 0xff6, 0xff7, 0xff8, 0xff9, 0xffa, 0xffb };
const uint32_t hotspots_FA[] = { 0xff8, 0xff9, 0xffa }; 
const uint32_t hotspotsE7Start = 0xfe0;
const uint32_t hotspotsE7End = 0xfeb;
const uint32_t hotspotsE0Start = 0xfe0;
const uint32_t hotspotsE0End = 0xfe7;
const uint32_t hotspotsE01800Start = 0x1ff0;
#define LEN(x) = (sizeof((x))/sizeof(*(x)))
uint32_t numHotspots = 0;
int lastBank = -1;
uint32_t crc;
uint8_t detectBuffer[128];
uint8_t blankValue;

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

void summarizeOptions(void) {
  strcpy(options,"Persistent:\r\n");
  if (hotplug) strcat(options, " hotplug\r\n"); else strcat(options, " no hotplug\r\n");
  if (stellaExt) strcat(options, " file with Stella extension\r\n"); else strcat(options, " no file with Stella extension\r\n");
  strcat(options,"\r\rNon-persistent:\r\n");
  if (force[0]) {
    strcat(options, " force: "); 
    strcat(options, force);
    strcat(options, "\r\n");
  }
  else {
    strcat(options, " autodetect\r\n");
  }
  strcat(options, "\r\nCommands available:\r\n"
    " command:reboot\r\n"
    " command:hotplug\r\n"
    " command:nohotplug\r\n"
    " command:force:XXX\r\n"
    "   XXX = 2k, 4k, 3f, cv, e0, e7, f4, f4s, f6, f6s, f8, f8s, fa, fe, dpc\r\n"
    " command:noforce\r\n"
    " command:stellaext\r\n"
    " command:nostellaext\r\n");
}

inline void setAddress(uint32_t address) {
    uint32_t mainAddressPortValue = (address & 0x7ff) | ( (address & 0x800) << (15-11));
  
    register uint32_t bsrr = (mainAddressPortValue) | ( ((~mainAddressPortValue) & mainAddressPortMask) << 16);
    uint32_t bit12AddressPortValue = (address & 0x1000) << (15-12);
    register uint32_t bsrr12 = (bit12AddressPortValue) | ( ((~bit12AddressPortValue) & bit12AddressPortMask) << 16);

    // *nearly* atomic address write; the order does matter in practice
    mainAddressRegs->BSRR = bsrr;
    bit12AddressRegs->BSRR = bsrr12;
}

inline uint8_t readDataByte() {
  uint32_t x = dataRegs->IDR;
  // pins: 3,4,10,11,12,13,14,15 -> bits 0-7
  return ((x>>3)&0b11)|( (x>>(10-2)) & 0b11111100);
}

// always sets bit 12 of address, so caller doesn't have to worry about it
uint8_t read(uint32_t address) {
  if (mapper == 0xFE) {
    // to prevent an accidental access to 0x01xx, we clear bit 8 early on
    *addressBit8Write = 0;
  }
  else if (mapper == 0x3F) {
    // to prevent an accidental access to 0x003F or below, we set bit 8 early on
    *addressBit8Write = 1;
  }
  *addressBit12Write = 0;
  DWTDelayMicroseconds(1); // I don't know if this delay is needed
  setAddress(address|0x1000);
  DWTDelayMicroseconds(1); 
  
  return readDataByte();
}


inline void dataWrite(uint8_t value) {
  uint32_t bsrr = ((value & 3) << 3) | ((value & ~3)<<8);
  dataRegs->BSRR = bsrr | ((~bsrr & dataPortMask) << 16);
}

// write but don't zero A12 first
uint8_t write(uint32_t address, uint8_t value, bool longDelay=false) {
  *addressBit12Write = 0;
  DWTDelayMicroseconds(1);  
  setAddress(address);
  SET_DATA_MODE(GPIO_OUTPUT_PP); // should be safe, since with A12=0, the cartridge should have let things float
  dataWrite(value);
  DWTDelayMicroseconds(3); // probably can be a lot shorter, but let's be super sure it registers
  if (longDelay)
    DWTDelayMicroseconds(3);
  SET_DATA_MODE(GPIO_INPUT_FLOATING);
}
void switchBankFE(uint8_t bank) {
    // two writes would be normal, but we just do a long write
    write(0x01FE, 0xF0^(bank<<5),true);
}

void switchBank3F(uint8_t bank) {
  write(0x003F, bank);
}

void switchBank(uint8_t bank) {
  if (mapper == 0xFE) {
    switchBankFE(bank);
  }
  else if (mapper == 0x3F) {
    switchBank3F(bank);
  }
  else if (numHotspots > 0) {
    read(hotspots[bank]);
    if (mapper == 0xFA) {
      pinMode(dataPins[0], INPUT_PULLUP); // D0 must be high to switch banks on FA: https://patents.google.com/patent/US4485457A/en
                  // but I don't know if making the pin high this late after the read is good enough!
      DWTDelayMicroseconds(4);
      pinMode(dataPins[0], INPUT);       
    }    
  }
}

// in theory, we could do sequential reads and it would be faster, but why bother?
uint8_t readDPCGraphics(uint16_t address) {
  write(0x1050, address & 0xFF);
  write(0x1058, address >> 8);
  return read(0x1008);
}

uint8_t readE0(uint32_t address) {
  uint8_t bank = address / 1024;
  if (bank != lastBank) {
    read(hotspotsE01800Start+bank);
    lastBank = bank;
  }
  return read(0x1800 + (address % 1024));
}

uint8_t readE7(uint32_t address) {
  if (address >= (romSize - 2048)) {
    uint32_t readAddress = address + 4096 - romSize;
    uint8_t x = read(readAddress);
    if (hotspotsE7Start <= readAddress && readAddress < hotspotsE7End)
      lastBank = -1;
    return x;
  }
  unsigned bank = address / 2048;
  uint8_t x;
  if (bank != lastBank) {
    if (romSize == 8*1024) {
      // 4 bank variation
        read(0xFE4+bank);
    }
    else if (romSize == 12*1024) {
      // 6 bank variation
      read(0xFE2+bank);      
    }
    else { //if (romSize == 16*1024)
      // 8 bank variation
      read(0xFE0+bank);
    }
  }
  lastBank = bank;
  return read(address % 2048);
}

uint32_t bankCRC[7];

// check if the data are all different from each other
bool distinct(const uint32_t* data, unsigned count) {
  for(unsigned i=1;i<count;i++)
    for(unsigned j=0;j<i;j++)
      if (data[i] == data[j])
        return false;
  return true;
}

bool detectE0() {
  read(hotspotsE01800Start);
  for (unsigned i=512,j=0;i<1024; (i+=7),j++)
    detectBuffer[j] = read(0x800+i);  
  read(hotspotsE01800Start+1);
  for (unsigned i=512,j=0;i<1024; (i+=7),j++)
    if (detectBuffer[j] != read(0x800+i))
      return false;
  return true;  
}

bool detectE7() {
  mapper = 0xE7;
  read(0xfe4);
  for (unsigned i=512,j=0;i<0x07F0; (i+=19),j++)
    detectBuffer[j] = read(i);  
  read(0xfe5);
  bool detected = false;
  for (unsigned i=512,j=0;i<0x07F0; (i+=19),j++)
    if (detectBuffer[j] != read(i)) {
      detected = true;
      break;
    }
  if (!detected)
    return false;

  // OK, so it's E7. Now we need to count the banks
  
  for (unsigned i=0; i<7; i++) {
    read(hotspotsE7Start+i);
    bankCRC[i] = unbankedCRC(512,1536);
  }

  if (distinct(bankCRC,7)) 
    romSize = 8*2048;
  else if (distinct(bankCRC+2,5)) 
    romSize = 6*2048;
  else 
    romSize = 4*2048;
  return true;
}

uint8_t bankedRead(uint32_t address) {
  if (mapper == 0x3F) {
    if (address >= 8192-2048) {
      // last half of memory is permanently mapped
      return read(address-4096);
    }
    else {
      int bank = address / 2048;
      switchBank(bank);
      return read(address % 2048);
    }
  }

  if (mapper == 0xE7) {
    return readE7(address);
  }
  
  if (mapper == 0xE0) {
    return readE0(address);
  }
  
  if (mapper == MAPPER_DPC && address >= romSize - 2048) {
    return readDPCGraphics(2047 - (address - (romSize-2048)));
  }

  if (mapper == MAPPER_CV) {
    return read(address-2048);
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

uint32_t bankedCRC(unsigned start, unsigned count) {
  uint32_t c = 0;
  uint32_t result = ~0;
  unsigned i = start;

  for (;  i < count ; i++) {
    result ^= bankedRead(i);
    result = crc32_for_byte(result);
  }

  return ~result;
}

uint32_t unbankedCRC(unsigned start, unsigned count) {
  uint32_t c = 0;
  uint32_t result = ~0;
  unsigned i = start;

  for (;  i < count ; i++) {
    result ^= read(i);
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
    if (detectBuffer[j] != read(i)) 
      return true;
  return false;
}

bool detect3F() {
  mapper = 0x3F;
  switchBank3F(0);
  for (unsigned i=512,j=0;i<0x07F0; (i+=19),j++)
    detectBuffer[j] = read(i);  
  switchBank3F(1);
  for (unsigned i=512,j=0;i<0x07F0; (i+=19),j++)
    if (detectBuffer[j] != read(i))
      return true;
  return false;
}

void dataPinState(WiringPinMode state) {
  for (unsigned i = 0 ; i < 8 ; i++)
    pinMode(dataPins[i], state);  
}

// checks if there is a write port in address:address+127
int detectWritePort(unsigned _address) {
  for (unsigned address = address; address < _address + 0x80; address++) {
    *addressBit12Write = 0; 
    delayMicroseconds(5); // now things should float
    dataPinState(INPUT_PULLDOWN); // hope this will make any subsequent floaty read go to zero
    delayMicroseconds(10);
    dataPinState(INPUTX);
    uint8_t x = read(address);
    if (x != 0xFF) {
      unsigned pin;      
      for (pin = 0 ; pin < 8 ; pin++)
        if ((x & (1<<pin)) == 0)
          break;
      pinMode(dataPins[pin], INPUT_PULLUP); // should be safe to pull up a pin that's zero
      uint8_t y = read(address);
      pinMode(dataPins[pin], INPUTX);
      return x != y;
    }
  }
  return -1;
}

bool detectSuperChip() {
  int detect = detectWritePort(0x1000);
  if (detect >= 0)
    return detect;

  // They're all 0xFF. It seems unlikely that a cartridge ROM would begin with a sequence of 
  // 0xFFs. So it's probably a superchip, with the stm reading all the floating values as high.
  // But we have no way to be sure, and it's better to just guess wrong---that way, we'll just
  // include some RAM in the dump, not a biggie.
  //
  // Ideally in this case we'd pull down a pin and see if that changes the read. But if we did
  // that, we'd violate STM's strictures against using internal pulldown/pullup with a 5V input.
  // 
  // I can't think of another safe test here, so let's just say it's not a superchip. 
  return false;
}

bool detectCV() {
  int detect = detectWritePort(0x1700);
  if (detect >= 0)
    return detect;
  return false;   
}

bool detectCartridge(bool finickyMode, uint32_t* valueP=NULL) {
  uint8_t resetLow = read(0x1FFC);
  uint8_t resetHigh = read(0x1FFD);
  if (resetLow == 0xFF && resetHigh == 0xFF) {
    lastNoCartridgeTime = millis();
    return false;
  }
  uint32_t address;
  uint8_t oldValue;
  if (resetLow != 0xFF) {
    address = 0x1FFC;
    oldValue = resetLow;
  }
  else {
    address = 0x1FFD;
    oldValue = resetHigh;
  }
  uint8_t pin;
  for (pin = 0 ; pin < 8 ; pin++)
    if ((oldValue & (1<<pin)) == 0) 
      break;
  // the bit is 0, so it should be safe to pull it up, unless bitrot changed it to 1 in the meanwhile
  pinMode(dataPins[pin], INPUT_PULLUP);
  uint8_t x = read(address);
  pinMode(dataPins[pin], INPUTX);
  // if successfully pulled up, no cartridge
  // if doesn't match old AND finicky, no cartridge
  if ((x & (1<<pin)) || (finicky && x != oldValue) ) {
    lastNoCartridgeTime = millis();
    return false;
  }
  if (valueP != NULL)
    *valueP = ((uint32_t)resetHigh<<8)|resetLow;
  return true;
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
  mapper = MAPPER_DPC;
  // detect the random number generator 
  *addressBit12Write = 0;
  read(0x70);
  uint8_t x = 0;
  for (int i=0; i<8; i++) {
    *addressBit12Write = 0;
    uint8_t y = read(0);
    if (y != x)
      return false;
    x = lfsr(x);
  }
  return true;
}

void identifyCartridge() {
  const char* msg = NULL;
  bool checkSuperChip = false;
  portStart = 0;
  portEnd = 0;
  port2Start = 0;
  port2End = 0;
  strcpy(info, "Cartridge type: ");
  
  if (!strcmp(force, "dpc") || (!force[0] && detectDPC())) {
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
  else if (!strcmp(force, "3f") || (!force[0] && detect3F())) {
    mapper = 0x3F;
    strcpy(stellaExtension, ".3f"); 
    strcat(info + strlen(info), "3F");
    hotspots = NULL;
    numHotspots = 0;
    romSize = 8192;
  }
  else if (!strcmp(force, "e0") || (force[0] && detectE0())) {
    mapper = 0xE0;
    strcpy(stellaExtension, ".e0");
    strcat(info + strlen(info), "E0");
    romSize = 8*1024;
    hotspots = NULL;
    numHotspots = 0;
    strcpy(stellaExtension, ".e7");
    strcat(info + strlen(info), "E7");
  }
  else if (!strncmp(force, "e7", 2)) {
    if (!strcmp(force, "e7_16k")) 
      romSize = 16*1024;
    else if (!strcmp(force, "e7_12k"))
      romSize = 12*1024;
    else if (!strcmp(force, "e7_8k"))
      romSize = 8*1024;
    else {
      if (!detectE7())
        romSize = 16*1024;
    }
    mapper = 0xE7;
    hotspots = NULL;
    numHotspots = 0;
    strcpy(stellaExtension, ".e7");
    strcat(info + strlen(info), "E7");
  }
  else if (!force[0] && detectE7()) { // will set romSize if detected
    mapper = 0xE7;
    hotspots = NULL;
    numHotspots = 0;
    strcpy(stellaExtension, ".e7");
    strcat(info + strlen(info), "E7");
  }
  else if (!strncmp(force, "f4", 2) || (!force[0] && diff(0xF4,hotspots_F4,8,0,7))) {
    mapper = 0xF4;
    hotspots = hotspots_F4;
    numHotspots = 8;
    strcat(info, "F4");
    strcpy(stellaExtension, ".f4");
    romSize = numHotspots * 4096;
    checkSuperChip = true;
  }
  else if (!strncmp(force, "f6", 2) || (!force[0] && diff(0xF6,hotspots_F6,4,0,1))) {
    mapper = 0xF6;
    hotspots = hotspots_F6;
    numHotspots = 4;
    strcpy(stellaExtension, ".f6");
    strcat(info, "F6");
    romSize = numHotspots * 4096;
    checkSuperChip = true;
  }
  else if (!strncmp(force, "fa", 2) || (!force[0] && diff(0xFA,hotspots_FA,3,1,2))) {
    mapper = 0xFA;
    hotspots = hotspots_FA;
    numHotspots = 3;
    strcpy(stellaExtension, ".fa");
    strcat(info, "FA");
    romSize = numHotspots * 4096; 
    portStart = 0;
    portEnd = 512;
  } 
  else if (!strncmp(force,"f8", 2) || (!force[0] && diff(0xF8,hotspots_F8,2,0,1))) {
    mapper = 0xF8;
    hotspots = hotspots_F8;
    numHotspots = 2;
    strcpy(stellaExtension, ".f8");
    strcat(info, "F8");
    romSize = numHotspots * 4096;
    checkSuperChip = true;
  }
  else if (!strncmp(force, "fe", 2) || (!force[0] && detectFE())) {
    mapper = 0xFE;
    hotspots = NULL;
    numHotspots = 0;
    strcpy(stellaExtension, ".fe");
    strcat(info, "FE");
    romSize = 8192;
    checkSuperChip = false;
  }
  else if (!strcmp(force, "cv") || (!force[0] && detectCV())) {
    mapper = MAPPER_CV;
    hotspots = NULL;
    numHotspots = 0;
    strcpy(stellaExtension, ".cv");
    strcat(info, "CV");
    romSize = 2048;
  }
  else {
    mapper = 0;
    hotspots = NULL;
    numHotspots = 0;
    if (!strcmp(force, "2k") || (!force[0] && check2k())) {
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
  
  if (checkSuperChip) {
    if ((force[0] && force[2]=='s') || (!force[0] && detectSuperChip())) {
      // Super Chip
      portStart = 0;
      portEnd = 256;
      strcat(info, "SC");
      strcat(stellaExtension, "s");
    }
  }
  
  lastBank = -1;
  gameNumber = -1;
  blankValue = 0;
  do 
  {
    crc = bankedCRC(0, romSize); 
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
  sprintf(info+strlen(info), "\r\nSize: %u\r\nCRC-32: %08x\r\nGame: %s\r\n", 
    romSize,
    crc,
    gameNumber < 0 ? "unidentified" : database[gameNumber].name);
  if (gameNumber < 0) {
    strcpy(filename, "game.a26");
    strcpy(stellaFilename, "game");
    strcpy(label, "2600 Cart");
  }
  else {
    strcpy((char*)detectBuffer, database[gameNumber].name);
    char* p = strstr((char*)detectBuffer, " (");
    if (p != NULL)
      *p = 0;
    strcpy(filename, (char*)detectBuffer);
    strcat(filename, ".a26");
    strcpy(stellaFilename, (char*)detectBuffer);
    unsigned i;
    for (i = 0 ; i < 11 && detectBuffer[i] && detectBuffer[i] != '('; i++) {
      label[i] = detectBuffer[i];
    }
    label[i] = 0;
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
  if (!strncmp((const char*)writebuff, "command:", 8)) {
    const char* p = (const char*)writebuff+8;
    if (!strncmp(p, "reboot", 6))
      nvic_sys_reset();
    else if (!strncmp(p, "hotplug", 7)) {
      hotplug = true;
      EEPROM8_storeValue(NO_HOTPLUG, 0);
    }
    else if (!strncmp(p, "nohotplug", 9)) {
      hotplug = false;
      EEPROM8_storeValue(NO_HOTPLUG, 1);
    }
    else if (!strncmp(p, "noforce", 7)) {
      force[0] = 0;
      restart = true;
    }
    else if (!strncmp(p, "force:", 6)) {
      p += 6;
      unsigned i = 0;
      while (*p && *p != '\r' && *p != '\n' && i<3)
        force[i++] = *p++;
      force[i] = 0;
      restart = true;
    }
    else if (!strncmp(p,"stellaext", 9)) {
      stellaExt = true;
      EEPROM8_storeValue(NO_STELLAEXT, 0);
      restart = true;
    }
    else if (!strncmp(p,"nostellaext", 9)) {
      stellaExt = false;
      EEPROM8_storeValue(NO_STELLAEXT, 1);
      restart = true;
    }
  }
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
  else if (!strcmp(name,"OPTIONS.TXT")) {
    strcpy((char*)buf, options);
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
    uint32_t value;
    uint32_t firstValue;
    if (detectCartridge(finicky, &firstValue)) {
      while (detectCartridge(finicky, &value) && (!finicky || value == firstValue)) {
        if (millis() - start >= CARTRIDGE_KEEP_TIME_MILLIS)
          return;
        delay(2);
      }
      if (millis() - lastNoCartridgeTime >= 8000) {
        // unreliable, but that may be the best we get
        return;
      }
    }
  }
}

void setup() {
  EEPROM8_init();
  hotplug = !EEPROM8_getValue(NO_HOTPLUG);
  stellaExt = !EEPROM8_getValue(NO_STELLAEXT);
  dataPinState(INPUTX);
  for (unsigned i = 0 ; i < 13 ; i++)
    pinMode(addressPins[i], OUTPUT);
  pinMode(LED, OUTPUT);

  bool firstTime = true;
  lastNoCartridgeTime = millis();
  
  finicky = hotplug;

  uint32_t newCRC;

  do {
    if (!firstTime) {
      delay(1000);
    }
    else {
      firstTime = false;
    }
    digitalWrite(LED,!LED_ON);
    if (hotplug)
      waitForCartridge();
    identifyCartridge();
  
    digitalWrite(LED,LED_ON);

    if (hotplug && !detectCartridge(false))
      continue;
    delay(200);
    newCRC = bankedCRC(0,romSize);
  } while(hotplug && crc != newCRC && millis() - lastNoCartridgeTime < FALLBACK_TO_NONFINICKY); 

  digitalWrite(LED,!LED_ON);

  FAT16SetRootDir(rootDir, sizeof(rootDir)/sizeof(*rootDir), fileReader);
  FAT16AddLabel(label);
  if (crc != newCRC) {
    finicky = false;
    FAT16AddLFN("INCONSIS.TXT", inconsistent);
    FAT16AddFile("INCONSIS.TXT", 0);
    strcat(info, "Inconsistent reading detected\r\n");
  }
  summarizeOptions();
  FAT16AddFile("OPTIONS.TXT", strlen(options));
  FAT16AddFile("LAUNCH.HTM", sizeof(launch_htm_0)-1+sizeof(launch_htm_1)-1+BASE64_ENCSIZE(romSize));
  FAT16AddFile("INFO.TXT", strlen(info)); // room for CRC-32 and crlf
  FAT16AddLFN("GAME.A26", filename);
  FAT16AddFile("GAME.A26", romSize);
  if (stellaExt) {
    FAT16AddLFN(stellaShortName, stellaFilename);
    FAT16AddFile(stellaShortName, romSize);
  }
#ifdef DEBUG
  for (unsigned i = 0 ; i < 8; i++) {
    char buf[5];
    sprintf(buf, "%04X", hotspots_F4[i]);
    FAT16AddFile(buf, 4096);
  }
#endif
  USBComposite.setProductId(PRODUCT_ID);
  USBComposite.clear();
  MassStorage.setDriveData(0, FAT16_NUM_SECTORS, FAT16ReadSectors, nullWrite);
  MassStorage.registerComponent();

  USBComposite.begin();
  while(!USBComposite);  

  digitalWrite(LED,LED_ON);
}


void loop() {
  if (restart || (hotplug && !detectCartridge(finicky))) {
    restart = false;
    digitalWrite(LED,!LED_ON);
    USBComposite.end();
    delay(500);
    setup();
//    nvic_sys_reset();
  }
  MassStorage.loop();
}




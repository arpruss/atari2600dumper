/*
 * You need 21 pins, 8 of them (data) 5V tolerant. The assignments below
 * are for an stm32f103c8t6 blue pill.
 * 
 * You need a 128kb flash version to have the full database of ROMs.
 *
 * View if you are facing the console:
 *             PA8  PA9  PA15 PA10 PC15 PB15 PB14 PB13 PB12 PB11
 * --------------------------------------------------------------
 * | GND  5V   A8   A9   A11  A10  A12  D7   D6   D5   D4   D3  |
 * |                                                            |
 * | A7   A6   A5   A4   A3   A2   A1   A0   D0   D1   D2   GND |
 * --------------------------------------------------------------
 *   PA7  PA6  PA5  PA4  PA3  PA2  PA1  PA0  PB3  PB4  PB10
 */

// cartridge types theoretically supported: 2K, 4K, 3F, CV, E0, E7, F4, F6, F8, FA, FE, UA, DPC, plus Super Chip variants of F4-F8
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
#undef SERIAL_TEST

#define INPUTX INPUT
#define LED    PC13
#define LED_ON 0

#define NO_HOTPLUG 0
#define NO_STELLAEXT 1
bool hotplug = true;
bool stellaExt = true;
bool finicky = true;
uint32_t lastNoCartridgeTime; // last time we had no cartridge at all (not finicky about this)
uint16_t force = 0;
bool restart = false;
char filename[255];
char stellaFilename[255];
const char inconsistent[] = "Inconsistent Read!";
char options[FAT16_SECTOR_SIZE];
char stellaShortName[] = "GAME.XXX";
char info[FAT16_SECTOR_SIZE];
char label[12];

FAT16RootDirEntry rootDir[6+2*FAT16_NUM_ROOT_DIR_ENTRIES_FOR_LFN(LONGEST_FILENAME)+FAT16_NUM_ROOT_DIR_ENTRIES_FOR_LFN(sizeof(inconsistent)-1)];

const char launch_htm_0[]="<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'><title>Javatari</title><meta name='description' content='Javatari - The online Atari 2600 emulator'></head>"
"<body><div id='javatari' style='text-align: center; margin: 20px auto 0; padding: 0 10px;'><div id='javatari-screen' style='box-shadow: 2px 2px 10px rgba(0, 0, 0, .7);'></div></div>"
"<script src='https://arpruss.github.io/javatari/javatari.js'></script><script>Javatari.CARTRIDGE_URL = 'data:application/octet-stream;base64,";
const char launch_htm_1[]="';Javatari.preLoadImagesAndStart();</script></body></html>\n";

#define CARTRIDGE_KEEP_TIME_MILLIS 2000 // cartridge must be kept in this long to register
#define FALLBACK_TO_NONFINICKY   10000 // after 10 sec, give up waiting for reliable reads and just assume it's an unreliable cartridge

int gameNumber = -1;

// GPIO settings. Note that if you change the pin numbers below, you will also need to change
// various masks and shifts, both here and in readDataByte(), setAddress(), dataWrite().

const unsigned dataPins[8] = { PB3,PB4,PB10,PB11,PB12,PB13,PB14,PB15 };
const unsigned addressPins[13] = { PA0,PA1,PA2,PA3,PA4,PA5,PA6,PA7,PA8,PA9,PA10, PA15,PC15 };
gpio_reg_map* const mainAddressRegs = GPIOA_BASE;
const uint32_t mainAddressPortMask = 0b1000011111111111;
gpio_reg_map* const bit12AddressRegs = GPIOC_BASE; // This is A12 = Chip Select on the cartridge
const uint32_t bit12AddressPortMask = 0b1000000000000000;
gpio_reg_map* const dataRegs = GPIOB_BASE;
volatile uint32_t* const addressBit12Write = bb_perip( &(bit12AddressRegs->ODR), 15);
volatile uint32_t* const addressBit9Write = bb_perip( &(mainAddressRegs->ODR), 9);
volatile uint32_t* const addressBit8Write = bb_perip( &(mainAddressRegs->ODR), 8);
const uint32_t dataCRLMask = 0xFFul << (3*4);
const uint32_t dataCRHMask = 0xFFFFFFul << 8;
const uint32_t dataPortMask = 0b1111110000011000;
#define DATA_CRL_MODE(mode) ( ((mode) << (3*4)) | ((mode) << (4*4)) )
#define DATA_CRH_MODE(mode) ( ((mode) << (2*4)) | ((mode) << (3*4)) | ((mode) << (4*4)) | ((mode) << (5*4)) | ((mode) << (6*4)) | ((mode) << (7*4)) )
#define SET_DATA_MODE(mode) ( ( dataRegs->CRL = (dataRegs->CRL & ~dataCRLMask) | DATA_CRL_MODE((mode)) ), \
                              ( dataRegs->CRH = (dataRegs->CRH & ~dataCRHMask) | DATA_CRH_MODE((mode)) ) )

#define SUPERCHIP 0x8000
#define MAPPER(type,size) ((type)|((uint16_t)(size)<<8))
#define MAPPER_DPC 0xD0
#define MAPPER_CV 0xC0
#define MAPPER_UA 0x0A
#define MAPPER_4K MAPPER(0,4)
#define MAPPER_2K MAPPER(0,2)

#if __has_include ("customisation.h")
#  include "customisation.h"
#endif

typedef struct {
  uint16_t id;
  char extension[4];
  const char* description;
  uint32_t romSize;
  uint32_t hotspotsStart;
  uint32_t hotspotsEnd;
  uint32_t portMask;
  uint32_t portStart;
  uint32_t portEnd;
  uint8_t (*read4K)(uint32_t address);
  void (*switchBank)(uint32_t bank);
  uint8_t (*bankedRead)(uint32_t address);
  uint16_t (*detect)(void);
} CartType;

void switchBankGeneric(uint32_t bank);
void switchBank3F(uint32_t bank);
void switchBankFA(uint32_t bank);
void switchBankFE(uint32_t bank);
void switchBankUA(uint32_t bank);
uint8_t readFE(uint32_t address);
uint8_t readUA(uint32_t address);
uint8_t read3F(uint32_t address);
uint8_t read(uint32_t address);
uint8_t bankedReadDPC(uint32_t address);
uint8_t bankedRead3F(uint32_t address);
uint8_t bankedReadE0(uint32_t address);
uint8_t bankedReadE7(uint32_t address);
uint8_t bankedReadGeneric(uint32_t address);
uint8_t bankedReadCV(uint32_t address);
uint16_t detectUA();
uint16_t detectDPC();
uint16_t detect3F();
uint16_t detectE0();
uint16_t detectE7();
uint16_t detectF4();
uint16_t detectF6();
uint16_t detectF8();
uint16_t detectFA();
uint16_t detectFE();
uint16_t detectCV();
uint16_t detect2K();

// detectors are called in this order
// a detector can also return the ID of a later carttype
const CartType cartTypes[] = {
  { MAPPER_DPC, "dpc", "DPC", 10240, 0xff8, 0xffa, 0x7FF, 0, 0x80, read, switchBankGeneric, bankedReadDPC, detectDPC },
  { MAPPER_CV, "cv", "CV", 2048, 0, 0, 0, 0, 0, read, NULL, bankedReadCV, detectCV },  
  { MAPPER(0xE7, 16), "e7", "E7 16K", 16*1024, 0xfe0, 0xfec, 0, 0, 0, read, NULL, bankedReadE7, detectE7 },
  { MAPPER(0xE7, 12), "e7", "E7 12K", 12*1024, 0xfe0, 0xfec, 0, 0, 0, read, NULL, bankedReadE7, NULL },
  { MAPPER(0xE7, 8), "e7", "E7 8K", 8*1024, 0xfe0, 0xfec, 0, 0, 0, read, NULL, bankedReadE7, NULL },
  { 0x3F, "3f", "3F", 8192, 0, 0, 0, 0, 0, read3F, switchBank3F, bankedRead3F, detect3F },
  { MAPPER_UA, "UA", "UA", 4096, 0, 0, 0, 0, 0, readUA, switchBankUA, bankedReadGeneric, detectUA },
  { 0xFE, "fe", "FE", 8*1024, 0, 0, 0, 0, 0, readFE, switchBankFE, bankedReadGeneric, detectFE },
  { 0xE0, "e0", "E0", 8192, 0xff0, 0xff8, 0, 0, 0, read, NULL, bankedReadE0, detectE0 },
  { 0xF4, "f4", "F4", 32*1024, 0xff4, 0xffc, 0, 0, 0, read, switchBankGeneric, bankedReadGeneric, detectF4 },
  { 0xF4|SUPERCHIP, "f4s", "F4SC", 32*1024, 0xff4, 0xffc, 0xFFF, 0, 0x100, read, switchBankGeneric, bankedReadGeneric, NULL },
  { 0xF6, "f6", "F6", 16*1024, 0xff6, 0xffa, 0, 0, 0, read, switchBankGeneric, bankedReadGeneric, detectF6 },
  { 0xF6|SUPERCHIP, "f6s", "F6SC", 16*1024, 0xff6, 0xffa, 0xFFF, 0, 0x100, read, switchBankGeneric, bankedReadGeneric, NULL },
  { 0xF8, "f8", "F8", 8*1024, 0xff8, 0xffa, 0, 0, 0, read, switchBankGeneric, bankedReadGeneric, detectF8 },
  { 0xF8|SUPERCHIP, "f8s", "F8SC", 8*1024, 0xff8, 0xffa, 0xFFF, 0, 0x100, read, switchBankGeneric, bankedReadGeneric, NULL },
  { 0xFA, "fa", "FA", 12*1024, 0xff8, 0xffb, 0xFFF, 0, 0x200, read, switchBankFA, bankedReadGeneric, detectFA },
  { MAPPER_2K, "2k", "2K", 2048, 0, 0, 0, 0, 0, read, NULL, read, detect2K },
  { MAPPER_4K, "4k", "4K", 4096, 0, 0, 0, 0, 0, read, NULL, read, NULL },
};

const CartType* cart;

#define LEN(x) (sizeof((x))/sizeof(*(x)))
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

void summarizeOptions(void) {
  strcpy(options,"Persistent:\r\n");
  if (hotplug) strcat(options, " hotplug\r\n"); else strcat(options, " no hotplug\r\n");
  if (stellaExt) strcat(options, " file with Stella extension\r\n"); else strcat(options, " no file with Stella extension\r\n");
  strcat(options,"\r\rNon-persistent:\r\n");
  if (force) {
    strcat(options, " force: ");
    strcat(options, findCartType(force)->description);
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
    "   XXX = 2k, 4k, 3f, cv, e0, e7_8k, e7_12k, e7_16k, f4, f4s, f6, f6s, f8, f8s, fa, fe, ua, dpc\r\n"
    " command:noforce\r\n"
    " command:stellaext\r\n"
    " command:nostellaext\r\n");
}

inline void setAddress(uint32_t address) {
//const unsigned addressPins[13] = { PA0,PA1,PA2,PA3,PA4,PA5,PA6,PA7,PA8,PA9,PA10, PA15,PC15 };
// Address pins A0-A10 on the cart are connected to PA0-PA10 on the pill, so they can be put into the
// port as-is. But address pin A11 is connected to PA15, so it needs to be shifted 4 bites left.
    uint32_t mainAddressPortValue = (address & 0x7ff) | ( (address & 0x800) << (15-11));
  
    register uint32_t bsrr = (mainAddressPortValue) | ( ((~mainAddressPortValue) & mainAddressPortMask) << 16);

// The A12 (=Chip Select) line is connected to PC15, so we extract it from the address and shift it left 3 bits    
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

inline void dataWrite(uint8_t value) {
  // put bits 0 and 1 into pins 3 and 4, and put bits 2-7 into pins 10-15
  uint32_t bsrr = ((value & 3) << 3) | ((value & ~3)<<8);
  dataRegs->BSRR = bsrr | ((~bsrr & dataPortMask) << 16);
}

// always sets bit 12 of address, so caller doesn't have to worry about it
uint8_t read(uint32_t address) {
  *addressBit12Write = 0;
  DWTDelayMicroseconds(1); // I don't know if this delay is needed
  setAddress(address|0x1000);
  DWTDelayMicroseconds(1); 
  
  return readDataByte();
}

// leave bit 12 of address as is
uint8_t read0UA(uint32_t address) {
  // to prevent an accidental access to 0x0220 or 0x0240, we clear bit 9 early on
  *addressBit9Write = 0;
  *addressBit12Write = 0;
  DWTDelayMicroseconds(1); // I don't know if this delay is needed
  setAddress(address);
  DWTDelayMicroseconds(1); 
  
  return readDataByte();
}

uint8_t readFE(uint32_t address) {
  // to prevent an accidental access to 0x01xx, we clear bit 8 early on
  *addressBit8Write = 0;
  return read(address);
}

uint8_t read3F(uint32_t address) {
  // to prevent an accidental access to 0x003F or below, we set bit 8 early on
  *addressBit8Write = 1;
  return read(address);
}

uint8_t readUA(uint32_t address) {
  // to prevent an accidental access to 0x0220 or 0x0240, we clear bit 9 early on
  *addressBit9Write = 0;
  return read(address);
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

void switchBankFE(uint32_t bank) {
    // two writes would be normal, but we just do a long write
    write(0x01FE, 0xF0^(bank<<5),true);
}

void switchBankUA(uint32_t bank) {
    if (bank == 0)
      read0UA(0x220);
    else
      read0UA(0x240);
}

void switchBank3F(uint32_t bank) {
  write(0x003F, bank);
}

void switchBankFA(uint32_t bank) {
  read(cart->hotspotsStart+bank);
  pinMode(dataPins[0], INPUT_PULLUP); // D0 must be high to switch banks on FA: https://patents.google.com/patent/US4485457A/en
              // but I don't know if making the pin high this late after the read is good enough!
  DWTDelayMicroseconds(4);
  pinMode(dataPins[0], INPUT);       
}

void switchBankGeneric(uint32_t bank) {
    cart->read4K(cart->hotspotsStart+bank);  
}

// in theory, we could do sequential reads and it would be faster, but why bother?
uint8_t readDPCGraphics(uint16_t address) {
  write(0x1050, address & 0xFF);
  write(0x1058, address >> 8);
  return read(0x1008);
}

uint8_t bankedReadE0(uint32_t address) {
  uint8_t bank = address / 1024;
  if (bank != lastBank) {
    read(cart->hotspotsStart+bank);
    lastBank = bank;
  }
  return read(0x1800 + (address % 1024));
}

uint8_t bankedReadE7(uint32_t address) {
  if (address >= (cart->romSize - 2048)) {
    uint32_t readAddress = address + 4096 - cart->romSize;
    uint8_t x = read(readAddress);
    if (cart->hotspotsStart <= readAddress && readAddress < cart->hotspotsEnd)
      lastBank = -1;
    return x;
  }
  unsigned bank = address / 2048;
  uint8_t x;
  if (bank != lastBank) {
    if (cart->romSize == 8*1024) {
      // 4 bank variation
        read(0xFE4+bank);
    }
    else if (cart->romSize == 12*1024) {
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

uint16_t detectE0() {
  read(cart->hotspotsStart);
  for (unsigned i=512,j=0;i<1024; (i+=7),j++)
    detectBuffer[j] = read(0x800+i);  
  read(cart->hotspotsStart+1);
  for (unsigned i=512,j=0;i<1024; (i+=7),j++)
    if (detectBuffer[j] != read(0x800+i))
      return 0xE0;
  return 0;  
}

uint16_t detectUA() {
  switchBankUA(0);
  for (unsigned i=512,j=0;i<0x07F0; (i+=19),j++)
    detectBuffer[j] = readUA(i);  
  switchBankUA(1);
  for (unsigned i=512,j=0;i<0x07F0; (i+=19),j++)
    if (detectBuffer[j] != readUA(i)) {
      return MAPPER_UA;
    }

  return 0;
}

uint16_t detectE7() {
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
    return 0;

  // OK, so it's E7. Now we need to count the banks
  
  for (unsigned i=0; i<7; i++) {
    read(cart->hotspotsStart+i);
    bankCRC[i] = unbankedCRC(512,1536);
  }

  if (distinct(bankCRC,7)) {
    return MAPPER(0xE7,16);
  }
  else if (distinct(bankCRC+2,5)) {
    return MAPPER(0xE7,12);
  }
  else {
    return MAPPER(0xE7,8);
  }
}

uint8_t bankedReadDPC(uint32_t address) {
  if (address >= cart->romSize - 2048) {
    return readDPCGraphics(2047 - (address - (cart->romSize-2048)));
  }
  return bankedReadGeneric(address);
}

uint8_t bankedReadGeneric(uint32_t address) {
  uint32_t maskedAddress = (address & cart->portMask);
  if (cart->portStart <= maskedAddress && maskedAddress < cart->portEnd)
    return blankValue;
  if (cart->romSize <= 4096)
    return cart->read4K(address);
  uint32_t bank = address / 4096;
  if (bank != lastBank) {
    cart->switchBank(bank);
    lastBank = bank;
  }
  uint32_t address4k = address % 4096;
  uint8_t value = cart->read4K(address4k);
  // check if what we read is one of the hotspots, so that
  // we might have unintentionally swapped banks
  if (cart->hotspotsStart <= address4k && address4k < cart->hotspotsEnd)
    lastBank = -1;
  return value;
}

uint8_t bankedRead3F(uint32_t address) {
  if (address >= 8192-2048) {
      // last half of memory is permanently mapped
      return read3F(address-4096);
  }
  else {
      uint32_t bank = address / 2048;
      switchBank3F(bank);
      return read3F(address % 2048);
  }
}

uint8_t bankedReadCV(uint32_t address) {
  return read(address-2048);
}

uint32_t bankedCRC(unsigned start, unsigned count) {
  uint32_t c = 0;
  uint32_t result = ~0;
  unsigned i = start;

  for (;  i < count ; i++) {
    result ^= cart->bankedRead(i);
    result = crc32_for_byte(result);
  }

  return ~result;
}

uint32_t unbankedCRC(unsigned start, unsigned count) {
  uint32_t c = 0;
  uint32_t result = ~0;
  unsigned i = start;

  for (;  i < count ; i++) {
    result ^= cart->read4K(i);
    result = crc32_for_byte(result);
  }

  return ~result;
}

const CartType* findCartType(uint16_t id) {
  for (uint32_t i = 0 ; i < LEN(cartTypes) ; i++) {
    if (cartTypes[i].id == id) {
      return cartTypes + i;
    }
  }
  return cartTypes + LEN(cartTypes) - 1;
}

void setCartType(uint16_t id) {
  cart = findCartType(id);
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

uint16_t detectCV() {
  int detect = detectWritePort(0x1700);
  if (detect > 0)
    return MAPPER_CV;
  return 0;
}

uint16_t detect2K(void) {
  for (unsigned i = 0 ; i < 2048 ; i++)
    if (read(i) != read(i+2048))
      return MAPPER_4K;
  return MAPPER_2K;
}

uint8_t lfsr(uint8_t LFSR) {
  return ((LFSR << 1) | (~(((LFSR >> 7) ^ (LFSR >> 5)) ^ ((LFSR >> 4) ^ (LFSR >> 3))) & 1)) & 0xff;
}

uint16_t detectDPC(void) {
  // detect the random number generator 
  *addressBit12Write = 0;
  read(0x70);
  uint8_t x = 0;
  for (int i=0; i<8; i++) {
    *addressBit12Write = 0;
    uint8_t y = read(0);
    if (y != x)
      return 0;
    x = lfsr(x);
  }
  return MAPPER_DPC;
}

bool diff(uint32_t b1, uint32_t b2) {
  // sample memory space every 37 bytes, skipping a potential RAM space at the beginning; this is nearly certain to pick up a difference 
  // between banks if there is a difference
  // detectBuffer is large enough for this sampling
  cart->switchBank(b1);
  for (unsigned i=512,j=0;i<0x0FF0; (i+=37),j++)
    detectBuffer[j] = read(i);
  cart->switchBank(b2);
  for (unsigned i=512,j=0;i<0x0FF0; (i+=37),j++)
    if (!(0x800<=i && i<0x880) && detectBuffer[j] != read(i)) // skip DPC port
      return true;
  return false;
}

uint16_t detect3F() {
  switchBank3F(0);
  for (unsigned i=512,j=0;i<0x07F0; (i+=19),j++)
    detectBuffer[j] = read3F(i);  
  switchBank3F(1);
  for (unsigned i=512,j=0;i<0x07F0; (i+=19),j++)
    if (detectBuffer[j] != read3F(i))
      return 0x3F;
  return 0;
}

uint16_t detectFE() {
  switchBankFE(0);
  for (unsigned i=512,j=0;i<0x0FF0; (i+=37),j++)
    detectBuffer[j] = cart->read4K(i);  
  switchBankFE(1);
  for (unsigned i=512,j=0;i<0x0FF0; (i+=37),j++)
    if (detectBuffer[j] != cart->read4K(i)) 
      return 0xFE;
  return 0;
}

uint16_t detectF4() {
  if (! diff(0,7))
    return 0;
  if (detectSuperChip())
    return SUPERCHIP | 0xF4;
  else
    return 0xF4;
}

uint16_t detectF6() {
  if (! diff(0,1))
    return 0;
  if (detectSuperChip())
    return SUPERCHIP | 0xF6;
  else
    return 0xF6;
}

uint16_t detectF8() {
  if (! diff(0,1))
    return 0;
  if (detectSuperChip())
    return SUPERCHIP | 0xF8;
  else
    return 0xF8;
}

uint16_t detectFA() {
  if (detectWritePort(0x1000)<1)
    return 0;
  // todo: check write port without crashing
  if (! diff(1,2))
    return 0;
  return 0xFA;
}


void identifyCartridge() {
  const char* msg = NULL;
  strcpy(info, "Cartridge type: ");

  if (force) 
    setCartType(force); 
  else {
    unsigned i;
    for (i=0; i<LEN(cartTypes); i++)
      if (NULL != cartTypes[i].detect) {
        cart = cartTypes+i;
        uint16_t id = cart->detect();
        if (id != 0) {
          setCartType(id);
          break;
        }
      }
    if (i >= LEN(cartTypes))
      setCartType(MAPPER_4K);
  }

  strcat(info, cart->description);
  
  lastBank = -1;
  gameNumber = -1;
  blankValue = 0;
  do 
  {
    crc = bankedCRC(0, cart->romSize); 
    for (int i = 0 ; i < sizeof database / sizeof *database ; i++) {
      if (cart->romSize == database[i].size && crc == database[i].crc) {
        gameNumber = i;
        break;
      }
    }

    if (gameNumber >= 0 || blankValue == 0xFF || cart->portEnd <= cart->portStart)
      break;
    blankValue = 0xFF;
  }
  while (1);

  sprintf(info+strlen(info), "\r\nSize: %u\r\nCRC-32: %08x\r\nGame: %s\r\n", 
    cart->romSize,
    crc,
    gameNumber < 0 ? "unidentified" : database[gameNumber].name);

  if (gameNumber < 0) {
    strcpy(filename, "game.a26");
    strcpy(stellaFilename, "game");
    strcpy(label, "2600 Cart");
  }
  else {
    strncpy((char*)detectBuffer, database[gameNumber].name,sizeof(detectBuffer));
    detectBuffer[sizeof(detectBuffer)-1] = 0;
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
  strcat(stellaFilename, ".");
  strcat(stellaFilename, cart->extension);
  const char* p = cart->extension;
  char* q = stellaShortName - 3;
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
      force = 0;
      restart = true;
    }
    else if (!strncmp(p, "force:", 6)) {
      p += 6;
      force = 0;
      if (!strncmp(p, "e7_16k", 6))
        force = MAPPER(0xE7, 16);
      else if (!strncmp(p, "e7_12k", 6))
        force = MAPPER(0xE7, 12);
      else if (!strncmp(p, "e7_8k", 5))
        force = MAPPER(0xE7, 8);
      else {
        char buf[8];
        unsigned i = 0;
        while (*p && *p != '\r' && *p != '\n' && i<7)
          buf[i++] = *p++;
        buf[i] = 0;
        for (i=0; i<LEN(cartTypes); i++) {
          if (!strcmp(buf, cartTypes[i].extension)) {
            force = cartTypes[i].id;
            break;
          }
        }
      }
      if (force)
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
    uint32_t b64Size = BASE64_ENCSIZE(cart->romSize);
    uint32_t destStart;
    uint32_t srcStart;
    uint32_t length;
    length = FAT16GetChunkCopyRange(0, sizeof(launch_htm_0)-1, sector, sectorCount, &destStart, &srcStart);
    if (length) memcpy(buf+destStart, launch_htm_0+srcStart, length);
    length = FAT16GetChunkCopyRange(sizeof(launch_htm_0)-1, b64Size, sector, sectorCount, &destStart, &srcStart);
    if (length) base64_encode((char*)buf+destStart, srcStart, length, cart->bankedRead, cart->romSize);
    length = FAT16GetChunkCopyRange(sizeof(launch_htm_0)-1+b64Size, sizeof(launch_htm_1)-1, sector, sectorCount, &destStart, &srcStart);
    if (length) memcpy(buf+destStart, launch_htm_1+srcStart, length);
}

bool fileReader(uint8_t *buf, const char* name, uint32_t sector, uint32_t sectorCount) {
  if (!strcmp(name,"GAME.A26") || !strcmp(name,stellaShortName)) {
    uint32_t size = sectorCount * FAT16_SECTOR_SIZE;
    uint32_t start = sector * FAT16_SECTOR_SIZE;
    for (unsigned i = 0 ; i < size ; i++)
      buf[i] = cart->bankedRead(start + i);
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

#ifdef SERIAL_TEST
void setup() {
  DWTInitTimer();
  dataPinState(INPUTX);
  for (unsigned i = 0 ; i < 13 ; i++)
    pinMode(addressPins[i], OUTPUT);
  pinMode(LED, OUTPUT);
  digitalWrite(LED, !LED_ON);
  Serial.begin();
  while(!Serial);
  delay(4000);
  digitalWrite(LED, LED_ON);

  for (unsigned i = 0 ; i < 4096 ; i += 16) {
    char buf[16];
    sprintf(buf, "%04x", i);
    Serial.print(buf);
    for (unsigned j = 0 ; j < 16 ; j++) {
      sprintf(buf, " %02x", read(i+j));
      Serial.print(buf);
    }
    Serial.println("");
  }
}

void loop() {
}

#else
void setup() {
  DWTInitTimer();
  EEPROM8_init(128);
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
    newCRC = bankedCRC(0,cart->romSize);
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
  FAT16AddFile("LAUNCH.HTM", sizeof(launch_htm_0)-1+sizeof(launch_htm_1)-1+BASE64_ENCSIZE(cart->romSize));
  FAT16AddFile("INFO.TXT", strlen(info)); // room for CRC-32 and crlf
  FAT16AddLFN("GAME.A26", filename);
  FAT16AddFile("GAME.A26", cart->romSize);
  if (stellaExt) {
    FAT16AddLFN(stellaShortName, stellaFilename);
    FAT16AddFile(stellaShortName, cart->romSize);
  }
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
#endif

#include <stdint.h>
#include <stdbool.h>

#include "em_device.h"
#include "em_msc.h"

#define WORD_SIZE   4UL
#define WORD_MASK   0x3UL


void init_flash(void)
{
  MSC_Init();
  MSC_ExecConfig_TypeDef execConfig = MSC_EXECCONFIG_DEFAULT;
  MSC_ExecConfigSet(&execConfig);
}

bool ulogger_nv_mem_erase(uint32_t address, uint32_t size)
{
  uint32_t erase_top = address + size;
  while (address < erase_top) {
      MSC_ErasePage((uint32_t *)address);
      address += FLASH_PAGE_SIZE;
  }
  return true;
}

bool ulogger_nv_mem_read(uint32_t address, uint8_t *data, uint32_t size)
{
  for (uint32_t i = 0; i < size; i++)
  {
      data[i] = *(uint8_t *)(address + i);
  }
  return true;
}

bool ulogger_nv_mem_write(uint32_t address, const uint8_t *data, uint32_t size)
{
  uint8_t i;
  uint8_t byte_offset;
  uint32_t word_data = 0xFFFFFFFF;
  MSC_Status_TypeDef result;

  byte_offset = address % WORD_SIZE;
  if (byte_offset) {
    // The first word will be written by the word write.
    // Start address is not word aligned, prepend leading bytes
    for (i=0; i < WORD_SIZE-byte_offset; i++) {
      word_data >>= 8;
      word_data |= (i < size) ? ((*(data++)) << 24) : (0xFF << 24);
    }
    result = MSC_WriteWord((uint32_t *)(address & ~WORD_MASK), (uint8_t *)&word_data, WORD_SIZE);
    if (result != mscReturnOk) {
      return false;
    }
    //Adjust the byte len, handle writes less than 1 WORD_SIZE
    size = size < (WORD_SIZE - byte_offset) ? 0 : size - (WORD_SIZE - byte_offset);
    address = (address & ~WORD_MASK) + WORD_SIZE;
  }

  byte_offset = size % WORD_SIZE;
  if (byte_offset) {
    // End address is not word aligned, append bytes
    word_data = 0xFFFFFFFF;

    for (i=0; i < byte_offset; i++) {
      word_data <<= 8;
      word_data |= (*(data+size-1-i));
    }

    result = MSC_WriteWord((uint32_t *)((address + size) & ~WORD_MASK), (uint8_t *)&word_data, WORD_SIZE);
    if (result != mscReturnOk) {
      return false;
    }
    size -= byte_offset;
  }

  // Write the full words
  if (size) {
    result = MSC_WriteWord((uint32_t *)address, data, size);
    if (result != mscReturnOk) {
      return false;
    }
  }
  return true;
}

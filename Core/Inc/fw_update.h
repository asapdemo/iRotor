#ifndef __FW_UPDATE_H
#define __FW_UPDATE_H

#include "main.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FW_UPDATE_FLASH_BASE 0x08000000UL
#define FW_UPDATE_FLASH_SIZE (63UL * 1024UL)
#define FW_UPDATE_FLASH_PAGE_SIZE 1024UL
#define FW_UPDATE_CHUNK_SIZE FW_UPDATE_FLASH_PAGE_SIZE

void FW_UpdateInit(SPI_HandleTypeDef *spi);
void FW_UpdateRequestRamServer(void);
uint8_t FW_UpdateIsRamServerRequested(void);
void FW_UpdateEnterRamServer(void);

#ifdef __cplusplus
}
#endif

#endif /* __FW_UPDATE_H */

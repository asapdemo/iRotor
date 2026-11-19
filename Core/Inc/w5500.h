#ifndef __W5500_H
#define __W5500_H

#include "main.h"

void W5500_Init(SPI_HandleTypeDef *spi, UART_HandleTypeDef *uart);
uint8_t W5500_IsInitialized(void);
uint8_t W5500_IsLinkUp(void);
void W5500_SendStatus(void);
uint8_t W5500_RunDhcp(void);
void W5500_SendIpInfo(void);
void W5500_ApplySettingsNetwork(void);
void W5500_WebService(void);

#endif /* __W5500_H */

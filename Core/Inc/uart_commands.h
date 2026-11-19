#ifndef __UART_COMMANDS_H
#define __UART_COMMANDS_H

#include "main.h"

void UART_CommandInit(UART_HandleTypeDef *uart);
void UART_CommandStartReceive(void);
void UART_CommandProcess(void);
void UART_CommandRxCpltCallback(UART_HandleTypeDef *huart);

#endif /* __UART_COMMANDS_H */

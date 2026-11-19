#include "fw_update.h"
#include "settings_service.h"
#include "web_pages.h"
#include "w5500.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct
{
  uint8_t initialized;
  uint8_t version;
  uint8_t phycfgr;
  uint8_t mode;
  HAL_StatusTypeDef last_spi_status;
} W5500_Status_t;

typedef struct
{
  uint8_t success;
  uint8_t ip[4];
  uint8_t mask[4];
  uint8_t gateway[4];
  uint8_t dns[4];
  uint8_t server[4];
  char message[32];
} DHCP_Result_t;

#define W5500_SPI_TIMEOUT_MS 100U
#define W5500_REG_MR 0x0000U
#define W5500_REG_PHYCFGR 0x002EU
#define W5500_REG_VERSIONR 0x0039U
#define W5500_COMMON_READ 0x00U
#define W5500_COMMON_WRITE 0x04U
#define W5500_VERSION_VALUE 0x04U
#define W5500_PHYCFGR_LINK 0x01U
#define W5500_PHYCFGR_SPEED 0x02U
#define W5500_PHYCFGR_DUPLEX 0x04U
#define W5500_DHCP_SOCKET 0U
#define W5500_DHCP_CLIENT_PORT 68U
#define W5500_DHCP_SERVER_PORT 67U
#define W5500_WEB_SOCKET 1U
#define W5500_WEB_PORT 80U
#define W5500_API_SOCKET 2U
#define W5500_API_LISTEN_SOCKET 3U
#define W5500_API_REQUEST_SIZE 192U
#define W5500_WEB_REQUEST_SIZE 1536U      /* modern browsers send 700-900 bytes of headers */
#define W5500_WEB_IDLE_TIMEOUT_MS 3000U   /* drop a connection that sends no complete request */
#define W5500_WEB_SEND_CHUNK_SIZE 1024U
#define W5500_SOCKET0_REG_BLOCK 0x08U
#define W5500_SOCKET0_TX_BLOCK 0x10U
#define W5500_SOCKET0_RX_BLOCK 0x18U
#define W5500_SOCKET_BUFFER_SIZE 2048U
#define W5500_SOCKET_BUFFER_MASK (W5500_SOCKET_BUFFER_SIZE - 1U)
#define W5500_SN_MR 0x0000U
#define W5500_SN_CR 0x0001U
#define W5500_SN_IR 0x0002U
#define W5500_SN_SR 0x0003U
#define W5500_SN_PORT 0x0004U
#define W5500_SN_DIPR 0x000CU
#define W5500_SN_DPORT 0x0010U
#define W5500_SN_TX_FSR 0x0020U
#define W5500_SN_TX_WR 0x0024U
#define W5500_SN_RX_RSR 0x0026U
#define W5500_SN_RX_RD 0x0028U
#define W5500_SN_MR_TCP 0x01U
#define W5500_SN_MR_UDP 0x02U
#define W5500_SN_CR_OPEN 0x01U
#define W5500_SN_CR_LISTEN 0x02U
#define W5500_SN_CR_DISCON 0x08U
#define W5500_SN_CR_CLOSE 0x10U
#define W5500_SN_CR_SEND 0x20U
#define W5500_SN_CR_RECV 0x40U
#define W5500_SN_IR_SENDOK 0x10U
#define W5500_SN_IR_TIMEOUT 0x08U
#define W5500_SN_SR_CLOSED 0x00U
#define W5500_SN_SR_INIT 0x13U
#define W5500_SN_SR_LISTEN 0x14U
#define W5500_SN_SR_ESTABLISHED 0x17U
#define W5500_SN_SR_CLOSE_WAIT 0x1CU
#define W5500_SN_SR_UDP 0x22U
#define W5500_REG_GAR 0x0001U
#define W5500_REG_SUBR 0x0005U
#define W5500_REG_SHAR 0x0009U
#define W5500_REG_SIPR 0x000FU
#define DHCP_PACKET_SIZE 548U
#define DHCP_OPTIONS_OFFSET 240U
#define DHCP_MAGIC_COOKIE_OFFSET 236U
#define DHCP_TIMEOUT_MS 6000U

static SPI_HandleTypeDef *w5500_spi;
static UART_HandleTypeDef *w5500_uart;
static W5500_Status_t w5500_status;

#define GPIO_SetPin(port, pin) ((port)->BSRR = (pin))
#define GPIO_ResetPin(port, pin) ((port)->BRR = (pin))
#define GPIO_ReadPin(port, pin) ((((port)->IDR & (pin)) != 0U) ? 1U : 0U)
#define W5500_Select() GPIO_ResetPin(W5500_CS_GPIO_Port, W5500_CS_Pin)
#define W5500_Unselect() GPIO_SetPin(W5500_CS_GPIO_Port, W5500_CS_Pin)
#define W5500_SpiEnable() (w5500_spi->Instance->CR1 |= SPI_CR1_SPE)


void UART1_SendString(const char *text)
{
  if ((w5500_uart == NULL) || (text == NULL))
  {
    return;
  }

  HAL_UART_Transmit(w5500_uart, (uint8_t *)text, (uint16_t)strlen(text), HAL_MAX_DELAY);
}

static void UART1_SendFormatted(const char *format, ...)
{
  char response[192];
  va_list args;
  int length;

  if ((w5500_uart == NULL) || (format == NULL))
  {
    return;
  }

  va_start(args, format);
  length = vsnprintf(response, sizeof(response), format, args);
  va_end(args);
  if (length > 0)
  {
    if (length >= (int)sizeof(response))
    {
      length = (int)sizeof(response) - 1;
    }
    HAL_UART_Transmit(w5500_uart, (uint8_t *)response, (uint16_t)length, HAL_MAX_DELAY);
  }
}

static HAL_StatusTypeDef W5500_SpiWaitFlag(uint32_t flag, uint8_t flag_state)
{
  uint32_t start = HAL_GetTick();

  while (((w5500_spi->Instance->SR & flag) != 0U) != (flag_state != 0U))
  {
    if ((HAL_GetTick() - start) >= W5500_SPI_TIMEOUT_MS)
    {
      return HAL_TIMEOUT;
    }
  }

  return HAL_OK;
}

static HAL_StatusTypeDef W5500_SpiTransferByte(uint8_t tx_data, uint8_t *rx_data)
{
  uint8_t received;

  W5500_SpiEnable();

  if (W5500_SpiWaitFlag(SPI_FLAG_TXE, 1U) != HAL_OK)
  {
    return HAL_TIMEOUT;
  }

  *(__IO uint8_t *)&w5500_spi->Instance->DR = tx_data;

  if (W5500_SpiWaitFlag(SPI_FLAG_RXNE, 1U) != HAL_OK)
  {
    return HAL_TIMEOUT;
  }

  received = *(__IO uint8_t *)&w5500_spi->Instance->DR;
  if (rx_data != NULL)
  {
    *rx_data = received;
  }

  return HAL_OK;
}

static HAL_StatusTypeDef W5500_SpiWrite(const uint8_t *data, uint16_t length)
{
  uint16_t index;

  for (index = 0U; index < length; index++)
  {
    if (W5500_SpiTransferByte(data[index], NULL) != HAL_OK)
    {
      return HAL_TIMEOUT;
    }
  }

  return W5500_SpiWaitFlag(SPI_FLAG_BSY, 0U);
}

static HAL_StatusTypeDef W5500_SpiRead(uint8_t *data, uint16_t length)
{
  uint16_t index;

  for (index = 0U; index < length; index++)
  {
    if (W5500_SpiTransferByte(0xFFU, &data[index]) != HAL_OK)
    {
      return HAL_TIMEOUT;
    }
  }

  return W5500_SpiWaitFlag(SPI_FLAG_BSY, 0U);
}
static HAL_StatusTypeDef W5500_ReadBlock(uint16_t address, uint8_t control, uint8_t *data, uint16_t length)
{
  HAL_StatusTypeDef status;
  uint8_t header[3] = {(uint8_t)(address >> 8), (uint8_t)address, (uint8_t)(control & 0xFCU)};
  W5500_Select();
  status = W5500_SpiWrite(header, sizeof(header));
  if (status == HAL_OK) status = W5500_SpiRead(data, length);
  W5500_Unselect();
  w5500_status.last_spi_status = status;
  return status;
}

static HAL_StatusTypeDef W5500_WriteBlock(uint16_t address, uint8_t control, const uint8_t *data, uint16_t length)
{
  HAL_StatusTypeDef status;
  uint8_t header[3] = {(uint8_t)(address >> 8), (uint8_t)address, (uint8_t)((control & 0xFCU) | W5500_COMMON_WRITE)};
  W5500_Select();
  status = W5500_SpiWrite(header, sizeof(header));
  if (status == HAL_OK) status = W5500_SpiWrite(data, length);
  W5500_Unselect();
  w5500_status.last_spi_status = status;
  return status;
}

#define W5500_ReadRegister(address, data) W5500_ReadBlock((address), W5500_COMMON_READ, (data), 1U)
#define W5500_WriteRegister(address, data) W5500_WriteBlock((address), W5500_COMMON_WRITE, (const uint8_t[]){(uint8_t)(data)}, 1U)
#define W5500_ReadSocket8(address, data) W5500_ReadBlock((address), W5500_SOCKET0_REG_BLOCK | W5500_COMMON_READ, (data), 1U)
#define W5500_WriteSocket8(address, data) W5500_WriteBlock((address), W5500_SOCKET0_REG_BLOCK | W5500_COMMON_WRITE, (const uint8_t[]){(uint8_t)(data)}, 1U)

static HAL_StatusTypeDef W5500_ReadSocket16(uint16_t address, uint16_t *data)
{
  uint8_t bytes[2];
  HAL_StatusTypeDef status = W5500_ReadBlock(address, W5500_SOCKET0_REG_BLOCK | W5500_COMMON_READ, bytes, sizeof(bytes));
  if (status == HAL_OK) *data = (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
  return status;
}

static HAL_StatusTypeDef W5500_WriteSocket16(uint16_t address, uint16_t data)
{
  uint8_t bytes[2] = {(uint8_t)(data >> 8), (uint8_t)data};
  return W5500_WriteBlock(address, W5500_SOCKET0_REG_BLOCK | W5500_COMMON_WRITE, bytes, sizeof(bytes));
}

static HAL_StatusTypeDef W5500_WaitSocketCommand(void)
{
  uint8_t command;
  uint32_t start = HAL_GetTick();

  do
  {
    if (W5500_ReadSocket8(W5500_SN_CR, &command) != HAL_OK) return w5500_status.last_spi_status;
    if (command == 0U) return HAL_OK;
  } while ((HAL_GetTick() - start) < W5500_SPI_TIMEOUT_MS);

  return HAL_TIMEOUT;
}

static HAL_StatusTypeDef W5500_OpenUdpSocket(uint16_t port)
{
  uint8_t status;
  uint32_t start = HAL_GetTick();

  (void)W5500_WriteSocket8(W5500_SN_CR, W5500_SN_CR_CLOSE);
  (void)W5500_WaitSocketCommand();
  (void)W5500_WriteSocket8(W5500_SN_IR, 0xFFU);
  if ((W5500_WriteSocket8(W5500_SN_MR, W5500_SN_MR_UDP) != HAL_OK) ||
      (W5500_WriteSocket16(W5500_SN_PORT, port) != HAL_OK) ||
      (W5500_WriteSocket8(W5500_SN_CR, W5500_SN_CR_OPEN) != HAL_OK) ||
      (W5500_WaitSocketCommand() != HAL_OK)) return w5500_status.last_spi_status;

  do
  {
    if (W5500_ReadSocket8(W5500_SN_SR, &status) != HAL_OK) return w5500_status.last_spi_status;
    if (status == W5500_SN_SR_UDP) return HAL_OK;
  } while ((HAL_GetTick() - start) < W5500_SPI_TIMEOUT_MS);

  return HAL_TIMEOUT;
}

static HAL_StatusTypeDef W5500_WriteTxBuffer(uint16_t address, const uint8_t *data, uint16_t length)
{
  uint16_t offset = address & W5500_SOCKET_BUFFER_MASK;
  uint16_t first = W5500_SOCKET_BUFFER_SIZE - offset;
  if (first > length) first = length;
  if (W5500_WriteBlock(offset, W5500_SOCKET0_TX_BLOCK | W5500_COMMON_WRITE, data, first) != HAL_OK) return w5500_status.last_spi_status;
  if (first < length) return W5500_WriteBlock(0U, W5500_SOCKET0_TX_BLOCK | W5500_COMMON_WRITE, &data[first], (uint16_t)(length - first));
  return HAL_OK;
}

static HAL_StatusTypeDef W5500_ReadRxBuffer(uint16_t address, uint8_t *data, uint16_t length)
{
  uint16_t offset = address & W5500_SOCKET_BUFFER_MASK;
  uint16_t first = W5500_SOCKET_BUFFER_SIZE - offset;
  if (first > length) first = length;
  if (W5500_ReadBlock(offset, W5500_SOCKET0_RX_BLOCK | W5500_COMMON_READ, data, first) != HAL_OK) return w5500_status.last_spi_status;
  if (first < length) return W5500_ReadBlock(0U, W5500_SOCKET0_RX_BLOCK | W5500_COMMON_READ, &data[first], (uint16_t)(length - first));
  return HAL_OK;
}

static HAL_StatusTypeDef W5500_SendUdp(const uint8_t dest_ip[4], uint16_t dest_port, const uint8_t *data, uint16_t length)
{
  uint8_t ir;
  uint16_t tx_free;
  uint16_t tx_wr;
  uint32_t start = HAL_GetTick();
  if ((W5500_WriteBlock(W5500_SN_DIPR, W5500_SOCKET0_REG_BLOCK | W5500_COMMON_WRITE, dest_ip, 4U) != HAL_OK) ||
      (W5500_WriteSocket16(W5500_SN_DPORT, dest_port) != HAL_OK)) return w5500_status.last_spi_status;

  do
  {
    if (W5500_ReadSocket16(W5500_SN_TX_FSR, &tx_free) != HAL_OK) return w5500_status.last_spi_status;
  } while ((tx_free < length) && ((HAL_GetTick() - start) < W5500_SPI_TIMEOUT_MS));

  if ((W5500_ReadSocket16(W5500_SN_TX_WR, &tx_wr) != HAL_OK) ||
      (W5500_WriteTxBuffer(tx_wr, data, length) != HAL_OK) ||
      (W5500_WriteSocket16(W5500_SN_TX_WR, (uint16_t)(tx_wr + length)) != HAL_OK) ||
      (W5500_WriteSocket8(W5500_SN_CR, W5500_SN_CR_SEND) != HAL_OK) ||
      (W5500_WaitSocketCommand() != HAL_OK)) return w5500_status.last_spi_status;

  do
  {
    if (W5500_ReadSocket8(W5500_SN_IR, &ir) != HAL_OK) return w5500_status.last_spi_status;
    if ((ir & W5500_SN_IR_SENDOK) != 0U)
    {
      (void)W5500_WriteSocket8(W5500_SN_IR, W5500_SN_IR_SENDOK);
      return HAL_OK;
    }
    if ((ir & W5500_SN_IR_TIMEOUT) != 0U)
    {
      (void)W5500_WriteSocket8(W5500_SN_IR, W5500_SN_IR_TIMEOUT);
      return HAL_TIMEOUT;
    }
  } while ((HAL_GetTick() - start) < W5500_SPI_TIMEOUT_MS);

  return HAL_TIMEOUT;
}

static HAL_StatusTypeDef W5500_ReceiveUdp(uint8_t *data, uint16_t max_length, uint16_t *length, uint32_t timeout_ms)
{
  uint8_t udp_header[8];
  uint16_t rx_size;
  uint16_t rx_rd;
  uint16_t udp_length;
  uint32_t start = HAL_GetTick();

  *length = 0U;
  do
  {
    if (W5500_ReadSocket16(W5500_SN_RX_RSR, &rx_size) != HAL_OK) return w5500_status.last_spi_status;
  } while ((rx_size < sizeof(udp_header)) && ((HAL_GetTick() - start) < timeout_ms));

  if ((W5500_ReadSocket16(W5500_SN_RX_RD, &rx_rd) != HAL_OK) ||
      (W5500_ReadRxBuffer(rx_rd, udp_header, sizeof(udp_header)) != HAL_OK)) return w5500_status.last_spi_status;

  udp_length = (uint16_t)(((uint16_t)udp_header[6] << 8) | udp_header[7]);
  if (udp_length > max_length) udp_length = max_length;
  if (W5500_ReadRxBuffer((uint16_t)(rx_rd + sizeof(udp_header)), data, udp_length) != HAL_OK) return w5500_status.last_spi_status;
  rx_rd = (uint16_t)(rx_rd + sizeof(udp_header) + (((uint16_t)udp_header[6] << 8) | udp_header[7]));
  (void)W5500_WriteSocket16(W5500_SN_RX_RD, rx_rd);
  (void)W5500_WriteSocket8(W5500_SN_CR, W5500_SN_CR_RECV);
  (void)W5500_WaitSocketCommand();
  *length = udp_length;
  return HAL_OK;
}

static void W5500_Reset(void)
{
  GPIO_SetPin(W5500_PINMODE0_GPIO_Port, W5500_PINMODE0_Pin);
  GPIO_SetPin(W5500_PINMODE1_GPIO_Port, W5500_PINMODE1_Pin);
  GPIO_SetPin(W5500_PINMODE2_GPIO_Port, W5500_PINMODE2_Pin);
  GPIO_SetPin(W5500_CS_GPIO_Port, W5500_CS_Pin);
  GPIO_ResetPin(W5500_Reset_GPIO_Port, W5500_Reset_Pin);
  HAL_Delay(5U);
  GPIO_SetPin(W5500_Reset_GPIO_Port, W5500_Reset_Pin);
  HAL_Delay(50U);
}

void W5500_Init(SPI_HandleTypeDef *spi, UART_HandleTypeDef *uart)
{
  w5500_spi = spi;
  w5500_uart = uart;
  memset(&w5500_status, 0, sizeof(w5500_status));
  w5500_status.last_spi_status = HAL_ERROR;

  if ((w5500_spi == NULL) || (w5500_uart == NULL))
  {
    return;
  }

  W5500_Reset();
  w5500_status.last_spi_status = HAL_OK;
  if (W5500_WriteRegister(W5500_REG_MR, 0x80U) != HAL_OK) return;

  if ((W5500_ReadRegister(W5500_REG_VERSIONR, &w5500_status.version) != HAL_OK) ||
      (W5500_ReadRegister(W5500_REG_PHYCFGR, &w5500_status.phycfgr) != HAL_OK) ||
      (W5500_ReadRegister(W5500_REG_MR, &w5500_status.mode) != HAL_OK)) return;
  w5500_status.initialized = (w5500_status.version == W5500_VERSION_VALUE) ? 1U : 0U;
}

uint8_t W5500_IsInitialized(void)
{
  return w5500_status.initialized;
}

uint8_t W5500_IsLinkUp(void)
{
  if (W5500_ReadRegister(W5500_REG_PHYCFGR, &w5500_status.phycfgr) != HAL_OK)
  {
    return 0U;
  }

  return ((w5500_status.phycfgr & W5500_PHYCFGR_LINK) != 0U) ? 1U : 0U;
}

static const char *W5500_HalStatusName(HAL_StatusTypeDef status)
{
  switch (status)
  {
    case HAL_OK: return "HAL_OK";
    case HAL_ERROR: return "HAL_ERROR";
    case HAL_BUSY: return "HAL_BUSY";
    case HAL_TIMEOUT: return "HAL_TIMEOUT";
    default: return "HAL_UNKNOWN";
  }
}

void W5500_SendStatus(void)
{
  const uint8_t int_state = GPIO_ReadPin(W5500_INT_GPIO_Port, W5500_INT_Pin);
  (void)W5500_ReadRegister(W5500_REG_VERSIONR, &w5500_status.version);
  (void)W5500_ReadRegister(W5500_REG_PHYCFGR, &w5500_status.phycfgr);
  (void)W5500_ReadRegister(W5500_REG_MR, &w5500_status.mode);
  w5500_status.initialized = (w5500_status.version == W5500_VERSION_VALUE) ? 1U : 0U;
  UART1_SendFormatted("w5500 init=%s spi=%s version=0x%02X link=%s speed=%s duplex=%s phycfgr=0x%02X mode=0x%02X int=%s\r\n",
                      w5500_status.initialized ? "ok" : "fail", W5500_HalStatusName(w5500_status.last_spi_status), w5500_status.version,
                      (w5500_status.phycfgr & W5500_PHYCFGR_LINK) ? "up" : "down",
                      (w5500_status.phycfgr & W5500_PHYCFGR_SPEED) ? "100M" : "10M",
                      (w5500_status.phycfgr & W5500_PHYCFGR_DUPLEX) ? "full" : "half",
                      w5500_status.phycfgr, w5500_status.mode, int_state ? "high" : "low");
}

static uint16_t DHCP_BuildMessage(uint8_t *packet, uint8_t message_type, uint32_t xid, const uint8_t mac[6], const uint8_t requested_ip[4], const uint8_t server_id[4])
{
  uint16_t index = DHCP_OPTIONS_OFFSET;
  memset(packet, 0, DHCP_PACKET_SIZE);
  packet[0] = 1U; packet[1] = 1U; packet[2] = 6U;
  packet[4] = (uint8_t)(xid >> 24); packet[5] = (uint8_t)(xid >> 16); packet[6] = (uint8_t)(xid >> 8); packet[7] = (uint8_t)xid;
  packet[10] = 0x80U;
  memcpy(&packet[28], mac, 6U);
  packet[DHCP_MAGIC_COOKIE_OFFSET] = 0x63U; packet[DHCP_MAGIC_COOKIE_OFFSET + 1U] = 0x82U; packet[DHCP_MAGIC_COOKIE_OFFSET + 2U] = 0x53U; packet[DHCP_MAGIC_COOKIE_OFFSET + 3U] = 0x63U;
  packet[index++] = 53U; packet[index++] = 1U; packet[index++] = message_type;
  packet[index++] = 55U; packet[index++] = 3U; packet[index++] = 1U; packet[index++] = 3U; packet[index++] = 6U;
  if ((requested_ip != NULL) && (server_id != NULL))
  {
    packet[index++] = 50U; packet[index++] = 4U; memcpy(&packet[index], requested_ip, 4U); index += 4U;
    packet[index++] = 54U; packet[index++] = 4U; memcpy(&packet[index], server_id, 4U); index += 4U;
  }
  packet[index++] = 255U;
  return index;
}

static uint8_t DHCP_ParseMessage(const uint8_t *packet, uint16_t length, uint32_t xid, const uint8_t mac[6], DHCP_Result_t *result)
{
  uint16_t index = DHCP_OPTIONS_OFFSET;
  uint8_t message_type = 0U;
  if ((length < DHCP_OPTIONS_OFFSET) || (packet[0] != 2U) || ((((uint32_t)packet[4] << 24) | ((uint32_t)packet[5] << 16) | ((uint32_t)packet[6] << 8) | packet[7]) != xid) || (memcmp(&packet[28], mac, 6U) != 0)) return 0U;
  if ((packet[DHCP_MAGIC_COOKIE_OFFSET] != 0x63U) || (packet[DHCP_MAGIC_COOKIE_OFFSET + 1U] != 0x82U) || (packet[DHCP_MAGIC_COOKIE_OFFSET + 2U] != 0x53U) || (packet[DHCP_MAGIC_COOKIE_OFFSET + 3U] != 0x63U)) return 0U;
  memcpy(result->ip, &packet[16], 4U);
  while (index < length)
  {
    uint8_t option = packet[index++];
    uint8_t size;
    if (option == 255U) break;
    if (option == 0U) continue;
    if (index >= length) break;
    size = packet[index++];
    if ((uint16_t)(index + size) > length) break;
    if ((option == 53U) && (size == 1U)) message_type = packet[index];
    else if ((option == 1U) && (size == 4U)) memcpy(result->mask, &packet[index], 4U);
    else if ((option == 3U) && (size >= 4U)) memcpy(result->gateway, &packet[index], 4U);
    else if ((option == 6U) && (size >= 4U)) memcpy(result->dns, &packet[index], 4U);
    else if ((option == 54U) && (size == 4U)) memcpy(result->server, &packet[index], 4U);
    index = (uint16_t)(index + size);
  }
  return message_type;
}

static void W5500_ApplyNetwork(const DHCP_Result_t *result, const uint8_t mac[6])
{
  (void)W5500_WriteBlock(W5500_REG_SHAR, W5500_COMMON_WRITE, mac, 6U);
  (void)W5500_WriteBlock(W5500_REG_GAR, W5500_COMMON_WRITE, result->gateway, 4U);
  (void)W5500_WriteBlock(W5500_REG_SUBR, W5500_COMMON_WRITE, result->mask, 4U);
  (void)W5500_WriteBlock(W5500_REG_SIPR, W5500_COMMON_WRITE, result->ip, 4U);
}

static uint8_t W5500_DhcpAcquire(DHCP_Result_t *result)
{
  static uint8_t packet[DHCP_PACKET_SIZE];
  static uint8_t rx_packet[DHCP_PACKET_SIZE];
  static const uint8_t broadcast_ip[4] = {255U, 255U, 255U, 255U};
  static const uint8_t zero_ip[4] = {0U, 0U, 0U, 0U};
  const uint8_t *mac = Settings_Get()->mac;
  uint16_t packet_length;
  uint16_t rx_length;
  uint32_t xid = HAL_GetTick() ^ 0x5500A5A5UL;
  uint32_t start = HAL_GetTick();
  uint8_t message_type = 0U;
  memset(result, 0, sizeof(*result));
  strcpy(result->message, "timeout");

  if (W5500_ReadRegister(W5500_REG_PHYCFGR, &w5500_status.phycfgr) != HAL_OK) { strcpy(result->message, "spi_error"); return 0U; }
  if ((w5500_status.phycfgr & W5500_PHYCFGR_LINK) == 0U) { strcpy(result->message, "link_down"); return 0U; }
  (void)W5500_WriteBlock(W5500_REG_SHAR, W5500_COMMON_WRITE, mac, 6U);
  (void)W5500_WriteBlock(W5500_REG_GAR, W5500_COMMON_WRITE, zero_ip, 4U);
  (void)W5500_WriteBlock(W5500_REG_SUBR, W5500_COMMON_WRITE, zero_ip, 4U);
  (void)W5500_WriteBlock(W5500_REG_SIPR, W5500_COMMON_WRITE, zero_ip, 4U);
  if (W5500_OpenUdpSocket(W5500_DHCP_CLIENT_PORT) != HAL_OK) { strcpy(result->message, "socket_error"); return 0U; }
  packet_length = DHCP_BuildMessage(packet, 1U, xid, mac, NULL, NULL);
  if (W5500_SendUdp(broadcast_ip, W5500_DHCP_SERVER_PORT, packet, packet_length) != HAL_OK) { strcpy(result->message, "discover_send_fail"); return 0U; }

  do
  {
    IWDG->KR = LL_IWDG_KEY_RELOAD; // Reload watchdog
    if (W5500_ReceiveUdp(rx_packet, sizeof(rx_packet), &rx_length, 250U) == HAL_OK)
    {
      message_type = DHCP_ParseMessage(rx_packet, rx_length, xid, mac, result);
      if (message_type == 2U) break;
    }
  } while ((HAL_GetTick() - start) < DHCP_TIMEOUT_MS);
  if (message_type != 2U) return 0U;

  packet_length = DHCP_BuildMessage(packet, 3U, xid, mac, result->ip, result->server);
  if (W5500_SendUdp(broadcast_ip, W5500_DHCP_SERVER_PORT, packet, packet_length) != HAL_OK) { strcpy(result->message, "request_send_fail"); return 0U; }
  start = HAL_GetTick();
  do
  {
    IWDG->KR = LL_IWDG_KEY_RELOAD; // Reload watchdog
    if (W5500_ReceiveUdp(rx_packet, sizeof(rx_packet), &rx_length, 250U) == HAL_OK)
    {
      message_type = DHCP_ParseMessage(rx_packet, rx_length, xid, mac, result);
      if (message_type == 5U) { W5500_ApplyNetwork(result, mac); result->success = 1U; strcpy(result->message, "ok"); return 1U; }
    }
  } while ((HAL_GetTick() - start) < DHCP_TIMEOUT_MS);

  return 0U;
}

uint8_t W5500_RunDhcp(void)
{
  DHCP_Result_t result;
  UART1_SendString("dhcp=start\r\n");
  (void)W5500_DhcpAcquire(&result);
  UART1_SendFormatted("dhcp result=%s status=%s ip=%u.%u.%u.%u mask=%u.%u.%u.%u gw=%u.%u.%u.%u dns=%u.%u.%u.%u server=%u.%u.%u.%u\r\n",
                      result.success ? "ok" : "fail", result.message,
                      result.ip[0], result.ip[1], result.ip[2], result.ip[3],
                      result.mask[0], result.mask[1], result.mask[2], result.mask[3],
                      result.gateway[0], result.gateway[1], result.gateway[2], result.gateway[3],
                      result.dns[0], result.dns[1], result.dns[2], result.dns[3],
                      result.server[0], result.server[1], result.server[2], result.server[3]);
  return result.success;
}

void W5500_SendIpInfo(void)
{
  uint8_t ip[4] = {0U};
  uint8_t mask[4] = {0U};
  uint8_t gateway[4] = {0U};
  uint8_t mac[6] = {0U};

  (void)W5500_ReadBlock(W5500_REG_SIPR, W5500_COMMON_READ, ip, sizeof(ip));
  (void)W5500_ReadBlock(W5500_REG_SUBR, W5500_COMMON_READ, mask, sizeof(mask));
  (void)W5500_ReadBlock(W5500_REG_GAR, W5500_COMMON_READ, gateway, sizeof(gateway));
  (void)W5500_ReadBlock(W5500_REG_SHAR, W5500_COMMON_READ, mac, sizeof(mac));

  UART1_SendFormatted("ip=%u.%u.%u.%u mask=%u.%u.%u.%u gw=%u.%u.%u.%u mac=%02X:%02X:%02X:%02X:%02X:%02X\r\n",
                      ip[0], ip[1], ip[2], ip[3],
                      mask[0], mask[1], mask[2], mask[3],
                      gateway[0], gateway[1], gateway[2], gateway[3],
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void W5500_ApplySettingsNetwork(void)
{
  const Settings_t *settings = Settings_Get();

  (void)W5500_WriteBlock(W5500_REG_SHAR, W5500_COMMON_WRITE, settings->mac, 6U);
  (void)W5500_WriteBlock(W5500_REG_GAR, W5500_COMMON_WRITE, settings->gateway, 4U);
  (void)W5500_WriteBlock(W5500_REG_SUBR, W5500_COMMON_WRITE, settings->mask, 4U);
  (void)W5500_WriteBlock(W5500_REG_SIPR, W5500_COMMON_WRITE, settings->ip, 4U);
}

static uint8_t W5500_SocketRegBlock(uint8_t socket)
{
  return (uint8_t)(W5500_SOCKET0_REG_BLOCK + (socket * 0x20U));
}

static uint8_t W5500_SocketTxBlock(uint8_t socket)
{
  return (uint8_t)(W5500_SOCKET0_TX_BLOCK + (socket * 0x20U));
}

static uint8_t W5500_SocketRxBlock(uint8_t socket)
{
  return (uint8_t)(W5500_SOCKET0_RX_BLOCK + (socket * 0x20U));
}

static HAL_StatusTypeDef W5500_ReadSocketN8(uint8_t socket, uint16_t address, uint8_t *data)
{
  return W5500_ReadBlock(address, W5500_SocketRegBlock(socket) | W5500_COMMON_READ, data, 1U);
}

static HAL_StatusTypeDef W5500_WriteSocketN8(uint8_t socket, uint16_t address, uint8_t data)
{
  return W5500_WriteBlock(address, W5500_SocketRegBlock(socket) | W5500_COMMON_WRITE, &data, 1U);
}

static HAL_StatusTypeDef W5500_ReadSocketN16(uint8_t socket, uint16_t address, uint16_t *data)
{
  uint8_t bytes[2];
  HAL_StatusTypeDef status = W5500_ReadBlock(address, W5500_SocketRegBlock(socket) | W5500_COMMON_READ, bytes, sizeof(bytes));

  if (status == HAL_OK)
  {
    *data = (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
  }

  return status;
}

static HAL_StatusTypeDef W5500_WriteSocketN16(uint8_t socket, uint16_t address, uint16_t data)
{
  uint8_t bytes[2] = {(uint8_t)(data >> 8), (uint8_t)data};
  return W5500_WriteBlock(address, W5500_SocketRegBlock(socket) | W5500_COMMON_WRITE, bytes, sizeof(bytes));
}

static HAL_StatusTypeDef W5500_WaitSocketNCommand(uint8_t socket)
{
  uint8_t command;
  uint32_t start = HAL_GetTick();

  do
  {
    if (W5500_ReadSocketN8(socket, W5500_SN_CR, &command) != HAL_OK)
    {
      return w5500_status.last_spi_status;
    }

    if (command == 0U)
    {
      return HAL_OK;
    }
  } while ((HAL_GetTick() - start) < W5500_SPI_TIMEOUT_MS);

  return HAL_TIMEOUT;
}

static HAL_StatusTypeDef W5500_WriteTxBufferN(uint8_t socket, uint16_t address, const uint8_t *data, uint16_t length)
{
  uint16_t offset = address & W5500_SOCKET_BUFFER_MASK;
  uint16_t first = W5500_SOCKET_BUFFER_SIZE - offset;

  if (first > length)
  {
    first = length;
  }

  if (W5500_WriteBlock(offset, W5500_SocketTxBlock(socket) | W5500_COMMON_WRITE, data, first) != HAL_OK)
  {
    return w5500_status.last_spi_status;
  }

  if (first < length)
  {
    return W5500_WriteBlock(0U, W5500_SocketTxBlock(socket) | W5500_COMMON_WRITE, &data[first], (uint16_t)(length - first));
  }

  return HAL_OK;
}

static HAL_StatusTypeDef W5500_ReadRxBufferN(uint8_t socket, uint16_t address, uint8_t *data, uint16_t length)
{
  uint16_t offset = address & W5500_SOCKET_BUFFER_MASK;
  uint16_t first = W5500_SOCKET_BUFFER_SIZE - offset;

  if (first > length)
  {
    first = length;
  }

  if (W5500_ReadBlock(offset, W5500_SocketRxBlock(socket) | W5500_COMMON_READ, data, first) != HAL_OK)
  {
    return w5500_status.last_spi_status;
  }

  if (first < length)
  {
    return W5500_ReadBlock(0U, W5500_SocketRxBlock(socket) | W5500_COMMON_READ, &data[first], (uint16_t)(length - first));
  }

  return HAL_OK;
}

/* TX buffer space only comes back when the peer ACKs, so a slow client (Wi-Fi, phone) can need
 * several round trips per 2 KB. Wait long enough for that, keep the watchdog fed, and never write
 * into space that is not free: that would overwrite data still waiting to be sent. */
#define W5500_TCP_SEND_TIMEOUT_MS 1500U

static HAL_StatusTypeDef W5500_SendTcp(uint8_t socket, const uint8_t *data, uint16_t length)
{
  uint8_t ir;
  uint16_t tx_free;
  uint16_t tx_wr;
  uint32_t start = HAL_GetTick();

  do
  {
    IWDG->KR = LL_IWDG_KEY_RELOAD;
    if (W5500_ReadSocketN16(socket, W5500_SN_TX_FSR, &tx_free) != HAL_OK)
    {
      return w5500_status.last_spi_status;
    }
  } while ((tx_free < length) && ((HAL_GetTick() - start) < W5500_TCP_SEND_TIMEOUT_MS));

  if (tx_free < length)
  {
    return HAL_TIMEOUT;
  }

  if ((W5500_ReadSocketN16(socket, W5500_SN_TX_WR, &tx_wr) != HAL_OK) ||
      (W5500_WriteTxBufferN(socket, tx_wr, data, length) != HAL_OK) ||
      (W5500_WriteSocketN16(socket, W5500_SN_TX_WR, (uint16_t)(tx_wr + length)) != HAL_OK) ||
      (W5500_WriteSocketN8(socket, W5500_SN_CR, W5500_SN_CR_SEND) != HAL_OK) ||
      (W5500_WaitSocketNCommand(socket) != HAL_OK))
  {
    return w5500_status.last_spi_status;
  }

  do
  {
    if (W5500_ReadSocketN8(socket, W5500_SN_IR, &ir) != HAL_OK)
    {
      return w5500_status.last_spi_status;
    }

    if ((ir & W5500_SN_IR_SENDOK) != 0U)
    {
      (void)W5500_WriteSocketN8(socket, W5500_SN_IR, W5500_SN_IR_SENDOK);
      return HAL_OK;
    }

    if ((ir & W5500_SN_IR_TIMEOUT) != 0U)
    {
      (void)W5500_WriteSocketN8(socket, W5500_SN_IR, W5500_SN_IR_TIMEOUT);
      return HAL_TIMEOUT;
    }
    IWDG->KR = LL_IWDG_KEY_RELOAD;
  } while ((HAL_GetTick() - start) < W5500_TCP_SEND_TIMEOUT_MS);

  return HAL_TIMEOUT;
}

static HAL_StatusTypeDef W5500_ReceiveTcp(uint8_t socket, char *data, uint16_t max_length, uint16_t *length)
{
  uint16_t rx_size;
  uint16_t rx_rd;

  *length = 0U;
  if ((max_length < 2U) || (W5500_ReadSocketN16(socket, W5500_SN_RX_RSR, &rx_size) != HAL_OK))
  {
    return w5500_status.last_spi_status;
  }

  if (rx_size == 0U)
  {
    return HAL_TIMEOUT;
  }

  if (rx_size >= max_length)
  {
    rx_size = (uint16_t)(max_length - 1U);
  }

  if ((W5500_ReadSocketN16(socket, W5500_SN_RX_RD, &rx_rd) != HAL_OK) ||
      (W5500_ReadRxBufferN(socket, rx_rd, (uint8_t *)data, rx_size) != HAL_OK))
  {
    return w5500_status.last_spi_status;
  }

  data[rx_size] = '\0';
  *length = rx_size;
  rx_rd = (uint16_t)(rx_rd + rx_size);
  (void)W5500_WriteSocketN16(socket, W5500_SN_RX_RD, rx_rd);
  (void)W5500_WriteSocketN8(socket, W5500_SN_CR, W5500_SN_CR_RECV);
  (void)W5500_WaitSocketNCommand(socket);
  return HAL_OK;
}

static HAL_StatusTypeDef W5500_OpenUdpSocketN(uint8_t socket, uint16_t port)
{
  uint8_t status;
  uint32_t start = HAL_GetTick();

  (void)W5500_WriteSocketN8(socket, W5500_SN_CR, W5500_SN_CR_CLOSE);
  (void)W5500_WaitSocketNCommand(socket);
  (void)W5500_WriteSocketN8(socket, W5500_SN_IR, 0xFFU);
  if ((W5500_WriteSocketN8(socket, W5500_SN_MR, W5500_SN_MR_UDP) != HAL_OK) ||
      (W5500_WriteSocketN16(socket, W5500_SN_PORT, port) != HAL_OK) ||
      (W5500_WriteSocketN8(socket, W5500_SN_CR, W5500_SN_CR_OPEN) != HAL_OK) ||
      (W5500_WaitSocketNCommand(socket) != HAL_OK))
  {
    return w5500_status.last_spi_status;
  }

  do
  {
    if (W5500_ReadSocketN8(socket, W5500_SN_SR, &status) != HAL_OK)
    {
      return w5500_status.last_spi_status;
    }

    if (status == W5500_SN_SR_UDP)
    {
      return HAL_OK;
    }
  } while ((HAL_GetTick() - start) < W5500_SPI_TIMEOUT_MS);

  return HAL_TIMEOUT;
}

static HAL_StatusTypeDef W5500_SendUdpN(uint8_t socket, const uint8_t dest_ip[4], uint16_t dest_port, const uint8_t *data, uint16_t length)
{
  uint8_t ir;
  uint16_t tx_free;
  uint16_t tx_wr;
  uint32_t start = HAL_GetTick();

  if ((W5500_WriteBlock(W5500_SN_DIPR, W5500_SocketRegBlock(socket) | W5500_COMMON_WRITE, dest_ip, 4U) != HAL_OK) ||
      (W5500_WriteSocketN16(socket, W5500_SN_DPORT, dest_port) != HAL_OK))
  {
    return w5500_status.last_spi_status;
  }

  do
  {
    if (W5500_ReadSocketN16(socket, W5500_SN_TX_FSR, &tx_free) != HAL_OK)
    {
      return w5500_status.last_spi_status;
    }
  } while ((tx_free < length) && ((HAL_GetTick() - start) < W5500_SPI_TIMEOUT_MS));

  if ((W5500_ReadSocketN16(socket, W5500_SN_TX_WR, &tx_wr) != HAL_OK) ||
      (W5500_WriteTxBufferN(socket, tx_wr, data, length) != HAL_OK) ||
      (W5500_WriteSocketN16(socket, W5500_SN_TX_WR, (uint16_t)(tx_wr + length)) != HAL_OK) ||
      (W5500_WriteSocketN8(socket, W5500_SN_CR, W5500_SN_CR_SEND) != HAL_OK) ||
      (W5500_WaitSocketNCommand(socket) != HAL_OK))
  {
    return w5500_status.last_spi_status;
  }

  do
  {
    if (W5500_ReadSocketN8(socket, W5500_SN_IR, &ir) != HAL_OK)
    {
      return w5500_status.last_spi_status;
    }

    if ((ir & W5500_SN_IR_SENDOK) != 0U)
    {
      (void)W5500_WriteSocketN8(socket, W5500_SN_IR, W5500_SN_IR_SENDOK);
      return HAL_OK;
    }

    if ((ir & W5500_SN_IR_TIMEOUT) != 0U)
    {
      (void)W5500_WriteSocketN8(socket, W5500_SN_IR, W5500_SN_IR_TIMEOUT);
      return HAL_TIMEOUT;
    }
  } while ((HAL_GetTick() - start) < W5500_SPI_TIMEOUT_MS);

  return HAL_TIMEOUT;
}

static HAL_StatusTypeDef W5500_ReceiveUdpN(uint8_t socket, uint8_t remote_ip[4], uint16_t *remote_port, char *data, uint16_t max_length, uint16_t *length)
{
  uint8_t udp_header[8];
  uint16_t rx_size;
  uint16_t rx_rd;
  uint16_t udp_length;

  *length = 0U;
  if ((max_length < 2U) || (W5500_ReadSocketN16(socket, W5500_SN_RX_RSR, &rx_size) != HAL_OK))
  {
    return w5500_status.last_spi_status;
  }

  if (rx_size < sizeof(udp_header))
  {
    return HAL_TIMEOUT;
  }

  if ((W5500_ReadSocketN16(socket, W5500_SN_RX_RD, &rx_rd) != HAL_OK) ||
      (W5500_ReadRxBufferN(socket, rx_rd, udp_header, sizeof(udp_header)) != HAL_OK))
  {
    return w5500_status.last_spi_status;
  }

  memcpy(remote_ip, udp_header, 4U);
  *remote_port = (uint16_t)(((uint16_t)udp_header[4] << 8) | udp_header[5]);
  udp_length = (uint16_t)(((uint16_t)udp_header[6] << 8) | udp_header[7]);
  if (udp_length >= max_length)
  {
    udp_length = (uint16_t)(max_length - 1U);
  }

  if (W5500_ReadRxBufferN(socket, (uint16_t)(rx_rd + sizeof(udp_header)), (uint8_t *)data, udp_length) != HAL_OK)
  {
    return w5500_status.last_spi_status;
  }

  data[udp_length] = '\0';
  *length = udp_length;
  rx_rd = (uint16_t)(rx_rd + sizeof(udp_header) + (((uint16_t)udp_header[6] << 8) | udp_header[7]));
  (void)W5500_WriteSocketN16(socket, W5500_SN_RX_RD, rx_rd);
  (void)W5500_WriteSocketN8(socket, W5500_SN_CR, W5500_SN_CR_RECV);
  (void)W5500_WaitSocketNCommand(socket);
  return HAL_OK;
}

/* Graceful close: DISCON sends FIN after the data still in the TX buffer is delivered.
 * A plain CLOSE right after the last send drops unacknowledged data, which truncates the page
 * on slower links. Fall back to CLOSE if the peer does not finish within the timeout. */
#define W5500_WEB_CLOSE_TIMEOUT_MS 500U

static void W5500_WebClose(uint8_t socket)
{
  uint8_t status = W5500_SN_SR_ESTABLISHED;
  uint32_t start = HAL_GetTick();

  (void)W5500_WriteSocketN8(socket, W5500_SN_CR, W5500_SN_CR_DISCON);
  (void)W5500_WaitSocketNCommand(socket);
  while ((HAL_GetTick() - start) < W5500_WEB_CLOSE_TIMEOUT_MS)
  {
    IWDG->KR = LL_IWDG_KEY_RELOAD;
    if ((W5500_ReadSocketN8(socket, W5500_SN_SR, &status) != HAL_OK) || (status == W5500_SN_SR_CLOSED))
    {
      break;
    }
  }
  if (status != W5500_SN_SR_CLOSED)
  {
    (void)W5500_WriteSocketN8(socket, W5500_SN_CR, W5500_SN_CR_CLOSE);
    (void)W5500_WaitSocketNCommand(socket);
  }
}

/* extra_headers: "" or complete header lines ending in "\r\n". The body may contain zero bytes. */
static void W5500_WebSendBody(uint8_t socket, const char *status, const char *content_type, const char *extra_headers,
                              const uint8_t *body, uint16_t body_length)
{
  char header[160];
  uint16_t offset = 0U;
  int header_length = snprintf(header, sizeof(header),
                               "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n%sConnection: close\r\nCache-Control: no-store\r\n\r\n",
                               status, content_type, (unsigned)body_length, extra_headers);

  if ((header_length > 0) && (header_length < (int)sizeof(header)) &&
      (W5500_SendTcp(socket, (const uint8_t *)header, (uint16_t)header_length) == HAL_OK))
  {
    while (offset < body_length)
    {
      uint16_t chunk_length = (uint16_t)(body_length - offset);

      if (chunk_length > W5500_WEB_SEND_CHUNK_SIZE)
      {
        chunk_length = W5500_WEB_SEND_CHUNK_SIZE;
      }

      if (W5500_SendTcp(socket, &body[offset], chunk_length) != HAL_OK)
      {
        break;   /* client gone or stalled: stop, the caller closes the socket */
      }
      offset = (uint16_t)(offset + chunk_length);
    }
  }
}

static void W5500_WebSendText(uint8_t socket, const char *status, const char *content_type, const char *body)
{
  W5500_WebSendBody(socket, status, content_type, "", (const uint8_t *)body, (uint16_t)strlen(body));
}

static void W5500_WebSendUnauthorized(uint8_t socket)
{
  static const char response[] =
    "HTTP/1.1 401 Unauthorized\r\n"
    "WWW-Authenticate: Basic realm=\"device\"\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 13\r\n"
    "Connection: close\r\n"
    "Cache-Control: no-store\r\n"
    "\r\n"
    "Unauthorized\n";

  (void)W5500_SendTcp(socket, (const uint8_t *)response, (uint16_t)(sizeof(response) - 1U));
}

static int8_t W5500_Base64Value(char symbol)
{
  if ((symbol >= 'A') && (symbol <= 'Z')) return (int8_t)(symbol - 'A');
  if ((symbol >= 'a') && (symbol <= 'z')) return (int8_t)(symbol - 'a' + 26);
  if ((symbol >= '0') && (symbol <= '9')) return (int8_t)(symbol - '0' + 52);
  if (symbol == '+') return 62;
  if (symbol == '/') return 63;
  return -1;
}

static uint8_t W5500_Base64Decode(const char *input, char *output, uint16_t output_length)
{
  uint32_t accumulator = 0UL;
  uint8_t bits = 0U;
  uint16_t index = 0U;

  if ((input == NULL) || (output == NULL) || (output_length == 0U))
  {
    return 0U;
  }

  while ((*input != '\0') && (*input != '\r') && (*input != '\n') && (*input != ' '))
  {
    int8_t value;

    if (*input == '=')
    {
      break;
    }

    value = W5500_Base64Value(*input++);
    if (value < 0)
    {
      return 0U;
    }

    accumulator = (accumulator << 6) | (uint32_t)value;
    bits = (uint8_t)(bits + 6U);
    if (bits >= 8U)
    {
      bits = (uint8_t)(bits - 8U);
      if (index >= (output_length - 1U))
      {
        return 0U;
      }
      output[index++] = (char)((accumulator >> bits) & 0xFFU);
    }
  }

  output[index] = '\0';
  return (index > 0U) ? 1U : 0U;
}

static uint8_t W5500_WebIsAuthorized(const char *request)
{
  const Settings_t *settings = Settings_Get();
  const char *header;
  char decoded[96];
  char *password;

  if (settings->web_auth_enabled == 0U)
  {
    return 1U;
  }

  header = strstr(request, "\r\nAuthorization: Basic ");
  if (header == NULL)
  {
    return 0U;
  }

  header += 23;
  if (W5500_Base64Decode(header, decoded, sizeof(decoded)) == 0U)
  {
    return 0U;
  }

  password = strchr(decoded, ':');
  if (password == NULL)
  {
    return 0U;
  }

  password++;
  return Settings_CheckWebPassword(password);
}

static void W5500_WebSendData(uint8_t socket)
{
  char body[384];

  WebPages_BuildDataJson(body, sizeof(body));
  W5500_WebSendText(socket, "200 OK", "application/json", body);
}

static void W5500_WebSendIndex(uint8_t socket)
{
  /* Stored gzipped in flash; every browser accepts gzip, so it is sent as is. */
  W5500_WebSendBody(socket, "200 OK", "text/html; charset=utf-8", "Content-Encoding: gzip\r\n",
                    web_ui_gz, web_ui_gz_len);
}

static void W5500_WebSendAzConfig(uint8_t socket)
{
  char body[768];   /* fw ~110 + mechanics ~70 + 8 presets x (31-byte name + ~15) */

  WebPages_BuildAzConfigJson(body, sizeof(body));
  W5500_WebSendText(socket, "200 OK", "application/json", body);
}

/* The old standalone pages now live in the single-page UI; keep their URLs working. */
static void W5500_WebRedirect(uint8_t socket, const char *location_header)
{
  W5500_WebSendBody(socket, "302 Found", "text/plain", location_header, (const uint8_t *)"", 0U);
}

static void W5500_WebSendSettingsData(uint8_t socket)
{
  char body[896];

  WebPages_BuildSettingsJson(body, sizeof(body));
  W5500_WebSendText(socket, "200 OK", "application/json", body);
}

static void W5500_WebHandleRequest(uint8_t socket, const char *request)
{
  if (W5500_WebIsAuthorized(request) == 0U)
  {
    W5500_WebSendUnauthorized(socket);
  }
  else if ((strncmp(request, "GET /data", 9) == 0) || (strncmp(request, "GET /uptime", 11) == 0))
  {
    W5500_WebSendData(socket);
  }
  else if (strncmp(request, "GET / ", 6) == 0)
  {
    W5500_WebSendIndex(socket);
  }
  else if (strncmp(request, "GET /board/data", 15) == 0)
  {
    W5500_WebSendSettingsData(socket);
  }
  else if (strncmp(request, "GET /board/set?", 15) == 0)
  {
    if (WebPages_HandleBoardRequest(request, 1U))
    {
      W5500_WebSendText(socket, "200 OK", "application/json", "{\"ok\":1}\n");
    }
    else
    {
      W5500_WebSendText(socket, "400 Bad Request", "application/json", "{\"ok\":0}\n");
    }
  }
  else if (strncmp(request, "GET /az/goto?", 13) == 0)
  {
    char reply[48];

    if (WebPages_HandleAzGoto(request, reply, sizeof(reply)))
    {
      W5500_WebSendText(socket, "200 OK", "application/json", reply);
    }
    else
    {
      W5500_WebSendText(socket, "400 Bad Request", "application/json", reply);
    }
  }
  else if (strncmp(request, "GET /az/config", 14) == 0)
  {
    W5500_WebSendAzConfig(socket);
  }
  else if (strncmp(request, "GET /az/", 8) == 0)
  {
    if (WebPages_HandleAzRequest(request))
    {
      W5500_WebSendText(socket, "200 OK", "application/json", "{\"ok\":1}\n");
    }
    else
    {
      W5500_WebSendText(socket, "400 Bad Request", "application/json", "{\"ok\":0}\n");
    }
  }
  else if (strncmp(request, "GET /fwupdate", 13) == 0)
  {
    W5500_WebRedirect(socket, "Location: /#/firmware\r\n");
  }
  else if (strncmp(request, "GET /net_settings/data", 22) == 0)
  {
    W5500_WebSendSettingsData(socket);
  }
  else if (strncmp(request, "GET /net_settings/set?", 22) == 0)
  {
    if (WebPages_HandleNetSettingsRequest(request, 1U))
    {
      W5500_WebSendText(socket, "200 OK", "application/json", "{\"ok\":1}\n");
    }
    else
    {
      W5500_WebSendText(socket, "400 Bad Request", "application/json", "{\"ok\":0}\n");
    }
  }
  else if (strncmp(request, "GET /net_settings", 17) == 0)
  {
    W5500_WebRedirect(socket, "Location: /#/network\r\n");
  }
  else if (strncmp(request, "POST /fwupdate/start", 20) == 0)
  {
    W5500_WebSendText(socket, "200 OK", "application/json", "{\"ram_server\":1}\n");
    FW_UpdateRequestRamServer();
  }
  else if (strncmp(request, "GET /pwm?", 9) == 0)
  {
    if (WebPages_HandlePwmRequest(request))
    {
      W5500_WebSendText(socket, "200 OK", "application/json", "{\"ok\":1}\n");
    }
    else
    {
      W5500_WebSendText(socket, "400 Bad Request", "application/json", "{\"ok\":0}\n");
    }
  }
  else if (strncmp(request, "GET /reboot", 11) == 0)
  {
    W5500_WebSendText(socket, "200 OK", "application/json", "{\"reboot\":1}\n");
    HAL_Delay(50U);
    NVIC_SystemReset();
  }
  else
  {
    W5500_WebSendText(socket, "404 Not Found", "text/plain", "not found\n");
  }
}

static void W5500_JsonApiClose(uint8_t socket)
{
  (void)W5500_WriteSocketN8(socket, W5500_SN_CR, W5500_SN_CR_CLOSE);
  (void)W5500_WaitSocketNCommand(socket);
}

static void W5500_JsonApiOpenTcpListen(uint8_t socket, uint16_t port)
{
  uint8_t status;

  if (W5500_ReadSocketN8(socket, W5500_SN_SR, &status) != HAL_OK)
  {
    return;
  }

  if (status == W5500_SN_SR_CLOSED)
  {
    (void)W5500_WriteSocketN8(socket, W5500_SN_IR, 0xFFU);
    if ((W5500_WriteSocketN8(socket, W5500_SN_MR, W5500_SN_MR_TCP) == HAL_OK) &&
        (W5500_WriteSocketN16(socket, W5500_SN_PORT, port) == HAL_OK) &&
        (W5500_WriteSocketN8(socket, W5500_SN_CR, W5500_SN_CR_OPEN) == HAL_OK) &&
        (W5500_WaitSocketNCommand(socket) == HAL_OK))
    {
      (void)W5500_WriteSocketN8(socket, W5500_SN_CR, W5500_SN_CR_LISTEN);
      (void)W5500_WaitSocketNCommand(socket);
    }
  }
  else if (status == W5500_SN_SR_INIT)
  {
    (void)W5500_WriteSocketN8(socket, W5500_SN_CR, W5500_SN_CR_LISTEN);
    (void)W5500_WaitSocketNCommand(socket);
  }
}

static void W5500_JsonApiService(void)
{
  static char request[W5500_API_REQUEST_SIZE];
  static uint8_t active_enabled = 0U;
  static uint8_t active_mode = 0xFFU;
  static uint8_t tcp_connected = 0U;
  static uint8_t tcp_active_socket = W5500_API_SOCKET;
  static uint8_t tcp_listen_socket = W5500_API_LISTEN_SOCKET;
  static uint16_t active_port = 0U;
  static uint32_t tcp_last_rx_tick = 0U;
  const Settings_t *settings = Settings_Get();
  char response[384];
  uint8_t remote_ip[4];
  uint8_t status;
  uint8_t listen_status;
  uint16_t remote_port;
  uint16_t length;

  if ((settings->json_api_enabled == 0U) || (settings->json_api_port == 0U))
  {
    if (active_enabled != 0U)
    {
      W5500_JsonApiClose(W5500_API_SOCKET);
      W5500_JsonApiClose(W5500_API_LISTEN_SOCKET);
      active_enabled = 0U;
    }
    return;
  }

  if ((active_enabled == 0U) ||
      (active_mode != settings->json_api_mode) ||
      (active_port != settings->json_api_port))
  {
    W5500_JsonApiClose(W5500_API_SOCKET);
    W5500_JsonApiClose(W5500_API_LISTEN_SOCKET);
    active_enabled = 1U;
    active_mode = settings->json_api_mode;
    active_port = settings->json_api_port;
    tcp_active_socket = W5500_API_SOCKET;
    tcp_listen_socket = W5500_API_LISTEN_SOCKET;
    tcp_connected = 0U;
    tcp_last_rx_tick = HAL_GetTick();
    return;
  }

  if (active_mode == SETTINGS_JSON_API_MODE_UDP)
  {
    if (W5500_ReadSocketN8(W5500_API_SOCKET, W5500_SN_SR, &status) != HAL_OK)
    {
      return;
    }

    if (status == W5500_SN_SR_CLOSED)
    {
      (void)W5500_OpenUdpSocketN(W5500_API_SOCKET, active_port);
      return;
    }

    if (status != W5500_SN_SR_UDP)
    {
      W5500_JsonApiClose(W5500_API_SOCKET);
      W5500_JsonApiClose(W5500_API_LISTEN_SOCKET);
      tcp_connected = 0U;
      return;
    }

    if ((W5500_ReceiveUdpN(W5500_API_SOCKET, remote_ip, &remote_port, request, sizeof(request), &length) == HAL_OK) && (length > 0U))
    {
      WebPages_BuildApiReply(response, sizeof(response), WebPages_HandleApiJson(request));
      (void)W5500_SendUdpN(W5500_API_SOCKET, remote_ip, remote_port, (const uint8_t *)response, (uint16_t)strlen(response));
    }
    return;
  }

  if (W5500_ReadSocketN8(tcp_listen_socket, W5500_SN_SR, &listen_status) == HAL_OK)
  {
    if ((listen_status == W5500_SN_SR_ESTABLISHED) || (listen_status == W5500_SN_SR_CLOSE_WAIT))
    {
      uint8_t old_active_socket = tcp_active_socket;

      W5500_JsonApiClose(tcp_active_socket);
      tcp_active_socket = tcp_listen_socket;
      tcp_listen_socket = old_active_socket;
      tcp_connected = 1U;
      tcp_last_rx_tick = HAL_GetTick();
    }
    else
    {
      W5500_JsonApiOpenTcpListen(tcp_listen_socket, active_port);
    }
  }

  W5500_JsonApiOpenTcpListen(tcp_listen_socket, active_port);

  if (W5500_ReadSocketN8(tcp_active_socket, W5500_SN_SR, &status) != HAL_OK)
  {
    return;
  }

  if ((status == W5500_SN_SR_ESTABLISHED) || (status == W5500_SN_SR_CLOSE_WAIT))
  {
    if (tcp_connected == 0U)
    {
      tcp_connected = 1U;
      tcp_last_rx_tick = HAL_GetTick();
    }

    if ((W5500_ReceiveTcp(tcp_active_socket, request, sizeof(request), &length) == HAL_OK) && (length > 0U))
    {
      tcp_last_rx_tick = HAL_GetTick();
      WebPages_BuildApiReply(response, sizeof(response), WebPages_HandleApiJson(request));
      (void)W5500_SendTcp(tcp_active_socket, (const uint8_t *)response, (uint16_t)strlen(response));
    }
    else if ((status == W5500_SN_SR_CLOSE_WAIT) || ((HAL_GetTick() - tcp_last_rx_tick) >= 30000UL))
    {
      W5500_JsonApiClose(tcp_active_socket);
      tcp_connected = 0U;
    }
  }
}

void W5500_WebService(void)
{
  static char request[W5500_WEB_REQUEST_SIZE];
  static uint16_t request_length = 0U;
  static uint8_t connected = 0U;
  static uint32_t connected_tick = 0U;
  uint8_t status;
  uint16_t length;

  if ((w5500_spi == NULL) || (w5500_uart == NULL))
  {
    return;
  }

  W5500_JsonApiService();

  if (W5500_ReadSocketN8(W5500_WEB_SOCKET, W5500_SN_SR, &status) != HAL_OK)
  {
    return;
  }

  if (status == W5500_SN_SR_CLOSED)
  {
    (void)W5500_WriteSocketN8(W5500_WEB_SOCKET, W5500_SN_IR, 0xFFU);
    if ((W5500_WriteSocketN8(W5500_WEB_SOCKET, W5500_SN_MR, W5500_SN_MR_TCP) == HAL_OK) &&
        (W5500_WriteSocketN16(W5500_WEB_SOCKET, W5500_SN_PORT, W5500_WEB_PORT) == HAL_OK) &&
        (W5500_WriteSocketN8(W5500_WEB_SOCKET, W5500_SN_CR, W5500_SN_CR_OPEN) == HAL_OK) &&
        (W5500_WaitSocketNCommand(W5500_WEB_SOCKET) == HAL_OK))
    {
      (void)W5500_WriteSocketN8(W5500_WEB_SOCKET, W5500_SN_CR, W5500_SN_CR_LISTEN);
      (void)W5500_WaitSocketNCommand(W5500_WEB_SOCKET);
    }
  }
  else if (status == W5500_SN_SR_INIT)
  {
    (void)W5500_WriteSocketN8(W5500_WEB_SOCKET, W5500_SN_CR, W5500_SN_CR_LISTEN);
    (void)W5500_WaitSocketNCommand(W5500_WEB_SOCKET);
  }
  else if ((status == W5500_SN_SR_ESTABLISHED) || (status == W5500_SN_SR_CLOSE_WAIT))
  {
    uint8_t complete;

    if (connected == 0U)
    {
      connected = 1U;
      connected_tick = HAL_GetTick();
      request_length = 0U;
      request[0] = '\0';
    }

    /* The request may arrive in several TCP segments: collect it until the blank line that ends the headers. */
    if ((W5500_ReceiveTcp(W5500_WEB_SOCKET, &request[request_length], (uint16_t)(sizeof(request) - request_length), &length) == HAL_OK) &&
        (length > 0U))
    {
      request_length = (uint16_t)(request_length + length);
    }
    complete = ((strstr(request, "\r\n\r\n") != NULL) || (request_length >= (sizeof(request) - 1U))) ? 1U : 0U;

    if ((complete != 0U) || ((request_length > 0U) && (status == W5500_SN_SR_CLOSE_WAIT)))
    {
      W5500_WebHandleRequest(W5500_WEB_SOCKET, request);
      W5500_WebClose(W5500_WEB_SOCKET);
      connected = 0U;
    }
    else if ((status == W5500_SN_SR_CLOSE_WAIT) || ((HAL_GetTick() - connected_tick) >= W5500_WEB_IDLE_TIMEOUT_MS))
    {
      /* Without this an idle connection (browser pre-connect, a phone that left Wi-Fi) holds the only web socket forever. */
      W5500_WebClose(W5500_WEB_SOCKET);
      connected = 0U;
    }
  }
  else
  {
    connected = 0U;
  }
}

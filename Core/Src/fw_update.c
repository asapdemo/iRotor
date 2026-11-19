#include "fw_update.h"

#include <stdint.h>

#define FW_RAM __attribute__((section(".RamFunc"), noinline))
#define FW_RAM_DATA __attribute__((section(".RamData")))
#define FW_UpdateFeedWatchdog() (IWDG->KR = LL_IWDG_KEY_RELOAD)
#define FW_IsDigit(ch) (((uint8_t)(ch) >= (uint8_t)'0') && ((uint8_t)(ch) <= (uint8_t)'9'))
#define FW_IsHex(ch) ((((uint8_t)(ch) >= (uint8_t)'0') && ((uint8_t)(ch) <= (uint8_t)'9')) || \
                      (((uint8_t)(ch) >= (uint8_t)'a') && ((uint8_t)(ch) <= (uint8_t)'f')) || \
                      (((uint8_t)(ch) >= (uint8_t)'A') && ((uint8_t)(ch) <= (uint8_t)'F')))
#define FW_HexValue(ch) (((uint8_t)(ch) <= (uint8_t)'9') ? ((uint8_t)(ch) - (uint8_t)'0') : \
                         (((uint8_t)(ch) >= (uint8_t)'a') ? ((uint8_t)(ch) - (uint8_t)'a' + 10U) : ((uint8_t)(ch) - (uint8_t)'A' + 10U)))
#define FW_SocketRegBlock(socket) ((uint8_t)(W5500_SOCKET0_REG_BLOCK + ((socket) * W5500_SOCKET_BLOCK_STEP)))
#define FW_SocketTxBlock(socket) ((uint8_t)(W5500_SOCKET0_TX_BLOCK + ((socket) * W5500_SOCKET_BLOCK_STEP)))
#define FW_SocketRxBlock(socket) ((uint8_t)(W5500_SOCKET0_RX_BLOCK + ((socket) * W5500_SOCKET_BLOCK_STEP)))
#define FW_SocketRead8(socket, address, data) FW_Block((address), (uint8_t)(FW_SocketRegBlock(socket) | W5500_COMMON_READ), (data), 1U, 0U)
#define FW_SocketWrite8(socket, address, data) FW_Block((address), (uint8_t)(FW_SocketRegBlock(socket) | W5500_COMMON_WRITE), (uint8_t[]){(uint8_t)(data)}, 1U, 1U)

#define W5500_SPI_TIMEOUT_LOOPS 200000UL
#define W5500_UPDATE_SOCKET 1U
#define W5500_UPDATE_PORT 80U
#define W5500_COMMON_READ 0x00U
#define W5500_COMMON_WRITE 0x04U
#define W5500_SOCKET0_REG_BLOCK 0x08U
#define W5500_SOCKET0_TX_BLOCK 0x10U
#define W5500_SOCKET0_RX_BLOCK 0x18U
#define W5500_SOCKET_BLOCK_STEP 0x20U
#define W5500_SOCKET_BUFFER_SIZE 2048U
#define W5500_SOCKET_BUFFER_MASK (W5500_SOCKET_BUFFER_SIZE - 1U)
#define W5500_SN_MR 0x0000U
#define W5500_SN_CR 0x0001U
#define W5500_SN_IR 0x0002U
#define W5500_SN_SR 0x0003U
#define W5500_SN_PORT 0x0004U
#define W5500_SN_TX_FSR 0x0020U
#define W5500_SN_TX_WR 0x0024U
#define W5500_SN_RX_RSR 0x0026U
#define W5500_SN_RX_RD 0x0028U
#define W5500_SN_MR_TCP 0x01U
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

#define FW_UPDATE_RAM_BUFFER_SIZE 1536U
#define FW_UPDATE_NVIC_ICER_COUNT 8U
#define FW_UPDATE_MAX_REQUEST_SIZE FW_UPDATE_RAM_BUFFER_SIZE

typedef enum
{
  FW_UPDATE_ROUTE_NONE = 0,
  FW_UPDATE_ROUTE_CHUNK,
  FW_UPDATE_ROUTE_FINISH
} FW_UpdateRoute_t;

static SPI_HandleTypeDef *fw_update_spi;
static volatile uint8_t fw_update_ram_server_requested;
static uint8_t fw_update_request_buffer[FW_UPDATE_RAM_BUFFER_SIZE];

static uint8_t fw_http_ok[] FW_RAM_DATA =
    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nCache-Control: no-store\r\nContent-Length: 8\r\n\r\n{\"ok\":1}\n";
static uint8_t fw_http_err[] FW_RAM_DATA =
    "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nConnection: close\r\nCache-Control: no-store\r\nContent-Length: 9\r\n\r\n{\"ok\":0}\n";
static uint8_t fw_http_done[] FW_RAM_DATA =
    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nCache-Control: no-store\r\nContent-Length: 11\r\n\r\n{\"done\":1}\n";
static uint8_t fw_route_chunk[] FW_RAM_DATA = "POST /chunk";
static uint8_t fw_route_finish[] FW_RAM_DATA = "POST /finish";
static uint8_t fw_header_end[] FW_RAM_DATA = "\r\n\r\n";
static uint8_t fw_header_content_length[] FW_RAM_DATA = "Content-Length:";
static uint8_t fw_header_content_length_lc[] FW_RAM_DATA = "content-length:";
static uint8_t fw_query_off[] FW_RAM_DATA = "off";
static uint8_t fw_query_len[] FW_RAM_DATA = "len";
static uint8_t fw_query_crc[] FW_RAM_DATA = "crc";

void FW_UpdateInit(SPI_HandleTypeDef *spi)
{
  fw_update_spi = spi;
  fw_update_ram_server_requested = 0U;
}

void FW_UpdateRequestRamServer(void)
{
  fw_update_ram_server_requested = 1U;
}

uint8_t FW_UpdateIsRamServerRequested(void)
{
  return fw_update_ram_server_requested;
}

static FW_RAM void FW_UpdateDelay(volatile uint32_t loops)
{
  while (loops-- != 0U)
  {
    FW_UpdateFeedWatchdog();
    __NOP();
  }
}

static FW_RAM uint16_t FW_StrLen(const uint8_t *text)
{
  uint16_t length = 0U;

  while (text[length] != 0U)
  {
    length++;
  }

  return length;
}

static FW_RAM uint8_t FW_MemEq(const uint8_t *a, const uint8_t *b, uint16_t length)
{
  uint16_t index;

  for (index = 0U; index < length; index++)
  {
    if (a[index] != (uint8_t)b[index])
    {
      return 0U;
    }
  }

  return 1U;
}

static FW_RAM uint32_t FW_ParseDec(const uint8_t *text)
{
  uint32_t value = 0U;

  while (FW_IsDigit(*text) != 0U)
  {
    value = (value * 10UL) + (uint32_t)(*text - (uint8_t)'0');
    text++;
  }

  return value;
}

static FW_RAM uint32_t FW_ParseHex(const uint8_t *text)
{
  uint32_t value = 0U;

  if ((text[0] == (uint8_t)'0') && ((text[1] == (uint8_t)'x') || (text[1] == (uint8_t)'X')))
  {
    text += 2;
  }

  while (FW_IsHex(*text) != 0U)
  {
    value = (value << 4) | FW_HexValue(*text);
    text++;
  }

  return value;
}

static FW_RAM const uint8_t *FW_FindPattern(const uint8_t *data, uint16_t length, const uint8_t *pattern, uint16_t pattern_length)
{
  uint16_t index;

  if (length < pattern_length)
  {
    return 0;
  }

  for (index = 0U; index <= (uint16_t)(length - pattern_length); index++)
  {
    if (FW_MemEq(&data[index], pattern, pattern_length) != 0U)
    {
      return &data[index];
    }
  }

  return 0;
}

static FW_RAM const uint8_t *FW_QueryValue(const uint8_t *request, uint16_t length, const uint8_t *name, uint16_t name_length)
{
  uint16_t index;

  if (length < (uint16_t)(name_length + 1U))
  {
    return 0;
  }

  for (index = 0U; index <= (uint16_t)(length - name_length - 1U); index++)
  {
    if (((request[index] == (uint8_t)'?') || (request[index] == (uint8_t)'&')) &&
        (FW_MemEq(&request[index + 1U], name, name_length) != 0U) &&
        (request[index + 1U + name_length] == (uint8_t)'='))
    {
      return &request[index + 2U + name_length];
    }
  }

  return 0;
}

static FW_RAM uint8_t FW_SpiWaitFlag(uint32_t flag, uint8_t state)
{
  uint32_t loops = W5500_SPI_TIMEOUT_LOOPS;

  while (((fw_update_spi->Instance->SR & flag) != 0U) != (state != 0U))
  {
    FW_UpdateFeedWatchdog();
    if (--loops == 0U)
    {
      return 0U;
    }
  }

  return 1U;
}

static FW_RAM uint8_t FW_SpiByte(uint8_t tx, uint8_t *rx)
{
  uint8_t value;

  fw_update_spi->Instance->CR1 |= SPI_CR1_SPE;
  if (FW_SpiWaitFlag(SPI_FLAG_TXE, 1U) == 0U) return 0U;
  *(__IO uint8_t *)&fw_update_spi->Instance->DR = tx;
  if (FW_SpiWaitFlag(SPI_FLAG_RXNE, 1U) == 0U) return 0U;
  value = *(__IO uint8_t *)&fw_update_spi->Instance->DR;
  if (rx != 0)
  {
    *rx = value;
  }

  return 1U;
}

static FW_RAM uint8_t FW_W5500Write(const uint8_t *data, uint16_t length)
{
  uint16_t index;

  for (index = 0U; index < length; index++)
  {
    if (FW_SpiByte(data[index], 0) == 0U)
    {
      return 0U;
    }
  }

  return FW_SpiWaitFlag(SPI_FLAG_BSY, 0U);
}

static FW_RAM uint8_t FW_W5500Read(uint8_t *data, uint16_t length)
{
  uint16_t index;

  for (index = 0U; index < length; index++)
  {
    if (FW_SpiByte(0xFFU, &data[index]) == 0U)
    {
      return 0U;
    }
  }

  return FW_SpiWaitFlag(SPI_FLAG_BSY, 0U);
}

static FW_RAM uint8_t FW_Block(uint16_t address, uint8_t control, uint8_t *data, uint16_t length, uint8_t write)
{
  uint8_t header[3];
  uint8_t ok;

  header[0] = (uint8_t)(address >> 8);
  header[1] = (uint8_t)address;
  header[2] = (uint8_t)(control & 0xFCU);
  if (write != 0U)
  {
    header[2] |= W5500_COMMON_WRITE;
  }

  W5500_CS_GPIO_Port->BRR = W5500_CS_Pin;
  ok = FW_W5500Write(header, sizeof(header));
  if (ok != 0U)
  {
    ok = (write != 0U) ? FW_W5500Write(data, length) : FW_W5500Read(data, length);
  }
  W5500_CS_GPIO_Port->BSRR = W5500_CS_Pin;

  return ok;
}

static FW_RAM uint8_t FW_SocketRead16(uint8_t socket, uint16_t address, uint16_t *data)
{
  uint8_t bytes[2];

  if (FW_Block(address, (uint8_t)(FW_SocketRegBlock(socket) | W5500_COMMON_READ), bytes, 2U, 0U) == 0U)
  {
    return 0U;
  }
  *data = (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
  return 1U;
}

static FW_RAM uint8_t FW_SocketWrite16(uint8_t socket, uint16_t address, uint16_t data)
{
  uint8_t bytes[2];

  bytes[0] = (uint8_t)(data >> 8);
  bytes[1] = (uint8_t)data;
  return FW_Block(address, (uint8_t)(FW_SocketRegBlock(socket) | W5500_COMMON_WRITE), bytes, 2U, 1U);
}

static FW_RAM uint8_t FW_WaitSocketCommand(uint8_t socket)
{
  uint8_t command;
  uint32_t loops = W5500_SPI_TIMEOUT_LOOPS;

  do
  {
    if (FW_SocketRead8(socket, W5500_SN_CR, &command) == 0U) return 0U;
    if (command == 0U) return 1U;
    FW_UpdateFeedWatchdog();
  } while (--loops != 0U);

  return 0U;
}

static FW_RAM uint8_t FW_WriteTx(uint8_t socket, uint16_t address, const uint8_t *data, uint16_t length)
{
  uint16_t offset = address & W5500_SOCKET_BUFFER_MASK;
  uint16_t first = (uint16_t)(W5500_SOCKET_BUFFER_SIZE - offset);

  if (first > length) first = length;
  if (FW_Block(offset, (uint8_t)(FW_SocketTxBlock(socket) | W5500_COMMON_WRITE), (uint8_t *)data, first, 1U) == 0U) return 0U;
  if (first < length)
  {
    return FW_Block(0U, (uint8_t)(FW_SocketTxBlock(socket) | W5500_COMMON_WRITE), (uint8_t *)&data[first], (uint16_t)(length - first), 1U);
  }
  return 1U;
}

static FW_RAM uint8_t FW_ReadRx(uint8_t socket, uint16_t address, uint8_t *data, uint16_t length)
{
  uint16_t offset = address & W5500_SOCKET_BUFFER_MASK;
  uint16_t first = (uint16_t)(W5500_SOCKET_BUFFER_SIZE - offset);

  if (first > length) first = length;
  if (FW_Block(offset, (uint8_t)(FW_SocketRxBlock(socket) | W5500_COMMON_READ), data, first, 0U) == 0U) return 0U;
  if (first < length)
  {
    return FW_Block(0U, (uint8_t)(FW_SocketRxBlock(socket) | W5500_COMMON_READ), &data[first], (uint16_t)(length - first), 0U);
  }
  return 1U;
}

static FW_RAM uint8_t FW_SendTcp(uint8_t socket, const uint8_t *data, uint16_t length)
{
  uint8_t ir;
  uint16_t tx_free;
  uint16_t tx_wr;
  uint32_t loops = W5500_SPI_TIMEOUT_LOOPS;

  do
  {
    if (FW_SocketRead16(socket, W5500_SN_TX_FSR, &tx_free) == 0U) return 0U;
    FW_UpdateFeedWatchdog();
  } while ((tx_free < length) && (--loops != 0U));

  if (tx_free < length) return 0U;
  if ((FW_SocketRead16(socket, W5500_SN_TX_WR, &tx_wr) == 0U) ||
      (FW_WriteTx(socket, tx_wr, data, length) == 0U) ||
      (FW_SocketWrite16(socket, W5500_SN_TX_WR, (uint16_t)(tx_wr + length)) == 0U) ||
      (FW_SocketWrite8(socket, W5500_SN_CR, W5500_SN_CR_SEND) == 0U) ||
      (FW_WaitSocketCommand(socket) == 0U)) return 0U;

  loops = W5500_SPI_TIMEOUT_LOOPS;
  do
  {
    if (FW_SocketRead8(socket, W5500_SN_IR, &ir) == 0U) return 0U;
    if ((ir & W5500_SN_IR_SENDOK) != 0U)
    {
      (void)FW_SocketWrite8(socket, W5500_SN_IR, W5500_SN_IR_SENDOK);
      return 1U;
    }
    if ((ir & W5500_SN_IR_TIMEOUT) != 0U)
    {
      (void)FW_SocketWrite8(socket, W5500_SN_IR, W5500_SN_IR_TIMEOUT);
      return 0U;
    }
    FW_UpdateFeedWatchdog();
  } while (--loops != 0U);

  return 0U;
}

static FW_RAM void FW_Close(uint8_t socket)
{
  (void)FW_SocketWrite8(socket, W5500_SN_CR, W5500_SN_CR_DISCON);
  (void)FW_WaitSocketCommand(socket);
  (void)FW_SocketWrite8(socket, W5500_SN_CR, W5500_SN_CR_CLOSE);
  (void)FW_WaitSocketCommand(socket);
}

static FW_RAM void FW_Listen(uint8_t socket)
{
  uint8_t status;

  if (FW_SocketRead8(socket, W5500_SN_SR, &status) == 0U) return;
  if (status == W5500_SN_SR_CLOSED)
  {
    (void)FW_SocketWrite8(socket, W5500_SN_IR, 0xFFU);
    (void)FW_SocketWrite8(socket, W5500_SN_MR, W5500_SN_MR_TCP);
    (void)FW_SocketWrite16(socket, W5500_SN_PORT, W5500_UPDATE_PORT);
    (void)FW_SocketWrite8(socket, W5500_SN_CR, W5500_SN_CR_OPEN);
    (void)FW_WaitSocketCommand(socket);
    (void)FW_SocketWrite8(socket, W5500_SN_CR, W5500_SN_CR_LISTEN);
    (void)FW_WaitSocketCommand(socket);
  }
  else if (status == W5500_SN_SR_INIT)
  {
    (void)FW_SocketWrite8(socket, W5500_SN_CR, W5500_SN_CR_LISTEN);
    (void)FW_WaitSocketCommand(socket);
  }
}

static FW_RAM uint16_t FW_ReceiveRequest(uint8_t socket, uint8_t *buffer, uint16_t max_length)
{
  uint16_t rx_size;
  uint16_t rx_rd;
  uint16_t read_size;

  if ((FW_SocketRead16(socket, W5500_SN_RX_RSR, &rx_size) == 0U) || (rx_size == 0U))
  {
    return 0U;
  }

  read_size = rx_size;
  if (read_size >= max_length)
  {
    read_size = (uint16_t)(max_length - 1U);
  }

  if ((FW_SocketRead16(socket, W5500_SN_RX_RD, &rx_rd) == 0U) ||
      (FW_ReadRx(socket, rx_rd, buffer, read_size) == 0U))
  {
    return 0U;
  }

  buffer[read_size] = 0U;
  rx_rd = (uint16_t)(rx_rd + rx_size);
  (void)FW_SocketWrite16(socket, W5500_SN_RX_RD, rx_rd);
  (void)FW_SocketWrite8(socket, W5500_SN_CR, W5500_SN_CR_RECV);
  (void)FW_WaitSocketCommand(socket);
  return read_size;
}

static FW_RAM uint16_t FW_GetCompleteRequest(uint8_t socket, uint8_t *buffer, uint16_t max_length)
{
  uint16_t total = 0U;
  uint32_t loops = W5500_SPI_TIMEOUT_LOOPS;

  while (loops-- != 0U)
  {
    uint16_t got = FW_ReceiveRequest(socket, &buffer[total], (uint16_t)(max_length - total));
    const uint8_t *header_end;
    uint32_t content_length = 0U;
    const uint8_t *cl;

    if (got != 0U)
    {
      total = (uint16_t)(total + got);
      if (total >= (max_length - 1U))
      {
        return total;
      }

      header_end = FW_FindPattern(buffer, total, fw_header_end, 4U);
      if (header_end != 0)
      {
        cl = FW_FindPattern(buffer, total, fw_header_content_length, 15U);
        if (cl == 0)
        {
          cl = FW_FindPattern(buffer, total, fw_header_content_length_lc, 15U);
        }
        if (cl != 0)
        {
          cl += 15;
          while ((*cl == (uint8_t)' ') || (*cl == (uint8_t)'\t')) cl++;
          content_length = FW_ParseDec(cl);
        }
        if ((uint32_t)(total - (uint16_t)((header_end + 4) - buffer)) >= content_length)
        {
          return total;
        }
      }
    }

    FW_UpdateFeedWatchdog();
  }

  return total;
}

static FW_RAM uint8_t FW_FlashWait(void)
{
  uint32_t loops = 800000UL;

  while ((FLASH->SR & FLASH_SR_BSY) != 0U)
  {
    FW_UpdateFeedWatchdog();
    if (--loops == 0U)
    {
      return 0U;
    }
  }

  if ((FLASH->SR & (FLASH_SR_PGERR | FLASH_SR_WRPRTERR)) != 0U)
  {
    FLASH->SR = FLASH_SR_PGERR | FLASH_SR_WRPRTERR;
    return 0U;
  }

  return 1U;
}

static FW_RAM void FW_FlashUnlock(void)
{
  if ((FLASH->CR & FLASH_CR_LOCK) != 0U)
  {
    FLASH->KEYR = FLASH_KEY1;
    FLASH->KEYR = FLASH_KEY2;
  }
}

static FW_RAM uint8_t FW_FlashErasePage(uint32_t address)
{
  if (FW_FlashWait() == 0U) return 0U;
  FLASH->CR |= FLASH_CR_PER;
  FLASH->AR = address;
  FLASH->CR |= FLASH_CR_STRT;
  if (FW_FlashWait() == 0U) return 0U;
  FLASH->CR &= ~FLASH_CR_PER;
  return 1U;
}

static FW_RAM uint8_t FW_FlashProgramHalfWord(uint32_t address, uint16_t data)
{
  if (FW_FlashWait() == 0U) return 0U;
  FLASH->CR |= FLASH_CR_PG;
  *(__IO uint16_t *)address = data;
  if (FW_FlashWait() == 0U) return 0U;
  FLASH->CR &= ~FLASH_CR_PG;
  return ((*(__IO uint16_t *)address == data) ? 1U : 0U);
}

static FW_RAM uint32_t FW_CrcFlash(uint32_t address, uint32_t length)
{
  uint32_t index;
  uint32_t words = (length + 3UL) / 4UL;
  __IO uint32_t *flash = (__IO uint32_t *)address;

  CRC->CR = CRC_CR_RESET;
  for (index = 0U; index < words; index++)
  {
    CRC->DR = flash[index];
    FW_UpdateFeedWatchdog();
  }

  return CRC->DR;
}

static FW_RAM uint8_t FW_WriteChunk(uint32_t offset, const uint8_t *data, uint32_t length, uint32_t expected_crc)
{
  uint32_t address = FW_UPDATE_FLASH_BASE + offset;
  uint32_t index;

  if ((offset >= FW_UPDATE_FLASH_SIZE) ||
      (length == 0U) ||
      (length > FW_UPDATE_CHUNK_SIZE) ||
      ((offset % FW_UPDATE_FLASH_PAGE_SIZE) != 0U) ||
      ((length % 4U) != 0U) ||
      ((offset + length) > FW_UPDATE_FLASH_SIZE))
  {
    return 0U;
  }

  if (FW_FlashErasePage(address) == 0U)
  {
    return 0U;
  }

  for (index = 0U; index < length; index += 2U)
  {
    uint16_t halfword = (uint16_t)data[index] | ((uint16_t)data[index + 1U] << 8);
    if (FW_FlashProgramHalfWord(address + index, halfword) == 0U)
    {
      return 0U;
    }
  }

  return (FW_CrcFlash(address, length) == expected_crc) ? 1U : 0U;
}

static FW_RAM FW_UpdateRoute_t FW_ParseRoute(const uint8_t *request, uint16_t length)
{
  if ((length >= 12U) && FW_MemEq(request, fw_route_chunk, 11U)) return FW_UPDATE_ROUTE_CHUNK;
  if ((length >= 13U) && FW_MemEq(request, fw_route_finish, 12U)) return FW_UPDATE_ROUTE_FINISH;
  return FW_UPDATE_ROUTE_NONE;
}

static FW_RAM uint8_t FW_HandleChunk(const uint8_t *request, uint16_t length)
{
  const uint8_t *offset_text = FW_QueryValue(request, length, fw_query_off, 3U);
  const uint8_t *length_text = FW_QueryValue(request, length, fw_query_len, 3U);
  const uint8_t *crc_text = FW_QueryValue(request, length, fw_query_crc, 3U);
  const uint8_t *body = FW_FindPattern(request, length, fw_header_end, 4U);
  uint32_t offset;
  uint32_t chunk_length;
  uint32_t expected_crc;

  if ((offset_text == 0) || (length_text == 0) || (crc_text == 0) || (body == 0))
  {
    return 0U;
  }

  body += 4;
  offset = FW_ParseDec(offset_text);
  chunk_length = FW_ParseDec(length_text);
  expected_crc = FW_ParseHex(crc_text);
  if ((uint32_t)(length - (uint16_t)(body - request)) < chunk_length)
  {
    return 0U;
  }

  return FW_WriteChunk(offset, body, chunk_length, expected_crc);
}

static FW_RAM void FW_DisableAllIrq(void)
{
  uint8_t index;

  __disable_irq();
  SysTick->CTRL = 0U;
  for (index = 0U; index < FW_UPDATE_NVIC_ICER_COUNT; index++)
  {
    NVIC->ICER[index] = 0xFFFFFFFFUL;
    NVIC->ICPR[index] = 0xFFFFFFFFUL;
  }
  __DSB();
  __ISB();
}

static FW_RAM void FW_Reset(void)
{
  __DSB();
  SCB->AIRCR = (0x5FAUL << SCB_AIRCR_VECTKEY_Pos) | SCB_AIRCR_SYSRESETREQ_Msk;
  while (1)
  {
  }
}

FW_RAM void FW_UpdateEnterRamServer(void)
{
  FW_UpdateRoute_t route;
  uint16_t length;
  uint8_t status;

  FW_DisableAllIrq();
  FW_FlashUnlock();
  FW_Close(W5500_UPDATE_SOCKET);
  FW_Listen(W5500_UPDATE_SOCKET);

  while (1)
  {
    FW_UpdateFeedWatchdog();
    FW_Listen(W5500_UPDATE_SOCKET);
    if (FW_SocketRead8(W5500_UPDATE_SOCKET, W5500_SN_SR, &status) == 0U)
    {
      continue;
    }

    if ((status == W5500_SN_SR_ESTABLISHED) || (status == W5500_SN_SR_CLOSE_WAIT))
    {
      length = FW_GetCompleteRequest(W5500_UPDATE_SOCKET, fw_update_request_buffer, FW_UPDATE_MAX_REQUEST_SIZE);
      route = FW_ParseRoute(fw_update_request_buffer, length);

      if ((route == FW_UPDATE_ROUTE_CHUNK) && (FW_HandleChunk(fw_update_request_buffer, length) != 0U))
      {
        (void)FW_SendTcp(W5500_UPDATE_SOCKET, fw_http_ok, FW_StrLen(fw_http_ok));
      }
      else if (route == FW_UPDATE_ROUTE_FINISH)
      {
        (void)FW_SendTcp(W5500_UPDATE_SOCKET, fw_http_done, FW_StrLen(fw_http_done));
        FW_Close(W5500_UPDATE_SOCKET);
        FW_UpdateDelay(400000UL);
        FW_Reset();
      }
      else
      {
        (void)FW_SendTcp(W5500_UPDATE_SOCKET, fw_http_err, FW_StrLen(fw_http_err));
      }

      FW_Close(W5500_UPDATE_SOCKET);
    }
  }
}

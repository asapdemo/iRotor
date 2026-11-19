#include "uart_commands.h"
#include "fw_info.h"
#include "settings_service.h"
#include "w5500.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum
{
  CMD_NONE = 0,
  CMD_PING,
  CMD_VERSION,
  CMD_CLOCK_INFO,
  CMD_WSTATUS,
  CMD_W_DHCP,
  CMD_IP_INFO,
  CMD_SET_DHCP,
  CMD_SET_AUTH,
  CMD_SET_IP,
  CMD_SET_MASK,
  CMD_SET_GW,
  CMD_SET_DNS,
  CMD_SET_MAC,
  CMD_SET_PASSWORD,
  CMD_SAVE,
  CMD_LOAD,
  CMD_GET_SETTINGS,
  CMD_HELP,
  CMD_REBOOT,
  CMD_WDTTEST,
  CMD_UNKNOWN
} Command_t;

#define UART_COMMAND_RX_BUFFER_SIZE 64U
#define UART_COMMAND_DMA_BUFFER_SIZE 128U
#define UART_CommandSendString(text) HAL_UART_Transmit(uart_command_uart, (uint8_t *)(text), (uint16_t)strlen(text), HAL_MAX_DELAY)
#define UART_COMMAND_IS_SPACE(ch) ((uint8_t)(ch) <= (uint8_t)' ')
#define UART_COMMAND_IS_PRINTABLE(ch) (((uint8_t)(ch) >= (uint8_t)' ') && ((uint8_t)(ch) <= (uint8_t)'~'))

static UART_HandleTypeDef *uart_command_uart;
static uint8_t uart_command_dma_buffer[UART_COMMAND_DMA_BUFFER_SIZE];
static char uart_command_buffer[UART_COMMAND_RX_BUFFER_SIZE];
static char pending_command_buffer[UART_COMMAND_RX_BUFFER_SIZE];
static uint16_t uart_command_length;
static uint16_t uart_command_dma_read_pos;
static volatile uint8_t pending_command_ready;

static void UART_CommandSendFormatted(const char *format, ...)
{
  char response[512];
  va_list args;
  int length;

  va_start(args, format);
  length = vsnprintf(response, sizeof(response), format, args);
  va_end(args);
  if (length > 0)
  {
    if (length >= (int)sizeof(response)) length = (int)sizeof(response) - 1;
    HAL_UART_Transmit(uart_command_uart, (uint8_t *)response, (uint16_t)length, HAL_MAX_DELAY);
  }
}

static void UART_CommandSendHex(const char *command)
{
  uint16_t index = 0U;

  UART_CommandSendString("rx hex:");
  while ((command[index] != '\0') && (index < UART_COMMAND_RX_BUFFER_SIZE))
  {
    UART_CommandSendFormatted(" %02X", (uint8_t)command[index]);
    index++;
  }
  UART_CommandSendString("\r\n");
}

static char *UART_CommandSkipSpaces(char *text)
{
  while ((*text != '\0') && UART_COMMAND_IS_SPACE(*text))
  {
    text++;
  }

  return text;
}

static uint8_t UART_CommandParseBool(char *args, uint8_t *value)
{
  char *end;
  uint32_t parsed;

  args = UART_CommandSkipSpaces(args);
  parsed = strtoul(args, &end, 10);
  if ((end == args) || (parsed > 1U))
  {
    return 0U;
  }

  *value = (uint8_t)parsed;
  return 1U;
}

static uint8_t UART_CommandParseByte(char **text, uint8_t base, uint8_t *value)
{
  char *end;
  uint32_t parsed;

  *text = UART_CommandSkipSpaces(*text);
  parsed = strtoul(*text, &end, base);
  if ((end == *text) || (parsed > 255U))
  {
    return 0U;
  }

  *value = (uint8_t)parsed;
  *text = end;
  return 1U;
}

static uint8_t UART_CommandParseDecimalByte(char **text, uint8_t *value)
{
  uint16_t parsed = 0U;
  uint8_t digits = 0U;

  *text = UART_CommandSkipSpaces(*text);
  while ((**text >= '0') && (**text <= '9'))
  {
    parsed = (uint16_t)((parsed * 10U) + (uint16_t)(**text - '0'));
    if (parsed > 255U)
    {
      return 0U;
    }

    (*text)++;
    digits++;
  }

  if (digits == 0U)
  {
    return 0U;
  }

  *value = (uint8_t)parsed;
  return 1U;
}

static uint8_t UART_CommandParseIp(char *args, uint8_t ip[4])
{
  uint8_t index;

  args = UART_CommandSkipSpaces(args);
  for (index = 0U; index < 4U; index++)
  {
    if (UART_CommandParseDecimalByte(&args, &ip[index]) == 0U)
    {
      return 0U;
    }

    if (index < 3U)
    {
      if (*args != '.')
      {
        return 0U;
      }
      args++;
    }
  }

  args = UART_CommandSkipSpaces(args);
  return (*args == '\0') ? 1U : 0U;
}

static uint8_t UART_CommandParseMac(char *args, uint8_t mac[6])
{
  uint8_t index;
  uint8_t all_zero = 1U;
  uint8_t all_ff = 1U;

  args = UART_CommandSkipSpaces(args);
  for (index = 0U; index < 6U; index++)
  {
    if (UART_CommandParseByte(&args, 16U, &mac[index]) == 0U)
    {
      return 0U;
    }

    if (mac[index] != 0x00U) all_zero = 0U;
    if (mac[index] != 0xFFU) all_ff = 0U;

    if (index < 5U)
    {
      if ((*args != ':') && (*args != '-'))
      {
        return 0U;
      }
      args++;
    }
  }

  args = UART_CommandSkipSpaces(args);
  if ((*args != '\0') || ((mac[0] & 0x01U) != 0U) || (all_zero != 0U) || (all_ff != 0U))
  {
    return 0U;
  }

  return 1U;
}

static uint8_t UART_CommandParsePassword(char *args, char password[SETTINGS_WEB_PASSWORD_LENGTH])
{
  uint8_t index = 0U;

  args = UART_CommandSkipSpaces(args);
  while (args[index] != '\0')
  {
    char symbol = args[index];

    if ((symbol < '!') || (symbol > '~') || (symbol == '"') || (symbol == '\\') || (index >= (SETTINGS_WEB_PASSWORD_LENGTH - 1U)))
    {
      return 0U;
    }
    password[index] = symbol;
    index++;
  }

  if (index == 0U)
  {
    return 0U;
  }

  password[index] = '\0';
  return 1U;
}

static void UART_CommandSetBoolSetting(char *args, uint8_t auth)
{
  Settings_t settings = *Settings_Get();
  uint8_t value;

  if (UART_CommandParseBool(args, &value) == 0U)
  {
    UART_CommandSendString(auth ? "usage: set_auth 0/1\r\n" : "usage: set_dhcp 0/1\r\n");
    return;
  }

  if (auth != 0U)
  {
    settings.web_auth_enabled = value;
  }
  else
  {
    settings.use_dhcp = value;
  }

  Settings_Set(&settings);
  UART_CommandSendFormatted("%s=%u\r\n", auth ? "web_auth" : "dhcp", value);
}

static void UART_CommandSetIpSetting(char *args, uint8_t field)
{
  Settings_t settings = *Settings_Get();
  uint8_t ip[4];

  if (UART_CommandParseIp(args, ip) == 0U)
  {
    switch (field)
    {
      case 0U: UART_CommandSendString("usage: set_ip A.B.C.D\r\n"); break;
      case 1U: UART_CommandSendString("usage: set_mask A.B.C.D\r\n"); break;
      case 2U: UART_CommandSendString("usage: set_gw A.B.C.D\r\n"); break;
      default: UART_CommandSendString("usage: set_dns A.B.C.D\r\n"); break;
    }
    return;
  }

  switch (field)
  {
    case 0U: memcpy(settings.ip, ip, sizeof(settings.ip)); break;
    case 1U: memcpy(settings.mask, ip, sizeof(settings.mask)); break;
    case 2U: memcpy(settings.gateway, ip, sizeof(settings.gateway)); break;
    default: memcpy(settings.dns, ip, sizeof(settings.dns)); break;
  }

  Settings_Set(&settings);
  UART_CommandSendFormatted("%s=%u.%u.%u.%u\r\n",
                            (field == 0U) ? "ip" : ((field == 1U) ? "mask" : ((field == 2U) ? "gateway" : "dns")),
                            ip[0], ip[1], ip[2], ip[3]);
}

static void UART_CommandSetMac(char *args)
{
  Settings_t settings = *Settings_Get();

  if (UART_CommandParseMac(args, settings.mac) == 0U)
  {
    UART_CommandSendString("usage: set_mac XX:XX:XX:XX:XX:XX\r\n");
    return;
  }

  Settings_Set(&settings);
  UART_CommandSendFormatted("mac=%02X:%02X:%02X:%02X:%02X:%02X\r\n",
                            settings.mac[0], settings.mac[1], settings.mac[2],
                            settings.mac[3], settings.mac[4], settings.mac[5]);
}

static void UART_CommandSetPassword(char *args)
{
  Settings_t settings = *Settings_Get();

  if (UART_CommandParsePassword(args, settings.web_password) == 0U)
  {
    UART_CommandSendString("usage: set_password PASSWORD\r\n");
    return;
  }

  Settings_Set(&settings);
  UART_CommandSendString("password=ok\r\n");
}

static void UART_CommandSendSettings(void)
{
  const Settings_t *settings = Settings_Get();

  UART_CommandSendFormatted("dhcp=%u dhcp_fallback_attempts=%u web_auth=%u password=%s default_servo1=%u default_servo2=%u servo1_speed=%u servo2_speed=%u servo1_center=%u servo2_center=%u ip=%u.%u.%u.%u mask=%u.%u.%u.%u gateway=%u.%u.%u.%u dns=%u.%u.%u.%u mac=%02X:%02X:%02X:%02X:%02X:%02X\r\n",
                            settings->use_dhcp,
                            settings->dhcp_fallback_attempts,
                            settings->web_auth_enabled,
                            settings->web_password,
                            settings->default_servo1,
                            settings->default_servo2,
                            settings->default_servo1_speed,
                            settings->default_servo2_speed,
                            settings->servo1_center,
                            settings->servo2_center,
                            settings->ip[0], settings->ip[1], settings->ip[2], settings->ip[3],
                            settings->mask[0], settings->mask[1], settings->mask[2], settings->mask[3],
                            settings->gateway[0], settings->gateway[1], settings->gateway[2], settings->gateway[3],
                            settings->dns[0], settings->dns[1], settings->dns[2], settings->dns[3],
                            settings->mac[0], settings->mac[1], settings->mac[2],
                            settings->mac[3], settings->mac[4], settings->mac[5]);
}

static void UART_CommandSendHelp(void)
{
  UART_CommandSendString("Commands:\r\n");
  UART_CommandSendString("  help - show this help\r\n");
  UART_CommandSendString("  ping - reply pong\r\n");
  UART_CommandSendString("  version - show firmware version\r\n");
  UART_CommandSendString("  clock_info - show system clock source and frequency\r\n");
  UART_CommandSendString("  wstatus - show W5500 status\r\n");
  UART_CommandSendString("  w_dhcp - run DHCP now\r\n");
  UART_CommandSendString("  ip_info - show current W5500 IP settings\r\n");
  UART_CommandSendString("  set_dhcp 0/1 - disable/enable DHCP in RAM settings\r\n");
  UART_CommandSendString("  set_auth 0/1 - disable/enable web authorization in RAM settings\r\n");
  UART_CommandSendString("  set_ip A.B.C.D - set static IP in RAM settings\r\n");
  UART_CommandSendString("  set_mask A.B.C.D - set subnet mask in RAM settings\r\n");
  UART_CommandSendString("  set_gw A.B.C.D - set gateway in RAM settings\r\n");
  UART_CommandSendString("  set_dns A.B.C.D - set DNS in RAM settings\r\n");
  UART_CommandSendString("  set_mac XX:XX:XX:XX:XX:XX - set MAC in RAM settings\r\n");
  UART_CommandSendString("  set_password PASSWORD - set web password in RAM settings\r\n");
  UART_CommandSendString("  get_settings - show RAM settings\r\n");
  UART_CommandSendString("  save - save RAM settings to Flash\r\n");
  UART_CommandSendString("  load - load settings from Flash\r\n");
  UART_CommandSendString("  reboot - reboot MCU\r\n");
  UART_CommandSendString("  wdt_test - stop feeding watchdog\r\n");
}

static void UART_CommandExecute(Command_t command, char *args)
{
  switch (command)
  {
    case CMD_PING: UART_CommandSendString("pong\r\n"); break;
    case CMD_VERSION:
      UART_CommandSendFormatted("fw=%u.%u.%u git=%s build=%s settings=%u\r\n",
                                fw_info.version_major, fw_info.version_minor, fw_info.version_patch,
                                fw_info.git, fw_info.build_time, fw_info.settings_version);
      break;
    case CMD_CLOCK_INFO:
    {
      const char *source = "UNKNOWN";
      switch (__HAL_RCC_GET_SYSCLK_SOURCE())
      {
        case RCC_SYSCLKSOURCE_STATUS_HSI: source = "HSI"; break;
        case RCC_SYSCLKSOURCE_STATUS_HSE: source = "HSE"; break;
        case RCC_SYSCLKSOURCE_STATUS_PLLCLK: source = "PLLCLK"; break;
        default: break;
      }
      UART_CommandSendFormatted("sysclk_source=%s sysclk_hz=%lu\r\n", source, HAL_RCC_GetSysClockFreq());
      break;
    }
    case CMD_WSTATUS: W5500_SendStatus(); break;
    case CMD_W_DHCP: W5500_RunDhcp(); break;
    case CMD_IP_INFO: W5500_SendIpInfo(); break;
    case CMD_SET_DHCP: UART_CommandSetBoolSetting(args, 0U); break;
    case CMD_SET_AUTH: UART_CommandSetBoolSetting(args, 1U); break;
    case CMD_SET_IP: UART_CommandSetIpSetting(args, 0U); break;
    case CMD_SET_MASK: UART_CommandSetIpSetting(args, 1U); break;
    case CMD_SET_GW: UART_CommandSetIpSetting(args, 2U); break;
    case CMD_SET_DNS: UART_CommandSetIpSetting(args, 3U); break;
    case CMD_SET_MAC: UART_CommandSetMac(args); break;
    case CMD_SET_PASSWORD: UART_CommandSetPassword(args); break;
    case CMD_SAVE: UART_CommandSendString(Settings_Save() ? "save=ok\r\n" : "save=fail\r\n"); break;
    case CMD_LOAD: UART_CommandSendString(Settings_Load() ? "load=ok\r\n" : "load=defaults\r\n"); break;
    case CMD_GET_SETTINGS: UART_CommandSendSettings(); break;
    case CMD_HELP: UART_CommandSendHelp(); break;
    case CMD_REBOOT: UART_CommandSendString("rebooting\r\n"); HAL_Delay(10U); NVIC_SystemReset(); break;
    case CMD_WDTTEST: UART_CommandSendString("watchdog test\r\n"); while(1); break;
    case CMD_UNKNOWN: UART_CommandSendString("Unknown command\r\n"); break;
    case CMD_NONE:
    default: break;
  }
}

static Command_t UART_CommandParse(char *command, char **args)
{
  char *name = command;
  uint16_t length = 0U;
  Command_t parsed_command = CMD_UNKNOWN;

  name = UART_CommandSkipSpaces(name);
  while ((name[length] != '\0') && !UART_COMMAND_IS_SPACE(name[length])) length++;
  if (length == 0U)
  {
    *args = name;
    return CMD_NONE;
  }

  if (name[length] != '\0')
  {
    name[length] = '\0';
    *args = UART_CommandSkipSpaces(&name[length + 1U]);
  }
  else
  {
    *args = &name[length];
  }

  if (strcmp(name, "ping") == 0) parsed_command = CMD_PING;
  else if (strcmp(name, "version") == 0) parsed_command = CMD_VERSION;
  else if (strcmp(name, "clock_info") == 0) parsed_command = CMD_CLOCK_INFO;
  else if ((strcmp(name, "wstatus") == 0) || (strcmp(name, "wstaus") == 0)) parsed_command = CMD_WSTATUS;
  else if (strcmp(name, "w_dhcp") == 0) parsed_command = CMD_W_DHCP;
  else if (strcmp(name, "ip_info") == 0) parsed_command = CMD_IP_INFO;
  else if (strcmp(name, "set_dhcp") == 0) parsed_command = CMD_SET_DHCP;
  else if (strcmp(name, "set_auth") == 0) parsed_command = CMD_SET_AUTH;
  else if (strcmp(name, "set_ip") == 0) parsed_command = CMD_SET_IP;
  else if (strcmp(name, "set_mask") == 0) parsed_command = CMD_SET_MASK;
  else if (strcmp(name, "set_gw") == 0) parsed_command = CMD_SET_GW;
  else if (strcmp(name, "set_dns") == 0) parsed_command = CMD_SET_DNS;
  else if (strcmp(name, "set_mac") == 0) parsed_command = CMD_SET_MAC;
  else if (strcmp(name, "set_password") == 0) parsed_command = CMD_SET_PASSWORD;
  else if (strcmp(name, "save") == 0) parsed_command = CMD_SAVE;
  else if (strcmp(name, "load") == 0) parsed_command = CMD_LOAD;
  else if (strcmp(name, "get_settings") == 0) parsed_command = CMD_GET_SETTINGS;
  else if (strcmp(name, "help") == 0) parsed_command = CMD_HELP;
  else if (strcmp(name, "reboot") == 0) parsed_command = CMD_REBOOT;
  else if (strcmp(name, "wdt_test") == 0) parsed_command = CMD_WDTTEST;
  else parsed_command = CMD_UNKNOWN;

  return parsed_command;
}

static void UART_CommandReceiveByte(uint8_t byte)
{
  if ((byte == '\n') || (byte == '\r'))
  {
    if (uart_command_length > 0U)
    {
      uart_command_buffer[uart_command_length] = '\0';
      if (pending_command_ready == 0U)
      {
        memcpy(pending_command_buffer, uart_command_buffer, uart_command_length + 1U);
        pending_command_ready = 1U;
      }
      uart_command_length = 0U;
    }
  }
  else if (!UART_COMMAND_IS_PRINTABLE(byte))
  {
    /* Ignore control bytes added by terminal programs. */
  }
  else if (uart_command_length < (UART_COMMAND_RX_BUFFER_SIZE - 1U))
  {
    uart_command_buffer[uart_command_length++] = (char)byte;
  }
  else
  {
    uart_command_length = 0U;
  }
}

static void UART_CommandPollReceive(void)
{
  uint16_t write_pos;

  if ((uart_command_uart == NULL) || (uart_command_uart->hdmarx == NULL))
  {
    return;
  }

  write_pos = (uint16_t)(UART_COMMAND_DMA_BUFFER_SIZE - __HAL_DMA_GET_COUNTER(uart_command_uart->hdmarx));
  if (write_pos >= UART_COMMAND_DMA_BUFFER_SIZE)
  {
    write_pos = 0U;
  }

  while (uart_command_dma_read_pos != write_pos)
  {
    UART_CommandReceiveByte(uart_command_dma_buffer[uart_command_dma_read_pos]);
    uart_command_dma_read_pos++;
    if (uart_command_dma_read_pos >= UART_COMMAND_DMA_BUFFER_SIZE)
    {
      uart_command_dma_read_pos = 0U;
    }

    if (pending_command_ready != 0U)
    {
      break;
    }
  }
}

void UART_CommandInit(UART_HandleTypeDef *uart)
{
  uart_command_uart = uart;
  uart_command_length = 0U;
  uart_command_dma_read_pos = 0U;
  pending_command_ready = 0U;
}

void UART_CommandStartReceive(void)
{
  if ((uart_command_uart == NULL) || (uart_command_uart->hdmarx == NULL))
  {
    Error_Handler();
  }

  uart_command_dma_read_pos = 0U;
  uart_command_uart->hdmarx->Init.Mode = DMA_CIRCULAR;
  if (HAL_DMA_Init(uart_command_uart->hdmarx) != HAL_OK) Error_Handler();
  if (HAL_UART_Receive_DMA(uart_command_uart, uart_command_dma_buffer, UART_COMMAND_DMA_BUFFER_SIZE) != HAL_OK) Error_Handler();
}

void UART_CommandProcess(void)
{
  char command_buffer[UART_COMMAND_RX_BUFFER_SIZE];
  char *args;
  Command_t command;

  UART_CommandPollReceive();

  if (pending_command_ready == 0U)
  {
    return;
  }

  __disable_irq();
  strncpy(command_buffer, pending_command_buffer, sizeof(command_buffer));
  command_buffer[sizeof(command_buffer) - 1U] = '\0';
  pending_command_ready = 0U;
  __enable_irq();

  command = UART_CommandParse(command_buffer, &args);
  if (command == CMD_UNKNOWN)
  {
    UART_CommandSendHex(command_buffer);
  }
  UART_CommandExecute(command, args);
}

void UART_CommandRxCpltCallback(UART_HandleTypeDef *huart)
{
  if ((uart_command_uart == NULL) || (huart->Instance != uart_command_uart->Instance))
  {
    return;
  }
}

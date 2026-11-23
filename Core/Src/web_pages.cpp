#include "web_pages.h"
#include "azimuth_service.h"
#include "fw_info.h"
#include "fw_update.h"
#include "pwm_service.h"
#include "settings_service.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* State JSON for /data and for every JSON API reply. error != AZ_OK adds "err" (JSON API only). */
static void WebPages_BuildState(char *buffer, uint16_t length, uint8_t error)
{
  char err[32] = "";
  char pos[8] = "null";
  char target[8] = "null";
  int16_t pos_x10;
  int16_t target_x10;

  Az_GetState(&pos_x10, &target_x10);
  if (pos_x10 != AZ_UNKNOWN) (void)snprintf(pos, sizeof(pos), "%d", pos_x10);
  if (target_x10 != AZ_UNKNOWN) (void)snprintf(target, sizeof(target), "%d", target_x10);
  if (error != (uint8_t)AZ_OK) (void)snprintf(err, sizeof(err), "\"err\":\"%s\",", Az_ErrorName((Az_Error_t)error));

  (void)snprintf(buffer, length,
                 "{%s\"uptime_s\":%lu,\"pwm\":{\"servo1\":%u,\"servo2\":%u,\"servo1_speed\":%u,\"servo2_speed\":%u,\"servo1_pos\":%u,\"servo2_pos\":%u,\"servo1_center\":%u,\"servo2_center\":%u},"
                 "\"az\":{\"pos\":%s,\"target\":%s}}\n",
                 err, HAL_GetTick() / 1000UL,
                 PWM_ServiceGetServo1(), PWM_ServiceGetServo2(),
                 PWM_ServiceGetServo1Speed(), PWM_ServiceGetServo2Speed(),
                 PWM_ServiceGetServo1Position(), PWM_ServiceGetServo2Position(),
                 Settings_Get()->servo1_center, Settings_Get()->servo2_center,
                 pos, target);
}

void WebPages_BuildDataJson(char *buffer, uint16_t length)
{
  WebPages_BuildState(buffer, length, (uint8_t)AZ_OK);
}

void WebPages_BuildApiReply(char *buffer, uint16_t length, uint8_t error)
{
  WebPages_BuildState(buffer, length, error);
}

void WebPages_BuildSettingsJson(char *buffer, uint16_t length)
{
  Settings_ToJson(buffer, length);
}

static const char *WebPages_QueryValue(const char *request, const char *name)
{
  uint16_t name_length = (uint16_t)strlen(name);
  const char *cursor = request;

  while (*cursor != '\0')
  {
    if (((*cursor == '?') || (*cursor == '&')) &&
        (strncmp(cursor + 1, name, name_length) == 0) &&
        (cursor[1 + name_length] == '='))
    {
      return cursor + 2 + name_length;
    }
    cursor++;
  }

  return NULL;
}

static uint8_t WebPages_ParseByte(const char **text, uint8_t *value)
{
  uint32_t parsed = 0U;
  const char *cursor = *text;

  if ((*cursor < '0') || (*cursor > '9'))
  {
    return 0U;
  }

  while ((*cursor >= '0') && (*cursor <= '9'))
  {
    parsed = (parsed * 10UL) + (uint32_t)(*cursor - '0');
    if (parsed > 255UL)
    {
      return 0U;
    }
    cursor++;
  }

  *value = (uint8_t)parsed;
  *text = cursor;
  return 1U;
}

static uint8_t WebPages_HexNibble(char symbol, uint8_t *value);
static uint8_t WebPages_ParseText(const char *text, char *output, uint16_t output_length, uint8_t is_name);

static uint8_t WebPages_ParseIp(const char *text, uint8_t ip[4])
{
  uint8_t index;

  if (text == NULL)
  {
    return 0U;
  }

  for (index = 0U; index < 4U; index++)
  {
    if (WebPages_ParseByte(&text, &ip[index]) == 0U)
    {
      return 0U;
    }

    if (index < 3U)
    {
      if (*text != '.')
      {
        return 0U;
      }
      text++;
    }
  }

  return ((*text == '\0') || (*text == '&') || (*text == ' ')) ? 1U : 0U;
}

static uint8_t WebPages_ParseHexByte(const char **text, uint8_t *value)
{
  uint8_t high;
  uint8_t low;

  if ((WebPages_HexNibble((*text)[0], &high) == 0U) ||
      (WebPages_HexNibble((*text)[1], &low) == 0U))
  {
    return 0U;
  }

  *value = (uint8_t)((high << 4) | low);
  *text += 2;
  return 1U;
}

static uint8_t WebPages_ParseMac(const char *text, uint8_t mac[6])
{
  char decoded[18];
  const char *cursor = decoded;
  uint8_t index;
  uint8_t all_zero = 1U;
  uint8_t all_ff = 1U;

  if (WebPages_ParseText(text, decoded, sizeof(decoded), 0U) == 0U)
  {
    return 0U;
  }

  for (index = 0U; index < 6U; index++)
  {
    if (WebPages_ParseHexByte(&cursor, &mac[index]) == 0U)
    {
      return 0U;
    }

    if (mac[index] != 0x00U) all_zero = 0U;
    if (mac[index] != 0xFFU) all_ff = 0U;

    if (index < 5U)
    {
      if ((*cursor != ':') && (*cursor != '-'))
      {
        return 0U;
      }
      cursor++;
    }
  }

  if ((*cursor != '\0') || ((mac[0] & 0x01U) != 0U) || (all_zero != 0U) || (all_ff != 0U))
  {
    return 0U;
  }

  return 1U;
}

static uint8_t WebPages_ParseUint(const char *text, uint32_t max, uint32_t *value)
{
  uint32_t parsed = 0U;

  if ((text == NULL) || ((*text < '0') || (*text > '9')))
  {
    return 0U;
  }

  while ((*text >= '0') && (*text <= '9'))
  {
    parsed = (parsed * 10UL) + (uint32_t)(*text - '0');
    if (parsed > max)
    {
      return 0U;
    }
    text++;
  }

  if ((*text != '\0') && (*text != '&') && (*text != ' ') && (*text != ',') && (*text != '}') && (*text != ']') && (*text != '"'))
  {
    return 0U;
  }

  *value = parsed;
  return 1U;
}

static uint8_t WebPages_IsJsonWhitespace(char symbol)
{
  return ((symbol == ' ') || (symbol == '\t') || (symbol == '\r') || (symbol == '\n')) ? 1U : 0U;
}

static const char *WebPages_SkipJsonWhitespace(const char *text)
{
  while (WebPages_IsJsonWhitespace(*text) != 0U)
  {
    text++;
  }

  return text;
}

static uint8_t WebPages_HexNibble(char symbol, uint8_t *value)
{
  if ((symbol >= '0') && (symbol <= '9'))
  {
    *value = (uint8_t)(symbol - '0');
    return 1U;
  }

  if ((symbol >= 'A') && (symbol <= 'F'))
  {
    *value = (uint8_t)(symbol - 'A' + 10);
    return 1U;
  }

  if ((symbol >= 'a') && (symbol <= 'f'))
  {
    *value = (uint8_t)(symbol - 'a' + 10);
    return 1U;
  }

  return 0U;
}

/* Decodes a URL query value. Passwords (is_name = 0): printable ASCII without spaces.
 * Preset names (is_name = 1): also spaces and UTF-8 bytes. Never '"', '\\' or control bytes,
 * so the result can be written into JSON as is. */
static uint8_t WebPages_ParseText(const char *text, char *output, uint16_t output_length, uint8_t is_name)
{
  uint16_t index = 0U;

  if ((text == NULL) || (output == NULL) || (output_length == 0U))
  {
    return 0U;
  }

  while ((*text != '\0') && (*text != '&') && (*text != ' '))
  {
    char symbol = *text++;

    if (symbol == '+')
    {
      symbol = ' ';
    }
    else if (symbol == '%')
    {
      uint8_t high;
      uint8_t low;

      if ((WebPages_HexNibble(text[0], &high) == 0U) || (WebPages_HexNibble(text[1], &low) == 0U))
      {
        return 0U;
      }

      symbol = (char)((high << 4) | low);
      text += 2;
    }

    uint8_t byte = (uint8_t)symbol;
    uint8_t allowed = ((byte >= (uint8_t)'!') && (byte <= (uint8_t)'~')) ? 1U : 0U;

    if ((is_name != 0U) && ((byte == (uint8_t)' ') || (byte >= 0x80U)))
    {
      allowed = 1U;
    }
    if ((allowed == 0U) || (symbol == '"') || (symbol == '\\') || (index >= (output_length - 1U)))
    {
      return 0U;
    }

    output[index++] = symbol;
  }

  output[index] = '\0';
  return (index > 0U) ? 1U : 0U;
}

static uint8_t WebPages_QueryValueEquals(const char *value, const char *expected)
{
  uint16_t length;

  if (value == NULL)
  {
    return 0U;
  }

  length = (uint16_t)strlen(expected);
  return ((strncmp(value, expected, length) == 0) &&
          ((value[length] == '\0') || (value[length] == '&') || (value[length] == ' '))) ? 1U : 0U;
}

static const char *WebPages_JsonValue(const char *request, const char *name)
{
  char quoted_name[16];
  const char *cursor;
  uint16_t name_length;

  (void)snprintf(quoted_name, sizeof(quoted_name), "\"%s\"", name);
  name_length = (uint16_t)strlen(quoted_name);
  cursor = request;

  while ((cursor = strstr(cursor, quoted_name)) != NULL)
  {
    const char *prefix = cursor;

    while ((prefix > request) && (WebPages_IsJsonWhitespace(prefix[-1]) != 0U))
    {
      prefix--;
    }

    if ((prefix == request) || ((prefix[-1] != '{') && (prefix[-1] != ',')))
    {
      cursor++;
      continue;
    }

    cursor += name_length;
    cursor = WebPages_SkipJsonWhitespace(cursor);
    if (*cursor != ':')
    {
      continue;
    }

    cursor++;
    cursor = WebPages_SkipJsonWhitespace(cursor);
    if (*cursor == '"')
    {
      cursor++;
    }

    return cursor;
  }

  return NULL;
}

uint8_t WebPages_HandleNetSettingsRequest(const char *request, uint8_t save)
{
  Settings_t settings = *Settings_Get();
  const char *dhcp = WebPages_QueryValue(request, "dhcp");
  uint32_t parsed;

  if (dhcp != NULL)
  {
    settings.use_dhcp = (*dhcp == '0') ? 0U : 1U;
  }

  if (WebPages_ParseUint(WebPages_QueryValue(request, "dhcp_fallback_attempts"), 255U, &parsed) == 0U)
  {
    return 0U;
  }
  settings.dhcp_fallback_attempts = (uint8_t)parsed;

  if ((WebPages_ParseIp(WebPages_QueryValue(request, "ip"), settings.ip) == 0U) ||
      (WebPages_ParseIp(WebPages_QueryValue(request, "mask"), settings.mask) == 0U) ||
      (WebPages_ParseIp(WebPages_QueryValue(request, "gateway"), settings.gateway) == 0U) ||
      (WebPages_ParseIp(WebPages_QueryValue(request, "dns"), settings.dns) == 0U))
  {
    return 0U;
  }

  if (WebPages_ParseMac(WebPages_QueryValue(request, "mac"), settings.mac) == 0U)
  {
    return 0U;
  }

  if (WebPages_ParseUint(WebPages_QueryValue(request, "json_api_enabled"), 1U, &parsed) == 0U)
  {
    return 0U;
  }
  settings.json_api_enabled = (uint8_t)parsed;

  if (WebPages_ParseUint(WebPages_QueryValue(request, "json_api_mode"), 1U, &parsed) == 0U)
  {
    return 0U;
  }
  settings.json_api_mode = (uint8_t)parsed;

  if (WebPages_ParseUint(WebPages_QueryValue(request, "json_api_port"), 65535UL, &parsed) == 0U)
  {
    return 0U;
  }
  settings.json_api_port = (uint16_t)parsed;

  if (WebPages_ParseUint(WebPages_QueryValue(request, "web_auth_enabled"), 1U, &parsed) == 0U)
  {
    return 0U;
  }
  settings.web_auth_enabled = (uint8_t)parsed;

  if (WebPages_ParseText(WebPages_QueryValue(request, "web_password"), settings.web_password, sizeof(settings.web_password), 0U) == 0U)
  {
    return 0U;
  }

  Settings_Set(&settings);
  if (save != 0U)
  {
    return Settings_Save();
  }

  return 1U;
}

uint8_t WebPages_HandleBoardRequest(const char *request, uint8_t save)
{
  Settings_t settings = *Settings_Get();
  uint32_t parsed;

  if (WebPages_ParseUint(WebPages_QueryValue(request, "servo1"), PWM_SERVICE_SERVO_MAX, &parsed) == 0U)
  {
    return 0U;
  }
  settings.default_servo1 = (uint16_t)parsed;

  if (WebPages_ParseUint(WebPages_QueryValue(request, "servo2"), PWM_SERVICE_SERVO_MAX, &parsed) == 0U)
  {
    return 0U;
  }
  settings.default_servo2 = (uint16_t)parsed;

  /* Speeds are optional so clients that only send positions keep working. */
  if (WebPages_QueryValue(request, "servo1_speed") != NULL)
  {
    if (WebPages_ParseUint(WebPages_QueryValue(request, "servo1_speed"), PWM_SERVICE_SPEED_MAX, &parsed) == 0U)
    {
      return 0U;
    }
    settings.default_servo1_speed = (uint8_t)parsed;
  }

  if (WebPages_QueryValue(request, "servo2_speed") != NULL)
  {
    if (WebPages_ParseUint(WebPages_QueryValue(request, "servo2_speed"), PWM_SERVICE_SPEED_MAX, &parsed) == 0U)
    {
      return 0U;
    }
    settings.default_servo2_speed = (uint8_t)parsed;
  }

  Settings_Set(&settings);
  PWM_ServiceApplyDefaults(Settings_Get()->default_servo1,
                           Settings_Get()->default_servo2,
                           Settings_Get()->default_servo1_speed,
                           Settings_Get()->default_servo2_speed);
  if (save != 0U)
  {
    return Settings_Save();
  }

  return 1U;
}

/* ------------------------------------------------------------------ azimuth UI storage
 * The browser does all azimuth math; the board only keeps the numbers in flash.
 * Angles are integers in 0.1 deg. */

static uint16_t WebPages_AppendJson(char *buffer, uint16_t length, uint16_t used, const char *format, ...)
  __attribute__((format(printf, 4, 5)));

static uint16_t WebPages_AppendJson(char *buffer, uint16_t length, uint16_t used, const char *format, ...)
{
  va_list args;
  int written;

  if (used >= length)
  {
    return used;
  }
  va_start(args, format);
  written = vsnprintf(&buffer[used], (size_t)(length - used), format, args);
  va_end(args);
  if (written < 0)
  {
    return used;
  }
  return ((uint32_t)used + (uint32_t)written >= length) ? length : (uint16_t)(used + written);
}

void WebPages_BuildAzConfigJson(char *buffer, uint16_t length)
{
  const Settings_t *settings = Settings_Get();
  uint16_t used;
  uint8_t slot;
  const char *separator = "";

  used = WebPages_AppendJson(buffer, length, 0U,
                             "{\"fw\":{\"v\":\"%u.%u.%u\",\"git\":\"%s\",\"build\":\"%s\",\"settings\":%u},"
                             "\"servo\":%u,\"pmin\":%u,\"pmax\":%u,\"range\":%u,\"inv\":%u,\"zero\":%u,\"cal\":",
                             fw_info.version_major, fw_info.version_minor, fw_info.version_patch,
                             fw_info.git, fw_info.build_time, fw_info.settings_version,
                             settings->az_servo, settings->az_pulse_min, settings->az_pulse_max,
                             settings->az_range_x10, settings->az_invert, AZ_ZERO_PULSE);
  if (settings->az_cal_pulse != 0U)
  {
    used = WebPages_AppendJson(buffer, length, used, "[%u,%u]", settings->az_cal_pulse, settings->az_cal_azimuth_x10);
  }
  else
  {
    used = WebPages_AppendJson(buffer, length, used, "null");
  }
  used = WebPages_AppendJson(buffer, length, used, ",\"presets\":[");
  for (slot = 0U; slot < SETTINGS_AZ_PRESET_COUNT; slot++)
  {
    const Settings_AzPreset_t *preset = &settings->az_presets[slot];

    if (preset->azimuth_x10 != SETTINGS_AZ_EMPTY)
    {
      used = WebPages_AppendJson(buffer, length, used, "%s[%u,%u,\"%s\"]", separator, slot, preset->azimuth_x10, preset->name);
      separator = ",";
    }
  }
  (void)WebPages_AppendJson(buffer, length, used, "]}\n");
}

/* GET /az/set?pmin=&pmax=&range=&inv=&servo=   mechanics
 * GET /az/cal?az=1470  |  /az/cal?clear=1      bind the servo's current position to an azimuth
 * GET /az/preset?slot=&az=&name=  |  &del=1    write or clear a preset slot
 * Every change is saved to flash. */
uint8_t WebPages_HandleAzRequest(const char *request)
{
  Settings_t settings = *Settings_Get();
  uint32_t value;

  if (strncmp(request, "GET /az/set?", 12) == 0)
  {
    uint32_t pmin;
    uint32_t pmax;
    uint32_t range;
    uint32_t invert;
    uint32_t servo;

    if ((WebPages_ParseUint(WebPages_QueryValue(request, "pmin"), PWM_SERVICE_SERVO_MAX, &pmin) == 0U) ||
        (WebPages_ParseUint(WebPages_QueryValue(request, "pmax"), PWM_SERVICE_SERVO_MAX, &pmax) == 0U) ||
        (WebPages_ParseUint(WebPages_QueryValue(request, "range"), 3600U, &range) == 0U) ||
        (WebPages_ParseUint(WebPages_QueryValue(request, "inv"), 1U, &invert) == 0U) ||
        (WebPages_ParseUint(WebPages_QueryValue(request, "servo"), 2U, &servo) == 0U) ||
        (pmin == 0U) || (pmin >= pmax) || (range == 0U) || (servo == 0U))
    {
      return 0U;
    }
    if (servo != settings.az_servo)
    {
      settings.az_cal_pulse = 0U;   /* the binding belonged to the other servo */
    }
    settings.az_pulse_min = (uint16_t)pmin;
    settings.az_pulse_max = (uint16_t)pmax;
    settings.az_range_x10 = (uint16_t)range;
    settings.az_invert = (uint8_t)invert;
    settings.az_servo = (uint8_t)servo;
  }
  else if (strncmp(request, "GET /az/cal?", 12) == 0)
  {
    if (WebPages_QueryValue(request, "clear") != NULL)
    {
      settings.az_cal_pulse = 0U;
    }
    else
    {
      uint16_t position = (settings.az_servo == 1U) ? PWM_ServiceGetServo1Position() : PWM_ServiceGetServo2Position();
      uint16_t target = (settings.az_servo == 1U) ? PWM_ServiceGetServo1() : PWM_ServiceGetServo2();

      /* Bind only a settled position inside the working range. */
      if ((WebPages_ParseUint(WebPages_QueryValue(request, "az"), 3599U, &value) == 0U) ||
          (position != target) || (position < settings.az_pulse_min) || (position > settings.az_pulse_max))
      {
        return 0U;
      }
      settings.az_cal_pulse = position;
      settings.az_cal_azimuth_x10 = (uint16_t)value;
    }
  }
  else if (strncmp(request, "GET /az/preset?", 15) == 0)
  {
    uint32_t slot;
    Settings_AzPreset_t *preset;

    if (WebPages_ParseUint(WebPages_QueryValue(request, "slot"), SETTINGS_AZ_PRESET_COUNT - 1U, &slot) == 0U)
    {
      return 0U;
    }
    preset = &settings.az_presets[slot];
    memset(preset->name, 0, sizeof(preset->name));
    preset->azimuth_x10 = SETTINGS_AZ_EMPTY;
    if (WebPages_QueryValue(request, "del") == NULL)
    {
      if ((WebPages_ParseUint(WebPages_QueryValue(request, "az"), 3599U, &value) == 0U) ||
          (WebPages_ParseText(WebPages_QueryValue(request, "name"), preset->name, sizeof(preset->name), 1U) == 0U))
      {
        return 0U;
      }
      preset->azimuth_x10 = (uint16_t)value;
    }
  }
  else
  {
    return 0U;
  }

  Settings_Set(&settings);
  return Settings_Save();
}

/* Stores the servo center in flash. value == 0 means "use the servo's current position". */
static uint8_t WebPages_SaveServoCenter(uint8_t servo, uint32_t value)
{
  Settings_t settings = *Settings_Get();

  if (value == 0UL)
  {
    value = (servo == 0U) ? PWM_ServiceGetServo1Position() : PWM_ServiceGetServo2Position();
  }
  if ((value == 0UL) || (value > PWM_SERVICE_SERVO_MAX))
  {
    return 0U;
  }

  if (servo == 0U)
  {
    settings.servo1_center = (uint16_t)value;
  }
  else
  {
    settings.servo2_center = (uint16_t)value;
  }
  Settings_Set(&settings);
  return Settings_Save();
}

/* Stores the speed as the servo's boot speed (default_servoN_speed). Writes flash only on a change. */
static uint8_t WebPages_SaveServoSpeed(uint8_t servo, uint8_t value)
{
  Settings_t settings = *Settings_Get();
  uint8_t *stored = (servo == 0U) ? &settings.default_servo1_speed : &settings.default_servo2_speed;

  if (*stored == value)
  {
    return 1U;
  }
  *stored = value;
  Settings_Set(&settings);
  return Settings_Save();
}

/* "save" flag of a request: 1 only for save=1. */
static uint8_t WebPages_IsSaveRequested(const char *text)
{
  uint32_t value;

  return ((text != NULL) && (WebPages_ParseUint(text, 1U, &value) != 0U) && (value == 1U)) ? 1U : 0U;
}

/* GET /pwm?ch=<channel>&v=<value>; for servoN_speed, &save=1 also stores it as the boot speed. */
uint8_t WebPages_HandlePwmRequest(const char *request)
{
  const char *channel = WebPages_QueryValue(request, "ch");
  uint32_t value;

  if ((WebPages_QueryValueEquals(channel, "servo1_center") != 0U) ||
      (WebPages_QueryValueEquals(channel, "servo2_center") != 0U))
  {
    const char *text = WebPages_QueryValue(request, "v");

    value = 0UL;
    if ((text != NULL) && (WebPages_ParseUint(text, PWM_SERVICE_SERVO_MAX, &value) == 0U))
    {
      return 0U;
    }
    return WebPages_SaveServoCenter((channel[5] == '1') ? 0U : 1U, value);
  }

  if (WebPages_QueryValueEquals(channel, "servo1") != 0U)
  {
    if (WebPages_ParseUint(WebPages_QueryValue(request, "v"), PWM_SERVICE_SERVO_MAX, &value) == 0U)
    {
      return 0U;
    }
    PWM_ServiceSetServo1((uint16_t)value);
    return 1U;
  }

  if (WebPages_QueryValueEquals(channel, "servo2") != 0U)
  {
    if (WebPages_ParseUint(WebPages_QueryValue(request, "v"), PWM_SERVICE_SERVO_MAX, &value) == 0U)
    {
      return 0U;
    }
    PWM_ServiceSetServo2((uint16_t)value);
    return 1U;
  }

  if (WebPages_QueryValueEquals(channel, "servo1_speed") != 0U)
  {
    if (WebPages_ParseUint(WebPages_QueryValue(request, "v"), PWM_SERVICE_SPEED_MAX, &value) == 0U)
    {
      return 0U;
    }
    PWM_ServiceSetServo1Speed((uint8_t)value);
    if (WebPages_IsSaveRequested(WebPages_QueryValue(request, "save")) != 0U)
    {
      return WebPages_SaveServoSpeed(0U, (uint8_t)value);
    }
    return 1U;
  }

  if (WebPages_QueryValueEquals(channel, "servo2_speed") != 0U)
  {
    if (WebPages_ParseUint(WebPages_QueryValue(request, "v"), PWM_SERVICE_SPEED_MAX, &value) == 0U)
    {
      return 0U;
    }
    PWM_ServiceSetServo2Speed((uint8_t)value);
    if (WebPages_IsSaveRequested(WebPages_QueryValue(request, "save")) != 0U)
    {
      return WebPages_SaveServoSpeed(1U, (uint8_t)value);
    }
    return 1U;
  }

  return 0U;
}

/* JSON API request ("save":1 also stores the speeds of the message as boot speeds). Order: centers, speeds, servo targets, then azimuth/preset/zero (so a speed in the
 * same message already applies, and "az" wins over a raw target for the azimuth servo).
 * Stops at the first bad value; what was applied before it stays applied.
 * Returns AZ_OK or the error reported to the client as "err". */
uint8_t WebPages_HandleApiJson(const char *request)
{
  static const char *const speed_keys[2] = {"servo1_speed", "servo2_speed"};
  static const char *const target_keys[2] = {"servo1", "servo2"};
  static const char *const center_keys[2] = {"servo1_center", "servo2_center"};
  uint8_t channel;
  uint32_t value;
  const char *text;
  const uint8_t save = WebPages_IsSaveRequested(WebPages_JsonValue(request, "save"));

  for (channel = 0U; channel < 2U; channel++)
  {
    text = WebPages_JsonValue(request, center_keys[channel]);
    if (text != NULL)
    {
      if (WebPages_ParseUint(text, PWM_SERVICE_SERVO_MAX, &value) == 0U)
      {
        return AZ_ERR_BAD_VALUE;
      }
      if (WebPages_SaveServoCenter(channel, value) == 0U)
      {
        return AZ_ERR_SAVE;
      }
    }
  }

  for (channel = 0U; channel < 2U; channel++)
  {
    text = WebPages_JsonValue(request, speed_keys[channel]);
    if (text != NULL)
    {
      if (WebPages_ParseUint(text, PWM_SERVICE_SPEED_MAX, &value) == 0U)
      {
        return AZ_ERR_BAD_VALUE;
      }
      if (channel == 0U) PWM_ServiceSetServo1Speed((uint8_t)value); else PWM_ServiceSetServo2Speed((uint8_t)value);
      if ((save != 0U) && (WebPages_SaveServoSpeed(channel, (uint8_t)value) == 0U))
      {
        return AZ_ERR_SAVE;
      }
    }
  }

  for (channel = 0U; channel < 2U; channel++)
  {
    text = WebPages_JsonValue(request, target_keys[channel]);
    if (text != NULL)
    {
      if (WebPages_ParseUint(text, PWM_SERVICE_SERVO_MAX, &value) == 0U)
      {
        return AZ_ERR_BAD_VALUE;
      }
      if (channel == 0U) PWM_ServiceSetServo1((uint16_t)value); else PWM_ServiceSetServo2((uint16_t)value);
    }
  }

  text = WebPages_JsonValue(request, "az");
  if (text != NULL)
  {
    if (WebPages_ParseUint(text, 3599U, &value) == 0U)
    {
      return AZ_ERR_BAD_VALUE;
    }
    return Az_Goto((uint16_t)value);
  }

  text = WebPages_JsonValue(request, "preset");
  if (text != NULL)
  {
    if (WebPages_ParseUint(text, SETTINGS_AZ_PRESET_COUNT - 1U, &value) == 0U)
    {
      return AZ_ERR_BAD_VALUE;
    }
    return Az_GotoPreset((uint8_t)value);
  }

  text = WebPages_JsonValue(request, "zero");
  if (text != NULL)
  {
    if ((WebPages_ParseUint(text, 1U, &value) == 0U) || (value != 1U))
    {
      return AZ_ERR_BAD_VALUE;
    }
    return Az_GotoZero();
  }

  return AZ_OK;
}

/* GET /az/goto?az=<0.1 deg> | ?preset=<slot> | ?zero=1  ->  {"ok":1,"pulse":1785} or {"ok":0,"err":"unreachable"} */
uint8_t WebPages_HandleAzGoto(const char *request, char *reply, uint16_t reply_length)
{
  const char *az_text = WebPages_QueryValue(request, "az");
  const char *preset_text = WebPages_QueryValue(request, "preset");
  const char *zero_text = WebPages_QueryValue(request, "zero");
  uint32_t value;
  Az_Error_t error = AZ_ERR_BAD_VALUE;

  if (az_text != NULL)
  {
    if (WebPages_ParseUint(az_text, 3599U, &value) != 0U)
    {
      error = Az_Goto((uint16_t)value);
    }
  }
  else if (preset_text != NULL)
  {
    if (WebPages_ParseUint(preset_text, SETTINGS_AZ_PRESET_COUNT - 1U, &value) != 0U)
    {
      error = Az_GotoPreset((uint8_t)value);
    }
  }
  else if (zero_text != NULL)
  {
    if ((WebPages_ParseUint(zero_text, 1U, &value) != 0U) && (value == 1U))
    {
      error = Az_GotoZero();
    }
  }

  if (error == AZ_OK)
  {
    (void)snprintf(reply, reply_length, "{\"ok\":1,\"pulse\":%u}\n",
                   (Settings_Get()->az_servo == 2U) ? PWM_ServiceGetServo2() : PWM_ServiceGetServo1());
    return 1U;
  }
  (void)snprintf(reply, reply_length, "{\"ok\":0,\"err\":\"%s\"}\n", Az_ErrorName(error));
  return 0U;
}

#include "settings_service.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define SETTINGS_MAGIC 0x53455431UL
/* SETTINGS_VERSION (14) is in settings_service.h: the firmware version stamp reports it.
 * v14 appends the rotation limits; v13 is an exact prefix of it.
 * v13 has the same layout as v12. That bump only made boards with older settings take the new
 * factory network defaults once (see Settings_Load); everything else is kept. */
#define SETTINGS_V13_VERSION 13UL
#define SETTINGS_V13_SIZE offsetof(Settings_t, az_limit_pulse_min)
#define SETTINGS_V12_VERSION 12UL
#define SETTINGS_V11_VERSION 11UL
#define SETTINGS_V11_SIZE offsetof(Settings_t, az_pulse_min)
#define SETTINGS_AZ_PULSE_MIN_DEFAULT 801U
#define SETTINGS_AZ_PULSE_MAX_DEFAULT 2012U
#define SETTINGS_AZ_RANGE_X10_DEFAULT 2630U

/* v11 had no uint32 fields, so its size is 72 and the azimuth block starts right after it. */
_Static_assert(offsetof(Settings_t, az_pulse_min) == 72U, "v11 layout changed: fix the migration");
/* v13/v12 = 72 + 10 + 2 + 8 * 34, with no tail padding (Settings_t has no 32-bit members). */
_Static_assert(SETTINGS_V13_SIZE == 356U, "v13 layout changed: fix the migration");
#define SETTINGS_SERVO_MAX 3000U
#define SETTINGS_STM32_UID_ADDRESS 0x1FFFF7E8UL

typedef struct
{
  uint32_t magic;
  uint32_t version;
  Settings_t settings;
  uint32_t crc;
} SettingsRecord_t;

static Settings_t settings_current;

static const Settings_t settings_defaults =
{
  0U,                                   /* use_dhcp: static address from boot (no DHCP wait) */
  3U,                                   /* dhcp_fallback_attempts */
  1U,                                   /* json_api_enabled */
  0U,                                   /* web_auth_enabled */
  {192U, 168U, 67U, 121U},              /* ip */
  {255U, 255U, 255U, 0U},               /* mask */
  {192U, 168U, 67U, 1U},                /* gateway */
  {8U, 8U, 8U, 8U},
  {0U, 0U, 0U, 0U, 0U, 0U},
  0U,                                   /* default_servo1 */
  0U,                                   /* default_servo2 */
  SETTINGS_JSON_API_DEFAULT_PORT,
  SETTINGS_JSON_API_MODE_UDP,
  255U,                                 /* default_servo1_speed */
  255U,                                 /* default_servo2_speed */
  SETTINGS_SERVO_CENTER_DEFAULT,
  SETTINGS_SERVO_CENTER_DEFAULT,
  "admin",
  SETTINGS_AZ_PULSE_MIN_DEFAULT,
  SETTINGS_AZ_PULSE_MAX_DEFAULT,
  SETTINGS_AZ_RANGE_X10_DEFAULT,
  0U,                                   /* az_cal_pulse: not bound */
  0U,
  1U,                                   /* az_servo */
  0U,                                   /* az_invert */
  {{0U, ""}},                           /* presets: emptied in Settings_Set (azimuth 0xFFFF) */
  0U,                                   /* az_limit_pulse_min: no limit */
  0U,                                   /* az_limit_pulse_max: no limit */
  SETTINGS_AZ_EMPTY,                    /* az_limit_plus_x10 */
  SETTINGS_AZ_EMPTY,                    /* az_limit_minus_x10 */
  0U                                    /* az_limit_ref_pulse */
};

static uint32_t Settings_Crc32(const void *data, uint32_t length)
{
  uint32_t index;
  uint32_t words = (length + 3UL) / 4UL;
  const uint32_t *word_data = (const uint32_t *)data;

  CRC->CR = CRC_CR_RESET;
  for (index = 0U; index < words; index++)
  {
    CRC->DR = word_data[index];
  }

  return CRC->DR;
}

static uint8_t Settings_IsMacValid(const uint8_t mac[6])
{
  uint8_t index;
  uint8_t all_zero = 1U;
  uint8_t all_ff = 1U;

  if ((mac[0] & 0x01U) != 0U)
  {
    return 0U;
  }

  for (index = 0U; index < 6U; index++)
  {
    if (mac[index] != 0x00U) all_zero = 0U;
    if (mac[index] != 0xFFU) all_ff = 0U;
  }

  return ((all_zero == 0U) && (all_ff == 0U)) ? 1U : 0U;
}

static void Settings_GenerateMac(uint8_t mac[6])
{
  const uint32_t *uid = (const uint32_t *)SETTINGS_STM32_UID_ADDRESS;
  uint32_t hash = Settings_Crc32(uid, 12UL);

  mac[0] = (uint8_t)((hash & 0xFCU) | 0x02U);
  mac[1] = (uint8_t)(hash >> 8);
  mac[2] = (uint8_t)(hash >> 16);
  mac[3] = (uint8_t)(hash >> 24);
  mac[4] = (uint8_t)(uid[0] ^ uid[2]);
  mac[5] = (uint8_t)(uid[1] ^ (uid[2] >> 8));
}

static uint8_t Settings_IsRecordValid(const SettingsRecord_t *record, uint32_t version)
{
  if ((record->magic != SETTINGS_MAGIC) || (record->version != version))
  {
    return 0U;
  }

  return (Settings_Crc32(&record->settings, sizeof(record->settings)) == record->crc) ? 1U : 0U;
}

/* Addressing goes back to the factory values; MAC, JSON API and the web password are kept. */
static void Settings_ResetNetworkToDefaults(Settings_t *settings)
{
  settings->use_dhcp = settings_defaults.use_dhcp;
  settings->dhcp_fallback_attempts = settings_defaults.dhcp_fallback_attempts;
  memcpy(settings->ip, settings_defaults.ip, sizeof(settings->ip));
  memcpy(settings->mask, settings_defaults.mask, sizeof(settings->mask));
  memcpy(settings->gateway, settings_defaults.gateway, sizeof(settings->gateway));
  memcpy(settings->dns, settings_defaults.dns, sizeof(settings->dns));
}

void Settings_SetDefaults(void)
{
  /* Through Settings_Set so the defaults get the same normalisation as loaded settings
   * (empty preset slots, MAC from the chip UID since the default MAC is all zero). */
  Settings_Set(&settings_defaults);
}

/* Copies an older record that is an exact prefix of Settings_t (size bytes) over *settings.
 * The old record is {magic, version, settings[size], crc} with the crc 4-byte aligned. */
static uint8_t Settings_LoadPrefix(uint32_t version, uint32_t size, Settings_t *settings)
{
  const SettingsRecord_t *record = (const SettingsRecord_t *)SETTINGS_FLASH_ADDRESS;
  const uint8_t *old_settings = (const uint8_t *)SETTINGS_FLASH_ADDRESS + (2U * sizeof(uint32_t));
  uint32_t old_crc = *(const uint32_t *)(old_settings + ((size + 3U) & ~3U));

  if ((record->magic != SETTINGS_MAGIC) || (record->version != version) ||
      (Settings_Crc32(old_settings, size) != old_crc))
  {
    return 0U;
  }
  memcpy(settings, old_settings, size);
  return 1U;
}

uint8_t Settings_Load(void)
{
  const SettingsRecord_t *record = (const SettingsRecord_t *)SETTINGS_FLASH_ADDRESS;

  Settings_t migrated;

  if (Settings_IsRecordValid(record, SETTINGS_VERSION) != 0U)
  {
    Settings_Set(&record->settings);
    return 1U;
  }

  Settings_SetDefaults();
  migrated = settings_current;   /* fields an old record lacks keep their defaults */

  if (Settings_LoadPrefix(SETTINGS_V13_VERSION, SETTINGS_V13_SIZE, &migrated) != 0U)
  {
    /* v13 -> v14: no rotation limits yet; everything else is kept. */
  }
  else if ((Settings_LoadPrefix(SETTINGS_V12_VERSION, SETTINGS_V13_SIZE, &migrated) != 0U) ||
           (Settings_LoadPrefix(SETTINGS_V11_VERSION, SETTINGS_V11_SIZE, &migrated) != 0U))
  {
    /* v12 (same layout as v13) and v11 (the azimuth block was appended in v12) are prefixes too.
     * They also take the factory network defaults once (the reason for v13). */
    Settings_ResetNetworkToDefaults(&migrated);
  }
  else
  {
    return 0U;   /* nothing usable in flash: factory defaults */
  }

  Settings_Set(&migrated);
  /* Persist right away so the migration (and a network reset) happens exactly once, not on every boot. */
  (void)Settings_Save();
  return 1U;
}

uint8_t Settings_Save(void)
{
  FLASH_EraseInitTypeDef erase = {0};
  SettingsRecord_t record;
  uint32_t page_error = 0U;
  uint32_t address;
  uint16_t *data;
  uint16_t index;

  record.magic = SETTINGS_MAGIC;
  record.version = SETTINGS_VERSION;
  record.settings = settings_current;
  record.crc = Settings_Crc32(&record.settings, sizeof(record.settings));

  if (HAL_FLASH_Unlock() != HAL_OK)
  {
    return 0U;
  }

  erase.TypeErase = FLASH_TYPEERASE_PAGES;
  erase.PageAddress = SETTINGS_FLASH_ADDRESS;
  erase.NbPages = 1U;
  if (HAL_FLASHEx_Erase(&erase, &page_error) != HAL_OK)
  {
    (void)HAL_FLASH_Lock();
    return 0U;
  }

  address = SETTINGS_FLASH_ADDRESS;
  data = (uint16_t *)&record;
  for (index = 0U; index < (sizeof(record) / 2U); index++)
  {
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, address, data[index]) != HAL_OK)
    {
      (void)HAL_FLASH_Lock();
      return 0U;
    }
    address += 2U;
  }

  (void)HAL_FLASH_Lock();
  return Settings_IsRecordValid((const SettingsRecord_t *)SETTINGS_FLASH_ADDRESS, SETTINGS_VERSION);
}

const Settings_t *Settings_Get(void)
{
  return &settings_current;
}

void Settings_Set(const Settings_t *settings)
{
  uint8_t index;

  if (settings == NULL)
  {
    return;
  }

  settings_current = *settings;
  settings_current.use_dhcp = (settings_current.use_dhcp != 0U) ? 1U : 0U;
  settings_current.json_api_enabled = (settings_current.json_api_enabled != 0U) ? 1U : 0U;
  settings_current.json_api_mode = (settings_current.json_api_mode == SETTINGS_JSON_API_MODE_TCP) ? SETTINGS_JSON_API_MODE_TCP : SETTINGS_JSON_API_MODE_UDP;
  if (settings_current.json_api_port == 0U) settings_current.json_api_port = SETTINGS_JSON_API_DEFAULT_PORT;
  if (Settings_IsMacValid(settings_current.mac) == 0U) Settings_GenerateMac(settings_current.mac);
  settings_current.web_auth_enabled = (settings_current.web_auth_enabled != 0U) ? 1U : 0U;
  settings_current.web_password[SETTINGS_WEB_PASSWORD_LENGTH - 1U] = '\0';
  for (index = 0U; index < (SETTINGS_WEB_PASSWORD_LENGTH - 1U); index++)
  {
    char symbol = settings_current.web_password[index];
    if (symbol == '\0')
    {
      break;
    }

    if ((symbol < '!') || (symbol > '~') || (symbol == '"') || (symbol == '\\'))
    {
      settings_current.web_password[index] = '\0';
      break;
    }
  }
  if (settings_current.web_password[0] == '\0') strncpy(settings_current.web_password, settings_defaults.web_password, SETTINGS_WEB_PASSWORD_LENGTH);
  if (settings_current.default_servo1 > SETTINGS_SERVO_MAX) settings_current.default_servo1 = SETTINGS_SERVO_MAX;
  if (settings_current.default_servo2 > SETTINGS_SERVO_MAX) settings_current.default_servo2 = SETTINGS_SERVO_MAX;
  if ((settings_current.servo1_center == 0U) || (settings_current.servo1_center > SETTINGS_SERVO_MAX)) settings_current.servo1_center = SETTINGS_SERVO_CENTER_DEFAULT;
  if ((settings_current.servo2_center == 0U) || (settings_current.servo2_center > SETTINGS_SERVO_MAX)) settings_current.servo2_center = SETTINGS_SERVO_CENTER_DEFAULT;

  if ((settings_current.az_pulse_min == 0U) || (settings_current.az_pulse_min >= settings_current.az_pulse_max) ||
      (settings_current.az_pulse_max > SETTINGS_SERVO_MAX))
  {
    settings_current.az_pulse_min = SETTINGS_AZ_PULSE_MIN_DEFAULT;
    settings_current.az_pulse_max = SETTINGS_AZ_PULSE_MAX_DEFAULT;
  }
  if ((settings_current.az_range_x10 == 0U) || (settings_current.az_range_x10 > 3600U)) settings_current.az_range_x10 = SETTINGS_AZ_RANGE_X10_DEFAULT;
  if ((settings_current.az_servo != 1U) && (settings_current.az_servo != 2U)) settings_current.az_servo = 1U;
  settings_current.az_invert = (settings_current.az_invert != 0U) ? 1U : 0U;
  if ((settings_current.az_cal_pulse > SETTINGS_SERVO_MAX) || (settings_current.az_cal_azimuth_x10 >= 3600U)) settings_current.az_cal_pulse = 0U;
  for (index = 0U; index < SETTINGS_AZ_PRESET_COUNT; index++)
  {
    Settings_AzPreset_t *preset = &settings_current.az_presets[index];

    preset->name[SETTINGS_AZ_PRESET_NAME_LENGTH - 1U] = '\0';
    if ((preset->name[0] == '\0') || (preset->azimuth_x10 >= 3600U))
    {
      preset->azimuth_x10 = SETTINGS_AZ_EMPTY;
      memset(preset->name, 0, sizeof(preset->name));
    }
  }
  if (settings_current.az_limit_pulse_min > SETTINGS_SERVO_MAX) settings_current.az_limit_pulse_min = 0U;
  if (settings_current.az_limit_pulse_max > SETTINGS_SERVO_MAX) settings_current.az_limit_pulse_max = 0U;
  if ((settings_current.az_limit_pulse_min != 0U) && (settings_current.az_limit_pulse_max != 0U) &&
      (settings_current.az_limit_pulse_min > settings_current.az_limit_pulse_max))
  {
    settings_current.az_limit_pulse_min = 0U;
    settings_current.az_limit_pulse_max = 0U;
  }
}

void Settings_ToJson(char *buffer, uint16_t length)
{
  (void)snprintf(buffer, length,
                 "{\"dhcp\":%u,\"dhcp_fallback_attempts\":%u,\"pwm_defaults\":{\"servo1\":%u,\"servo2\":%u,\"servo1_speed\":%u,\"servo2_speed\":%u,\"servo1_center\":%u,\"servo2_center\":%u},\"json_api\":{\"enabled\":%u,\"mode\":%u,\"port\":%u},\"web_auth\":{\"enabled\":%u,\"password\":\"%s\"},\"ip\":\"%u.%u.%u.%u\",\"mask\":\"%u.%u.%u.%u\",\"gateway\":\"%u.%u.%u.%u\",\"dns\":\"%u.%u.%u.%u\",\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\"}\n",
                 settings_current.use_dhcp,
                 settings_current.dhcp_fallback_attempts,
                 settings_current.default_servo1,
                 settings_current.default_servo2,
                 settings_current.default_servo1_speed,
                 settings_current.default_servo2_speed,
                 settings_current.servo1_center,
                 settings_current.servo2_center,
                 settings_current.json_api_enabled,
                 settings_current.json_api_mode,
                 settings_current.json_api_port,
                 settings_current.web_auth_enabled,
                 settings_current.web_password,
                 settings_current.ip[0], settings_current.ip[1], settings_current.ip[2], settings_current.ip[3],
                 settings_current.mask[0], settings_current.mask[1], settings_current.mask[2], settings_current.mask[3],
                 settings_current.gateway[0], settings_current.gateway[1], settings_current.gateway[2], settings_current.gateway[3],
                 settings_current.dns[0], settings_current.dns[1], settings_current.dns[2], settings_current.dns[3],
                 settings_current.mac[0], settings_current.mac[1], settings_current.mac[2],
                 settings_current.mac[3], settings_current.mac[4], settings_current.mac[5]);
}

uint8_t Settings_CheckWebPassword(const char *password)
{
  if ((password == NULL) || (password[0] == '\0'))
  {
    return 0U;
  }

  return (strncmp(password, settings_current.web_password, SETTINGS_WEB_PASSWORD_LENGTH) == 0) ? 1U : 0U;
}

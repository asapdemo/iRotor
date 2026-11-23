#ifndef __SETTINGS_SERVICE_H
#define __SETTINGS_SERVICE_H

#include "main.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bump when Settings_t changes (and migrate in Settings_Load). Also reported in the firmware
 * version stamp, so the web firmware tab can warn when an update will reset settings. */
#define SETTINGS_VERSION 14UL
#define SETTINGS_FLASH_ADDRESS 0x0800FC00UL
#define SETTINGS_FLASH_PAGE_SIZE 1024UL
#define SETTINGS_JSON_API_MODE_UDP 0U
#define SETTINGS_JSON_API_MODE_TCP 1U
#define SETTINGS_JSON_API_DEFAULT_PORT 6700U
#define SETTINGS_WEB_PASSWORD_LENGTH 32U
#define SETTINGS_SERVO_CENTER_DEFAULT 1372U
#define SETTINGS_AZ_PRESET_COUNT 8U
#define SETTINGS_AZ_PRESET_NAME_LENGTH 32U   /* bytes incl. NUL, UTF-8: ~15 Cyrillic letters */
#define SETTINGS_AZ_EMPTY 0xFFFFU            /* azimuth_x10 of an empty preset slot */

typedef struct
{
  uint16_t azimuth_x10;                      /* 0..3599 = 0.0..359.9 deg, SETTINGS_AZ_EMPTY = unused */
  char name[SETTINGS_AZ_PRESET_NAME_LENGTH];
} Settings_AzPreset_t;

typedef struct
{
  uint8_t use_dhcp;
  uint8_t dhcp_fallback_attempts;
  uint8_t json_api_enabled;
  uint8_t web_auth_enabled;
  uint8_t ip[4];
  uint8_t mask[4];
  uint8_t gateway[4];
  uint8_t dns[4];
  uint8_t mac[6];
  uint16_t default_servo1;
  uint16_t default_servo2;
  uint16_t json_api_port;
  uint8_t json_api_mode;
  uint8_t default_servo1_speed;
  uint8_t default_servo2_speed;
  uint16_t servo1_center;
  uint16_t servo2_center;
  char web_password[SETTINGS_WEB_PASSWORD_LENGTH];
  /* --- azimuth UI (v12). Keep these last: a v11 record is an exact prefix and is migrated on load. */
  uint16_t az_pulse_min;                     /* pulse range that covers az_range_x10 of rotation */
  uint16_t az_pulse_max;
  uint16_t az_range_x10;                     /* real rotation over that range, 0.1 deg */
  uint16_t az_cal_pulse;                     /* bound pulse, 0 = not bound to an azimuth */
  uint16_t az_cal_azimuth_x10;               /* azimuth of az_cal_pulse, 0.1 deg */
  uint8_t az_servo;                          /* 1 or 2 */
  uint8_t az_invert;                         /* 1 = a larger pulse turns counter-clockwise */
  Settings_AzPreset_t az_presets[SETTINGS_AZ_PRESET_COUNT];
  /* --- rotation limits (v14), appended: a v13 record is an exact prefix. Stored as pulses so they stay
   * on the same physical spot when the binding changes. 0 = no limit on that side. */
  uint16_t az_limit_pulse_min;
  uint16_t az_limit_pulse_max;
  /* The limits as entered (0.1 deg, SETTINGS_AZ_EMPTY = side free) and the binding pulse they were
   * entered against: reported as is while the binding stays there (pulses round to ~0.2 deg). */
  uint16_t az_limit_plus_x10;
  uint16_t az_limit_minus_x10;
  uint16_t az_limit_ref_pulse;
} Settings_t;

void Settings_SetDefaults(void);
uint8_t Settings_Load(void);
uint8_t Settings_Save(void);
const Settings_t *Settings_Get(void);
void Settings_Set(const Settings_t *settings);
void Settings_ToJson(char *buffer, uint16_t length);
uint8_t Settings_CheckWebPassword(const char *password);

#ifdef __cplusplus
}
#endif

#endif /* __SETTINGS_SERVICE_H */

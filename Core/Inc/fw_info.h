#ifndef __FW_INFO_H
#define __FW_INFO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Version stamp stored inside the firmware image.
 *
 * The web "Прошивка" tab finds it in a .bin before flashing (it scans for the magic at
 * 4-byte steps) to show what is about to be written and to refuse firmware for another board.
 * Keep the layout stable: the page reads fixed offsets.
 *
 *   offset  size  field
 *        0     8  magic "AZFWINF1" (not NUL-terminated)
 *        8     4  board_id, the bytes "SAZ1"
 *       12     2  version_major          (little-endian)
 *       14     2  version_minor
 *       16     2  version_patch
 *       18     2  settings_version       (SETTINGS_VERSION of this build)
 *       20    40  git describe, NUL-terminated
 *       60    20  build time "YYYY-MM-DD HH:MM", NUL-terminated
 *   total 80 bytes
 *
 * Do not write the magic as a string literal anywhere else in the firmware:
 * a second copy in .rodata would confuse the search. */
#define FW_INFO_BOARD_ID 0x315A4153UL   /* "SAZ1" in memory order */

typedef struct
{
  char magic[8];
  uint32_t board_id;
  uint16_t version_major;
  uint16_t version_minor;
  uint16_t version_patch;
  uint16_t settings_version;
  char git[40];
  char build_time[20];
} FwInfo_t;

extern const FwInfo_t fw_info;

#ifdef __cplusplus
}
#endif

#endif /* __FW_INFO_H */

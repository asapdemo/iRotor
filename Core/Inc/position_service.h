#ifndef __POSITION_SERVICE_H
#define __POSITION_SERVICE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Last known servo positions, kept across reboots.
 * The servos have no feedback, so after power-up the board does not know where the mechanism stands.
 * This module logs the last position of both servos into two dedicated flash pages
 * (append-only, 4 bytes per record, erased only when full) and hands it back to pwm_service at boot,
 * so the first move after a reboot ramps from there at the set speed instead of jumping. */

#define POSITION_FLASH_ADDRESS 0x0800F400UL   /* 2 pages below the settings page; the linker stops at 61K */
#define POSITION_FLASH_PAGES 2U

/* Reads the log and gives the positions to pwm_service. Call after PWM_ServiceInit, before ApplyDefaults. */
void Position_ServiceInit(void);
/* Call from the superloop: logs the positions once the servos stop (and every few seconds during long moves). */
void Position_ServiceTask(void);

#ifdef __cplusplus
}
#endif

#endif /* __POSITION_SERVICE_H */

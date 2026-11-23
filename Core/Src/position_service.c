#include "position_service.h"

#include "main.h"
#include "pwm_service.h"

/* Layout: a 32-bit magic at the start, then records of two half-words {servo1, servo2} in us.
 * An erased slot reads 0xFFFF. A slot whose first half-word is written is "used"; a record is valid only
 * when both halves are <= PWM_SERVICE_SERVO_MAX (a write torn by a power loss is skipped). */
#define POSITION_MAGIC 0x474F4C50UL                     /* "PLOG" */
#define POSITION_PAGE_SIZE 1024UL
#define POSITION_AREA_SIZE (POSITION_FLASH_PAGES * POSITION_PAGE_SIZE)
#define POSITION_FIRST_RECORD (POSITION_FLASH_ADDRESS + 4UL)
#define POSITION_RECORD_SIZE 4UL
#define POSITION_RECORD_COUNT ((POSITION_AREA_SIZE - 4UL) / POSITION_RECORD_SIZE)
#define POSITION_ERASED 0xFFFFU

#define POSITION_CHECK_MS 100U          /* how often the task looks at the servos */
#define POSITION_STILL_MS 1000U         /* stopped this long -> log */
#define POSITION_MOVING_MS 10000U       /* while moving, log at most this often */

static uint32_t position_next_slot;     /* index of the first erased slot; POSITION_RECORD_COUNT = full */
static uint16_t position_saved[2];
static uint8_t position_have_saved;
static uint32_t position_check_tick;
static uint32_t position_still_since;
static uint32_t position_saved_tick;
static uint8_t position_was_moving;

static uint16_t Position_Read16(uint32_t address)
{
  return *(volatile const uint16_t *)address;
}

static uint8_t Position_HasMagic(void)
{
  return (*(volatile const uint32_t *)POSITION_FLASH_ADDRESS == POSITION_MAGIC) ? 1U : 0U;
}

static uint8_t Position_Erase(void)
{
  FLASH_EraseInitTypeDef erase;
  uint32_t page_error = 0UL;
  uint8_t ok;

  erase.TypeErase = FLASH_TYPEERASE_PAGES;
  erase.PageAddress = POSITION_FLASH_ADDRESS;
  erase.NbPages = POSITION_FLASH_PAGES;
  ok = (HAL_FLASHEx_Erase(&erase, &page_error) == HAL_OK) ? 1U : 0U;
  IWDG->KR = LL_IWDG_KEY_RELOAD;
  if (ok != 0U)
  {
    ok = ((HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, POSITION_FLASH_ADDRESS, POSITION_MAGIC & 0xFFFFUL) == HAL_OK) &&
          (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, POSITION_FLASH_ADDRESS + 2UL, POSITION_MAGIC >> 16) == HAL_OK)) ? 1U : 0U;
  }
  position_next_slot = 0UL;
  return ok;
}

static void Position_Save(uint16_t servo1, uint16_t servo2)
{
  uint32_t address;

  if (HAL_FLASH_Unlock() != HAL_OK)
  {
    return;
  }
  if (((Position_HasMagic() == 0U) || (position_next_slot >= POSITION_RECORD_COUNT)) && (Position_Erase() == 0U))
  {
    (void)HAL_FLASH_Lock();
    return;
  }

  address = POSITION_FIRST_RECORD + (position_next_slot * POSITION_RECORD_SIZE);
  position_next_slot++;                 /* even a failed write uses the slot up */
  if ((HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, address, servo1) == HAL_OK) &&
      (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, address + 2UL, servo2) == HAL_OK))
  {
    position_saved[0] = servo1;
    position_saved[1] = servo2;
    position_have_saved = 1U;
  }
  (void)HAL_FLASH_Lock();
}

void Position_ServiceInit(void)
{
  uint32_t slot;

  position_next_slot = POSITION_RECORD_COUNT;   /* no magic: the first save erases the area */
  position_have_saved = 0U;
  if (Position_HasMagic() != 0U)
  {
    for (slot = 0UL; slot < POSITION_RECORD_COUNT; slot++)
    {
      uint32_t address = POSITION_FIRST_RECORD + (slot * POSITION_RECORD_SIZE);
      uint16_t servo1 = Position_Read16(address);
      uint16_t servo2 = Position_Read16(address + 2UL);

      if (servo1 == POSITION_ERASED)
      {
        break;
      }
      if ((servo1 <= PWM_SERVICE_SERVO_MAX) && (servo2 <= PWM_SERVICE_SERVO_MAX))
      {
        position_saved[0] = servo1;
        position_saved[1] = servo2;
        position_have_saved = 1U;
      }
    }
    position_next_slot = slot;
  }

  if (position_have_saved != 0U)
  {
    PWM_ServiceSetKnownPositions(position_saved[0], position_saved[1]);
  }
  position_check_tick = HAL_GetTick();
  position_still_since = position_check_tick;
  position_saved_tick = position_check_tick;
}

void Position_ServiceTask(void)
{
  uint32_t now = HAL_GetTick();
  uint16_t servo1;
  uint16_t servo2;

  if ((now - position_check_tick) < POSITION_CHECK_MS)
  {
    return;
  }
  position_check_tick = now;

  if (PWM_ServiceIsMoving() != 0U)
  {
    if (position_was_moving == 0U)
    {
      position_saved_tick = now;        /* a move starts: the periodic log counts from here */
    }
    position_was_moving = 1U;
    position_still_since = now;
  }
  else
  {
    position_was_moving = 0U;
  }

  servo1 = PWM_ServiceGetKnownPosition(0U);
  servo2 = PWM_ServiceGetKnownPosition(1U);
  if ((position_have_saved != 0U) && (servo1 == position_saved[0]) && (servo2 == position_saved[1]))
  {
    return;
  }

  if (((now - position_still_since) >= POSITION_STILL_MS) || ((now - position_saved_tick) >= POSITION_MOVING_MS))
  {
    position_saved_tick = now;
    position_still_since = now;         /* a failed write is retried in a second, not every 100 ms */
    Position_Save(servo1, servo2);
  }
}

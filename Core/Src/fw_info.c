#include "fw_info.h"
#include "fw_version.h"        /* generated in the build directory by tools/fw_version.cmake */
#include "settings_service.h"

_Static_assert(sizeof(FwInfo_t) == 80U, "FwInfo_t layout is read by web_ui/index.html at fixed offsets");

const FwInfo_t fw_info =
{
  {'A', 'Z', 'F', 'W', 'I', 'N', 'F', '1'},
  FW_INFO_BOARD_ID,
  FW_VERSION_MAJOR,
  FW_VERSION_MINOR,
  FW_VERSION_PATCH,
  (uint16_t)SETTINGS_VERSION,
  FW_GIT_DESCRIBE,
  FW_BUILD_TIME
};

#ifndef __WEB_PAGES_H
#define __WEB_PAGES_H

#include "main.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Azimuth control UI: web_ui/index.html, minified + gzipped into web_ui_gz.c by tools/build_web_ui.py */
extern const uint8_t web_ui_gz[];
extern const uint16_t web_ui_gz_len;

void WebPages_BuildDataJson(char *buffer, uint16_t length);
void WebPages_BuildSettingsJson(char *buffer, uint16_t length);
uint8_t WebPages_HandlePwmRequest(const char *request);
uint8_t WebPages_HandleApiJson(const char *request);      /* returns Az_Error_t (AZ_OK = 0) */
void WebPages_BuildApiReply(char *buffer, uint16_t length, uint8_t error);
uint8_t WebPages_HandleAzGoto(const char *request, char *reply, uint16_t reply_length);
uint8_t WebPages_HandleBoardRequest(const char *request, uint8_t save);
uint8_t WebPages_HandleNetSettingsRequest(const char *request, uint8_t save);
void WebPages_BuildAzConfigJson(char *buffer, uint16_t length);
uint8_t WebPages_HandleAzRequest(const char *request);

#ifdef __cplusplus
}
#endif

#endif /* __WEB_PAGES_H */

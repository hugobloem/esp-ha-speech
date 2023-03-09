#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// void app_api_rest_test(void)

void app_api_rest_get(char* path, char* response_buffer);
void app_api_rest_post(char* path, char* response_buffer, char* message);

#ifdef __cplusplus
}
#endif

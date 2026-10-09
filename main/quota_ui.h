#pragma once

#include "quota_service.h"

void quota_ui_init(void);
void quota_ui_render(const quota_navigation_t *navigation, const quota_service_view_t *service,
                     int battery_percent, bool usb_powered);

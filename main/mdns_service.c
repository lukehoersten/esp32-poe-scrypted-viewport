#include "mdns_service.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "mdns.h"

#include "viewport_state.h"

static const char *TAG       = "mdns";
static const char *SERVICE   = "_scrypted-viewport";
static const char *PROTO     = "_tcp";
static const uint16_t PORT   = 80;

// Pull hostname + TXT values from viewport_state under one lock acquisition,
// then push them at the mDNS service. mdns_service_add() rejects with
// INVALID_ARG if the hostname isn't set, so order matters on first apply.
// viewport_name is always populated (MAC-derived default) so the hostname
// is always `viewport-<name>`.
static esp_err_t apply_state(bool include_hostname)
{
    char hostname[80];
    char name[64];
    const char *resolution, *orientation;

    viewport_state_lock();
    viewport_state_t *st = viewport_state_get();
    snprintf(hostname, sizeof(hostname), "viewport-%s", st->viewport_name);
    snprintf(name,     sizeof(name),     "%s",         st->viewport_name);
    resolution  = viewport_state_resolution_str();
    orientation = (st->orientation == VIEWPORT_ORIENTATION_PORTRAIT)
                      ? "portrait" : "landscape";
    viewport_state_unlock();

    if (include_hostname) {
        ESP_RETURN_ON_ERROR(mdns_hostname_set(hostname), TAG, "hostname_set");
    }
    mdns_txt_item_t txt[] = {
        { "version",     VIEWPORT_VERSION },
        { "resolution",  resolution },
        { "orientation", orientation },
        { "name",        name },
    };
    return mdns_service_txt_set(SERVICE, PROTO, txt,
                                sizeof(txt) / sizeof(txt[0]));
}

esp_err_t mdns_service_start(void)
{
    char hostname[80];
    viewport_state_lock();
    viewport_state_t *st = viewport_state_get();
    snprintf(hostname, sizeof(hostname), "viewport-%s", st->viewport_name);
    viewport_state_unlock();

    ESP_RETURN_ON_ERROR(mdns_init(),                       TAG, "mdns_init");
    ESP_RETURN_ON_ERROR(mdns_hostname_set(hostname),        TAG, "hostname_set");
    ESP_RETURN_ON_ERROR(
        mdns_service_add(NULL, SERVICE, PROTO, PORT, NULL, 0),
        TAG, "service_add");
    ESP_RETURN_ON_ERROR(apply_state(false),                 TAG, "txt_set");

    ESP_LOGI(TAG, "mDNS up — %s.local advertising %s.%s.local on :%u",
             hostname, SERVICE, PROTO, PORT);
    return ESP_OK;
}

esp_err_t mdns_service_refresh(void)
{
    return apply_state(true);
}

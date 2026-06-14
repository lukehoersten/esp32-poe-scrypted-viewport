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

static esp_err_t apply_records(void)
{
    viewport_state_lock();
    viewport_state_t *st = viewport_state_get();

    char hostname[80];
    if (st->viewport_name[0]) {
        snprintf(hostname, sizeof(hostname), "viewport-%s", st->viewport_name);
    } else {
        snprintf(hostname, sizeof(hostname), "viewport");
    }

    const char *resolution  = viewport_state_resolution_str();
    const char *orientation = (st->orientation == VIEWPORT_ORIENTATION_PORTRAIT)
                                  ? "portrait" : "landscape";
    const char *name_value  = st->viewport_name[0] ? st->viewport_name : "";

    viewport_state_unlock();

    mdns_txt_item_t txt[] = {
        { "version",     VIEWPORT_VERSION },
        { "resolution",  resolution },
        { "orientation", orientation },
        { "name",        name_value },
    };
    const size_t txt_count = sizeof(txt) / sizeof(txt[0]);

    ESP_RETURN_ON_ERROR(mdns_hostname_set(hostname), TAG, "hostname_set");
    ESP_RETURN_ON_ERROR(mdns_service_txt_set(SERVICE, PROTO, txt, txt_count),
                       TAG, "txt_set");

    ESP_LOGI(TAG, "advertising %s.local on %s.%s.local:%u",
             hostname, SERVICE, PROTO, PORT);
    return ESP_OK;
}

esp_err_t mdns_service_start(void)
{
    ESP_RETURN_ON_ERROR(mdns_init(), TAG, "mdns_init");
    ESP_RETURN_ON_ERROR(
        mdns_service_add(NULL, SERVICE, PROTO, PORT, NULL, 0),
        TAG, "service_add");
    return apply_records();
}

esp_err_t mdns_service_refresh(void)
{
    return apply_records();
}

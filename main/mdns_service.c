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

// Pull the current hostname + TXT values from viewport_state under the lock.
static void snapshot_state(char *hostname, size_t host_cap,
                           const char **resolution,
                           const char **orientation,
                           char *name, size_t name_cap)
{
    viewport_state_lock();
    viewport_state_t *st = viewport_state_get();

    if (st->viewport_name[0]) {
        snprintf(hostname, host_cap, "viewport-%s", st->viewport_name);
    } else {
        snprintf(hostname, host_cap, "viewport");
    }
    *resolution  = viewport_state_resolution_str();
    *orientation = (st->orientation == VIEWPORT_ORIENTATION_PORTRAIT)
                       ? "portrait" : "landscape";
    snprintf(name, name_cap, "%s", st->viewport_name[0] ? st->viewport_name : "");

    viewport_state_unlock();
}

static esp_err_t apply_hostname(void)
{
    char hostname[80];
    const char *resolution, *orientation;
    char name[64];
    snapshot_state(hostname, sizeof(hostname),
                   &resolution, &orientation,
                   name, sizeof(name));
    return mdns_hostname_set(hostname);
}

static esp_err_t apply_txt(void)
{
    char hostname[80];
    const char *resolution, *orientation;
    char name[64];
    snapshot_state(hostname, sizeof(hostname),
                   &resolution, &orientation,
                   name, sizeof(name));

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
    // Order matters: mdns_service_add() rejects with INVALID_ARG if the
    // hostname isn't set yet. So: init -> hostname -> service_add -> TXT.
    ESP_RETURN_ON_ERROR(mdns_init(),           TAG, "mdns_init");
    ESP_RETURN_ON_ERROR(apply_hostname(),       TAG, "hostname_set");
    ESP_RETURN_ON_ERROR(
        mdns_service_add(NULL, SERVICE, PROTO, PORT, NULL, 0),
        TAG, "service_add");
    ESP_RETURN_ON_ERROR(apply_txt(),            TAG, "txt_set");

    char hostname[80];
    const char *resolution, *orientation;
    char name[64];
    snapshot_state(hostname, sizeof(hostname),
                   &resolution, &orientation,
                   name, sizeof(name));
    ESP_LOGI(TAG, "mDNS up — %s.local advertising %s.%s.local on :%u",
             hostname, SERVICE, PROTO, PORT);
    return ESP_OK;
}

esp_err_t mdns_service_refresh(void)
{
    ESP_RETURN_ON_ERROR(apply_hostname(), TAG, "hostname_set");
    return apply_txt();
}

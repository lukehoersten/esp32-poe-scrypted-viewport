#pragma once

#include "esp_err.h"
#include "viewport_state.h"

esp_err_t state_client_init(void);

// Queue a state-change POST to <scrypted>/state.
//
// Concurrency model (per spec):
//   - At most one HTTP POST in flight; a dedicated worker task drives it.
//   - Depth-1 queue. If a POST is already queued, replace it with the newer
//     one — the in-flight POST is never cancelled.
//   - Fire-and-forget from the caller's perspective. Local state change
//     proceeds immediately regardless of POST outcome.
//
// Silently dropped if the device has no Scrypted URL configured.
void state_client_post(viewport_run_state_t state);

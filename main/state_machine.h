#pragma once

#include "esp_err.h"
#include "viewport_state.h"

esp_err_t state_machine_init(void);

// Transition to AWAKE or ASLEEP. Idempotent: no-op if already there.
// Side-effects (under one critical section):
//   AWAKE  -> backlight on, idle timer (re)armed
//   ASLEEP -> backlight off, idle timer cancelled, framebuffer discarded
esp_err_t state_machine_set(viewport_run_state_t target);

// Device-initiated variant (tap, idle timer expiry). Drives the same
// transition as state_machine_set() and additionally enqueues a
// {viewport,state} POST to <scrypted>/state via state_client when a
// `scrypted` URL is configured.
void state_machine_set_local(viewport_run_state_t target);

// Called by /frame after a successful paint. Restarts the idle timer
// while awake; no-op otherwise.
void state_machine_frame_painted(void);

// Snapshot of current state.
viewport_run_state_t state_machine_current(void);

#pragma once

/// Initialise event handling (call after ui_init and sound_init).
void event_init(void);

/// Call from main loop to check for BLE events and dispatch sounds/UI.
void event_tick(void);

/// Returns true while the permission dialog is showing.
bool event_permission_active(void);

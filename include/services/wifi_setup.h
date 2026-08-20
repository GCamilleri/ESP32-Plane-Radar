#pragma once

#include <cstdint>

/** True when the next boot should show the setup screen first (after credential reset). */
bool wifiShowsSetupScreenOnBoot();
void wifiResetCredentialsAndReboot();
/** Boot flow: connect with UI, open portal only if saved creds fail. */
bool wifiSetupConnect();
/** Reconnect using saved creds; never opens the captive portal. */
bool wifiReconnect();
/** Keeps the LAN config portal alive; call every loop() iteration. */
void wifiLoop();
bool wifiBootButtonPressed();
/** GPIO + interrupt setup; call once early in setup(). */
void bootButtonInit();
/** Latched short tap (survives blocking HTTP/display work). */
bool bootButtonConsumeTap();
/** Debounced multi-tap gesture. Returns tap count (0 = nothing yet). */
uint8_t bootButtonConsumeGesture();
/** True if the BOOT button is physically held right now. */
bool bootButtonIsHeld();
/** How long (ms) the current press has been held; 0 if not held. */
unsigned long bootButtonHeldMs();
/**
 * Arm or disarm the hold-to-reset gesture. Disarm it wherever a hold already
 * means something else (the settings menu selects on a 1 s hold), or a slightly
 * long press there would erase the user's credentials. Re-arming never fires on
 * the press that is already in progress.
 */
void bootButtonSetLongPressEnabled(bool enabled);
/** Call each loop iteration; triggers WiFi reset on long hold when armed. */
void bootButtonPollLongPress();

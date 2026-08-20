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
/**
 * True once the LAN config portal is actually serving.
 *
 * The toggle that enables it only records a preference; the server comes up on
 * the next wifiLoop() and only if the link is up. Without this the menu would
 * claim the portal is running before it is.
 */
bool wifiLanConfigActive();
/**
 * STA address as a string, or an empty string when the link is down. The menu
 * shows it because the LAN portal deliberately runs no access point: there is no
 * SSID to find, so an address on screen is the only way to reach it.
 */
const char* wifiLocalIpString();
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

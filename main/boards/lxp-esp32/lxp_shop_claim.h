#ifndef LXP_SHOP_CLAIM_H
#define LXP_SHOP_CLAIM_H

#include <string>
#include <wifi_configuration_ap.h>

/**
 * Normalize pairing input (uppercase, strip LXPTERM: / ?c=) and POST claim to LXP API.
 * On success writes NVS namespace "lxp" (mqtt_host/port/user/pass, shop_id, device_id).
 */
WifiLxpClaimResult LxpClaimShop(const std::string& pairing_code_raw);

/**
 * Retry shop claim from NVS key pending_pairing (written during SoftAP /submit).
 * On success clears pending_pairing. Safe to call when network is up.
 */
void LxpTryClaimPendingPairing();

#endif  // LXP_SHOP_CLAIM_H

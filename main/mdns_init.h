#pragma once

/* Initialize mDNS and register synth-32.local / synth.local / synth32.local.
 * Must be called after WiFi SoftAP is up. */
void mdns_init_synth(void);

/* Start the captive-portal UDP DNS responder on port 53.
 * Resolves every hostname to 192.168.4.1, triggering the OS portal popup. */
void dns_responder_start(void);

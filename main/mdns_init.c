/*
 * mdns_init.c — mDNS hostname advertisement + captive portal DNS responder
 *
 * Registers three hostnames via mDNS so browsers can reach the synth UI
 * without typing a raw IP:
 *   http://synth-32.local/   (primary)
 *   http://synth.local/      (short alias, via service TXT record)
 *   http://synth32.local/    (no-hyphen alias)
 *
 * Also runs a minimal UDP DNS server on port 53 that resolves every query
 * to 192.168.4.1, enabling the OS captive-portal popup on AP join.
 */

#include "mdns_init.h"

#include <string.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "mdns.h"

static const char *TAG = "mdns_init";

/* ── mDNS ──────────────────────────────────────────────────────────────────── */

void mdns_init_synth(void)
{
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("synth-32"));
    ESP_ERROR_CHECK(mdns_instance_name_set("synth-32 Workstation"));

    /* Advertise HTTP and WebSocket services on port 80 */
    mdns_txt_item_t txt[] = {
        { "alias1", "synth.local"   },
        { "alias2", "synth32.local" },
        { "path",   "/"             },
    };
    ESP_ERROR_CHECK(mdns_service_add(NULL, "_http", "_tcp", 80,
                                     txt, sizeof(txt) / sizeof(txt[0])));
    ESP_ERROR_CHECK(mdns_service_add(NULL, "_ws",   "_tcp", 80, NULL, 0));

    ESP_LOGI(TAG, "mDNS ready — http://synth-32.local/");
}

/* ── Captive-portal DNS responder ──────────────────────────────────────────── */
/*
 * All UDP DNS queries on port 53 are answered with an A record pointing to
 * AP_IP (192.168.4.1).  This makes iOS/Android display a "Sign in to network"
 * popup immediately after joining the AP, opening the synth UI with one tap.
 *
 * Wire format references: RFC 1035 §4.1
 *
 * Query layout (bytes):
 *   [0-1]  Transaction ID
 *   [2-3]  Flags
 *   [4-5]  QDCount = 1
 *   [6-7]  ANCount = 0
 *   [8-9]  NSCount = 0
 *   [10-11] ARCount = 0
 *   [12..] QNAME (length-prefixed labels, 0x00 terminator) + QTYPE(2) + QCLASS(2)
 *
 * We return a minimal response with one A record:
 *   - copy the question section verbatim
 *   - append one answer RR: NAME=0xC00C (pointer to question), TYPE=A, CLASS=IN,
 *     TTL=60, RDLEN=4, RDATA=AP_IP bytes
 */

#define DNS_PORT        53
#define AP_IP_A         192
#define AP_IP_B         168
#define AP_IP_C         4
#define AP_IP_D         1
#define DNS_BUF_SIZE    512
#define DNS_TASK_STACK  3072

static void dns_responder_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket failed: %d", errno);
        vTaskDelete(NULL);
        return;
    }

    /* Allow reuse so a restart doesn't block on TIME_WAIT */
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind port 53 failed: %d — captive portal unavailable", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Captive-portal DNS responder listening on port 53");

    static uint8_t buf[DNS_BUF_SIZE];

    while (1) {
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        int len = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                           (struct sockaddr *)&client, &clen);
        if (len < 12) continue; /* too short to be a valid DNS query */

        /* Build reply in-place:
         * - Keep transaction ID [0-1]
         * - Set flags: QR=1 (response), AA=1, RCODE=0
         * - ANCount = 1, rest = 0
         */
        buf[2]  = 0x81; /* QR=1, Opcode=0, AA=1, TC=0, RD=0 */
        buf[3]  = 0x80; /* RA=1, Z=0, RCODE=0 */
        /* QDCount stays as-is (already 1) */
        buf[6]  = 0x00; buf[7]  = 0x01; /* ANCount = 1 */
        buf[8]  = 0x00; buf[9]  = 0x00; /* NSCount = 0 */
        buf[10] = 0x00; buf[11] = 0x00; /* ARCount = 0 */

        /* Append A record answer after the question section.
         * We don't parse the question — just append after the full received
         * query bytes, which already contains the question section. */
        int ans_off = len;
        if (ans_off + 16 > DNS_BUF_SIZE) {
            /* Oversized query — just echo a minimal NOERROR with 0 answers */
            buf[7] = 0x00; /* ANCount = 0 */
            sendto(sock, buf, len, 0, (struct sockaddr *)&client, clen);
            continue;
        }

        /* NAME: pointer 0xC00C (points to QNAME at byte offset 12) */
        buf[ans_off + 0] = 0xC0;
        buf[ans_off + 1] = 0x0C;
        /* TYPE: A = 0x0001 */
        buf[ans_off + 2] = 0x00;
        buf[ans_off + 3] = 0x01;
        /* CLASS: IN = 0x0001 */
        buf[ans_off + 4] = 0x00;
        buf[ans_off + 5] = 0x01;
        /* TTL: 60 seconds */
        buf[ans_off + 6] = 0x00;
        buf[ans_off + 7] = 0x00;
        buf[ans_off + 8] = 0x00;
        buf[ans_off + 9] = 0x3C;
        /* RDLENGTH: 4 */
        buf[ans_off + 10] = 0x00;
        buf[ans_off + 11] = 0x04;
        /* RDATA: 192.168.4.1 */
        buf[ans_off + 12] = AP_IP_A;
        buf[ans_off + 13] = AP_IP_B;
        buf[ans_off + 14] = AP_IP_C;
        buf[ans_off + 15] = AP_IP_D;

        sendto(sock, buf, ans_off + 16, 0, (struct sockaddr *)&client, clen);
    }

    close(sock);
    vTaskDelete(NULL);
}

void dns_responder_start(void)
{
    xTaskCreate(dns_responder_task, "dns_resp", DNS_TASK_STACK, NULL, 5, NULL);
}

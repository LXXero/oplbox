/* SPDX-License-Identifier: GPL-3.0-only
 * fm1wifi_frame: the frames the WiFi core's FIFOs carry, as data (stages 4-6, docs/WIFI.md and docs/WIFI_STAGE4.md).
 * Pure code: no register, no port, no time source; the hardware side is in fm1wifi.c, which calls this. So the
 * host tests run all of it on byte arrays (tests/wifi_frame.c).
 *
 *  - Received frames (stage 4): the buffer fm1wifi_rx_read fills, parsed into the RX descriptor's fields (rt2800's
 *    RXWI, Linux rt2800.h) and the 802.11 frame behind it; beacons and probe responses into a scan list
 *    (SSID, BSSID, channel, RSSI, security) from IEEE 802.11-2020 clause 9.3.3 and 9.4.2.
 *  - Frames to send (stage 5): the FIFO image (JieLi's two words, then rt2800's TXWI, then the frame) and the
 *    management frames a station sends: probe request, open-system authentication, association request,
 *    deauthentication (IEEE 802.11-2020 9.3.3).
 *  - The station (stage 6): open-system authentication and association as a state machine with timeouts, beacon
 *    loss and deauthentication / disassociation handling (IEEE 802.11-2020 11.3), fed one frame and the time at a
 *    time; it says what to send. The caller owns the radio and the clock.
 *
 * Own code, written from those documents and from the byte layout docs/WIFI_STAGE4.md gives; no JieLi code.
 */
#pragma once
#include <stdint.h>

/* ---- received frames (stage 4) */

/* The RX FIFO hands over one frame as a length word N, then N bytes (the RXWI, the 802.11 frame, padding), then a
 * 4-byte trailer: what fm1wifi_rx_read writes into its buffer, the length word and the trailer turned into CPU
 * byte order (they come big-endian; JieLi's sdio_recv_int_hdl swaps them the same way). */
#define FM1WIFI_RXWI_LEN 16u
#define FM1WIFI_RX_LEN_MAX 4088u                       /* the largest N JieLi's read takes (it resets above) */
#define FM1WIFI_RX_BUF (FM1WIFI_RX_LEN_MAX + 8u)        /* a buffer for any frame: length word, N bytes, trailer */

/* The trailer (rt2800usb.h's RXD word 0): which flags the core set for this frame. */
#define FM1WIFI_RXD_DATA (1u << 1)
#define FM1WIFI_RXD_UNICAST_TO_ME (1u << 4)
#define FM1WIFI_RXD_MULTICAST (1u << 5)
#define FM1WIFI_RXD_BROADCAST (1u << 6)
#define FM1WIFI_RXD_MY_BSS (1u << 7)
#define FM1WIFI_RXD_CRC_ERROR (1u << 8)
#define FM1WIFI_RXD_L2PAD (1u << 14)                    /* 2 bytes between a header whose length is not a multiple of 4 and the body */
#define FM1WIFI_RXD_CIPHER_ERR (3u << 9)                /* rt2800usb.h RXD_W0_CIPHER_ERROR: 1 ICV, 2 MIC, 3 key; 0 none */
#define FM1WIFI_RXD_DECRYPTED (1u << 16)                /* the MAC decrypted the frame and took its IV and MIC off */

typedef struct {
    const uint8_t *frame;     /* the 802.11 header, inside the caller's buffer */
    const uint8_t *body;      /* the frame body (after the header and, with L2PAD, the pad) */
    uint16_t len;             /* the MPDU: RXWI's MPDU_TOTAL_BYTE_COUNT */
    uint16_t hdr_len, body_len;
    uint16_t fc;              /* frame control, little-endian as sent */
    int8_t rssi;              /* RXWI's RSSI0 (word 2 bits 7-0) read as a signed dBm value, as JieLi hands it to its
                               * AGC (adj_agc_max_gain): unconfirmed, the FM-1 shows what it means (docs/WIFI_STAGE4.md) */
    uint8_t mcs, phymode, bw, wcid;
    uint32_t rxwi[4];         /* the descriptor's words as they came */
    uint32_t info;            /* the trailer word (FM1WIFI_RXD_*) */
} fm1wifi_rx;

/* Parse buf (n valid bytes, as fm1wifi_rx_read left it). FM1WIFI_OK, or FM1WIFI_EFRAME when the length word, the
 * RXWI's byte count or the header do not fit inside n (f is then only partly filled). */
int fm1wifi_rx_parse(const uint8_t *buf, uint32_t n, fm1wifi_rx *f);

/* 802.11 frame control: the type and subtype as the first byte holds them (type << 2 | subtype << 4). */
#define FM1WIFI_FC_ASSOC_REQ 0x00u
#define FM1WIFI_FC_ASSOC_RESP 0x10u
#define FM1WIFI_FC_PROBE_REQ 0x40u
#define FM1WIFI_FC_PROBE_RESP 0x50u
#define FM1WIFI_FC_BEACON 0x80u
#define FM1WIFI_FC_DISASSOC 0xA0u
#define FM1WIFI_FC_AUTH 0xB0u
#define FM1WIFI_FC_DEAUTH 0xC0u
#define FM1WIFI_FC_DATA 0x08u
#define FM1WIFI_FC_QOS_DATA 0x88u
#define FM1WIFI_FC_KIND(fc) ((uint8_t)((fc) & 0xFCu))  /* the first byte without the protocol version */

/* What a beacon or probe response says about its network. */
enum { FM1WIFI_SEC_OPEN, FM1WIFI_SEC_WEP, FM1WIFI_SEC_WPA, FM1WIFI_SEC_WPA2 };
const char *fm1wifi_sec_name(unsigned sec);    /* "OPEN", "WEP", "WPA", "WPA2" */
#define FM1WIFI_RSN_MAX 34u
typedef struct {
    uint8_t bssid[6];
    char ssid[33];            /* NUL-terminated; empty for a hidden network */
    uint8_t ssid_len;
    uint8_t channel;          /* the DS Parameter Set's channel; the channel it was heard on when absent */
    uint8_t sec;              /* FM1WIFI_SEC_* */
    int8_t rssi;              /* the strongest frame from it */
    uint16_t cap, interval;   /* capability information; beacon interval in TU (1024 us) */
    uint8_t rates[12], n_rates;   /* Supported Rates then Extended Supported Rates, as sent (bit 7: basic) */
    uint8_t rsn[FM1WIFI_RSN_MAX], rsn_len;   /* the RSN element whole (ID, length, body), for WPA2 (stage 8) */
    uint8_t heard;            /* frames from it so far (saturates at 255) */
    uint8_t probed;           /* 1: a probe response came from it (an active scan's answer) */
} fm1wifi_bss;

/* A beacon or probe response (f) heard on channel ch, into b. FM1WIFI_OK, FM1WIFI_EFRAME if f is neither or too
 * short for the fixed fields. Elements that run past the frame's end are ignored, not an error. */
int fm1wifi_bss_parse(const fm1wifi_rx *f, unsigned ch, fm1wifi_bss *b);

#define FM1WIFI_SCAN_MAX 16u
typedef struct {
    fm1wifi_bss e[FM1WIFI_SCAN_MAX];
    uint8_t n;
    uint8_t full;             /* networks not kept because the list was full */
} fm1wifi_scan_list;
void fm1wifi_scan_clear(fm1wifi_scan_list *l);
/* Add b, or merge it into the entry with its BSSID (the strongest RSSI, a hidden SSID filled in by a probe
 * response, probed and heard accumulated). Returns the entry, or 0 when the list is full. */
fm1wifi_bss *fm1wifi_scan_add(fm1wifi_scan_list *l, const fm1wifi_bss *b);
/* The strongest entry whose SSID is ssid (0: none). */
const fm1wifi_bss *fm1wifi_scan_find(const fm1wifi_scan_list *l, const char *ssid);

/* ---- frames to send (stage 5) */

/* The TX FIFO image as JieLi's usb_submit_urb builds it (docs/WIFI_STAGE4.md "The TX FIFO"): word 0 a length the DMA
 * does not send (frame + 22), word 1 JieLi's header (bit 0 = rt2800 TXINFO's WIV, bits 2-1 its QSEL), then the
 * TXWI (16 bytes, rt2800.h), then the frame. The DMA starts at the image + 4 and sends frame + 26 bytes. */
#define FM1WIFI_TX_HDR 24u
#define FM1WIFI_TX_FRAME_MAX 1600u
#define FM1WIFI_TX_BUF (FM1WIFI_TX_HDR + FM1WIFI_TX_FRAME_MAX + 8u)
#define FM1WIFI_TX_ACK 1u          /* the frame is unicast: the MAC waits for the ACK and retries (TXWI ACK) */
#define FM1WIFI_TX_OFDM6 2u        /* send at OFDM 6 Mbit/s; default CCK 1 Mbit/s, long preamble */
/* The MAC encrypts the frame with the key of WCID FM1WIFI_TX_WCID's (stage 8): JieLi's word's WIV 0 (the MAC inserts
 * the IV from its IV/EIV table), the TXWI's WCID that entry (rt2800lib.c rt2800_write_tx_data, rt2800usb.c). The
 * frame has its Protected bit set and no IV or MIC: the MAC adds both. */
#define FM1WIFI_TX_CRYPT 4u
#define FM1WIFI_TX_WCID(w) (((unsigned)(w) & 0xFFu) << 8)
/* Build the image of frame (len bytes) into img (word-aligned, FM1WIFI_TX_BUF bytes). Returns the DMA length
 * (len + 26), or 0 when len is 0 or above FM1WIFI_TX_FRAME_MAX. */
uint32_t fm1wifi_tx_image(uint8_t *img, const uint8_t *frame, uint32_t len, unsigned flags);
/* The TXWI's fields as the image holds them (host tests, the WiFi page). */
#define FM1WIFI_TXWI_W0_MCS 0x007F0000u
#define FM1WIFI_TXWI_W0_TX_OP 0x00000300u
#define FM1WIFI_TXWI_W0_PHYMODE 0xC0000000u
#define FM1WIFI_TXWI_W1_ACK 0x00000001u
#define FM1WIFI_TXWI_W1_NSEQ 0x00000002u
#define FM1WIFI_TXWI_W1_WCID 0x0000FF00u
#define FM1WIFI_TXWI_W1_MPDU_LEN 0x0FFF0000u

/* Management frames a station sends, written into f (at least 256 bytes): their length. seq is the sequence
 * number (the station counts it; the TXWI does not ask the MAC for one). */
uint32_t fm1wifi_probe_req(uint8_t *f, const uint8_t sa[6], const char *ssid, uint16_t seq);   /* ssid 0 or "": any */
uint32_t fm1wifi_auth_open(uint8_t *f, const uint8_t sa[6], const uint8_t bssid[6], uint16_t seq);
uint32_t fm1wifi_assoc_req(uint8_t *f, const uint8_t sa[6], const fm1wifi_bss *b, const uint8_t *ies, uint32_t n_ies,
                           uint16_t seq);   /* ies: further elements (the RSN element for WPA2), or 0 */
uint32_t fm1wifi_deauth(uint8_t *f, const uint8_t sa[6], const uint8_t bssid[6], uint16_t reason, uint16_t seq);

/* ---- data frames (stage 8): EAPOL to and from the AP */

#define FM1WIFI_FC_TO_DS 0x0100u
#define FM1WIFI_FC_FROM_DS 0x0200u
#define FM1WIFI_FC_PROTECTED 0x4000u
#define FM1WIFI_ETH_EAPOL 0x888Eu
#define FM1WIFI_EAPOL_MAX 512u          /* the longest EAPOL PDU sent or taken */
/* An EAPOL PDU (n bytes, at most FM1WIFI_EAPOL_MAX) to the AP: a data frame To DS (address 1 the BSSID, 2 sa, 3 the
 * BSSID: the AP is the authenticator), RFC 1042 LLC/SNAP, ethertype 0x888E. protect sets the Protected bit (send it
 * with FM1WIFI_TX_CRYPT). f holds 32 + n bytes. Returns its length, 0 when n is 0 or too long. */
uint32_t fm1wifi_eapol_frame(uint8_t *f, const uint8_t sa[6], const uint8_t bssid[6], const uint8_t *pdu, uint32_t n,
                             int protect, uint16_t seq);
/* The EAPOL PDU in f when f is a data frame (or QoS data) From DS, from bssid (address 2) to sa (address 1), with
 * RFC 1042 LLC/SNAP and ethertype 0x888E, sent in the clear or decrypted by the MAC (FM1WIFI_RXD_DECRYPTED with no
 * cipher error): its length, *pdu its first byte. 0 for anything else. */
uint32_t fm1wifi_eapol_pdu(const fm1wifi_rx *f, const uint8_t bssid[6], const uint8_t sa[6], const uint8_t **pdu);

/* ---- the station (stage 6) */

enum { FM1WIFI_STA_IDLE, FM1WIFI_STA_AUTH, FM1WIFI_STA_ASSOC, FM1WIFI_STA_UP, FM1WIFI_STA_DOWN };
const char *fm1wifi_sta_state_name(unsigned s);  /* "IDLE", "AUTH", "ASSOC", "UP", "DOWN" */
/* Why the station went DOWN (or did not come UP). */
enum { FM1WIFI_WHY_NONE, FM1WIFI_WHY_AUTH_TIMEOUT, FM1WIFI_WHY_AUTH_REFUSED, FM1WIFI_WHY_ASSOC_TIMEOUT,
       FM1WIFI_WHY_ASSOC_REFUSED, FM1WIFI_WHY_DEAUTH, FM1WIFI_WHY_DISASSOC, FM1WIFI_WHY_BEACON_LOSS,
       FM1WIFI_WHY_LEFT };
const char *fm1wifi_why_name(unsigned why);      /* "AUTH TIMEOUT", "AUTH REFUSED", ... at most 14 characters */
#define FM1WIFI_STA_TRIES 3u            /* authentication and association requests each, before giving up */
#define FM1WIFI_STA_WAIT_MS 300u        /* for each answer */
#define FM1WIFI_STA_LOSS_BEACONS 10u    /* beacon intervals without a beacon: lost (at least 1000 ms) */
typedef struct {
    uint8_t state, why;
    uint8_t sa[6];
    fm1wifi_bss bss;
    uint16_t seq;             /* the next sequence number */
    uint16_t status;          /* the last authentication or association response's status code */
    uint16_t reason;          /* a deauthentication's or disassociation's reason code */
    uint16_t aid;             /* the association ID, once UP */
    uint8_t tries;
    uint32_t t_sent, t_beacon;   /* when the last request went out; when the last beacon came (ms) */
    uint32_t beacons;         /* beacons from the BSS since UP */
    const uint8_t *ies;       /* further association request elements (the RSN element), or 0 */
    uint32_t n_ies;
} fm1wifi_sta;

/* Start joining b as sa at time now (ms): the next fm1wifi_sta_step sends the authentication request. */
void fm1wifi_sta_start(fm1wifi_sta *s, const uint8_t sa[6], const fm1wifi_bss *b, uint32_t now);
/* One step at time now (ms) with a received frame (or 0). When the station has something to send, it writes the
 * frame into out (at least 256 bytes), sets *flags (FM1WIFI_TX_*) and returns its length; else 0. Frames not from
 * the BSS or not for sa are ignored. */
uint32_t fm1wifi_sta_step(fm1wifi_sta *s, const fm1wifi_rx *f, uint32_t now, uint8_t *out, unsigned *flags);
/* Leave: DOWN with WHY_LEFT; the deauthentication frame to send (reason 3, leaving) into out, its length (0 when
 * the station was not authenticated). */
uint32_t fm1wifi_sta_leave(fm1wifi_sta *s, uint8_t *out, unsigned *flags);

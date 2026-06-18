/* net.h — minimal network stack demo (ARP + ICMP). */
#pragma once

/* Bring up the NIC, ARP-resolve the gateway, and ping it. Prints results.
 * No-op message if there's no NIC. */
void net_demo(void);

#include <stdint.h>
/* Ping the gateway 3 times; returns the number of echo replies (-1 = no ARP). */
int net_ping_gateway(void);
/* DNS-resolve a host and ICMP-echo it 3 times via the gateway; reply count or -1. */
int net_ping_host(const char *host);
/* Resolve a hostname to an IPv4 address via DNS. 0 on success, -1 otherwise. */
int dns_resolve(const char *host, uint8_t out_ip[4]);
/* HTTP/1.0 GET http://host/path -> raw response into out (max bytes).
 * Returns bytes received, or -1 on error. */
int http_get(const char *host, const char *path, char *out, int max);
/* HTTP/1.0 POST: sends `body` (bodylen) with Content-Type `ctype`; raw response into out. -1 on error. (M702) */
int http_post(const char *host, const char *path, const char *ctype, const char *body, int bodylen, char *out, int max);

/* A minimal TCP stream connection (one at a time; used by HTTP and TLS). */
typedef struct {
    uint8_t  ip[4], gw[6];
    uint16_t sport, dport;
    uint32_t myseq, theirseq;
    int      up;                 /* 1 while the connection is open */
} tcp_conn;

int  tcp_connect(tcp_conn *c, const uint8_t ip[4], uint16_t port);  /* 0 / -1 */
int  tcp_write(tcp_conn *c, const uint8_t *data, int len);          /* len / -1 */
int  tcp_read(tcp_conn *c, uint8_t *out, int max, uint64_t ticks);  /* bytes, 0=timeout, -1=closed */
void tcp_close(tcp_conn *c);

const uint8_t *net_ip(void);        /* our IPv4 address (4 bytes) */
const uint8_t *net_gateway(void);   /* the gateway IPv4 address (4 bytes) */
const uint8_t *net_mac(void);       /* our 6-byte hardware (MAC) address */
const uint8_t *net_dns(void);       /* the DNS resolver IPv4 address (4 bytes) */

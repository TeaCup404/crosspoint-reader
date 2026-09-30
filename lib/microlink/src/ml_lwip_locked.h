/*
 * X4 HOMESYNC PATCH: lwIP core locking for MicroLink on the prebuilt Arduino core.
 *
 * The ESP32-S3 Arduino libs are built with CONFIG_LWIP_TCPIP_CORE_LOCKING and
 * CONFIG_LWIP_CHECK_THREAD_SAFETY, so every raw lwIP call (netif, udp, and the
 * wireguard-lwip netif driver that uses them) must hold the TCPIP core lock or
 * lwIP asserts ("Required to lock TCPIP core functionality!"). Upstream
 * MicroLink calls them from its own task without the lock, which the keychain's
 * ESP-IDF build tolerated because the check was off there.
 *
 * Include this after the lwIP / wireguardif headers: it routes those calls to
 * wrappers in ml_lwip_locked.c that take the lock, unless the caller already
 * holds it (callbacks on the TCPIP thread), so nothing deadlocks.
 */
#pragma once

#include "lwip/netif.h"
#include "lwip/udp.h"
#include "wireguardif.h"

err_t ml_locked_wireguardif_init(struct netif *netif);
err_t ml_locked_wireguardif_add_peer(struct netif *netif, struct wireguardif_peer *peer, u8_t *peer_index);
err_t ml_locked_wireguardif_remove_peer(struct netif *netif, u8_t peer_index);
err_t ml_locked_wireguardif_update_endpoint(struct netif *netif, u8_t peer_index, const ip_addr_t *ip, u16_t port);
err_t ml_locked_wireguardif_connect(struct netif *netif, u8_t peer_index);
err_t ml_locked_wireguardif_connect_derp(struct netif *netif, u8_t peer_index);
err_t ml_locked_wireguardif_peer_is_up(struct netif *netif, u8_t peer_index, ip_addr_t *current_ip,
                                       u16_t *current_port);
void ml_locked_wireguardif_force_derp_output(struct netif *netif, bool force);
void ml_locked_wireguardif_network_rx(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr,
                                      u16_t port);
void ml_locked_wireguardif_periodic(struct netif *netif);
void ml_locked_wireguardif_shutdown(struct netif *netif);

void ml_locked_netif_set_up(struct netif *netif);
void ml_locked_netif_set_down(struct netif *netif);
void ml_locked_netif_set_link_up(struct netif *netif);
void ml_locked_netif_set_link_down(struct netif *netif);
void ml_locked_netif_remove(struct netif *netif);

struct udp_pcb *ml_locked_udp_new(void);
err_t ml_locked_udp_bind(struct udp_pcb *pcb, const ip_addr_t *ipaddr, u16_t port);
void ml_locked_udp_bind_netif(struct udp_pcb *pcb, const struct netif *netif);
err_t ml_locked_udp_sendto(struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *dst_ip, u16_t dst_port);
void ml_locked_udp_recv(struct udp_pcb *pcb, udp_recv_fn recv, void *recv_arg);
void ml_locked_udp_remove(struct udp_pcb *pcb);

#define wireguardif_init(...) ml_locked_wireguardif_init(__VA_ARGS__)
#define wireguardif_add_peer(...) ml_locked_wireguardif_add_peer(__VA_ARGS__)
#define wireguardif_remove_peer(...) ml_locked_wireguardif_remove_peer(__VA_ARGS__)
#define wireguardif_update_endpoint(...) ml_locked_wireguardif_update_endpoint(__VA_ARGS__)
#define wireguardif_connect(...) ml_locked_wireguardif_connect(__VA_ARGS__)
#define wireguardif_connect_derp(...) ml_locked_wireguardif_connect_derp(__VA_ARGS__)
#define wireguardif_peer_is_up(...) ml_locked_wireguardif_peer_is_up(__VA_ARGS__)
#define wireguardif_force_derp_output(...) ml_locked_wireguardif_force_derp_output(__VA_ARGS__)
#define wireguardif_network_rx(...) ml_locked_wireguardif_network_rx(__VA_ARGS__)
#define wireguardif_periodic(...) ml_locked_wireguardif_periodic(__VA_ARGS__)
#define wireguardif_shutdown(...) ml_locked_wireguardif_shutdown(__VA_ARGS__)

#define netif_set_up(...) ml_locked_netif_set_up(__VA_ARGS__)
#define netif_set_down(...) ml_locked_netif_set_down(__VA_ARGS__)
#define netif_set_link_up(...) ml_locked_netif_set_link_up(__VA_ARGS__)
#define netif_set_link_down(...) ml_locked_netif_set_link_down(__VA_ARGS__)
#define netif_remove(...) ml_locked_netif_remove(__VA_ARGS__)

#define udp_new(...) ml_locked_udp_new(__VA_ARGS__)
#define udp_bind(...) ml_locked_udp_bind(__VA_ARGS__)
#define udp_bind_netif(...) ml_locked_udp_bind_netif(__VA_ARGS__)
#define udp_sendto(...) ml_locked_udp_sendto(__VA_ARGS__)
#define udp_recv(...) ml_locked_udp_recv(__VA_ARGS__)
#define udp_remove(...) ml_locked_udp_remove(__VA_ARGS__)

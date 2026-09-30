/* X4 HOMESYNC PATCH: see ml_lwip_locked.h. Must NOT include that header. */
#include <stdbool.h>

#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "lwip/udp.h"
#include "wireguardif.h"

/* Not in wireguardif.h; ml_wg_mgr.c declares it the same way. */
extern void wireguardif_network_rx(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port);

/* Take the TCPIP core lock unless this task already holds it (lwIP callbacks
 * run on the TCPIP thread with it held; FreeRTOS mutexes are not recursive). */
static bool lock(void) {
#if LWIP_TCPIP_CORE_LOCKING
    if (sys_thread_tcpip(LWIP_CORE_LOCK_QUERY_HOLDER)) return false;
    LOCK_TCPIP_CORE();
    return true;
#else
    return false;
#endif
}

static void unlock(bool locked) {
#if LWIP_TCPIP_CORE_LOCKING
    if (locked) UNLOCK_TCPIP_CORE();
#else
    (void)locked;
#endif
}

#define LOCKED_RET(type, call) \
    bool l = lock();           \
    type r = call;             \
    unlock(l);                 \
    return r
#define LOCKED_VOID(call) \
    bool l = lock();      \
    call;                 \
    unlock(l)

err_t ml_locked_wireguardif_init(struct netif *netif) { LOCKED_RET(err_t, wireguardif_init(netif)); }
err_t ml_locked_wireguardif_add_peer(struct netif *netif, struct wireguardif_peer *peer, u8_t *peer_index) {
    LOCKED_RET(err_t, wireguardif_add_peer(netif, peer, peer_index));
}
err_t ml_locked_wireguardif_remove_peer(struct netif *netif, u8_t peer_index) {
    LOCKED_RET(err_t, wireguardif_remove_peer(netif, peer_index));
}
err_t ml_locked_wireguardif_update_endpoint(struct netif *netif, u8_t peer_index, const ip_addr_t *ip, u16_t port) {
    LOCKED_RET(err_t, wireguardif_update_endpoint(netif, peer_index, ip, port));
}
err_t ml_locked_wireguardif_connect(struct netif *netif, u8_t peer_index) {
    LOCKED_RET(err_t, wireguardif_connect(netif, peer_index));
}
err_t ml_locked_wireguardif_connect_derp(struct netif *netif, u8_t peer_index) {
    LOCKED_RET(err_t, wireguardif_connect_derp(netif, peer_index));
}
err_t ml_locked_wireguardif_peer_is_up(struct netif *netif, u8_t peer_index, ip_addr_t *current_ip,
                                       u16_t *current_port) {
    LOCKED_RET(err_t, wireguardif_peer_is_up(netif, peer_index, current_ip, current_port));
}
void ml_locked_wireguardif_force_derp_output(struct netif *netif, bool force) {
    LOCKED_VOID(wireguardif_force_derp_output(netif, force));
}
void ml_locked_wireguardif_network_rx(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr,
                                      u16_t port) {
    LOCKED_VOID(wireguardif_network_rx(arg, pcb, p, addr, port));
}
void ml_locked_wireguardif_periodic(struct netif *netif) { LOCKED_VOID(wireguardif_periodic(netif)); }
void ml_locked_wireguardif_shutdown(struct netif *netif) { LOCKED_VOID(wireguardif_shutdown(netif)); }

void ml_locked_netif_set_up(struct netif *netif) { LOCKED_VOID(netif_set_up(netif)); }
void ml_locked_netif_set_down(struct netif *netif) { LOCKED_VOID(netif_set_down(netif)); }
void ml_locked_netif_set_link_up(struct netif *netif) { LOCKED_VOID(netif_set_link_up(netif)); }
void ml_locked_netif_set_link_down(struct netif *netif) { LOCKED_VOID(netif_set_link_down(netif)); }
void ml_locked_netif_remove(struct netif *netif) { LOCKED_VOID(netif_remove(netif)); }

struct udp_pcb *ml_locked_udp_new(void) { LOCKED_RET(struct udp_pcb *, udp_new()); }
err_t ml_locked_udp_bind(struct udp_pcb *pcb, const ip_addr_t *ipaddr, u16_t port) {
    LOCKED_RET(err_t, udp_bind(pcb, ipaddr, port));
}
void ml_locked_udp_bind_netif(struct udp_pcb *pcb, const struct netif *netif) {
    LOCKED_VOID(udp_bind_netif(pcb, netif));
}
err_t ml_locked_udp_sendto(struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *dst_ip, u16_t dst_port) {
    LOCKED_RET(err_t, udp_sendto(pcb, p, dst_ip, dst_port));
}
void ml_locked_udp_recv(struct udp_pcb *pcb, udp_recv_fn recv, void *recv_arg) {
    LOCKED_VOID(udp_recv(pcb, recv, recv_arg));
}
void ml_locked_udp_remove(struct udp_pcb *pcb) { LOCKED_VOID(udp_remove(pcb)); }

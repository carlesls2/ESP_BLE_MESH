/* neighbor.h - Nodes this board hears directly over the radio
 *
 * A mesh node knows which addresses it can *reach*, but not which ones it can
 * *hear* -- with relay on, a message may have crossed several hops before it
 * arrived. This table keeps only the direct ones, so the host can draw the
 * real radio topology instead of the logical one.
 *
 * DIRECT means one hop. The network header's TTL tells: a relay decrements it,
 * so a packet that arrives with the TTL its sender used has not been relayed.
 * Two TTLs are known to be "as sent":
 *
 *   0                  never relayed by definition -- the neighbor HELLO uses
 *                      it, which is what keeps the table filled
 *   NEIGH_SENDER_TTL   the network default TTL every vendor message goes out
 *                      with (config_server.default_ttl in BLETask.c)
 *
 * Anything else was relayed, and its RSSI describes the last relay rather than
 * the source, so it is ignored rather than recorded under the wrong address.
 *
 * Fed from the mesh receive task, read from the host command tasks; a spinlock
 * guards the table and every critical section is a copy.
 */

#ifndef _NEIGHBOR_H_
#define _NEIGHBOR_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Table capacity. Above CONFIG_BLE_MESH_MAX_PROV_NODES (10) so a full network
 * plus a few strangers fit; when full, the longest-silent entry is evicted. */
#define NEIGH_MAX 16

/* Must match config_server.default_ttl in BLETask.c. */
#define NEIGH_SENDER_TTL 7

/* An entry not heard for this long is left out of every report. Six missed
 * HELLOs at the ~10 s interval -- long enough to ride out collisions, short
 * enough that a powered-off board drops out within a minute. */
#define NEIGH_STALE_S 60

typedef struct {
    uint16_t addr;
    int8_t   rssi_last;
    int8_t   rssi_avg;    /* exponential average, 1/8 weight per sample */
    uint16_t age_s;       /* seconds since last heard, saturating */
    uint16_t heard;       /* packets counted, saturating */
} neighbor_entry_t;

/* Records one received packet. `own_addr` is this board's primary unicast,
 * passed in so the module needs no mesh API; packets from it, from non-unicast
 * sources, and relayed packets (see above) are ignored. */
void neighbor_note(uint16_t src, uint16_t own_addr, uint8_t recv_ttl, int8_t rssi);

/* Copies out the fresh entries (heard within NEIGH_STALE_S), strongest average
 * RSSI first. Returns how many were written, at most `max`. */
size_t neighbor_snapshot(neighbor_entry_t *out, size_t max);

/* Forgets everyone. Called on a role switch, which changes networks. */
void neighbor_clear(void);

#ifdef __cplusplus
}
#endif
#endif /* _NEIGHBOR_H_ */

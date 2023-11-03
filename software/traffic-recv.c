#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>


#include "mmio.h"
#include "mt-utils.h"
#include "nic-rss.h"
#include "encoding.h"
#include "dslib.h"


/* #define NO_NET_DEBUG */
#define TRAFFIC_RECV_DEBUG

#define N_CORES 12
#define RUN_CORES 4
#define TX_DESC_CNT 128
#define RX_DESC_CNT 128
#define PACKET_BYTES 1500
#define PACKET_BYTES_PADDED (PACKET_BYTES+8)
#define MAX_PACKETS_TO_FORWARD (10000 / RUN_CORES)
#define PAGESIZE_BYTES 4096

uint8_t rx_desc[RUN_CORES][RX_DESC_CNT][PACKET_BYTES_PADDED];
uint8_t tx_desc[RUN_CORES][RX_DESC_CNT][PACKET_BYTES_PADDED];

void forward_traffic(int core_id) {
  acquire_lock();
  Queue* rx_idx;
  Queue* tx_idx;
  Queue* inflight_rx;

  // setup rx desc
  rx_idx = newQueue(RX_DESC_CNT);
  for (int i = 0; i < RX_DESC_CNT; i++) {
    enqueue(rx_idx, i);
  }

  // setup tx desc
  tx_idx = newQueue(TX_DESC_CNT);
  for (int i = 0; i < TX_DESC_CNT; i++) {
    enqueue(tx_idx, i);
  }

  inflight_rx = newQueue(RX_DESC_CNT);

  fprintf(stdout, "Core %d start forwarding traffic\n", core_id);
  release_lock();

  syncpoint(RUN_CORES);

  int cnt = 0, prev_cnt = 0;
  int recv_avail, send_avail;
  do {
    int rid, tid;

    // Push in as much rx descriptors as it can
#ifndef NO_NET_DEBUG
    recv_avail = nic_recv_req_avail(core_id);
    asm volatile("fence");
#else
    recv_avail = RX_DESC_CNT;
#endif
    for (int i = 0; i < recv_avail; i++) {
      if (!isempty(rx_idx) && (space(inflight_rx) > 0)) {
        rid = dequeue(rx_idx);
        enqueue(inflight_rx, rid);
#ifndef NO_NET_DEBUG
        nic_set_recv_addr(core_id, (uint64_t)rx_desc[core_id][rid]);
#endif
      }
    }

    acquire_lock();
    if (cnt - prev_cnt > 1000) {
      printf("core %d cnt %d\n", core_id, cnt);
#ifndef NO_NET_DEBUG
      if (core_id == 0) {
        printf("rd: %d wr: %d\n", (int)nic_ddio_rd_avg_lat(N_CORES), (int)nic_ddio_wr_avg_lat(N_CORES));
      }
#endif
      prev_cnt = cnt;
    }
    release_lock();

    // Receive packets & forward it
#ifndef NO_NET_DEBUG
    int recv_comps = nic_recv_comp_avail(core_id);
    asm volatile("fence");
    assert(recv_comps <= size(inflight_rx));
/* acquire_lock(); */
/* printf("recv: %d, ifrx: %d\n", recv_comps, size(inflight_rx)); */
/* release_lock(); */
#else
    int recv_comps = size(inflight_rx);
#endif
    for (int i = 0; i < recv_comps; i++) {
      // Get first rx descriptor id that was used
#ifndef NO_NET_DEBUG
      uint64_t len = nic_recv_comp(core_id);
#endif

      // Copy the rx descriptor to the tx descriptor
      assert(!isempty(inflight_rx));
/* assert(space(rx_idx) > 0); */
/* assert(!isempty(tx_idx)); */

      rid = dequeue(inflight_rx);
      enqueue(rx_idx, rid);

      tid = dequeue(tx_idx);
      memcpy((void*)tx_desc[core_id][tid], (void*)rx_desc[core_id][rid], sizeof(uint8_t)*PACKET_BYTES);

      // Send the tx descriptor
#ifndef NO_NET_DEBUG
      nic_send((void*)tx_desc[core_id][tid], len, core_id);
#endif

      // release the used tx descriptor
      enqueue(tx_idx, tid);
      cnt++;
    }
  } while (cnt < MAX_PACKETS_TO_FORWARD);

  acquire_lock();
  printf("Core %d done forwarding\n", core_id);
  release_lock();
}


void __main(void) {
  size_t mhartid = read_csr(mhartid);
  if (mhartid >= RUN_CORES) while (1);


  for (int i = 0; i < RUN_CORES; i++) {
    if (mhartid == i) {
      forward_traffic(mhartid);
    }
  }

  barrier(RUN_CORES);
  if (mhartid > 0) while (1);
#ifndef NO_NET_DEBUG
  fprintf(stdout, "rd: %d wr: %d\n", (int)nic_ddio_rd_avg_lat(N_CORES), (int)nic_ddio_wr_avg_lat(N_CORES));
#endif
}

int main(void) {
  __main();
  return 0;
}

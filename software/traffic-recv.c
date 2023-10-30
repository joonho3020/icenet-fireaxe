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

#define N_CORES 8
#define RUN_CORES 2
#define TX_DESC_CNT 128
#define RX_DESC_CNT 128
#define PACKET_BYTES 1500
#define PACKET_BYTES_PADDED (PACKET_BYTES+8)
#define MAX_PACKETS_TO_FORWARD 1000000


void forward_traffic(int core_id, Stack* rx, uint8_t** rx_desc, Stack* tx, uint8_t** tx_desc) {

  acquire_lock();
  fprintf(stdout, "Core %d start forwarding traffic\n", core_id);
  release_lock();

  Queue* inflight_rx = newQueue(RX_DESC_CNT);

  int cnt = 0;
  int recv_avail, send_avail;
  do {
    int rid, tid, h;

    // Push in as much rx descriptors as it can
#ifndef NO_NET_DEBUG
    recv_avail = nic_recv_req_avail(core_id);
    asm volatile("fence");
#else
    recv_avail = RX_DESC_CNT;
#endif
    for (int i = 0; i < recv_avail; i++) {
      acquire_lock();
      h = height(rx);
      if (h > 0) {
        rid = pop(rx);
        enqueue(inflight_rx, rid);
/* printf("core: %d rid: %d inflight: %d\n", core_id, rid, size(inflight_rx)); */
#ifndef NO_NET_DEBUG
        nic_set_recv_addr(core_id, (uint64_t)rx_desc[rid]);
#endif
      }
      release_lock();
    }

    acquire_lock();
    printf("core: %d inflight: %d cnt: %d\n", core_id, size(inflight_rx), cnt);
    release_lock();

    // Receive packets & forward it
#ifndef NO_NET_DEBUG
    int recv_comps = nic_recv_comp_avail(core_id);
    asm volatile("fence");
#else
    int recv_comps = size(inflight_rx);
#endif
    for (int i = 0; i < recv_comps; i++) {
      // Get first rx descriptor id that was used
#ifndef NO_NET_DEBUG
      uint64_t len = nic_recv_comp(core_id);
#endif
      assert(!isempty(inflight_rx));
      rid = dequeue(inflight_rx);

      // Copy the rx descriptor to the tx descriptor
      acquire_lock();
      assert(!stackfull(rx));
      push(rx, rid);

      assert(height(tx) > 0);
      tid = pop(tx);

#ifdef TRAFFIC_RECV_DEBUG
/* printf("fwd core: %d rid: %d tid: %d\n", core_id, rid, tid); */
#endif

      memcpy((void*)tx_desc[tid], (void*)rx_desc[rid], sizeof(uint8_t)*PACKET_BYTES);
      release_lock();

      // Send the tx descriptor
#ifndef NO_NET_DEBUG
      nic_send((void*)tx_desc[tid], len, core_id);
#endif

      // release the used tx descriptor
      acquire_lock();
      push(tx, tid);
      release_lock();

      cnt++;
    }
  } while (cnt < MAX_PACKETS_TO_FORWARD);
}


void __main(void) {
  size_t mhartid = read_csr(mhartid);
  if (mhartid >= RUN_CORES) while (1);

  static volatile int setup_done;
  static Stack* rx_idx;
  static uint8_t* rx_desc[RX_DESC_CNT];
  static Stack* tx_idx;
  static uint8_t* tx_desc[TX_DESC_CNT];
  if (mhartid == 0) {
    // setup rx desc
    rx_idx = newStack(RX_DESC_CNT);
    for (int i = 0; i < RX_DESC_CNT; i++) {
      rx_desc[i] = (uint8_t*)malloc(sizeof(uint8_t) * PACKET_BYTES_PADDED);
      push(rx_idx, RX_DESC_CNT - i - 1);
    }

    // setup tx desc
    tx_idx = newStack(TX_DESC_CNT);
    for (int i = 0; i < TX_DESC_CNT; i++) {
      tx_desc[i] = (uint8_t*)malloc(sizeof(uint8_t) * PACKET_BYTES_PADDED);
      push(tx_idx, TX_DESC_CNT - i - 1);
    }

    // setup complete
    setup_done = 1;
  } else {
    // spin until core 0 finishes nic descriptor setup
    while (!setup_done)
      ;
  }

  for (int i = 0; i < RUN_CORES; i++) {
    if (mhartid == i) {
      forward_traffic(mhartid, rx_idx, &rx_desc, tx_idx, &tx_desc);
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

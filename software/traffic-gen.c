#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include <assert.h>


#include "mmio.h"
#include "mt-utils.h"
#include "nic-rss.h"
#include "encoding.h"
#include "dslib.h"


#define TX_CORES 2
#define TX_DESC_CNT 100
#define PACKET_BYTES 1424
#define PACKET_BYTES_PADDED (PACKET_BYTES+8)


#define TRAFFIC_GEN_DEBUG_PRINT
#define START_CYCLE 1000000
/* #define NO_NIC_DEBUG */



void gen_traffic(int core_id) {
  uint8_t* tx_desc[TX_DESC_CNT];
  for (int i = 0; i < TX_DESC_CNT; i++) {
    tx_desc[i] = (uint8_t*)malloc(sizeof(uint8_t) * PACKET_BYTES_PADDED);
    for (int j = 0; j < TX_DESC_CNT; j++) {
      tx_desc[i][j] = i * TX_DESC_CNT + j;
    }
  }

#ifdef TRAFFIC_GEN_DEBUG_PRINT
  acquire_lock();
  fprintf(stdout, "Core %d finished memory setup for TX DESC\n", core_id);
  release_lock();
#endif

  uint64_t packets[TX_DESC_CNT];
  Queue* pending = newQueue(TX_DESC_CNT);
  Queue* inflight = newQueue(TX_DESC_CNT);
  for (int i = 0; i < TX_DESC_CNT; i++) {
    uint64_t pkt_addr = (uint64_t)&tx_desc[i][0];
    uint64_t pkt_size = (uint64_t)PACKET_BYTES;
    uint64_t pkt = (pkt_size << 48) | pkt_addr;
    packets[i] = pkt;
    enqueue(pending, i);
  }

  uint64_t cycle;
  do {
    cycle = rdcycle();
  } while (cycle < START_CYCLE);

  do {
    for (int i = 0; i < size(pending); i++) {
      int pidx = dequeue(pending);
#ifndef NO_NIC_DEBUG
/* fprintf(stdout, "pending.deq: %d\n", pidx); */
      nic_send_req(core_id, packets[pidx]);
#endif
      enqueue(inflight, pidx);
    }

/* printf("space(inflight): %d size(inflight): %d\n", space(inflight), size(inflight)); */

    int ncomps = 0;
#ifndef NO_NIC_DEBUG
    ncomps = nic_send_comp_avail(core_id);
#endif
    asm volatile("fence");
#ifndef NO_NIC_DEBUG
    for (int i = 0; i < ncomps; i++) {
#else
    for (int i = 0; i < size(inflight); i++) {
#endif
      assert(!isempty(inflight));
      int pidx = dequeue(inflight);
      enqueue(pending, pidx);
#ifndef NO_NIC_DEBUG
      nic_send_comp(core_id);
/* fprintf(stdout, "nic_send_comp %d\n", pidx); */
#endif
    }
  } while (1);
}

void __main(void) {
  size_t mhartid = read_csr(mhartid);

  if (mhartid >= TX_CORES) while (1);

  for (int i = 0; i < TX_CORES; i++) {
    if (mhartid == i) {
      gen_traffic(mhartid);
    }
  }

  if (mhartid > 0) while (1);
}

int main(void) {
  __main();
  return 0;
}


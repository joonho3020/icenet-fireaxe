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


#define TX_CORES 1
#define TX_DESC_CNT 100
#define PACKET_BYTES 1424
#define PACKET_BYTES_PADDED (PACKET_BYTES+8)


#define TRAFFIC_GEN_DEBUG_PRINT
#define START_CYCLE 100
/* #define NO_NIC_DEBUG */


uint8_t tx_desc[TX_CORES][TX_DESC_CNT][PACKET_BYTES_PADDED];

void gen_traffic(int core_id) {
/* for (int i = 0; i < TX_DESC_CNT; i++) { */
/* for (int j = 0; j < PACKET_BYTES_PADDED; j++) { */
/* tx_desc[core_id][i][j] = i * TX_DESC_CNT + j; */
/* } */
/* } */

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


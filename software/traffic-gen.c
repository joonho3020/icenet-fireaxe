#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>


#include "mmio.h"
#include "mt-utils.h"
#include "nic-rss.h"
#include "encoding.h"


#define TX_CORES 2
#define RX_CORES 2
#define TX_DESC_CNT 128
#define RX_DESC_CNT 128
#define PACKET_BYTES 1500
#define PACKET_BYTES_PADDED (PACKET_BYTES+8)


#define TRAFFIC_GEN_DEBUG_PRINT
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
  fprintf(stdout, "Core %d finished memory setup for TX DESC\n", core_id);
#endif

  uint64_t packets[TX_DESC_CNT];
  bool sent[TX_DESC_CNT];
  for (int i = 0; i < TX_DESC_CNT; i++) {
    uint64_t pkt_addr = (uint64_t)&tx_desc[i][0];
    uint64_t pkt_size = (uint64_t)PACKET_BYTES;
    uint64_t pkt = (pkt_size << 48) | pkt_addr;
    packets[i] = pkt;
    sent[i] = false;
  }


#ifndef NO_NIC_DEBUG
  do {
    int cnt = 0;
    for (int i = 0; i < TX_DESC_CNT; i++) {
      if (!sent[i]) {
        nic_send_req(core_id, packets[i]);
        sent[i] = true;
        cnt++;
      }
    }
#ifdef TRAFFIC_GEN_DEBUG_PRINT
    fprintf(stdout, "Core %d sending %d packets\n", core_id, cnt);
#endif

    cnt = 0;
    int ncomps = nic_send_comp_avail(core_id);
    asm volatile("fence");
#ifdef TRAFFIC_GEN_DEBUG_PRINT
    fprintf(stdout, "Core %d sending %d completions\n", core_id, ncomps);
#endif
    for (int i = 0; i < TX_DESC_CNT && cnt < ncomps; i++) {
      if (sent[i]) {
        nic_send_comp(core_id);
        sent[i] = false;
        cnt++;
      }
    }
  } while (1);
#endif
}

void recv_traffic(int core_id) {
  uint8_t* rx_desc[RX_DESC_CNT];
  for (int i = 0; i < RX_DESC_CNT; i++) {
    rx_desc[i] = (uint8_t*)malloc(sizeof(uint8_t) * PACKET_BYTES_PADDED);
  }

#ifdef TRAFFIC_GEN_DEBUG_PRINT
  fprintf(stdout, "Core %d finished memory setup for RX DESC\n", core_id);
#endif

#ifndef NO_NIC_DEBUG
  do {
    for (int i = 0; i < RX_DESC_CNT; i++) {
      nic_set_recv_addr(core_id, (uint64_t)rx_desc[i]);
    }

    int recv_comps_left = RX_DESC_CNT;
    int ncomps;
    while (recv_comps_left > 0) {
      ncomps = nic_recv_comp_avail(core_id);
      asm volatile ("fence");
      for (int i = 0; i < ncomps; i++)
        nic_recv_comp(core_id);
      recv_comps_left -= ncomps;
#ifdef TRAFFIC_GEN_DEBUG_PRINT
      fprintf(stdout, "Core %d recv ncomps: %d\n", core_id, ncomps);
#endif
    }
  } while (1);
#endif
}

void __main(void) {
  size_t mhartid = read_csr(mhartid);

  if (mhartid >= TX_CORES + RX_CORES) while (1);

  for (int i = TX_CORES; i < TX_CORES + RX_CORES; i++) {
    if (mhartid == i) {
      recv_traffic(mhartid);
    }
  }

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

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>


#include "mmio.h"
#include "mt-utils.h"
#include "nic-rss.h"
#include "encoding.h"


/* #define NO_NET_DEBUG */
#define TRAFFIC_RECV_DEBUG

#define N_CORES 8
#define RUN_CORES 1
#define TX_DESC_CNT 128
#define RX_DESC_CNT 128
#define PACKET_BYTES 1500
#define PACKET_BYTES_PADDED (PACKET_BYTES+8)
#define MAX_PACKETS_TO_FORWARD 1000


void forward_traffic(int core_id) {
  uint8_t* rx_desc[RX_DESC_CNT];
  for (int i = 0; i < RX_DESC_CNT; i++) {
    rx_desc[i] = (uint8_t*)malloc(sizeof(uint8_t) * PACKET_BYTES_PADDED);
  }

  uint8_t* tx_desc[TX_DESC_CNT];
  for (int i = 0; i < TX_DESC_CNT; i++) {
    tx_desc[i] = (uint8_t*)malloc(sizeof(uint8_t) * PACKET_BYTES_PADDED);
  }

#ifdef TRAFFIC_RECV_DEBUG
  fprintf(stdout, "%d\n", core_id);
#endif

#ifndef NO_NET_DEBUG
  int rx_id = 0;
  int tx_id = 0;
  int cnt = 0;
  do {
    int len = nic_recv((void*)rx_desc[rx_id], core_id);
#ifdef TRAFFIC_RECV_DEBUG
    printf("Received %d Bytes\n", len);
#endif
    memcpy((void*)tx_desc[tx_id], (void*)rx_desc[rx_id], sizeof(uint8_t)*PACKET_BYTES);
    nic_send((void*)tx_desc[tx_id], PACKET_BYTES, core_id);
#ifdef TRAFFIC_RECV_DEBUG
    printf("Sending %d Bytes\n", PACKET_BYTES);
#endif
    rx_id = (rx_id + 1) % RX_DESC_CNT;
    tx_id = (tx_id + 1) % TX_DESC_CNT;
    cnt ++;
  } while (cnt < MAX_PACKETS_TO_FORWARD);
#endif
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
  fprintf(stdout, "rd: %d wr: %d\n", (int)nic_ddio_rd_avg_lat(N_CORES), (int)nic_ddio_wr_avg_lat(N_CORES));
}

int main(void) {
  __main();
  return 0;
}

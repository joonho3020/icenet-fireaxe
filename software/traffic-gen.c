#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include <assert.h>


#include "mmio.h"
#include "mt-utils.h"
#include "nic-rss.h"
#include "encoding.h"


#define TX_CORES 1
#define RX_CORES 1
#define TX_DESC_CNT 10
#define RX_DESC_CNT 10
#define PACKET_BYTES 1424
#define PACKET_BYTES_PADDED (PACKET_BYTES+8)


#define TRAFFIC_GEN_DEBUG_PRINT
/* #define NO_NIC_DEBUG */


#define SUCCESS 0
#define FAIL -1

typedef struct Queue {
  int* data;
  int head;
  int tail;
  int size;
  int full;
} Queue;

Queue* newQueue(int size) {
  Queue* q = (Queue*)malloc(sizeof(Queue));
  q->data = (int*)malloc(sizeof(int) * size);
  q->head = 0;
  q->tail = 0;
  q->size = size;
  q->full = 0;
};

void freeQueue(Queue* q) {
  free(q->data);
  free(q);
}

int enqueue(Queue* q, int entry) {
  if (q->full) return FAIL;
  q->data[q->tail] = entry;
  q->tail = (q->tail + 1) % q->size;
  if (q->tail == q->head) q->full = 1;
  return SUCCESS;
}

int isempty(Queue *q) {
  return !q->full && (q->tail == q->head);
}

int size(Queue *q) {
  if (q->full) return q->size;
  if (q->tail >= q->head) return q->tail - q->head;
  else return (q->size + 1 - q->head + q->tail);
}

int space(Queue *q) {
  return q->size - size(q);
}

int dequeue(Queue *q) {
  assert(!isempty(q));
  int ret = q->data[q->head];
  q->head = (q->head + 1) % q->size;
  return ret;
}


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
  Queue* pending = newQueue(TX_DESC_CNT);
  Queue* inflight = newQueue(TX_DESC_CNT);
  for (int i = 0; i < TX_DESC_CNT; i++) {
    uint64_t pkt_addr = (uint64_t)&tx_desc[i][0];
    uint64_t pkt_size = (uint64_t)PACKET_BYTES;
    uint64_t pkt = (pkt_size << 48) | pkt_addr;
    packets[i] = pkt;
    enqueue(pending, i);
  }


  do {
    printf("space(pending): %d\n", space(pending));
    for (int i = 0; i < size(pending); i++) {
      int pidx = dequeue(pending);
      fprintf(stdout, "pending.deq: %d\n", pidx);
#ifndef NO_NIC_DEBUG
      nic_send_req(core_id, packets[pidx]);
#endif
      enqueue(inflight, pidx);
    }

    printf("space(inflight): %d size(inflight): %d\n", space(inflight), size(inflight));

    int ncomps = 0;
#ifndef NO_NIC_DEBUG
    ncomps = nic_send_comp_avail(core_id);
/* fprintf(stdout, "Core %d sending %d completions\n", core_id, ncomps); */
#endif
    asm volatile("fence");
#ifndef NO_NIC_DEBUG
    for (int i = 0; i < ncomps; i++) {
#else
    for (int i = 0; i < size(inflight); i++) {
#endif
      assert(!isempty(inflight));
      int pidx = dequeue(inflight);
      printf("inflight.deq: %d\n", pidx);
      enqueue(pending, pidx);
#ifndef NO_NIC_DEBUG
      nic_send_comp(core_id);
      fprintf(stdout, "nic_send_comp %d\n", pidx);
#endif
    }
  } while (0);
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

/* for (int i = TX_CORES; i < TX_CORES + RX_CORES; i++) { */
/* if (mhartid == i) { */
/* recv_traffic(mhartid); */
/* } */
/* } */

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


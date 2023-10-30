#ifndef __DSLIB_H__
#define __DSLIB_H__

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include <assert.h>

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
  else return (q->size - q->head + q->tail);
}

int space(Queue *q) {
  return q->size - size(q);
}

int dequeue(Queue *q) {
  assert(!isempty(q));
  int ret = q->data[q->head];
  q->head = (q->head + 1) % q->size;
  q->full = 0;
  return ret;
}


typedef struct Stack {
  int *data;
  int head;
  int size;
} Stack;

Stack* newStack(int max_h) {
  Stack* s = (Stack*)malloc(sizeof(Stack));
  s->data = (int*)malloc(sizeof(int)*max_h);
  s->head = 0;
  s->size = max_h;
}

void freeStack(Stack *s) {
  free(s->data);
  free(s);
}

int height(Stack* s) {
  return s->head;
}

int stackfull(Stack *s) {
  return (s->head == s->size + 1);
}

void push(Stack* s, int x) {
  s->data[s->head++] = x;
}

int pop(Stack *s) {
  assert(s->head > 0);
  int ret = s->data[--s->head];
  return ret;
}

#endif //__DSLIB_H__

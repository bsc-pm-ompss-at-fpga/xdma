/*--------------------------------------------------------------------
  (C) Copyright 2017-2020 Barcelona Supercomputing Center
                          Centro Nacional de Supercomputacion

  This file is part of OmpSs@FPGA toolchain.

  This code is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 3 of
  the License, or (at your option) any later version.

  OmpSs@FPGA toolchain is distributed in the hope that it will be
  useful, but WITHOUT ANY WARRANTY; without even the implied
  warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
  See the GNU General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this code. If not, see <www.gnu.org/licenses/>.
--------------------------------------------------------------------*/

#ifndef LOCK_FREE_QUEUE_H
#define LOCK_FREE_QUEUE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>

typedef struct node_t {
    void            *_data;
    struct node_t   *_next;
} node_t;

typedef struct {
    node_t * volatile _head;
    node_t * volatile _tail;
} queue_t;

/**
  * Create and initialize a new queue
  */
queue_t * _queueInit()
{
    queue_t * ret = (queue_t *)malloc(sizeof(queue_t));
    if (ret == NULL) return NULL;

    node_t * node = (node_t *)malloc(sizeof(node_t));
    if (node == NULL) {
        free(ret);
        return NULL;
    }
    node->_data = node->_next = NULL;
    ret->_head = ret->_tail = node;

    return ret;
}

/**
  * Atomically insert a new element in the queue
  */
void _queuePush( queue_t * const q, void * elem )
{
    node_t * n = (node_t *)malloc(sizeof(node_t));
    if (n == NULL) return;

    n->_data = elem;
    n->_next = NULL;
    node_t * l;
    do {
        l = q->_tail;
    } while (!__sync_bool_compare_and_swap(&q->_tail, l, n));
    l->_next = n;
#if 0
    node_t * t, * l;
    while(1) {
        t = q->_tail;
        l = t->_next;
        if (t == q->_tail) {
            if (l == NULL) {
                if (__sync_bool_compare_and_swap(&t->_next, l, n)) {
                    break;
                }
            } else {
                __sync_bool_compare_and_swap(&q->_tail, t, l);
            }
        }
    }
    __sync_bool_compare_and_swap(&q->_tail, t, n);
#endif
}

/**
  * Returns the next element of the queue
  * NOTE: The function is not thread-safe and a race condition may appear if queueTryPop is called
  *       at the same time
  */
void * _queueFront( queue_t * const q )
{
    node_t * n = q->_head->_next;
    return n == NULL ? n : n->_data;
}

/**
  * Remove the next element of the queue
  * NOTE: The function is not thread-safe and a race condition may appear if queueTryPop is called
  *       at the same time
  */
void _queuePop( queue_t * const q )
{
    node_t * l = q->_head->_next;
    if (l == NULL) return;

    q->_head->_next = l->_next;
    free(l);
}

/**
  * Atomically extracts from the queue the next element and returns it.
  * If the queue is empty returns NULL
  */
void * _queueTryPop( queue_t * const q )
{
    node_t * l;
    do {
        l = q->_head;
    } while (l == NULL || !__sync_bool_compare_and_swap(&q->_head, l, NULL));

    if (l->_next == NULL) {
        q->_head = l;
        return NULL;
    }

    void * ret = l->_next->_data;
    q->_head = l->_next;
    free(l);
    return ret;
}

/**
  * Finalize the queue
  */
void _queueFini( queue_t * q )
{

    for (node_t * n = q->_head->_next, * l = NULL; n != NULL; n = l) {
        l = n->_next;
        free(n);
    }
    free(q);
}

#ifdef __cplusplus
}
#endif

#endif /* __LOCK_FREE_QUEUE_H__ */

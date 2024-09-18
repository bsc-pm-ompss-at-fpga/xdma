/*--------------------------------------------------------------------
  (C) Copyright 2017-2021 Barcelona Supercomputing Center
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

#ifndef __TICKET_LOCK_H__
#define __TICKET_LOCK_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>

typedef struct {
    unsigned long volatile current;  //< Current serving id
    unsigned long volatile next;     //< Next id to assign
} ticketLock_t;

/**
 * Initialize the lock
 */
static void ticketLockInit(ticketLock_t *const l)
{
    if (l == NULL) return;

    l->current = 0;
    l->next = 0;
}

/**
 * Acquire the lock
 */
static void ticketLockAcquire(ticketLock_t *const l)
{
    unsigned long const mine = __atomic_fetch_add(&l->next, 1, __ATOMIC_RELAXED);
    while (1) {
        if (mine == l->current) break;
    }
}

/**
 * Release the lock
 */
static void ticketLockRelease(ticketLock_t *const l) { l->current++; }

/**
 * Finalize the lock
 */
static void ticketLockFini(ticketLock_t *const l) {}

#ifdef __cplusplus
}
#endif

#endif /* __TICKET_LOCK_H__ */

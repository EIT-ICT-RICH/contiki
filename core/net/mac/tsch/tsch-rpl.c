/*
 * Copyright (c) 2014, Swedish Institute of Computer Science.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */
/**
 * \file
 *         Interaction between TSCH and RPL
 *
 * \author Simon Duquennoy <simonduq@sics.se>
 */

#include "contiki.h"
#include "net/rpl/rpl.h"
#include "net/rpl/rpl-private.h"
#include "net/mac/tsch/tsch.h"
#include "net/mac/tsch/tsch-private.h"
#include "net/mac/tsch/tsch-schedule.h"
#include "tsch-rpl.h"

#define DEBUG DEBUG_NONE
#include "net/ip/uip-debug.h"

/* To use, set #define TSCH_CALLBACK_JOINING_NETWORK tsch_rpl_callback_joining_network */
void
tsch_rpl_callback_joining_network()
{
}

/* Upon leaving a TSCH network, perform a local repair
 * (cleanup neighbor state, reset Trickle timer etc)
 * To use, set #define TSCH_CALLBACK_LEAVING_NETWORK tsch_rpl_callback_leaving_network */
void
tsch_rpl_callback_leaving_network()
{
  rpl_dag_t *dag = rpl_get_any_dag();
  if(dag != NULL) {
    rpl_local_repair(dag->instance);
  }
}

/* Called whenever TSCH swtiches time source
To use, set #define TSCH_CALLBACK_NEW_TIME_SOURCE tsch_rpl_callback_new_time_source */
void
tsch_rpl_callback_new_time_source(struct tsch_neighbor *old, struct tsch_neighbor *new)
{
  /* We currently do not need to do anything here */
}

/* Set TSCH EB period based on current RPL DIO period.
 * To use, set #define RPL_CALLBACK_PARENT_SWITCH tsch_rpl_callback_new_dio_interval */
void
tsch_rpl_callback_new_dio_interval(uint8_t dio_interval)
{
  /* Transmit EBs only if we have a valid rank as per 6TiSCH minimal */
  rpl_dag_t *dag = rpl_get_any_dag();
  if(dag != NULL && dag->rank != INFINITE_RANK) {
    tsch_set_eb_period(TSCH_EB_PERIOD);
    /* Set join priority based on RPL rank */
    tsch_set_join_priority(DAG_RANK(dag->rank, dag->instance) - 1);
  } else {
    tsch_set_eb_period(0);
  }
}

/* Set TSCH time source based on current RPL preferred parent.
 * To use, set #define RPL_CALLBACK_PARENT_SWITCH tsch_rpl_callback_parent_switch */
void
tsch_rpl_callback_parent_switch(rpl_parent_t *old, rpl_parent_t *new)
{
  if(tsch_is_associated == 1) {
    rpl_dag_t *dag = rpl_get_any_dag();
    if(dag != NULL) {
      rpl_parent_t *preferred_parent = dag->preferred_parent;
      if(preferred_parent != NULL) {
        tsch_queue_update_time_source(
            (const linkaddr_t *)uip_ds6_nbr_lladdr_from_ipaddr(
                rpl_get_parent_ipaddr(preferred_parent)));
      }
    }
  }
}
/*
 * Copyright (c) 2010, Swedish Institute of Computer Science.
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
 *         Slip fallback interface
 * \author
 *         Niclas Finne <nfi@sics.se>
 *         Joakim Eriksson <joakime@sics.se>
 *         Joel Hoglund <joel@sics.se>
 *         Nicolas Tsiftes <nvt@sics.se>
 */

#include "net/uip.h"
#include "net/uip-ds6.h"
#include "net/rpl/rpl.h"
#include "dev/slip.h"
#if CONTIKI_TARGET_JN5168
#include "dev/uart0.h"
#else
#include "dev/uart1.h"
#endif
#include <string.h>

#define UIP_IP_BUF        ((struct uip_ip_hdr *)&uip_buf[UIP_LLH_LEN])

#define DEBUG DEBUG_PRINT
#include "net/uip-debug.h"

#ifndef BAUD2UBR
#define BAUD2UBR(X) (X)
#endif

void set_bridge_addr(const uip_ipaddr_t *addr);
static uip_ipaddr_t last_sender;
/*---------------------------------------------------------------------------*/
static void
slip_input_callback(void)
{
  PRINTF("SIN: %u\n", uip_len);
  if(uip_buf[0] == '!') {
    PRINTF("Got configuration message of type %c\n", uip_buf[1]);
    uip_len = 0;
    if(uip_buf[1] == 'A') {
      uip_ipaddr_t addr;
      memcpy(&addr, &uip_buf[2], 16);
      PRINTF("Received bridge host address ");
      PRINT6ADDR(&addr);
      PRINTF("\n");
      set_bridge_addr(&addr);
    }
  } else if (uip_buf[0] == '?') {
    PRINTF("Got request message of type %c\n", uip_buf[1]);
    if(uip_buf[1] == 'M') {
      char* hexchar = "0123456789abcdef";
      int j;
      /* this is just a test so far... just to see if it works */
      uip_buf[0] = '!';
      for(j = 0; j < 8; j++) {
        uip_buf[2 + j * 2] = hexchar[uip_lladdr.addr[j] >> 4];
        uip_buf[3 + j * 2] = hexchar[uip_lladdr.addr[j] & 15];
      }
      uip_len = 18;
      slip_send();

    } else if (uip_buf[1] == 'A') {
      /* This is a request for the IP address */
      uip_buf[0] = '!';
      PRINTF("slip-bridge: got-ipv6-addr-request\n");
      /* Reply with a global IP address, otherwise ignore */
      if (uip_ds6_get_global(ADDR_PREFERRED) == NULL) {
        PRINTF("slip-bridge: no-preferred-addr yet\n");
   	    goto _exit;
      }
      uint8_t *addr = &(uip_ds6_get_global(ADDR_PREFERRED)->ipaddr.u8[0]);
      memcpy(&uip_buf[2], &addr[0], 16);
      uint8_t prefix_len = 0;
      rpl_dag_t *rpl_dag = rpl_get_any_dag();
      if (rpl_dag != NULL) {
        prefix_len = rpl_dag->prefix_info.length;
      }
      prefix_len = (prefix_len == 0) ? 64 : prefix_len; /* If not set, send default (64) */
      uip_buf[18] = prefix_len;
      PRINTF("slip-bridge: sending ipv6 address reply and prefix length %u\n", prefix_len);
      uip_len = 19;
      slip_send();
    }
    uip_len = 0;
  }
_exit:
  /* Save the last sender received over SLIP to avoid bouncing the
     packet back if no route is found */
  uip_ipaddr_copy(&last_sender, &UIP_IP_BUF->srcipaddr);
}
/*---------------------------------------------------------------------------*/
static void
init(void)
{
  /* the BAUD2UBR config does not matter in JN5168,
   * because it is configured in contiki-main */
  slip_arch_init(BAUD2UBR(115200));
  process_start(&slip_process, NULL);
  slip_set_input_callback(slip_input_callback);
}
/*---------------------------------------------------------------------------*/
static void
output(void)
{
  if(uip_ipaddr_cmp(&last_sender, &UIP_IP_BUF->srcipaddr)) {
    /* Do not bounce packets back over SLIP if the packet was received
       over SLIP */
    PRINTF("slip-bridge: Destination off-link but no route src=");
    PRINT6ADDR(&UIP_IP_BUF->srcipaddr);
    PRINTF(" dst=");
    PRINT6ADDR(&UIP_IP_BUF->destipaddr);
    PRINTF("\n");
  } else {
    PRINTF("SUT: %u\n", uip_len);
    slip_send();
    printf("\n");
  }
}

/*---------------------------------------------------------------------------*/
#if !SLIP_BRIDGE_CONF_NO_PUTCHAR
#ifdef putchar
#undef putchar
#endif
int
putchar(int c)
{
#define SLIP_END     0300
  static char debug_frame = 0;

  if(!debug_frame) {            /* Start of debug output */
    slip_arch_writeb(SLIP_END);
    slip_arch_writeb('\r');     /* Type debug line == '\r' */
    debug_frame = 1;
  }

  /* Need to also print '\n' because for example COOJA will not show
     any output before line end */
  slip_arch_writeb((char)c);

  /*
   * Line buffered output, a newline marks the end of debug output and
   * implicitly flushes debug output.
   */
  if(c == '\n') {
    slip_arch_writeb(SLIP_END);
    debug_frame = 0;
  }
  return c;
}
#endif
/*---------------------------------------------------------------------------*/
const struct uip_fallback_interface rpl_interface = {
  init, output
};
/*---------------------------------------------------------------------------*/
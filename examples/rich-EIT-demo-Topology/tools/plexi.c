/**
 *  \file RICH CoAP Scheduler Interface for Contiki 3.x
 *  
 *  \author George Exarchakos <g.exarchakos@tue.nl>
 *  		Ilker Oztelcan <i.oztelcan@tue.nl>
 *  
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "contiki.h"
#include "net/rpl/rpl.h"
#include "rich.h"

#include "rest-engine.h"
#include "net/ip/uip.h"
#include "net/ipv6/uip-ds6-route.h"
#include "net/ip/uip-debug.h"
#include "net/packetbuf.h"
#include "common-conf.h"

#include "net/mac/tsch/tsch.h"
#include "net/mac/tsch/tsch-private.h"
#include "net/mac/tsch/tsch-schedule.h"
#include "net/mac/tsch/tsch-queue.h"

#include "jsontree.h"
#include "jsonparse.h"

#include "er-coap-engine.h"

#include "plexi.h"

#define MAX_DATA_LEN REST_MAX_CHUNK_SIZE
#define NO_LOCK 0
#define DAG_GET_LOCK 1
#define NEIGHBORS_GET_LOCK 2
#define SLOTFRAME_GET_LOCK 3
#define SLOTFRAME_DEL_LOCK 4
#define SLOTFRAME_POST_LOCK 5
#define LINK_GET_LOCK 6
#define LINK_DEL_LOCK 7
#define LINK_POST_LOCK 8

#define DEBUG DEBUG_PRINT
#define CONTENT_PRINTF(...) { \
	if(content_len < sizeof(content)) \
		content_len += snprintf(content+content_len, sizeof(content)-content_len, __VA_ARGS__); \
}

static void plexi_dag_event_handler(void);
static void plexi_get_dag_handler(void *request, void *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset);
static void plexi_get_dag_handler(void *request, void *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset);

static void plexi_neighbors_event_handler(void);
#if PLEXI_WITH_TRAFFIC_GENERATOR
	static void plexi_generate_traffic(void*);
#endif
static void plexi_get_neighbors_handler(void *request, void *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset);

static void plexi_get_slotframe_handler(void* request, void* response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset);
static void plexi_post_slotframe_handler(void* request, void* response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset);
static void plexi_delete_slotframe_handler(void* request, void* response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset);

static void plexi_get_links_handler(void *request, void *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset);
static void plexi_delete_links_handler(void *request, void *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset);
static void plexi_post_links_handler(void* request, void* response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset);
static void plexi_links_event_handler(void);

#if PLEXI_WITH_LINK_STATISTICS
	#define STATS_GET_LOCK 9
	#define STATS_DEL_LOCK 10
	#define STATS_POST_LOCK 11
	static void plexi_stats_event_handler(void);
	static void plexi_get_stats_handler(void *request, void *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset);
	static void plexi_delete_stats_handler(void *request, void *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset);
	static void plexi_post_stats_handler(void* request, void* response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset);

	static void plexi_packet_received(void);
	static void plexi_packet_sent(int mac_status);
	void plexi_update_ewma_statistics(uint8_t metric, void* old_value, plexi_stats_value_t new_value);

	void plexi_purge_neighbor_statistics(linkaddr_t *neighbor);
	void plexi_purge_statistics(plexi_stats *stats);
	void plexi_purge_link_statistics(struct tsch_link *link);
	void plexi_purge_enhanced_statistics(plexi_enhanced_stats *stats);

	uint16_t plexi_get_statistics_id(plexi_stats* stats);
	int plexi_set_statistics_id(plexi_stats* stats, uint16_t id);
	uint8_t plexi_get_statistics_enable(plexi_stats* stats);
	int plexi_set_statistics_enable(plexi_stats* stats, uint8_t enable);
	uint8_t plexi_get_statistics_metric(plexi_stats* stats);
	int plexi_set_statistics_metric(plexi_stats* stats, uint8_t metric);
	uint16_t plexi_get_statistics_window(plexi_stats* stats);
	int plexi_set_statistics_window(plexi_stats* stats, uint16_t window);

	MEMB(plexi_stats_mem, plexi_stats, PLEXI_MAX_STATISTICS);
	MEMB(plexi_enhanced_stats_mem, plexi_enhanced_stats, PLEXI_MAX_STATISTICS);

	RIME_SNIFFER(plexi_sniffer, plexi_packet_received, plexi_packet_sent);

	void printubin(plexi_stats_value_t a);
	void printsbin(plexi_stats_value_st a);

#endif

#if PLEXI_WITH_VICINITY_MONITOR
	static void plexi_vicinity_updater(void* ptr);
	static void plexi_vicinity_event_handler(void);
	static void plexi_get_vicinity_handler(void *request, void *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset);

	MEMB(plexi_vicinity_mem, plexi_proximate, PLEXI_MAX_PROXIMATES);
	LIST(plexi_vicinity);
#endif

#if PLEXI_WITH_QUEUE_STATISTICS
	static void plexi_get_queue_handler(void *request, void *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset);
	static void plexi_queue_event_handler(void);
	static void plexi_queue_changed(uint8_t event, struct tsch_neighbor* n);
#endif

int jsonparse_find_field(struct jsonparse_state *js, char *field_buf, int field_buf_len);
static int na_to_linkaddr(const char *na_inbuf, int bufsize, linkaddr_t *linkaddress);
static int linkaddr_to_na(char* buf, linkaddr_t *addr);

static struct ctimer ct;
static char content[MAX_DATA_LEN];
static int content_len = 0;
static char inbox_msg[MAX_DATA_LEN];
static int inbox_msg_len = 0;
static int inbox_msg_lock = NO_LOCK;
static uint16_t new_tx_slotframe = 0;
static uint16_t new_tx_timeslot = 0;

static uint16_t traffic_counter = 0;


/*********** RICH scheduler resources *************************************************************/

/**************************************************************************************************/
/** Observable dodag resource and event handler to obtain RPL parent and children 				  */ 
/**************************************************************************************************/
EVENT_RESOURCE(resource_rpl_dag,								/* name */
               "obs;title=\"RPL DAG Parent and Children\"",		/* attributes */
               plexi_get_dag_handler,							/* GET handler */
               NULL,											/* POST handler */
               NULL,											/* PUT handler */
               NULL,											/* DELETE handler */
               plexi_dag_event_handler);						/* event handler */


/** Builds the JSON response to requests for the rpl/dag resource details.						   *
 *	The response is a JSON object of two elements:												   *
 *	{																							   *
 *		"parent": [IP addresses of preferred parent according to RPL],							   *
 *		"child": [IP addresses of RPL children]													   *
 *	}
*/
static void plexi_get_dag_handler(void *request,
									void *response,
									uint8_t *buffer, 
									uint16_t preferred_size, 
									int32_t *offset) {
	if(inbox_msg_lock != NO_LOCK && inbox_msg_lock != DAG_GET_LOCK) {
		coap_set_status_code(response, SERVICE_UNAVAILABLE_5_03);
		coap_set_payload(response, "Server too busy. Retry later.", 29);
		return;
	}
	inbox_msg_lock = NO_LOCK;
	unsigned int accept = -1;
	REST.get_header_accept(request, &accept);
	/* make sure the request accepts JSON reply or does not specify the reply type */
	if(accept == -1 || accept == REST.type.APPLICATION_JSON) {
		content_len = 0;
		CONTENT_PRINTF("{");
		// TODO: get details per dag id other than the default
		/* Ask RPL to provide the preferred parent or if not known (e.g. LBR) leave it empty */
		rpl_parent_t *parent = rpl_get_any_dag()->preferred_parent;
		if(parent != NULL) {
			/* retrieve the IP address of the parent */
			uip_ipaddr_t *parent_addr = rpl_get_parent_ipaddr(parent); 
			CONTENT_PRINTF("\"%s\":[\"%x:%x:%x:%x\"]", DAG_PARENT_LABEL,
				UIP_HTONS(parent_addr->u16[4]), UIP_HTONS(parent_addr->u16[5]),
				UIP_HTONS(parent_addr->u16[6]), UIP_HTONS(parent_addr->u16[7])
			);
		} else {
			CONTENT_PRINTF("\"%s\":[]", DAG_PARENT_LABEL);
		}
		
		CONTENT_PRINTF(",\"%s\":[", DAG_CHILD_LABEL);
		
		//uip_ds6_route_t *r;
		int first_item = 1;
		uip_ipaddr_t *last_next_hop = NULL;
		uip_ipaddr_t *curr_next_hop = NULL;
		/* Iterate over routing table and record all children */
// 		for (r = uip_ds6_route_head(); r != NULL; r = uip_ds6_route_next(r)) {
//			/* the first entry in the routing table is the parent, so it is skipped */
//			curr_next_hop = uip_ds6_route_nexthop(r); 
//			if (curr_next_hop != last_next_hop) {
//				if (!first_item) {
//					CONTENT_PRINTF(",");
//				}
//				first_item = 0;
//				CONTENT_PRINTF("\"%x:%x:%x:%x\"",
//					UIP_HTONS(curr_next_hop->u16[4]), UIP_HTONS(curr_next_hop->u16[5]),
//					UIP_HTONS(curr_next_hop->u16[6]), UIP_HTONS(curr_next_hop->u16[7])
//				);
//				last_next_hop = curr_next_hop;
//			}
//		}
		nbr_table_item_t *r;
		for(r = nbr_table_head(nbr_routes); r != NULL; r = nbr_table_next(nbr_routes, r))
		{
			if(r != nbr_table_head(nbr_routes))
			{
				CONTENT_PRINTF(",");
			}
			linkaddr_t *addr = (linkaddr_t *)nbr_table_get_lladdr(nbr_routes,r);
				CONTENT_PRINTF("\"2%02x:%02x%02x:%02x:%02x%02x\"",
					UIP_HTONS(addr->u8[1]), UIP_HTONS(addr->u8[2]), UIP_HTONS(addr->u8[3]),
					UIP_HTONS(addr->u8[5]), UIP_HTONS(addr->u8[6]), UIP_HTONS(addr->u8[7])
				);
		}
		CONTENT_PRINTF("]}");
		/* Build the header of the reply */
		REST.set_header_content_type(response, REST.type.APPLICATION_JSON); 
		/* Build the payload of the reply */
		REST.set_response_payload(response, (uint8_t *)content, content_len);
	}
}

/* Notify all clients who observe changes to rpl/dag resource i.e. to the RPL dodag connections */
static void plexi_dag_event_handler() {
	/* Registered observers are notified and will trigger the GET handler to create the response. */
	REST.notify_subscribers(&resource_rpl_dag);
}

#if PLEXI_WITH_VICINITY_MONITOR
	/**************************************************************************************************/
	/** Observable resource for the list of reachable nodes and event handler to obtain all their data*/
	/**************************************************************************************************/
	PERIODIC_RESOURCE(resource_mac_vicinity,						/* name */
			"obs;title=\"6top vicinity\"",						/* attributes */
			plexi_get_vicinity_handler,							/* GET handler */
			NULL,												/* POST handler */
			NULL,												/* PUT handler */
			NULL,												/* DELETE handler */
			10*PLEXI_PHEROMONE_WINDOW,							/* period */
			plexi_vicinity_event_handler);						/* event handler */

	static void plexi_get_vicinity_handler(void *request, void *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset) {
		if(inbox_msg_lock != NO_LOCK && inbox_msg_lock != NEIGHBORS_GET_LOCK) {
			coap_set_status_code(response, SERVICE_UNAVAILABLE_5_03);
			coap_set_payload(response, "Server too busy. Retry later.", 29);
			return;
		}
		inbox_msg_lock = NO_LOCK;
		content_len = 0;
		unsigned int accept = -1;
		REST.get_header_accept(request, &accept);
		if(accept == -1 || accept == REST.type.APPLICATION_JSON) {
			char *end;
			char *uri_path = NULL;
			const char *query = NULL;
			int uri_len = REST.get_url(request, (const char**)(&uri_path));
			*(uri_path+uri_len) = '\0';
			int base_len = strlen(resource_mac_vicinity.url);
			int query_len = REST.get_query(request, &query);
			char *query_tna = NULL;
			linkaddr_t *tna = NULL;
			int query_tna_len = REST.get_query_variable(request, FRAME_ID_LABEL, (const char**)(&query_tna));
			if(query_tna) {
				*(query_tna+query_tna_len) = '\0';
				int success = na_to_linkaddr(query_tna, query_tna_len, tna);
				if(!success) tna = NULL;
			}
			if(query_len > 0 && !query_tna) {
				coap_set_status_code(response, NOT_IMPLEMENTED_5_01);
				coap_set_payload(response, "Supports queries only on target node address", 44);
				return;
			}

			char *uri_subresource = uri_path+base_len;
			if(*uri_subresource == '/') uri_subresource++;
			if(uri_len > base_len + 1 && strcmp(VICINITY_AGE_LABEL,uri_subresource) && strcmp(VICINITY_PHEROMONE_LABEL,uri_subresource)) {
				coap_set_status_code(response, NOT_FOUND_4_04);
				coap_set_payload(response, "Invalid subresource", 19);
				return;
			}
			plexi_proximate *p;
			int first_item = 1;
			for(p = list_head(plexi_vicinity); p != NULL; p = list_item_next(p)) {
				if(!query_tna || (tna && linkaddr_cmp(&p->proximate, tna))) {
					if(first_item) {
						if(!tna) {
							CONTENT_PRINTF("[");
						}
						first_item = 0;
					} else
						CONTENT_PRINTF(",");
					if(!strcmp(VICINITY_AGE_LABEL,uri_subresource)) {
						CONTENT_PRINTF("%u",clock_time()-p->since);
					} else if(!strcmp(VICINITY_PHEROMONE_LABEL,uri_subresource)) {
						CONTENT_PRINTF("%u",p->pheromone);
					} else {
						CONTENT_PRINTF("{");
						if(!tna) {
							char tmp[20];
							linkaddr_to_na(tmp, &p->proximate);
							CONTENT_PRINTF("\"%s\":\"%s\",",NEIGHBORS_TNA_LABEL, tmp);
						}
						CONTENT_PRINTF("\"%s\":%u,\"%s\":%u}", VICINITY_AGE_LABEL, clock_time()-p->since, VICINITY_PHEROMONE_LABEL, p->pheromone);
					}
				}
			}
			if(!tna)
				CONTENT_PRINTF("]");
			if(!first_item) {
				REST.set_header_content_type(response, REST.type.APPLICATION_JSON);
				REST.set_response_payload(response, (uint8_t *)content, content_len);
			} else {
				coap_set_status_code(response, NOT_FOUND_4_04);
				coap_set_payload(response, "Resource was not found", 22);
				return;
			}
		} else {
			coap_set_status_code(response, NOT_ACCEPTABLE_4_06);
			return;
		}
	}

	static void plexi_vicinity_updater(void* ptr) {
		plexi_proximate *p;
		clock_time_t now = clock_time();
		for(p = list_head(plexi_vicinity); p != NULL; p = list_item_next(p)) {
			if(now-p->since > PLEXI_PHEROMONE_WINDOW) {
				p->pheromone -= PLEXI_PHEROMONE_DECAY;
			}
			if(p->pheromone <= 0) {
				plexi_proximate *k = p;
				p = p->next;
				list_remove(plexi_vicinity, k);
				memb_free(&plexi_vicinity_mem, k);
			}
		}
	}

	static void plexi_vicinity_event_handler(void) {
		REST.notify_subscribers(&resource_mac_vicinity);
	}

#endif

/**************************************************************************************************/
/** Observable neighbor list resource and event handler to obtain all neighbor data 			  */
/**************************************************************************************************/
#ifdef PLEXI_NEIGHBOR_UPDATE_INTER
PARENT_PERIODIC_RESOURCE(resource_6top_nbrs,								/* name */
		"obs;title=\"6top neighbours\"",						/* attributes */
		plexi_get_neighbors_handler,							/* GET handler */
		NULL,													/* POST handler */
		NULL,													/* PUT handler */
		NULL,													/* DELETE handler */
		PLEXI_NEIGHBOR_UPDATE_INTERVAL,
		plexi_neighbors_event_handler);							/* EVENT handler */
#else
PARENT_RESOURCE(resource_6top_nbrs,								/* name */
		"title=\"6top neighbours\"",						/* attributes */
		plexi_get_neighbors_handler,							/* GET handler */
		NULL,													/* POST handler */
		NULL,													/* PUT handler */
		NULL													/* DELETE handler */
		);
#endif

#if PLEXI_WITH_TRAFFIC_GENERATOR
	char TRAFFIC[12] = {'\0'};
#endif
/** Builds the JSON response to requests for 6top/nbrs resource.								   *
 * The response consists of an object with the neighbors and the time elapsed since the last       *
 * confirmed interaction with that neighbor. That is:											   *
 * 	{																							   *
		"IP address of neigbor 1":time in miliseconds,											   *
		...																						   *
		"IP address of neigbor 2":time in miliseconds											   *
	}																							   *
*/
static void plexi_get_neighbors_handler(void *request, void *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset) {
#if PLEXI_WITH_TRAFFIC_GENERATOR
	if(strlen(TRAFFIC) > 0) {
		content_len = 0;
		CONTENT_PRINTF("{\"traffic\":\"");
		CONTENT_PRINTF(TRAFFIC);
		CONTENT_PRINTF("\"}");
		REST.set_header_content_type(response, REST.type.APPLICATION_JSON);
		REST.set_response_payload(response, (uint8_t *)content, content_len);
		return;
	}
#endif
	if(inbox_msg_lock != NO_LOCK && inbox_msg_lock != NEIGHBORS_GET_LOCK) {
		coap_set_status_code(response, SERVICE_UNAVAILABLE_5_03);
		coap_set_payload(response, "Server too busy. Retry later.", 29);
		return;
	}
	inbox_msg_lock = NO_LOCK;
	content_len = 0;
	unsigned int accept = -1;
	REST.get_header_accept(request, &accept);
	if(accept == -1 || accept == REST.type.APPLICATION_JSON) {
		char *end;
		char *uri_path = NULL;
		const char *query = NULL;
		int uri_len = REST.get_url(request, (const char**)(&uri_path));
		int base_len = 0, query_len = 0, query_value_len = 0;
		char *uri_subresource = NULL, *query_value = NULL;
		linkaddr_t tna = linkaddr_null;
		
		if(uri_len>0) {
			*(uri_path+uri_len) = '\0';
			base_len = strlen(resource_6top_nbrs.url);
			uri_subresource = uri_path+base_len;
			if(*uri_subresource == '/') {
				uri_subresource++;
			}
			query_len = REST.get_query(request, &query);
			query_value_len = REST.get_query_variable(request, NEIGHBORS_TNA_LABEL, (const char**)(&query_value));
			if(query_value) {
				*(query_value+query_value_len) = '\0';
				int success = na_to_linkaddr(query_value, query_value_len, &tna);
				if(!success) {
					coap_set_status_code(response, BAD_REQUEST_4_00);
					coap_set_payload(response, "Bad node address format", 23);
					return;
				}
			}
		}
#if PLEXI_WITH_LINK_STATISTICS
		if((uri_len > base_len + 1 && strcmp(NEIGHBORS_TNA_LABEL,uri_subresource) \
			  && strcmp(STATS_RSSI_LABEL,uri_subresource) \
			  && strcmp(STATS_LQI_LABEL,uri_subresource) \
			  && strcmp(STATS_ETX_LABEL,uri_subresource) \
			  && strcmp(STATS_PDR_LABEL,uri_subresource) \
			  && strcmp(NEIGHBORS_ASN_LABEL,uri_subresource) \
			  ) || (query && !query_value)) {
			coap_set_status_code(response, BAD_REQUEST_4_00);
			coap_set_payload(response, "Supports only queries on neighbor address", 41);
			return;
		}
#else
		if((uri_len > base_len + 1 && strcmp(NEIGHBORS_TNA_LABEL,uri_subresource)) \
			|| (query && !query_value)) {
			coap_set_status_code(response, BAD_REQUEST_4_00);
			coap_set_payload(response, "Supports only queries on neighbor address", 41);
			return;
		}
#endif
		
		uint8_t found = 0;
		char buf[32];
		if(linkaddr_cmp(&tna,&linkaddr_null)) {
			CONTENT_PRINTF("[");
		}
		uip_ds6_nbr_t *nbr;
		clock_time_t now = clock_time();
		int first_item = 1;
		uip_ipaddr_t *last_next_hop = NULL;
		uip_ipaddr_t *curr_next_hop = NULL;
		for(nbr = nbr_table_head(ds6_neighbors); nbr != NULL; nbr = nbr_table_next(ds6_neighbors, nbr)) {
			curr_next_hop = (uip_ipaddr_t *)uip_ds6_nbr_get_ipaddr(nbr);
			linkaddr_t *lla = (linkaddr_t *)uip_ds6_nbr_get_ll(nbr);
			if(curr_next_hop != last_next_hop) {
				if(first_item) {
					first_item = 0;
				} else if(found) {
					CONTENT_PRINTF(",");
				}
				int success = linkaddr_to_na(buf, lla);
				if(!strcmp(NEIGHBORS_TNA_LABEL,uri_subresource)) {
					if(success) {
						found = 1;
						CONTENT_PRINTF("\"%s\"",buf);
					}
				} else {
#if PLEXI_WITH_LINK_STATISTICS
					int rssi = (int)0xFFFFFFFFFFFFFFFF, lqi = -1, asn = -1, etx = -1, pdr = -1;
					int rssi_counter = 0, lqi_counter = 0, asn_counter = 0, etx_counter = 0, pdr_counter = 0;
					struct tsch_slotframe * slotframe = (struct tsch_slotframe*)tsch_schedule_get_next_slotframe(NULL);
					while(slotframe) {
						struct tsch_link* link = (struct tsch_link*)tsch_schedule_get_next_link_of(slotframe, NULL);
						while(link) {
							if(memb_inmemb(&plexi_stats_mem, link->data)) {
								plexi_stats *stats = (plexi_stats*)link->data;
								while(stats) {
									uint8_t metric = plexi_get_statistics_metric(stats);
									plexi_stats_value_st value = -1;
									if(linkaddr_cmp(lla, &link->addr))
										value = (plexi_stats_value_st)stats->value;
									else if(memb_inmemb(&plexi_enhanced_stats_mem, list_head(stats->enhancement))) {
										plexi_enhanced_stats * es;
										for(es = list_head(stats->enhancement); es!=NULL; es = list_item_next(es)) {
											if(linkaddr_cmp(lla, &es->target)) {
												value = (plexi_stats_value_st)es->value;
												break;
											}
										}
									}
									if(value != -1) {
										if(metric == RSSI) {
											if(rssi==(int)0xFFFFFFFFFFFFFFFF) { rssi = (plexi_stats_value_st)value; }
											else { rssi += (plexi_stats_value_st)value; }
											rssi_counter++;
										} else if(metric == LQI) {
											if(lqi==-1) { lqi = value; }
											else { lqi += value; }
											lqi_counter++;
										} else if(metric == ETX) {
											if(etx==-1) { etx = (int)value; }
											else { etx += (int)value; }
											etx_counter++;
										} else if(metric == PDR) {
											if(pdr==-1) { pdr = value; }
											else { pdr += value; }
											pdr_counter++;
										} else if(metric == ASN) {
											if(asn==-1 || value > asn) { asn = value; }
											asn_counter++;
										}
									}
									stats = stats->next;
								}
							}
							link = (struct tsch_link*)tsch_schedule_get_next_link_of(slotframe, link);
						}
						slotframe = (struct tsch_slotframe *)tsch_schedule_get_next_slotframe(slotframe);
					}
					if(rssi < (int)0xFFFFFFFFFFFFFFFF && !strcmp(STATS_RSSI_LABEL,uri_subresource)) {
						found = 1;
						CONTENT_PRINTF("%d", rssi/rssi_counter);
					} else if(lqi > -1 && !strcmp(STATS_LQI_LABEL,uri_subresource)) {
						found = 1;
						CONTENT_PRINTF("%u", lqi/lqi_counter);
					} else if(etx > -1 && !strcmp(STATS_ETX_LABEL,uri_subresource)) {
						found = 1;
						CONTENT_PRINTF("%d", etx/256/etx_counter);
					} else if(pdr > -1 && !strcmp(STATS_PDR_LABEL,uri_subresource)) {
						found = 1;
						CONTENT_PRINTF("%u", pdr/pdr_counter);
					} else if(asn > -1 && !strcmp(NEIGHBORS_ASN_LABEL,uri_subresource)) {
						found = 1;
						CONTENT_PRINTF("\"%lx\"", asn);
					} else if(base_len == uri_len) {
						found = 1;
						CONTENT_PRINTF("{\"%s\":\"%s\"", NEIGHBORS_TNA_LABEL, buf);
						if(rssi < (int)0xFFFFFFFFFFFFFFFF) {
							CONTENT_PRINTF(",\"%s\":%d", STATS_RSSI_LABEL, rssi/rssi_counter);
						}
						if(lqi > -1) {
							CONTENT_PRINTF(",\"%s\":%u", STATS_LQI_LABEL, lqi/lqi_counter);
						}
						if(etx > -1) {
							CONTENT_PRINTF(",\"%s\":%d", STATS_ETX_LABEL, etx/256/etx_counter);
						}
						if(pdr > -1) {
							CONTENT_PRINTF(",\"%s\":%u", STATS_PDR_LABEL, pdr/pdr_counter);
						}
						if(asn > -1) {
							CONTENT_PRINTF(",\"%s\":\"%lx\"", NEIGHBORS_ASN_LABEL, asn);
						}
						CONTENT_PRINTF("}");
					}
#else
					if(base_len == uri_len) {
						CONTENT_PRINTF("{\"%s\":\"%s\"}", NEIGHBORS_TNA_LABEL, buf);
					}
#endif
				}
			}
		}
		if(linkaddr_cmp(&tna,&linkaddr_null)) { CONTENT_PRINTF("]"); }
		REST.set_header_content_type(response, REST.type.APPLICATION_JSON);
		REST.set_response_payload(response, (uint8_t *)content, content_len);
	} else {
		coap_set_status_code(response, NOT_ACCEPTABLE_4_06);
		return;
	}
}

#ifdef PLEXI_NEIGHBOR_UPDATE_INTERVAL
	/* Notifies all clients who observe changes to the 6top/nbrs resource */
	static void plexi_neighbors_event_handler(void) {
		REST.notify_subscribers(&resource_6top_nbrs);
	}
#endif

/* Wait for 30s without activity before notifying subscribers */
static struct ctimer route_changed_timer;

static void plexi_route_changed_handler(void* ptr) {
	REST.notify_subscribers(&resource_rpl_dag);
#ifdef PLEXI_NEIGHBOR_UPDATE_INTERVAL
	REST.notify_subscribers(&resource_6top_nbrs);
#endif
}
/* Callback function to be called when a change to the rpl/dag resource has occurred.
 * Any change is delayed 30seconds before it is propagated to the observers.
*/
static void route_changed_callback(int event, uip_ipaddr_t *route, uip_ipaddr_t *ipaddr, int num_routes) {
  /* We have added or removed a routing entry, notify subscribers */
	if(event == UIP_DS6_NOTIFICATION_ROUTE_ADD || event == UIP_DS6_NOTIFICATION_ROUTE_RM) {
		printf("PLEXI: notifying observers of rpl/dag resource \n");//setting route_changed callback with 30s delay\n");
		ctimer_set(&route_changed_timer, 5*CLOCK_SECOND, plexi_route_changed_handler, NULL);
	}
}

#if PLEXI_WITH_TRAFFIC_GENERATOR

static void plexi_generate_traffic(void* ptr) {
	snprintf(TRAFFIC,12,"%u",++traffic_counter);
	printf("PLEXIFLEX,%x.%lx,%u\n",current_asn.ms1b, current_asn.ls4b,traffic_counter);
	plexi_neighbors_event_handler();
	*TRAFFIC = '\0';
}

#endif

/**************************************************************************************************
 **              Slotframe Resource to GET, POST or DELETE slotframes               			  * 
 **************************************************************************************************/

PARENT_RESOURCE(resource_6top_slotframe,		/* name */
		"title=\"6top Slotframe\";",     	/* attributes */
		plexi_get_slotframe_handler,			/*GET handler*/
		plexi_post_slotframe_handler,			/*POST handler*/
		NULL,									/*PUT handler*/
		plexi_delete_slotframe_handler);		/*DELETE handler*/ 
 
/** Builds the response to requests for 6top/slotFrame resource and its subresources. Each slotframe
 * is an object like:
 *	{
 *		"id":<id>,
 *		"slots":<num of slots in frame>
 *	}
 * The response to 6top/slotFrame is an array of the complete slotframe objects above.
 *	[
 *		{
 *			"id":<id>,
 *			"slots":<num of slots in frame>
 *		}
 *	]
 * Subresources are also possible:
 * 	The response to 6top/slotFrame/id is an array of slotframe ids
 * 	The response to 6top/slotFrame/slots is an array of slotframe sizes 
 * 
 * Queries are also possible.
 * Queries over 6top/slotFrame generate arrays of complete slotFrame objects.
 * For example:
 * 	The response to 6top/slotFrame?id=4 is the complete slotframe object with id=4.
 * 		Since id is the unique handle of a slotframe, the response will be just one slotframe object
 *		 (not an array).
 *		{
 *			"id":<id>,
 *			"slots":<num of slots in frame>
 *		}
 * 	The response to 6top/slotFrame?slots=4 is an array with the complete slotframe objects of size 4. That is:
 *	[
 *		{
 *			"id":<id>,
 *			"slots":<num of slots in frame>
 *		}
 *	]
 * 
 * Queries over subresources e.g. 6top/slotFrame/id generate arrays of the subresource.
 * For example:
 * 	The response to 6top/slotFrame/slots?id=4 is an array of the size of slotframe with id=4: e.g. [101]
 * 	The response to 6top/slotFrame/id?slots=101 is an array of the ids of slotframes with slots=101: e.g. [1,3,4]
 *	Obviously, response to 6top/slotFrame/id?id=4 will give: [4], and
 *	response to 6top/slotFrame/slots?slots=101 will give an array will all elements equal to 101 and as many as the number of slotframes with that size: e.g. [101,101,101]
 */ 
static void plexi_get_slotframe_handler(void* request, void* response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset) {
	if(inbox_msg_lock != NO_LOCK && inbox_msg_lock != SLOTFRAME_GET_LOCK) {
		coap_set_status_code(response, SERVICE_UNAVAILABLE_5_03);
		coap_set_payload(response, "Server too busy. Retry later.", 29);
		return;
	}
	inbox_msg_lock = NO_LOCK;
	content_len = 0;
	unsigned int accept = -1;
	REST.get_header_accept(request, &accept);
	
	if(accept == -1 || accept == REST.type.APPLICATION_JSON) {
		char *end;
		char *uri_path = NULL;
		const char *query = NULL;
		int uri_len = REST.get_url(request, (const char**)(&uri_path));
		*(uri_path+uri_len) = '\0';
		int base_len = strlen(resource_6top_slotframe.url);
		char *uri_subresource = uri_path+base_len;
		if(*uri_subresource == '/')
			uri_subresource++;
		int query_len = REST.get_query(request, &query);
		char *query_value = NULL;
		unsigned long value = -1;
		int query_value_len = REST.get_query_variable(request, FRAME_ID_LABEL, (const char**)(&query_value));
		if(!query_value) {
			query_value_len = REST.get_query_variable(request, FRAME_SLOTS_LABEL, (const char**)(&query_value));
		}
		if(query_value) {
			*(query_value+query_value_len) = '\0';
			value = (unsigned)strtoul((const char*)query_value, &end, 10);
		}
		if((uri_len > base_len + 1 && strcmp(FRAME_ID_LABEL,uri_subresource) && strcmp(FRAME_SLOTS_LABEL,uri_subresource)) || (query && !query_value)) {
			coap_set_status_code(response, NOT_IMPLEMENTED_5_01);
			coap_set_payload(response, "Supports only slot frame id XOR size as subresource or query", 60);
			return;
		}
		int item_counter = 0;
		CONTENT_PRINTF("[");
		struct tsch_slotframe* slotframe = (struct tsch_slotframe*)tsch_schedule_get_next_slotframe(NULL);
		while(slotframe) {
			if(!query_value || (!strncmp(FRAME_ID_LABEL,query,sizeof(FRAME_ID_LABEL)-1) && slotframe->handle == value) || \
				(!strncmp(FRAME_SLOTS_LABEL,query,sizeof(FRAME_SLOTS_LABEL)-1) && slotframe->size.val == value)) {
				if(item_counter > 0) {
					CONTENT_PRINTF(",");
				} else if(query_value && uri_len == base_len && !strncmp(FRAME_ID_LABEL,query,sizeof(FRAME_ID_LABEL)-1) && slotframe->handle == value) {
					content_len = 0;
				}
				item_counter++;
				if(!strcmp(FRAME_ID_LABEL,uri_subresource)) {
					CONTENT_PRINTF("%u",slotframe->handle);
				} else if(!strcmp(FRAME_SLOTS_LABEL,uri_subresource)) {
					CONTENT_PRINTF("%u",slotframe->size.val);
				} else {
					CONTENT_PRINTF("{\"%s\":%u,\"%s\":%u}", FRAME_ID_LABEL, slotframe->handle, FRAME_SLOTS_LABEL, slotframe->size.val);
				}
			}
			slotframe = (struct tsch_slotframe*)tsch_schedule_get_next_slotframe(slotframe);
		}
		if(!query || uri_len != base_len || strncmp(FRAME_ID_LABEL,query,sizeof(FRAME_ID_LABEL)-1)) {
			CONTENT_PRINTF("]");
		}
		if(item_counter>0) {
			REST.set_header_content_type(response, REST.type.APPLICATION_JSON);
			REST.set_response_payload(response, (uint8_t *)content, content_len);
		} else {
			coap_set_status_code(response, NOT_FOUND_4_04);
			coap_set_payload(response, "No slotframe was found", 22);
			return;
		}
	} else {
		coap_set_status_code(response, NOT_ACCEPTABLE_4_06);
		return;
	}
}

/**
 *  Installs a number of slotframes with the provided ids and amount of slots in the payload.
 *  
 *  Request to add new slotframes with a JSON payload of the form:
 *	[
 *		{
 *			"id":<id 1>,
 *			"slots":<num of slots in frame 1>
 *		},
 *		{
 *			"id":<id 2>,
 *			"slots":<num of slots in frame 2>
 *		},
		...
 *	]
 *  
 *  Response is an array of 0 and 1 indicating unsuccessful and successful creation of the slotframe.
 *  The location of the 0 and 1 in the array is the same as the location of their corresponding slotframes in the request.
 *  For example: if [0,1,...] is the response, the first slotframe in the request was not created whereas the second was created...
 *	
 *	A slotframe is not created if the provided id already exists or
 *	the schedule is not editable at that moment because it is being changed by another process.
 */
static void plexi_post_slotframe_handler(void* request, void* response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset)  {
	if(inbox_msg_lock != NO_LOCK && inbox_msg_lock != SLOTFRAME_POST_LOCK) {
		coap_set_status_code(response, SERVICE_UNAVAILABLE_5_03);
		coap_set_payload(response, "Server too busy. Retry later.", 29);
		return;
	}
	inbox_msg_lock = NO_LOCK;
	content_len = 0;
	int state;
	int request_content_len;
	int first_item = 1;
	const uint8_t *request_content;
  
	char *content_ptr;
	char field_buf[32] = "";
	int new_sf_count = 0; /* The number of newly added slotframes */
	int ns = 0; /* number of slots */
	int fd = 0;
	/* Add new slotframe */

	unsigned int accept = -1;
	REST.get_header_accept(request, &accept);
	if(accept == -1 || accept == REST.type.APPLICATION_JSON) {
	
		request_content_len = REST.get_request_payload(request, &request_content);
	
		struct jsonparse_state js;
		jsonparse_setup(&js, (const char *)request_content, request_content_len);
	
		/* Start creating response */
		CONTENT_PRINTF("[");
	
		/* Parse json input */
		while((state=jsonparse_find_field(&js, field_buf, sizeof(field_buf)))) {
			switch(state) {
				case '{': /* New element */
					ns = 0;
					fd = 0;
					break;
				case '}': { /* End of current element */
					struct tsch_slotframe *slotframe = (struct tsch_slotframe *)tsch_schedule_get_slotframe_by_handle(fd);
					if(!first_item) {
						CONTENT_PRINTF(",");
					}
					first_item = 0;
					if(slotframe || fd<0) {
						printf("PLEXI:! could not add slotframe %u with length %u\n", fd, ns);
						CONTENT_PRINTF("0");
					} else {
						if(tsch_schedule_add_slotframe(fd, ns)) {
							new_sf_count++;
							printf("PLEXI: added slotframe %u with length %u\n", fd, ns);
							CONTENT_PRINTF("1", fd);
						} else {
							CONTENT_PRINTF("0");
						}
					}
					break;
				}
				case JSON_TYPE_NUMBER: //Try to remove the if statement and change { to [ on line 601.
					if(!strncmp(field_buf, FRAME_ID_LABEL, sizeof(field_buf))) {
						fd = jsonparse_get_value_as_int(&js);
					} else if(!strncmp(field_buf, FRAME_SLOTS_LABEL, sizeof(field_buf))) {
						ns = jsonparse_get_value_as_int(&js);
					} 
					break;
			}
		}
		CONTENT_PRINTF("]");
		/* Check if json parsing succeeded */
		if(js.error == JSON_ERROR_OK) {
			REST.set_header_content_type(response, REST.type.APPLICATION_JSON);
			REST.set_response_payload(response, (uint8_t *)content, content_len);	 
		} else {
			coap_set_status_code(response, BAD_REQUEST_4_00);
			coap_set_payload(response, "Can only support JSON payload format", 36);
		}
	} else {
		coap_set_status_code(response, NOT_ACCEPTABLE_4_06);
		return;
	}
}

/** Deletes an existing slotframe.
 *	DELETE request is on the resource 6top/slotFrame with a query on specific slotframe ID. Hence,
 *	DELETE 6top/slotFrame?id=2
 *
 *	Note: more generic queries are not supported. e.g. To delete all slotFrames of size 101 slots
 *	(i.e. 6top/slotFrame?slots=101) is not yet supported. To achieve the same combine:
 *		1. GET 6top/slotFrame/id?slots=101 returns an array of ids: [x,y,z]
 *		2. Loop over results of 1 with DELETE 6top/slotFrame?id=w, where w={x,y,z}
 */
static void plexi_delete_slotframe_handler(void* request, void* response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset) {
	if(inbox_msg_lock != NO_LOCK && inbox_msg_lock != SLOTFRAME_DEL_LOCK) {
		coap_set_status_code(response, SERVICE_UNAVAILABLE_5_03);
		coap_set_payload(response, "Server too busy. Retry later.", 29);
		return;
	}
	inbox_msg_lock = NO_LOCK;
	content_len = 0;
	unsigned int accept = -1;
	REST.get_header_accept(request, &accept);

	if(accept == -1 || accept == REST.type.APPLICATION_JSON) {
		char *end;
		char *uri_path = NULL;
		int uri_len = REST.get_url(request, (const char**)(&uri_path));
		*(uri_path+uri_len) = '\0';
		int base_len = strlen(resource_6top_slotframe.url);
		if(uri_len > base_len + 1) {
			coap_set_status_code(response, NOT_IMPLEMENTED_5_01);
			coap_set_payload(response, "Subresources are not supported for DELETE method", 48);
			return;
		}
		const char *query = NULL;
		int query_len = REST.get_query(request, &query);
		char *query_value = NULL;
		int query_value_len = REST.get_query_variable(request, FRAME_ID_LABEL, (const char**)(&query_value));
		unsigned long value = -1;
		
		if((uri_len == base_len || uri_len == base_len+1) && query && query_value) {
			*(query_value+query_value_len) = '\0';
			int id = (unsigned)strtoul((const char*)query_value, &end, 10);
		
			struct tsch_slotframe *sf = (struct tsch_slotframe *)tsch_schedule_get_slotframe_by_handle(id);
			if(sf && tsch_schedule_remove_slotframe(sf)) {
				int slots = sf->size.val;
				printf("PLEXI: deleted slotframe {%s:%u, %s:%u}\n", FRAME_ID_LABEL, id, FRAME_SLOTS_LABEL, slots);
				CONTENT_PRINTF("{\"%s\":%u, \"%s\":%u}", FRAME_ID_LABEL, id, FRAME_SLOTS_LABEL, slots);
				REST.set_header_content_type(response, REST.type.APPLICATION_JSON);
				REST.set_response_payload(response, (uint8_t *)content, content_len);
			}
			coap_set_status_code(response, DELETED_2_02);
		} else if(!query) {
			// TODO: make sure it is idempotent
			struct tsch_slotframe* sf = NULL;
			short int first_item = 1;
			while((sf=(struct tsch_slotframe*)tsch_schedule_get_next_slotframe(NULL))) {
				if(first_item) {
					CONTENT_PRINTF("[");
					first_item = 0;
				} else {
					CONTENT_PRINTF(",");
				}
				int slots = sf->size.val;
				int id = sf->handle;
				if(sf && tsch_schedule_remove_slotframe(sf)) {
					printf("PLEXI: deleted slotframe {%s:%u, %s:%u}\n", FRAME_ID_LABEL, id, FRAME_SLOTS_LABEL, slots);
					CONTENT_PRINTF("{\"%s\":%u, \"%s\":%u}", FRAME_ID_LABEL, id, FRAME_SLOTS_LABEL, slots);
				}
			}
			if(!first_item)
				CONTENT_PRINTF("]");
			REST.set_header_content_type(response, REST.type.APPLICATION_JSON);
			REST.set_response_payload(response, (uint8_t *)content, content_len);
			coap_set_status_code(response, DELETED_2_02);
		} else if(query) {
			coap_set_status_code(response, NOT_IMPLEMENTED_5_01);
			coap_set_payload(response, "Supports only slot frame id as query", 29);
			return;
		}
	} else {
		coap_set_status_code(response, NOT_ACCEPTABLE_4_06);
		return;
	}
}


/**************************************************************************************************/
/** Resource and handler to GET, POST and DELETE links 											  */ 
/**************************************************************************************************/
PARENT_PERIODIC_RESOURCE(resource_6top_links,		/* name */
		"obs;title=\"6top links\"",				/* attributes */
		plexi_get_links_handler,			/* GET handler */
		plexi_post_links_handler,			/* POST handler */
		NULL,								/* PUT handler */
		plexi_delete_links_handler,			/* DELETE handler */
		PLEXI_LINK_UPDATE_INTERVAL,
		plexi_links_event_handler);

/** Gets (details about) a set of links as specified by the query.
 *  A request can specify to get a subresource of a link.
 *  GET /6top/cellList would provide an array of all links (all frames) of the requested node in the following format:
 *   [
 *     {
 *		link:<link id>,
 *		frame:<slotframe id>,
 *		slot:<timeslot offset>,
 *		channel:<channel offset>,
 *		option:<link options>,
 *		type:<link type>,
 *	},
 *     . . .
 *     {
 *		link:<link id>,
 *		frame:<slotframe id>,
 *		slot:<timeslot offset>,
 *		channel:<channel offset>,
 *		option:<link options>,
 *		type:<link type>,
 *	}
 *    ]
 *
 *  GET /6top/cellList/frame would provide an array of the slotframe IDs of all links of the requested node in the following format:
 *   [0,0,0,0,0,2,2,2,2,3,3,3,3,3,3,3,. . .,6,6,6]
 *   Instead of "frame" subresource any other subresource may be used e.g. /6top/cellist/slot
 *
 *  GET /6top/cellList?link=0 would provide the dictionary with the complete details of the cell with handle =0.
 *     {
 *		link:0,
 *		frame:<slotframe id>,
 *		slot:<timeslot offset>,
 *		channel:<channel offset>,
 *		option:<link options>,
 *		type:<link type>,
 *	}
 * 
 * GET /6top/cellList/slot?link=0 will give a scalar with the slot of the link with handle =0.
 *     [2]
 */
static void plexi_get_links_handler(void *request, void *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset) {
	if(inbox_msg_lock != NO_LOCK && inbox_msg_lock != LINK_GET_LOCK) {
		coap_set_status_code(response, SERVICE_UNAVAILABLE_5_03);
		coap_set_payload(response, "Server too busy. Retry later", 28);
		return;
	}
	inbox_msg_lock = NO_LOCK;
	content_len = 0;
	unsigned int accept = -1;
	REST.get_header_accept(request, &accept);
	
	if(accept == -1 || accept == REST.type.APPLICATION_JSON) {
		char *end;

		char *uri_path = NULL;
		const char *query = NULL;
		int uri_len = REST.get_url(request, (const char**)(&uri_path));
		int base_len = 0, query_len=0, query_id_len = 0, query_frame_len = 0, query_slot_len = 0, query_channel_len = 0;
		char *query_id = NULL, *query_frame = NULL, *query_slot = NULL, *query_channel = NULL, *uri_subresource = NULL;
		unsigned long id = -1, frame = -1, slot = -1, channel = -1;
		short int flag = 0;
		
		if(uri_len>0) {
			*(uri_path+uri_len) = '\0';
			base_len = strlen(resource_6top_links.url);

			/* Parse the query options and support only the id, the slotframe, the slotoffset and channeloffset queries */
			query_len = REST.get_query(request, &query);
			query_id_len = REST.get_query_variable(request, LINK_ID_LABEL, (const char**)(&query_id));
			query_frame_len = REST.get_query_variable(request, FRAME_ID_LABEL, (const char**)(&query_frame));
			query_slot_len = REST.get_query_variable(request, LINK_SLOT_LABEL, (const char**)(&query_slot));
			query_channel_len = REST.get_query_variable(request, LINK_CHANNEL_LABEL, (const char**)(&query_channel));
			if(query_id) {
				*(query_id+query_id_len) = '\0';
				id = (unsigned)strtoul(query_id, &end, 10);
				flag|=8;
			}
			if(query_frame) {
				*(query_frame+query_frame_len) = '\0';
				frame = (unsigned)strtoul(query_frame, &end, 10);
				flag|=4;
			}
			if(query_slot) {
				*(query_slot+query_slot_len) = '\0';
				slot = (unsigned)strtoul(query_slot, &end, 10);
				flag|=2;
			}
			if(query_channel) {
				*(query_channel+query_channel_len) = '\0';
				channel = (unsigned)strtoul(query_channel, &end, 10);
				flag|=1;
			}
			if(query_len > 0 && (!flag || (query_id && id < 0) || (query_frame && frame < 0) || (query_slot && slot < 0) || (query_channel && channel < 0))) {
				coap_set_status_code(response, NOT_IMPLEMENTED_5_01);
				coap_set_payload(response, "Supports queries only on slot frame id and/or slotoffset and channeloffset", 74);
				return;
			}

			/* Parse subresources and make sure you can filter the results */
			uri_subresource = uri_path+base_len;
			if(*uri_subresource == '/')
				uri_subresource++;
			if((uri_len > base_len + 1 && strcmp(LINK_ID_LABEL,uri_subresource) && strcmp(FRAME_ID_LABEL,uri_subresource) \
				 && strcmp(LINK_SLOT_LABEL,uri_subresource) && strcmp(LINK_CHANNEL_LABEL,uri_subresource) \
				 && strcmp(LINK_OPTION_LABEL,uri_subresource) && strcmp(LINK_TYPE_LABEL,uri_subresource) \
				  && strcmp(NEIGHBORS_TNA_LABEL,uri_subresource) && strcmp(LINK_STATS_LABEL,uri_subresource))) {
				coap_set_status_code(response, NOT_FOUND_4_04);
				coap_set_payload(response, "Invalid subresource", 19);
				return;
			}
		} else {
			base_len = (int)strlen(LINK_RESOURCE);
			uri_len = (int)(base_len+1+strlen(LINK_STATS_LABEL));
			uri_subresource = LINK_STATS_LABEL;
		}
		struct tsch_slotframe* slotframe = (struct tsch_slotframe*)tsch_schedule_get_next_slotframe(NULL);
		int first_item = 1;
		while(slotframe) {
			if(!(flag&4) || frame == slotframe->handle) {
				struct tsch_link* link = (struct tsch_link*)tsch_schedule_get_next_link_of(slotframe, NULL);
				while(link) {
					if((!(flag&2) || slot == link->timeslot) && (!(flag&1) || channel == link->channel_offset)) {
						if(!(flag&8) || id == link->handle) {
							if(first_item) {
								if(flag < 7 || uri_len > base_len + 1)
									CONTENT_PRINTF("[");
								first_item = 0;
							} else {
								CONTENT_PRINTF(",");
							}
							if(!strcmp(LINK_ID_LABEL,uri_subresource)) {
								CONTENT_PRINTF("%u",link->handle);
							} else if(!strcmp(FRAME_ID_LABEL,uri_subresource)) {
								CONTENT_PRINTF("%u",link->slotframe_handle);
							} else if(!strcmp(LINK_SLOT_LABEL,uri_subresource)) {
								CONTENT_PRINTF("%u",link->timeslot);
							} else if(!strcmp(LINK_CHANNEL_LABEL,uri_subresource)) {
								CONTENT_PRINTF("%u",link->channel_offset);
							} else if(!strcmp(LINK_OPTION_LABEL,uri_subresource)) {
								CONTENT_PRINTF("%u",link->link_options);
							} else if(!strcmp(LINK_TYPE_LABEL,uri_subresource)) {
								CONTENT_PRINTF("%u",link->link_type);
							} else if(!strcmp(NEIGHBORS_TNA_LABEL,uri_subresource)) {
								if(!linkaddr_cmp(&link->addr, &linkaddr_null)) {
									char na[32];
									linkaddr_to_na(na, &link->addr);
									CONTENT_PRINTF("\"%s\"",na);
								} else {
									coap_set_status_code(response, NOT_FOUND_4_04);
									coap_set_payload(response, "Link has no target node address.", 32);
									return;
								}
							} else if(!strcmp(LINK_STATS_LABEL,uri_subresource)) {
#if PLEXI_WITH_LINK_STATISTICS
								if(memb_inmemb(&plexi_stats_mem, link->data)) {
									plexi_stats *stats = (plexi_stats*)link->data;
									uint8_t first_stat = 1;
									while(stats!=NULL) {
										if(!first_stat) { CONTENT_PRINTF(",");
										} else { first_stat = 0; }
										CONTENT_PRINTF("{\"%s\":%u,\"%s\":", STATS_ID_LABEL, plexi_get_statistics_id(stats), STATS_VALUE_LABEL);
										if(plexi_get_statistics_metric(stats) == ASN) {
											CONTENT_PRINTF("\"%lx\"}",(int)stats->value);
										} else if(plexi_get_statistics_metric(stats) == RSSI) {
											int x = (plexi_stats_value_st)stats->value;
											CONTENT_PRINTF("%d}",x);
										} else {
											CONTENT_PRINTF("%u}",(unsigned)stats->value);
										}
										stats = stats->next;
									}
								} else {
#endif
									coap_set_status_code(response, NOT_FOUND_4_04);
									coap_set_payload(response, "No specified statistics was found", 33);
									return;
#if PLEXI_WITH_LINK_STATISTICS
								}
#endif
							} else {
								CONTENT_PRINTF("{\"%s\":%u,\"%s\":%u,\"%s\":%u,\"%s\":%u,\"%s\":%u,\"%s\":%u",\
									LINK_ID_LABEL, link->handle, FRAME_ID_LABEL, link->slotframe_handle, \
									LINK_SLOT_LABEL, link->timeslot, LINK_CHANNEL_LABEL, link->channel_offset,\
									LINK_OPTION_LABEL, link->link_options, LINK_TYPE_LABEL, link->link_type);
								if(!linkaddr_cmp(&link->addr, &linkaddr_null)) {
									char na[32];
									linkaddr_to_na(na, &link->addr);
									CONTENT_PRINTF(",\"%s\":\"%s\"",NEIGHBORS_TNA_LABEL,na);
								}
#if PLEXI_WITH_LINK_STATISTICS
								if(memb_inmemb(&plexi_stats_mem, link->data)) {
									CONTENT_PRINTF(",\"%s\":[",LINK_STATS_LABEL);
									plexi_stats *stats = (plexi_stats*)link->data;
									uint8_t first_stat = 1;
									while(stats!=NULL) {
										if(!first_stat) { CONTENT_PRINTF(",");
										} else { first_stat = 0; }
										CONTENT_PRINTF("{\"%s\":%u,\"%s\":", STATS_ID_LABEL, plexi_get_statistics_id(stats), STATS_VALUE_LABEL);
										if(plexi_get_statistics_metric(stats) == ASN) {
											CONTENT_PRINTF("\"%lx\"}",(int)stats->value);
										} else if(plexi_get_statistics_metric(stats) == RSSI) {
											int x = (plexi_stats_value_st)stats->value;
											CONTENT_PRINTF("%d}",x);
										} else {
											CONTENT_PRINTF("%u}",(unsigned)stats->value);
										} 
										stats = stats->next;
									}
									CONTENT_PRINTF("]");
								}
#endif
								CONTENT_PRINTF("}");
							}
						}
					}
					link = (struct tsch_link*)tsch_schedule_get_next_link_of(slotframe, link);
				}
			}
			slotframe = (struct tsch_slotframe*)tsch_schedule_get_next_slotframe(slotframe);
		}
		if(flag < 7 || uri_len > base_len + 1)
			CONTENT_PRINTF("]");
		if(!first_item) {
			REST.set_header_content_type(response, REST.type.APPLICATION_JSON);
			REST.set_response_payload(response, (uint8_t *)content, content_len);
		} else {
			coap_set_status_code(response, NOT_FOUND_4_04);
			coap_set_payload(response, "No specified statistics resource not found", 42);
			return;
		}
	} else {
		coap_set_status_code(response, NOT_ACCEPTABLE_4_06);
		return;
	}
}

/** Deletes an existing link.
 *	DELETE request is on the resource 6top/cellList with a query on specific link linkID. Hence,
 *	DELETE 6top/cellList?linkID=2
 *	The response to this request is the complete link object as:
 *	{
 *		cd:<link id>,
 *		fd:<slotframe id>,
 *		so:<timeslot offset>,
 *		co:<channel offset>,
 *		lo:<link options>,
 *		lt:<link type>,
 *	}
 *
 *	Note: more generic queries are not supported. e.g. To delete all slotFrames of size 101 slots
 *	(i.e. 6top/slotFrame?slots=101) is not yet supported. To achieve the same combine:
 *		1. GET 6top/slotFrame/id?slots=101 returns an array of ids: [x,y,z]
 *		2. Loop over results of 1 with DELETE 6top/slotFrame?id=w, where w={x,y,z}
 */
static void plexi_delete_links_handler(void *request, void *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset) {
	if(inbox_msg_lock != NO_LOCK && inbox_msg_lock != LINK_DEL_LOCK) {
		coap_set_status_code(response, SERVICE_UNAVAILABLE_5_03);
		coap_set_payload(response, "Server too busy. Retry later", 28);
		return;
	}
	inbox_msg_lock = NO_LOCK;
	content_len = 0;
	unsigned int accept = -1;
	REST.get_header_accept(request, &accept);

	if(accept == -1 || accept == REST.type.APPLICATION_JSON) {
		char *end;

		char *uri_path = NULL;
		const char *query = NULL;
		int uri_len = REST.get_url(request, (const char**)(&uri_path));
		*(uri_path+uri_len) = '\0';
		int base_len = strlen(resource_6top_links.url);

		/* Parse the query options and support only the slotframe, the slotoffset and channeloffset queries */
		int query_len = REST.get_query(request, &query);
		char *query_frame = NULL, *query_slot = NULL, *query_channel = NULL;
		unsigned long frame = -1, slot = -1, channel = -1;
		uint8_t flags = 0;
		int query_frame_len = REST.get_query_variable(request, FRAME_ID_LABEL, (const char**)(&query_frame));
		int query_slot_len = REST.get_query_variable(request, LINK_SLOT_LABEL, (const char**)(&query_slot));
		int query_channel_len = REST.get_query_variable(request, LINK_CHANNEL_LABEL, (const char**)(&query_channel));
		if(query_frame) {
			*(query_frame+query_frame_len) = '\0';
			frame = (unsigned)strtoul(query_frame, &end, 10);
			flags|=4;
		}
		if(query_slot) {
			*(query_slot+query_slot_len) = '\0';
			slot = (unsigned)strtoul(query_slot, &end, 10);
			flags|=2;
		}
		if(query_channel) {
			*(query_channel+query_channel_len) = '\0';
			channel = (unsigned)strtoul(query_channel, &end, 10);
			flags|=1;
		}
		if(query_len > 0 && (!flags || (query_frame && frame < 0) || (query_slot && slot < 0) || (query_channel && channel < 0))) {
			coap_set_status_code(response, NOT_IMPLEMENTED_5_01);
			coap_set_payload(response, "Supports queries only on slot frame id and/or slotoffset and channeloffset", 74);
			return;
		}

		/* Parse subresources and make sure you can filter the results */
		if(uri_len > base_len + 1) {
			coap_set_status_code(response, NOT_IMPLEMENTED_5_01);
			coap_set_payload(response, "Subresources are not supported for DELETE method", 48);
			return;
		}

		struct tsch_slotframe* slotframe = (struct tsch_slotframe*)tsch_schedule_get_next_slotframe(NULL);
		int first_item = 1;
		while(slotframe) {
			if(!(flags&4) || frame == slotframe->handle) {
				struct tsch_link* link = (struct tsch_link*)tsch_schedule_get_next_link_of(slotframe, NULL);
				while(link) {
					struct tsch_link* next_link = (struct tsch_link*)tsch_schedule_get_next_link_of(slotframe, link);
					int link_handle = link->handle;
					int link_slotframe_handle = link->slotframe_handle;
					int link_timeslot = link->timeslot;
					int link_channel_offset = link->channel_offset;
					int link_link_options = link->link_options;
					int link_link_type = link->link_type;
					char na[32];
					*na = '\0';
					if(!linkaddr_cmp(&link->addr, &linkaddr_null)) linkaddr_to_na(na, &link->addr);
					if((!(flags&2) || link_timeslot==slot) && (!(flags&1) || link_channel_offset==channel)) {
						int deleted = tsch_schedule_remove_link(slotframe,link);
						if(((!(flags&2) && !(flags&1)) || ((flags&2) && !(flags&1) && slot == link_timeslot) || (!(flags&2) && (flags&1) && channel == link_channel_offset) || ((flags&2) && (flags&1) && slot == link_timeslot && channel == link_channel_offset)) && deleted) {
							if(first_item) {
								if(flags < 7)
									CONTENT_PRINTF("[");
								first_item = 0;
							} else {
								CONTENT_PRINTF(",");
							}
							printf("PLEXI: deleted link {\"%s\":%u,\"%s\":%u,\"%s\":%u,\"%s\":%u,\"%s\":%u,\"%s\":%u}", \
								LINK_ID_LABEL, link_handle, FRAME_ID_LABEL, link_slotframe_handle, LINK_SLOT_LABEL, link_timeslot, \
								LINK_CHANNEL_LABEL, link_channel_offset, LINK_OPTION_LABEL, link_link_options, LINK_TYPE_LABEL, link_link_type);
							CONTENT_PRINTF("{\"%s\":%u,\"%s\":%u,\"%s\":%u,\"%s\":%u,\"%s\":%u,\"%s\":%u",\
								LINK_ID_LABEL, link_handle, FRAME_ID_LABEL, link_slotframe_handle, \
								LINK_SLOT_LABEL, link_timeslot, LINK_CHANNEL_LABEL, link_channel_offset,\
								LINK_OPTION_LABEL, link_link_options, LINK_TYPE_LABEL, link_link_type);
							if(strlen(na) > 0) CONTENT_PRINTF(",\"%s\":\"%s\"",NEIGHBORS_TNA_LABEL,na);
							CONTENT_PRINTF("}");
						}
					}
					link = next_link;
				}
			}
			slotframe = (struct tsch_slotframe*)tsch_schedule_get_next_slotframe(slotframe);
		}
		if(flags < 7)
			CONTENT_PRINTF("]");
		REST.set_header_content_type(response, REST.type.APPLICATION_JSON);
		if(flags != 7 || !first_item)
			REST.set_response_payload(response, (uint8_t *)content, content_len);
		coap_set_status_code(response, DELETED_2_02);
	} else {
		coap_set_status_code(response, NOT_ACCEPTABLE_4_06);
		return;
	}
}

static void plexi_post_links_handler(void* request, void* response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset) {
	if(inbox_msg_lock == NO_LOCK) {
		inbox_msg_len = 0;
		*inbox_msg='\0';
	} else if(inbox_msg_lock != LINK_POST_LOCK) {
		coap_set_status_code(response, SERVICE_UNAVAILABLE_5_03);
		coap_set_payload(response, "Server too busy. Retry later.", 29);
		return;
	}
	content_len = 0;
	int first_item = 1;	
	unsigned int accept = -1;
	REST.get_header_accept(request, &accept);
	
	if(accept == -1 || accept == REST.type.APPLICATION_JSON) {
		const uint8_t *request_content;
		int request_content_len;
		request_content_len = coap_get_payload(request, &request_content);
		if(inbox_msg_len+request_content_len>MAX_DATA_LEN) {
			coap_set_status_code(response, NOT_IMPLEMENTED_5_01);
			coap_set_payload(response, "Server reached internal buffer limit. Shorten payload.", 54);
			return;
		}
		int x = coap_block1_handler(request, response, inbox_msg, &inbox_msg_len, MAX_DATA_LEN);
		if(inbox_msg_len<MAX_DATA_LEN) {
			*(inbox_msg+inbox_msg_len)='\0';
		}
		if(x==1) {
			inbox_msg_lock = LINK_POST_LOCK;
			return;
		} else if(x==-1) {
			inbox_msg_lock = NO_LOCK;
			return;
		}
		// TODO: It is assumed that the node processes the post request fast enough to return the
		//       response within the window assumed by client before retransmitting
		inbox_msg_lock = NO_LOCK;
		int i=0;
		for(i=0; i<inbox_msg_len; i++) {
			if(inbox_msg[i]=='[') {
				coap_set_status_code(response, BAD_REQUEST_4_00);
				coap_set_payload(response, "Array of links is not supported yet. POST each link separately.", 63);
				return;
			}
		}
		int state;

		int so = 0; 	//* slot offset *
		int co = 0; 	//* channel offset *
		int fd = 0; 	//* slotframeID (handle) *
		int lo = 0; 	//* link options *
		int lt = 0; 	//* link type *
		linkaddr_t na; 	//* node address *

		char field_buf[32] = "";
		char value_buf[32] = "";
		struct jsonparse_state js;
		jsonparse_setup(&js, (const char *)inbox_msg, inbox_msg_len);
		while((state=jsonparse_find_field(&js, field_buf, sizeof(field_buf)))) {
			switch(state) {
				case '{': //* New element *
					so = co = fd = lo = lt = 0;
					linkaddr_copy(&na, &linkaddr_null);
					break;
				case '}': { //* End of current element *
					struct tsch_slotframe *slotframe;
					struct tsch_link *link;
					slotframe = (struct tsch_slotframe*)tsch_schedule_get_slotframe_by_handle((uint16_t)fd);
					if(slotframe) {
						new_tx_timeslot = so;
						new_tx_slotframe = fd;
						if((link = (struct tsch_link *)tsch_schedule_add_link(slotframe, (uint8_t)lo, lt, &na, (uint16_t)so, (uint16_t)co))) {
							char buf[32];
							linkaddr_to_na(buf, &na);
							printf("PLEXI: added {\"%s\":%u,\"%s\":%u,\"%s\":%u,\"%s\":%u,\"%s\":%u,\"%s\":%u", \
								LINK_ID_LABEL, link->handle, FRAME_ID_LABEL, fd, LINK_SLOT_LABEL, so, \
								LINK_CHANNEL_LABEL, co, LINK_OPTION_LABEL, lo, LINK_TYPE_LABEL, lt);
							if(!linkaddr_cmp(&na, &linkaddr_null)) {
								char buf[32];
								linkaddr_to_na(buf, &na);
								printf(",\"%s\":\"%s\"",NEIGHBORS_TNA_LABEL,buf);
							}
							printf("}\n");
							//* Update response *
							if(!first_item) {
								CONTENT_PRINTF(",");
							} else {
								CONTENT_PRINTF("[");
							}
							first_item = 0;
							CONTENT_PRINTF("%u", link->handle);
						} else {
							coap_set_status_code(response, INTERNAL_SERVER_ERROR_5_00);
							coap_set_payload(response, "Link could not be added", 23);
							return;
						}
					} else {
						coap_set_status_code(response, NOT_FOUND_4_04);
						coap_set_payload(response, "Slotframe handle not found", 26);
						return;
					}
					break;
				}
				case JSON_TYPE_NUMBER:
					if(!strncmp(field_buf, LINK_SLOT_LABEL, sizeof(field_buf))) {
						so = jsonparse_get_value_as_int(&js);
					} else if(!strncmp(field_buf, LINK_CHANNEL_LABEL, sizeof(field_buf))) {
						co = jsonparse_get_value_as_int(&js);
					} else if(!strncmp(field_buf, FRAME_ID_LABEL, sizeof(field_buf))) {
						fd = jsonparse_get_value_as_int(&js);
					} else if(!strncmp(field_buf, LINK_OPTION_LABEL, sizeof(field_buf))) {
						lo = jsonparse_get_value_as_int(&js);
					} else if(!strncmp(field_buf, LINK_TYPE_LABEL, sizeof(field_buf))) {
						lt = jsonparse_get_value_as_int(&js);
					}
					break;
				case JSON_TYPE_STRING:
					if(!strncmp(field_buf, NEIGHBORS_TNA_LABEL, sizeof(field_buf))) {
						jsonparse_copy_value(&js, value_buf, sizeof(value_buf));
						int x = na_to_linkaddr(value_buf, sizeof(value_buf), &na);
						if(!x) {
							coap_set_status_code(response, BAD_REQUEST_4_00);
							coap_set_payload(response, "Invalid target node address", 27);
							return;
						}
					}
					break;
			}
		}
		CONTENT_PRINTF("]");
		
		REST.set_header_content_type(response, REST.type.APPLICATION_JSON);
		REST.set_response_payload(response, (uint8_t *)content, content_len);
	}
}

/* Notifies all clients who observe changes to the 6top/nbrs resource */
static void plexi_links_event_handler() {
	REST.notify_subscribers(&resource_6top_links);
}


#if PLEXI_WITH_LINK_STATISTICS
/**************************************************************************************************/
/** Resource and handler to GET, POST and DELETE statistics										  */ 
/**************************************************************************************************/
PARENT_RESOURCE(resource_6top_stats,								/* name */
               "title=\"6top Statistics\"",					/* attributes */
               plexi_get_stats_handler,							/* GET handler */
               plexi_post_stats_handler,						/* POST handler */
               NULL,											/* PUT handler */
               plexi_delete_stats_handler);						/* DELETE handler */


static void plexi_get_stats_handler(void *request, void *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset) {
	if(inbox_msg_lock != NO_LOCK && inbox_msg_lock != STATS_GET_LOCK) {
		coap_set_status_code(response, SERVICE_UNAVAILABLE_5_03);
		coap_set_payload(response, "Server too busy. Retry later", 28);
		return;
	}
	inbox_msg_lock = NO_LOCK;
	content_len = 0;
	unsigned int accept = -1;
	REST.get_header_accept(request, &accept);
	
	if(accept == -1 || accept == REST.type.APPLICATION_JSON) {
		char *end;
		char *uri_path = NULL;
		const char *query = NULL;
		int uri_len = REST.get_url(request, (const char**)(&uri_path));
		*(uri_path+uri_len) = '\0';
		int base_len = strlen(resource_6top_stats.url);
		uint8_t flags = 0;

		/* Parse the query options and support only the slotframe, the slotoffset and channeloffset queries */
		int query_len = REST.get_query(request, &query);
		char *query_id = NULL, \
			*query_frame = NULL, \
			*query_slot = NULL, \
			*query_channel = NULL, \
			*query_tna = NULL, \
			*query_metric = NULL, \
			*query_enable = NULL;
		int frame = -1, metric = NONE, enable = ENABLE;
		int id = -1, slot = -1, channel = -1;
		linkaddr_t tna;
		int query_id_len = REST.get_query_variable(request, STATS_ID_LABEL, (const char**)(&query_id));
		int query_frame_len = REST.get_query_variable(request, FRAME_ID_LABEL, (const char**)(&query_frame));
		int query_slot_len = REST.get_query_variable(request, LINK_SLOT_LABEL, (const char**)(&query_slot));
		int query_channel_len = REST.get_query_variable(request, LINK_CHANNEL_LABEL, (const char**)(&query_channel));
		int query_tna_len = REST.get_query_variable(request, NEIGHBORS_TNA_LABEL, (const char**)(&query_tna));
		int query_metric_len = REST.get_query_variable(request, STATS_METRIC_LABEL, (const char**)(&query_metric));
		int query_enable_len = REST.get_query_variable(request, STATS_ENABLE_LABEL, (const char**)(&query_enable));
		if(query_frame) {
			*(query_frame+query_frame_len) = '\0';
			frame = (unsigned)strtoul(query_frame, &end, 10);
			flags|=1;
		}
		if(query_slot) {
			*(query_slot+query_slot_len) = '\0';
			slot = (unsigned)strtoul(query_slot, &end, 10);
			flags|=2;
		}
		if(query_channel) {
			*(query_channel+query_channel_len) = '\0';
			channel = (unsigned)strtoul(query_channel, &end, 10);
			flags|=4;
		}
		if(query_tna) {
			*(query_tna+query_tna_len) = '\0';
			na_to_linkaddr(query_tna, query_tna_len, &tna);
			flags|=8;
		}
		if(query_metric) {
			*(query_metric+query_metric_len) = '\0';
			if(!strcmp(STATS_ETX_LABEL, query_metric)) {
				metric = ETX;
			} else if(!strcmp(STATS_RSSI_LABEL, query_metric)) {
				metric = RSSI;
			} else if(!strcmp(STATS_LQI_LABEL, query_metric)) {
				metric = LQI;
			} else if(!strcmp(STATS_PDR_LABEL, query_metric)) {
				metric = PDR;
			} else if(!strcmp(NEIGHBORS_ASN_LABEL, query_metric)) {
				metric = ASN;
			} else {
				coap_set_status_code(response, NOT_FOUND_4_04);
				coap_set_payload(response, "Unrecognized metric", 19);
				return;
			}
			flags|=16;
		}
		if(query_enable) {
			*(query_enable+query_enable_len) = '\0';
			if(!strcmp("y", query_enable) || !strcmp("yes", query_enable) || !strcmp("true", query_enable) || !strcmp("1", query_enable))
				enable = ENABLE;
			else if(!strcmp("n", query_enable) || !strcmp("no", query_enable) || !strcmp("false", query_enable) || !strcmp("0", query_enable))
				enable = DISABLE;
			flags|=32;
		}
		if(query_id) {
			*(query_id+query_id_len) = '\0';
			id = (unsigned)strtoul(query_id, &end, 10);
			flags|=64;
		}
		if(query_len > 0 && (!flags || (query_frame && frame < 0) || (query_slot && slot < 0) || (query_channel && channel < 0))) {
			coap_set_status_code(response, NOT_IMPLEMENTED_5_01);
			coap_set_payload(response, "Supports queries only on slot frame id and/or slotoffset and channeloffset", 74);
			return;
		}

		/* Parse subresources and make sure you can filter the results */
		char *uri_subresource = uri_path+base_len;
		if(*uri_subresource == '/')
			uri_subresource++;
		if((uri_len > base_len + 1 && strcmp(FRAME_ID_LABEL,uri_subresource) && strcmp(LINK_SLOT_LABEL,uri_subresource) \
			  && strcmp(LINK_CHANNEL_LABEL,uri_subresource) && strcmp(STATS_WINDOW_LABEL,uri_subresource) \
			  && strcmp(STATS_METRIC_LABEL,uri_subresource) && strcmp(STATS_VALUE_LABEL,uri_subresource) \
			  && strcmp(NEIGHBORS_TNA_LABEL,uri_subresource) && strcmp(STATS_ENABLE_LABEL,uri_subresource) \
			  && strcmp(STATS_ID_LABEL,uri_subresource) )) {
			coap_set_status_code(response, NOT_FOUND_4_04);
			coap_set_payload(response, "Invalid subresource", 19);
			return;
		}
		struct tsch_slotframe *slotframe_ptr = NULL;
		if(flags&1){
			slotframe_ptr = (struct tsch_slotframe*)tsch_schedule_get_slotframe_by_handle(frame);
		} else {
			slotframe_ptr = (struct tsch_slotframe*)tsch_schedule_get_next_slotframe(NULL);
		}
		if(!slotframe_ptr) {
			coap_set_status_code(response, NOT_FOUND_4_04);
			coap_set_payload(response, "No slotframes found", 19);
			return;
		}
		int first_item = 1;
		do {
			struct tsch_link* link = NULL;
			if(flags&2) {
				link = (struct tsch_link*)tsch_schedule_get_link_by_timeslot(slotframe_ptr, slot);
			} else {
				link = (struct tsch_link*)tsch_schedule_get_next_link_of(slotframe_ptr, NULL);
			}
			if(!link) continue;
			do {
				if((!(flags&4) || link->channel_offset == channel) && (!(flags&8) || linkaddr_cmp(&link->addr, &tna))) {
					plexi_stats *last_stats = NULL;
					if(memb_inmemb(&plexi_stats_mem, link->data)) {
						last_stats = (plexi_stats*)link->data;
						while(last_stats != NULL) {
							if( (!(flags&16) || metric == plexi_get_statistics_metric(last_stats)) && \
								  (!(flags&32) || enable == plexi_get_statistics_enable(last_stats)) && \
								  (!(flags&64) || id == plexi_get_statistics_id(last_stats)) ) {
								if(first_item) {
									if(!(flags&64)) { CONTENT_PRINTF("["); }
									first_item = 0;
								} else {
									CONTENT_PRINTF(",");
								}
								if(!strcmp(FRAME_ID_LABEL,uri_subresource)) {
									CONTENT_PRINTF("%u",link->slotframe_handle);
								} else if(!strcmp(LINK_SLOT_LABEL,uri_subresource)) {
									CONTENT_PRINTF("%u",link->timeslot);
								} else if(!strcmp(LINK_CHANNEL_LABEL,uri_subresource)) {
									CONTENT_PRINTF("%u",link->channel_offset);
								} else if(!strcmp(STATS_METRIC_LABEL,uri_subresource)) {
									if(plexi_get_statistics_metric(last_stats) == ETX) {
										CONTENT_PRINTF("%s",STATS_ETX_LABEL);
									} else if(plexi_get_statistics_metric(last_stats) == RSSI) {
										CONTENT_PRINTF("%s",STATS_RSSI_LABEL);
									} else if(plexi_get_statistics_metric(last_stats) == LQI) {
										CONTENT_PRINTF("%s",STATS_LQI_LABEL);
									} else if(plexi_get_statistics_metric(last_stats) == PDR) {
										CONTENT_PRINTF("%s",STATS_PDR_LABEL);
									} else if(plexi_get_statistics_metric(last_stats) == ASN) {
										CONTENT_PRINTF("%s",NEIGHBORS_ASN_LABEL);
									}
								} else if(!strcmp(STATS_ENABLE_LABEL,uri_subresource)) {
									if(plexi_get_statistics_enable(last_stats) == ENABLE) {
										CONTENT_PRINTF("1");
									} else if(plexi_get_statistics_enable(last_stats) == DISABLE) {
										CONTENT_PRINTF("0");
									}
								} else if(!strcmp(NEIGHBORS_TNA_LABEL,uri_subresource)) {
									if(!linkaddr_cmp(&link->addr, &linkaddr_null)) {
										char na[32];
										linkaddr_to_na(na, &link->addr);
										CONTENT_PRINTF("\"%s\"",na);
									}
								} else if(!strcmp(STATS_ID_LABEL,uri_subresource)) {
									CONTENT_PRINTF("%u",plexi_get_statistics_id(last_stats));
								} else {
									CONTENT_PRINTF("{\"%s\":%u,\"%s\":%u,\"%s\":%u,\"%s\":%u",\
										STATS_ID_LABEL, plexi_get_statistics_id(last_stats), FRAME_ID_LABEL, link->slotframe_handle, \
										LINK_SLOT_LABEL, link->timeslot, LINK_CHANNEL_LABEL, link->channel_offset);
									if(plexi_get_statistics_metric(last_stats) == ETX) {
										CONTENT_PRINTF(",\"%s\":\"%s\"", STATS_METRIC_LABEL, STATS_ETX_LABEL);
									} else if(plexi_get_statistics_metric(last_stats) == RSSI) {
										CONTENT_PRINTF(",\"%s\":\"%s\"", STATS_METRIC_LABEL, STATS_RSSI_LABEL);
									} else if(plexi_get_statistics_metric(last_stats) == LQI) {
										CONTENT_PRINTF(",\"%s\":\"%s\"", STATS_METRIC_LABEL, STATS_LQI_LABEL);
									} else if(plexi_get_statistics_metric(last_stats) == PDR) {
										CONTENT_PRINTF(",\"%s\":\"%s\"", STATS_METRIC_LABEL, STATS_PDR_LABEL);
									} else if(plexi_get_statistics_metric(last_stats) == ASN) {
										CONTENT_PRINTF(",\"%s\":\"%s\"", STATS_METRIC_LABEL, NEIGHBORS_ASN_LABEL);
									}
									if(plexi_get_statistics_enable(last_stats) == ENABLE) {
										CONTENT_PRINTF(",\"%s\":1",STATS_ENABLE_LABEL);
									} else if(plexi_get_statistics_enable(last_stats) == DISABLE) {
										CONTENT_PRINTF(",\"%s\":0",STATS_ENABLE_LABEL);
									}
									if(!linkaddr_cmp(&link->addr, &linkaddr_null)) {
										char na[32];
										linkaddr_to_na(na, &link->addr);
										CONTENT_PRINTF(",\"%s\":\"%s\"",NEIGHBORS_TNA_LABEL,na);
									}
									CONTENT_PRINTF("}");
								}
							}
							last_stats = last_stats->next;
						}
					}
				}
			} while(!(flags&2) && (link = (struct tsch_link*)tsch_schedule_get_next_link_of(slotframe_ptr, link)));
		} while(!(flags&1) && (slotframe_ptr = (struct tsch_slotframe*)tsch_schedule_get_next_slotframe(slotframe_ptr)));
		
		if(!first_item) {
			if(!(flags&64)) CONTENT_PRINTF("]");
			REST.set_header_content_type(response, REST.type.APPLICATION_JSON);
			REST.set_response_payload(response, (uint8_t *)content, content_len);
		} else {
			coap_set_status_code(response, NOT_FOUND_4_04);
			coap_set_payload(response, "No specified statistics resource found", 38);
		}
	} else {
		coap_set_status_code(response, NOT_ACCEPTABLE_4_06);
	}
}

static void plexi_delete_stats_handler(void *request, void *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset) {
	if(inbox_msg_lock != NO_LOCK && inbox_msg_lock != STATS_DEL_LOCK) {
		coap_set_status_code(response, SERVICE_UNAVAILABLE_5_03);
		coap_set_payload(response, "Server too busy. Retry later", 28);
		return;
	}
	inbox_msg_lock = NO_LOCK;
	content_len = 0;
	unsigned int accept = -1;
	REST.get_header_accept(request, &accept);
	
	if(accept == -1 || accept == REST.type.APPLICATION_JSON) {
		char *end;

		char *uri_path = NULL;
		const char *query = NULL;
		int uri_len = REST.get_url(request, (const char**)(&uri_path));
		*(uri_path+uri_len) = '\0';
		int base_len = strlen(resource_6top_stats.url);
		uint8_t flags = 0;

		/* Parse the query options and support only the slotframe, the slotoffset and channeloffset queries */
		int query_len = REST.get_query(request, &query);
		char *query_id = NULL, \
			*query_frame = NULL, \
			*query_slot = NULL, \
			*query_channel = NULL, \
			*query_tna = NULL, \
			*query_metric = NULL, \
			*query_enable = NULL;
		int frame = -1, metric = NONE, enable = ENABLE;
		int id = -1, slot = -1, channel = -1;
		linkaddr_t tna;
		int query_id_len = REST.get_query_variable(request, STATS_ID_LABEL, (const char**)(&query_id));
		int query_frame_len = REST.get_query_variable(request, FRAME_ID_LABEL, (const char**)(&query_frame));
		int query_slot_len = REST.get_query_variable(request, LINK_SLOT_LABEL, (const char**)(&query_slot));
		int query_channel_len = REST.get_query_variable(request, LINK_CHANNEL_LABEL, (const char**)(&query_channel));
		int query_tna_len = REST.get_query_variable(request, NEIGHBORS_TNA_LABEL, (const char**)(&query_tna));
		int query_metric_len = REST.get_query_variable(request, STATS_METRIC_LABEL, (const char**)(&query_metric));
		int query_enable_len = REST.get_query_variable(request, STATS_ENABLE_LABEL, (const char**)(&query_enable));
		if(query_frame) {
			*(query_frame+query_frame_len) = '\0';
			frame = (unsigned)strtoul(query_frame, &end, 10);
			flags|=1;
		}
		if(query_slot) {
			*(query_slot+query_slot_len) = '\0';
			slot = (unsigned)strtoul(query_slot, &end, 10);
			flags|=2;
		}
		if(query_channel) {
			*(query_channel+query_channel_len) = '\0';
			channel = (unsigned)strtoul(query_channel, &end, 10);
			flags|=4;
		}
		if(query_tna) {
			*(query_tna+query_tna_len) = '\0';
			na_to_linkaddr(query_tna, query_tna_len, &tna);
			flags|=8;
		}
		if(query_metric) {
			*(query_metric+query_metric_len) = '\0';
			if(!strcmp(STATS_ETX_LABEL, query_metric)) {
				metric = ETX;
			} else if(!strcmp(STATS_RSSI_LABEL, query_metric)) {
				metric = RSSI;
			} else if(!strcmp(STATS_LQI_LABEL, query_metric)) {
				metric = LQI;
			} else if(!strcmp(STATS_PDR_LABEL, query_metric)) {
				metric = PDR;
			} else if(!strcmp(NEIGHBORS_ASN_LABEL, query_metric)) {
				metric = ASN;
			} else {
				coap_set_status_code(response, NOT_FOUND_4_04);
				coap_set_payload(response, "Unrecognized metric", 19);
				return;
			}
			flags|=16;
		}
		if(query_enable) {
			*(query_enable+query_enable_len) = '\0';
			if(!strcmp("y", query_enable) || !strcmp("yes", query_enable) || !strcmp("true", query_enable) || !strcmp("1", query_enable))
				enable = ENABLE;
			else if(!strcmp("n", query_enable) || !strcmp("no", query_enable) || !strcmp("false", query_enable) || !strcmp("0", query_enable))
				enable = DISABLE;
			flags|=32;
		}
		if(query_id) {
			*(query_id+query_id_len) = '\0';
			id = (unsigned)strtoul(query_id, &end, 10);
			flags|=64;
		}
		if(query_len > 0 && (!flags || (query_frame && frame < 0) || (query_slot && slot < 0) || (query_channel && channel < 0))) {
			coap_set_status_code(response, NOT_IMPLEMENTED_5_01);
			coap_set_payload(response, "Supports queries only on slot frame id and/or slotoffset and channeloffset", 74);
			return;
		}

		/* Parse subresources and make sure you can filter the results */
		char *uri_subresource = uri_path+base_len;
		if(*uri_subresource == '/')
			uri_subresource++;
		if(uri_len > base_len + 1) {
			coap_set_status_code(response, NOT_FOUND_4_04);
			coap_set_payload(response, "Subresources are not allowed", 28);
			return;
		}
		struct tsch_slotframe *slotframe_ptr = NULL;
		if(flags&1){
			slotframe_ptr = (struct tsch_slotframe*)tsch_schedule_get_slotframe_by_handle(frame);
		} else {
			slotframe_ptr = (struct tsch_slotframe*)tsch_schedule_get_next_slotframe(NULL);
		}
		if(!slotframe_ptr) {
			coap_set_status_code(response, NOT_FOUND_4_04);
			coap_set_payload(response, "No slotframes found", 19);
			return;
		}
		int first_item = 1;
		do {
			struct tsch_link* link = NULL;
			if(flags&2) {
				link = (struct tsch_link*)tsch_schedule_get_link_by_timeslot(slotframe_ptr, slot);
			} else {
				link = (struct tsch_link*)tsch_schedule_get_next_link_of(slotframe_ptr, NULL);
			}
			if(!link) continue;
			do {
				if((!(flags&4) || link->channel_offset == channel) && (!(flags&8) || linkaddr_cmp(&link->addr, &tna))) {
					plexi_stats *last_stats = NULL;
					if(memb_inmemb(&plexi_stats_mem, link->data)) {
						plexi_stats *previous_stats = NULL;
						last_stats = (plexi_stats*)link->data;
						while(last_stats != NULL) {
							uint8_t to_print = 0;
							uint8_t local_metric, local_enable;
							uint16_t local_id, local_window;
							plexi_stats_value_t local_value;
							if( (!(flags&16) || metric == plexi_get_statistics_metric(last_stats)) && \
								  (!(flags&32) || enable == plexi_get_statistics_enable(last_stats)) && \
								  (!(flags&64) || id == plexi_get_statistics_id(last_stats)) ) {
								local_metric = plexi_get_statistics_metric(last_stats);
								local_enable = plexi_get_statistics_enable(last_stats);
								local_id = plexi_get_statistics_id(last_stats);
								local_window = plexi_get_statistics_window(last_stats);
								if( !(flags&8) || linkaddr_cmp(&tna, &link->addr)) {
									to_print = 1;
									local_value = last_stats->value;
									plexi_stats *to_be_deleted = last_stats;
									if(last_stats == link->data) {
										link->data = last_stats->next;
										last_stats = (plexi_stats *)link->data;
									} else {
										previous_stats->next = last_stats->next;
										last_stats = last_stats->next;
									}
									plexi_purge_statistics(to_be_deleted);
								} else {
									if(flags&8) {
										to_print = 1;
										plexi_enhanced_stats *es = NULL;
										for(es = (plexi_enhanced_stats *)list_head(last_stats->enhancement); \
											  es!=NULL; es = list_item_next(es) ) {
											if(linkaddr_cmp(&tna, &es->target)) {
												local_value = es->value;
												list_remove(last_stats->enhancement, es);
												plexi_purge_enhanced_statistics(es);
												break;
											}
										}
									}
									previous_stats = last_stats;
									last_stats = last_stats->next;
								}
							} else {
								previous_stats = last_stats;
								last_stats = last_stats->next;
							}
							if(to_print) {
								if(first_item) {
									CONTENT_PRINTF("[");
									first_item = 0;
								} else {
									CONTENT_PRINTF(",");
								}
								CONTENT_PRINTF("{\"%s\":%u,\"%s\":%u,\"%s\":%u,\"%s\":%u",\
									STATS_ID_LABEL, local_id, FRAME_ID_LABEL, link->slotframe_handle, \
									LINK_SLOT_LABEL, link->timeslot, LINK_CHANNEL_LABEL, link->channel_offset);
								if(local_metric == ETX) {
									CONTENT_PRINTF(",\"%s\":\"%s\"", STATS_METRIC_LABEL, STATS_ETX_LABEL);
								} else if(local_metric == RSSI) {
									CONTENT_PRINTF(",\"%s\":\"%s\"", STATS_METRIC_LABEL, STATS_RSSI_LABEL);
								} else if(local_metric == LQI) {
									CONTENT_PRINTF(",\"%s\":\"%s\"", STATS_METRIC_LABEL, STATS_LQI_LABEL);
								} else if(local_metric == PDR) {
									CONTENT_PRINTF(",\"%s\":\"%s\"", STATS_METRIC_LABEL, STATS_PDR_LABEL);
								} else if(local_metric == ASN) {
									CONTENT_PRINTF(",\"%s\":\"%s\"", STATS_METRIC_LABEL, NEIGHBORS_ASN_LABEL);
								}
								if(local_enable == ENABLE) {
									CONTENT_PRINTF(",\"%s\":1",STATS_ENABLE_LABEL);
								} else if(local_enable == DISABLE) {
									CONTENT_PRINTF(",\"%s\":0",STATS_ENABLE_LABEL);
								}
								if(linkaddr_cmp(&tna, &linkaddr_null)) {
									char na[32];
									linkaddr_to_na(na, &link->addr);
									CONTENT_PRINTF(",\"%s\":\"%s\"",NEIGHBORS_TNA_LABEL,na);
								}
								CONTENT_PRINTF("}");
							}
						}
					}
				}
			} while(!(flags&2) && (link = (struct tsch_link*)tsch_schedule_get_next_link_of(slotframe_ptr, link)));
		} while(!(flags&1) && (slotframe_ptr = (struct tsch_slotframe*)tsch_schedule_get_next_slotframe(slotframe_ptr)));
		
		if(!first_item) {
			CONTENT_PRINTF("]");
			REST.set_header_content_type(response, REST.type.APPLICATION_JSON);
			REST.set_response_payload(response, (uint8_t *)content, content_len);
		} else {
			coap_set_status_code(response, NOT_FOUND_4_04);
			coap_set_payload(response, "Nothing to delete", 17);
			return;
		}
	} else {
		coap_set_status_code(response, NOT_ACCEPTABLE_4_06);
		return;
	}
}

static void plexi_post_stats_handler(void* request, void* response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset) {
	if(inbox_msg_lock == NO_LOCK) {
		inbox_msg_len = 0;
		*inbox_msg='\0';
	} else if(inbox_msg_lock != STATS_POST_LOCK) {
		coap_set_status_code(response, SERVICE_UNAVAILABLE_5_03);
		coap_set_payload(response, "Server too busy. Retry later.", 29);
		return;
	}
	inbox_msg_lock = NO_LOCK;
	content_len = 0;
	unsigned int accept = -1;
	REST.get_header_accept(request, &accept);
	if(accept == -1 || accept == REST.type.APPLICATION_JSON) {
	
		int state;
		const uint8_t *request_content;
		int request_content_len;
		char field_buf[32] = "";
		char value_buf[32] = "";

		request_content_len = REST.get_request_payload(request, &request_content);
		if(inbox_msg_len+request_content_len>MAX_DATA_LEN) {
			coap_set_status_code(response, NOT_IMPLEMENTED_5_01);
			coap_set_payload(response, "Server reached internal buffer limit. Shorten payload.", 54);
			return;
		}
		int x = coap_block1_handler(request, response, inbox_msg, &inbox_msg_len, MAX_DATA_LEN);
		if(inbox_msg_len<MAX_DATA_LEN) {
			*(inbox_msg+inbox_msg_len)='\0';
		}
		if(x==1) {
			inbox_msg_lock = STATS_POST_LOCK;
			return;
		} else if(x==-1) {
			inbox_msg_lock = NO_LOCK;
			return;
		}
		// TODO: It is assumed that the node processes the post request fast enough to return the
		//       response within the window assumed by client before retransmitting
		inbox_msg_lock = NO_LOCK;
		struct jsonparse_state js;
		jsonparse_setup(&js, (const char *)inbox_msg, inbox_msg_len);
	
		plexi_stats stats;
		int channel, slot, slotframe;
		linkaddr_t tna;
		uint8_t flags = 0;
		int to_initialize = 0;
		short unsigned int installed = 0;
		/* Parse json input */
		while((state=jsonparse_find_field(&js, field_buf, sizeof(field_buf)))) {
			switch(state) {
				case '{': /* New element */
					plexi_set_statistics_window(&stats, 0);
					plexi_set_statistics_enable(&stats, (uint8_t)DISABLE);
					plexi_set_statistics_metric(&stats, (uint8_t)NONE);
					stats.value = (plexi_stats_value_t)(-1);
					plexi_set_statistics_id(&stats,(uint16_t)(-1));
					channel = -1;
					slot = -1;
					slotframe = -1;
					break;
				case '}': { /* End of current element */
					if(plexi_get_statistics_metric(&stats) == NONE) {
						coap_set_status_code(response, BAD_REQUEST_4_00);
						coap_set_payload(response, "Invalid statistics configuration (metric missing)", 49);
						return;
					}
					struct tsch_slotframe *slotframe_ptr = NULL;
					if(flags&1){
						slotframe_ptr = (struct tsch_slotframe*)tsch_schedule_get_slotframe_by_handle(slotframe);
					} else {
						slotframe_ptr = (struct tsch_slotframe*)tsch_schedule_get_next_slotframe(NULL);
					}
					if(!slotframe_ptr) {
						coap_set_status_code(response, NOT_FOUND_4_04);
						coap_set_payload(response, "No slotframes found", 19);
						return;
					}
					do {
						struct tsch_link* link = NULL;
						if(flags&2) {
							link = (struct tsch_link*)tsch_schedule_get_link_by_timeslot(slotframe_ptr, slot);
						} else {
							link = (struct tsch_link*)tsch_schedule_get_next_link_of(slotframe_ptr, NULL);
						}
						if(!link) continue;
						do {
							if(!(flags&4) || link->channel_offset == channel) {
								if(!(flags&8) || linkaddr_cmp(&link->addr, &tna)) {
									plexi_stats *last_stats = NULL, *previous_stats = NULL;
									if(memb_inmemb(&plexi_stats_mem, link->data)) {
										last_stats = (plexi_stats*)link->data;
										while(last_stats != NULL) {
											if(plexi_get_statistics_metric(&stats) == plexi_get_statistics_metric(last_stats) \
											 && (!(flags&16) || (flags&16 && plexi_get_statistics_id(last_stats) == plexi_get_statistics_id(&stats))))
												 break;
											else if((plexi_get_statistics_metric(&stats) == plexi_get_statistics_metric(last_stats) \
											 && flags&16 && plexi_get_statistics_id(last_stats) != plexi_get_statistics_id(&stats)) || \
											 (plexi_get_statistics_metric(&stats) != plexi_get_statistics_metric(last_stats) \
											 && flags&16 && plexi_get_statistics_id(last_stats) == plexi_get_statistics_id(&stats))) {
												coap_set_status_code(response, BAD_REQUEST_4_00);
												coap_set_payload(response, "Statistics ID represents a different metric", 43);
												return;
											 }
											previous_stats = last_stats;
											last_stats = previous_stats->next;
										}
									}
									if(last_stats == NULL) {
										if(link->link_options != 1 && (plexi_get_statistics_metric(&stats) == ETX || plexi_get_statistics_metric(&stats) == PDR)) {
											coap_set_status_code(response, BAD_REQUEST_4_00);
											coap_set_payload(response, "Broadcast cells cannot measure ETX and PDR", 42);
											return;
										}
										plexi_stats *link_stats = memb_alloc(&plexi_stats_mem);
										if(link_stats == NULL) {
											coap_set_status_code(response, INTERNAL_SERVER_ERROR_5_00);
											coap_set_payload(response, "Not enough memory (too many statistics)", 39);
											return;
										}
										plexi_set_statistics_id(link_stats, plexi_get_statistics_id(&stats));
										link_stats->next = NULL;
										plexi_set_statistics_window(link_stats, plexi_get_statistics_window(&stats));
										plexi_set_statistics_metric(link_stats, plexi_get_statistics_metric(&stats));
										if(plexi_get_statistics_metric(link_stats) == RSSI) {
											link_stats->value = (plexi_stats_value_t)0xFFFFFFFFFFFFFFFF;
										} else {
											link_stats->value = (plexi_stats_value_t)(-1);
										}
										plexi_set_statistics_enable(link_stats, plexi_get_statistics_enable(&stats));
										LIST_STRUCT_INIT(link_stats,enhancement);
										if(to_initialize) link_stats->value = stats.value;
										if(previous_stats != NULL) previous_stats->next = link_stats;
										else link->data = (void*)link_stats;
									} else {
										plexi_set_statistics_window(last_stats, plexi_get_statistics_window(&stats));
										plexi_set_statistics_enable(last_stats, plexi_get_statistics_enable(&stats));
										last_stats->value = stats.value;
									}
									installed = 1;
								}
							}
						} while(!(flags&2) && (link = (struct tsch_link*)tsch_schedule_get_next_link_of(slotframe_ptr, link)));
					} while(!(flags&1) && (slotframe_ptr = (struct tsch_slotframe*)tsch_schedule_get_next_slotframe(slotframe_ptr)));
					if(installed) {
						coap_set_status_code(response, CHANGED_2_04);
					} else {
						coap_set_status_code(response, NOT_FOUND_4_04);
						coap_set_payload(response, "Link not found to install statistics resource", 45);
					}
					return;
				}
				case JSON_TYPE_NUMBER: //Try to remove the if statement and change { to [ on line 601.
					if(!strncmp(field_buf, FRAME_ID_LABEL, sizeof(field_buf))) {
						slotframe = jsonparse_get_value_as_int(&js);
						flags |= 1;
					} else if(!strncmp(field_buf, LINK_SLOT_LABEL, sizeof(field_buf))) {
						slot = jsonparse_get_value_as_int(&js);
						flags |= 2;
					} else if(!strncmp(field_buf, LINK_CHANNEL_LABEL, sizeof(field_buf))) {
						channel = jsonparse_get_value_as_int(&js);
						flags |= 4;
					} else if(!strncmp(field_buf, STATS_VALUE_LABEL, sizeof(field_buf))) {
						stats.value = (plexi_stats_value_t)jsonparse_get_value_as_int(&js);
						to_initialize = 1;
					} else if(!strncmp(field_buf, STATS_ID_LABEL, sizeof(field_buf))) {
						plexi_set_statistics_id(&stats, jsonparse_get_value_as_int(&js));
						if(plexi_get_statistics_id(&stats) < 1) {
							coap_set_status_code(response, BAD_REQUEST_4_00);
							coap_set_payload(response, "Invalid statistics configuration (invalid id)", 45);
							return;
						}
						flags |= 16;
					} else if(!strncmp(field_buf, STATS_ENABLE_LABEL, sizeof(field_buf))) {
						int x = (uint16_t)jsonparse_get_value_as_int(&js);
						if(x == 1) plexi_set_statistics_enable(&stats, (uint8_t)ENABLE);
						else plexi_set_statistics_enable(&stats, (uint8_t)DISABLE);
					}
					break;
				case JSON_TYPE_STRING:
					if(!strncmp(field_buf, NEIGHBORS_TNA_LABEL, sizeof(field_buf))) {
						jsonparse_copy_value(&js, value_buf, sizeof(value_buf));
						int x = na_to_linkaddr(value_buf, sizeof(value_buf), &tna);
						if(!x) {
							coap_set_status_code(response, BAD_REQUEST_4_00);
							coap_set_payload(response, "Invalid target node address", 27);
							return;
						}
						flags |= 8;
					} else if(!strncmp(field_buf, STATS_ENABLE_LABEL, sizeof(field_buf))) {
						jsonparse_copy_value(&js, value_buf, sizeof(value_buf));
						if(!strcmp("y",value_buf) || !strcmp("yes",value_buf) || !strcmp("true",value_buf)) {
							plexi_set_statistics_enable(&stats, (uint8_t)ENABLE);
						} else if(!strcmp("n",value_buf) || !strcmp("no",value_buf) || !strcmp("false",value_buf)) {
							plexi_set_statistics_enable(&stats, (uint8_t)DISABLE);
						}
					} else if(!strncmp(field_buf, STATS_METRIC_LABEL, sizeof(field_buf))) {
						jsonparse_copy_value(&js, value_buf, sizeof(value_buf));
						if(!strncmp(value_buf,STATS_ETX_LABEL,sizeof(STATS_ETX_LABEL)))
							plexi_set_statistics_metric(&stats, (uint8_t)ETX);
						else if(!strncmp(value_buf,STATS_RSSI_LABEL,sizeof(STATS_RSSI_LABEL)))
							plexi_set_statistics_metric(&stats, (uint8_t)RSSI);
						else if(!strncmp(value_buf,STATS_LQI_LABEL,sizeof(STATS_LQI_LABEL)))
							plexi_set_statistics_metric(&stats, (uint8_t)LQI);
						else if(!strncmp(value_buf,NEIGHBORS_ASN_LABEL,sizeof(NEIGHBORS_ASN_LABEL)))
							plexi_set_statistics_metric(&stats, (uint8_t)ASN);
						else if(!strncmp(value_buf,STATS_PDR_LABEL,sizeof(STATS_PDR_LABEL)))
							plexi_set_statistics_metric(&stats, (uint8_t)PDR);
						else {
							coap_set_status_code(response, NOT_IMPLEMENTED_5_01);
							coap_set_payload(response, "Unknown metric", 14);
							return;
						}
					}
					break;
			}
		}
		/* Check if json parsing succeeded */
		if(js.error == JSON_ERROR_OK) {
			REST.set_header_content_type(response, REST.type.APPLICATION_JSON);
			REST.set_response_payload(response, (uint8_t *)content, content_len);	 
		} else {
			coap_set_status_code(response, BAD_REQUEST_4_00);
			coap_set_payload(response, "Can only support JSON payload format", 36);
		}
	} else {
		coap_set_status_code(response, NOT_ACCEPTABLE_4_06);
		return;
	}
}

static void plexi_packet_received(void) {
	linkaddr_t* sender = (linkaddr_t*)packetbuf_addr(PACKETBUF_ADDR_SENDER);
	linkaddr_t* self = (linkaddr_t*)packetbuf_addr(PACKETBUF_ADDR_RECEIVER);
	struct tsch_slotframe *slotframe = tsch_schedule_get_slotframe_by_handle((uint16_t)packetbuf_attr(PACKETBUF_ATTR_TSCH_SLOTFRAME));
	uint16_t slotoffset = (uint16_t)packetbuf_attr(PACKETBUF_ATTR_TSCH_TIMESLOT);
	struct tsch_link *link = tsch_schedule_get_link_by_timeslot(slotframe,slotoffset);
	if(memb_inmemb(&plexi_stats_mem, link->data)) {
		plexi_stats *stats = (plexi_stats*)link->data;
		while(stats!=NULL) {
			if(plexi_get_statistics_metric(stats) == RSSI) {
				plexi_stats_value_st s_value = (int16_t)packetbuf_attr(PACKETBUF_ATTR_RSSI);
				plexi_stats_value_t u_value = s_value;
				plexi_update_ewma_statistics(plexi_get_statistics_metric(stats), &stats->value, u_value);
			} else if(plexi_get_statistics_metric(stats) == LQI) {
				plexi_update_ewma_statistics(plexi_get_statistics_metric(stats), &stats->value, (plexi_stats_value_t)packetbuf_attr(PACKETBUF_ATTR_LINK_QUALITY));
			} else if(plexi_get_statistics_metric(stats) == ASN) {
				stats->value = packetbuf_attr(PACKETBUF_ATTR_TSCH_ASN_2_1);
			}
			if(link->link_options & LINK_OPTION_SHARED) {
				uint8_t found = 0;
				plexi_enhanced_stats *es = NULL;
				for(es = list_head(stats->enhancement);es!=NULL; es = list_item_next(es)) {
					if(linkaddr_cmp(&(es->target),sender)) {
						found = 1;
						break;
					}
				}
				if(!found) {
					es = memb_alloc(&plexi_enhanced_stats_mem);
					linkaddr_copy(&(es->target),sender);
					if(plexi_get_statistics_metric(stats) == RSSI) {
						es->value = (plexi_stats_value_t)0xFFFFFFFFFFFFFFFF;
					} else {
						es->value = -1;
					}
					list_add(stats->enhancement, es);
				}
				if(plexi_get_statistics_metric(stats) == RSSI) {
					plexi_stats_value_st s_value = (int16_t)packetbuf_attr(PACKETBUF_ATTR_RSSI);
					plexi_stats_value_t u_value = s_value;
					plexi_update_ewma_statistics(plexi_get_statistics_metric(stats), &es->value, u_value);
				} else if(plexi_get_statistics_metric(stats) == LQI) {
					plexi_update_ewma_statistics(plexi_get_statistics_metric(stats), &es->value, (plexi_stats_value_t)packetbuf_attr(PACKETBUF_ATTR_LINK_QUALITY));
				} else if(plexi_get_statistics_metric(stats) == ASN) {
					es->value = packetbuf_attr(PACKETBUF_ATTR_TSCH_ASN_2_1);
				}
			}
			stats = stats->next;
		}
	}
#if PLEXI_WITH_VICINITY_MONITOR
	short int to_add = 1;
	short int to_remove = 1-PLEXI_MAX_PROXIMATES;
	plexi_proximate *p;
	char buf[32];
	linkaddr_to_na(buf, sender);
	for(p = list_head(plexi_vicinity); p != NULL; p = list_item_next(p)) {
		if(linkaddr_cmp(&p->proximate, sender)) {
			p->since = clock_time();
			p->pheromone += PLEXI_PHEROMONE_CHUNK;
			to_add = 0;
		}
		to_remove++;
	}
	if(to_remove) {
		plexi_proximate *weakest = NULL;
		for(p = list_head(plexi_vicinity); p != NULL; p = list_item_next(p)) {
			if( weakest==NULL || p->pheromone < weakest->pheromone) {
				weakest = p;
			}
		}
		if(weakest) {
			list_remove(plexi_vicinity, weakest);
			memb_free(&plexi_vicinity_mem, weakest);
			weakest = NULL;
		}
	}
	if(to_add) {
		plexi_proximate *prox = memb_alloc(&plexi_vicinity_mem);
		linkaddr_copy(&prox->proximate, sender);
		prox->since = clock_time();
		prox->pheromone = PLEXI_PHEROMONE_CHUNK;
		list_add(plexi_vicinity, prox);
	}
#endif
}


static void plexi_packet_sent(int mac_status) {
	if(mac_status == MAC_TX_OK && packetbuf_attr(PACKETBUF_ATTR_MAC_ACK)) {
		struct tsch_slotframe *slotframe = (struct tsch_slotframe *)tsch_schedule_get_slotframe_by_handle((uint16_t)packetbuf_attr(PACKETBUF_ATTR_TSCH_SLOTFRAME));
		uint16_t slotoffset = (uint16_t)packetbuf_attr(PACKETBUF_ATTR_TSCH_TIMESLOT);
		struct tsch_link *link = (struct tsch_link *)tsch_schedule_get_link_by_timeslot(slotframe,slotoffset);
		if(memb_inmemb(&plexi_stats_mem, link->data)) {
			plexi_stats *stats = (plexi_stats*)link->data;
			while(stats!=NULL) {
				if(plexi_get_statistics_metric(stats) == ETX || plexi_get_statistics_metric(stats) == PDR) {
					plexi_update_ewma_statistics(plexi_get_statistics_metric(stats), &stats->value, 256*packetbuf_attr(PACKETBUF_ATTR_TSCH_TRANSMISSIONS));
				}
				stats = stats->next;
			}
		}
#if PLEXI_WITH_VICINITY_MONITOR
		short int to_add = 1;
		short int to_remove = 1-PLEXI_MAX_PROXIMATES;
		linkaddr_t* receiver = (linkaddr_t*)packetbuf_addr(PACKETBUF_ADDR_RECEIVER);
		plexi_proximate *p;
		for(p = list_head(plexi_vicinity); p != NULL; p = list_item_next(p)) {
			if(linkaddr_cmp(&p->proximate, receiver)) {
				p->since = clock_time();
				p->pheromone += PLEXI_PHEROMONE_CHUNK;
				to_add = 0;
			}
			to_remove++;
		}
		if(to_remove>0) {
			plexi_proximate *weakest = NULL;
			for(p = list_head(plexi_vicinity); p != NULL; p = list_item_next(p)) {
				if( weakest==NULL || p->pheromone < weakest->pheromone) {
					weakest = p;
				}
			}
			if(weakest) {
				list_remove(plexi_vicinity, weakest);
				memb_free(&plexi_vicinity_mem, weakest);
				weakest = NULL;
			}
		}
		if(to_add) {
			plexi_proximate *prox = memb_alloc(&plexi_vicinity_mem);
			linkaddr_copy(&prox->proximate, receiver);
			prox->since = clock_time();
			prox->pheromone = PLEXI_PHEROMONE_CHUNK;
			list_add(plexi_vicinity, prox);
		}
#endif
	}
}


void plexi_update_ewma_statistics(uint8_t metric, void* old_value, plexi_stats_value_t new_value) {
	if(old_value) {
		if(metric == RSSI) {
			plexi_stats_value_st v = new_value;
			if(*((plexi_stats_value_t*)old_value)!=(plexi_stats_value_t)0xFFFFFFFFFFFFFFFF) {
				v = (v * 10 + *((plexi_stats_value_st*)old_value) * 90) / 100;
			}
			*((plexi_stats_value_st*)old_value) = v;
		} else {
			plexi_stats_value_t v = new_value;
			if(*((plexi_stats_value_t*)old_value) != (plexi_stats_value_t)(-1)) {
				v = (new_value * 10 + *((plexi_stats_value_t*)old_value) * 90) / 100;
			}
			if(metric == LQI || metric == ETX) {
				*((plexi_stats_value_t*)old_value) = v;
			} else if(metric == PDR) {
				*((plexi_stats_value_t*)old_value) = 100*256/v;
			}
		}
	}
}

void plexi_purge_neighbor_statistics(linkaddr_t *neighbor) {
	
}

void plexi_purge_link_statistics(struct tsch_link *link) {
	plexi_purge_statistics((plexi_stats *)link->data);
}

void plexi_purge_statistics(plexi_stats *stats) {
	if(memb_inmemb(&plexi_stats_mem, stats)) {
		plexi_enhanced_stats * es = list_head(stats->enhancement);
		while(es!=NULL) {
			es = list_pop(stats->enhancement);
			plexi_purge_enhanced_statistics(es);
		}
		memb_free(&plexi_stats_mem, stats);
		stats = NULL;
	}
}

void plexi_purge_enhanced_statistics(plexi_enhanced_stats *stats) {
	memb_free(&plexi_enhanced_stats_mem, stats);
}


uint16_t plexi_get_statistics_id(plexi_stats* stats) {
	if(!stats) return (uint16_t)(-1);
#if PLEXI_STATISTICS_MODE & PLEXI_DENSE_STATISTICS == PLEXI_DENSE_STATISTICS
	return (uint16_t)(stats->metainfo>>5)&31;
#else
	return stats->id;
#endif
}

int plexi_set_statistics_id(plexi_stats* stats, uint16_t id) {
	if(!stats) return 0;
#if PLEXI_STATISTICS_MODE & PLEXI_DENSE_STATISTICS == PLEXI_DENSE_STATISTICS
	if(id<32) {
		stats->metainfo = (stats->metainfo&64543)|(id<<5);
		return 1;
	} else {
		return 0;
	}
#else
	stats->id = id;
	return 1;
#endif
}

uint8_t plexi_get_statistics_enable(plexi_stats* stats) {
	if(!stats) return (uint8_t)(-1);
#if PLEXI_STATISTICS_MODE & PLEXI_DENSE_STATISTICS == PLEXI_DENSE_STATISTICS
	return (uint8_t)(stats->metainfo&1);
#else
	return stats->enable;
#endif
}

int plexi_set_statistics_enable(plexi_stats* stats, uint8_t enable) {
	if(!stats) return 0;
#if PLEXI_STATISTICS_MODE & PLEXI_DENSE_STATISTICS == PLEXI_DENSE_STATISTICS
	if(enable<2) {
		stats->metainfo = (stats->metainfo&65534)|enable;
		return 1;
	} else {
		return 0;
	}
#else
	stats->enable = enable;
	return 1;
#endif
}

uint8_t plexi_get_statistics_metric(plexi_stats* stats) {
	if(!stats) return (uint8_t)(-1);
#if PLEXI_STATISTICS_MODE & PLEXI_DENSE_STATISTICS == PLEXI_DENSE_STATISTICS
	return (uint8_t)((stats->metainfo>>1)&15);
#else
	return stats->metric;
#endif
}

int plexi_set_statistics_metric(plexi_stats* stats, uint8_t metric) {
	if(!stats) return 0;
#if PLEXI_STATISTICS_MODE & PLEXI_DENSE_STATISTICS == PLEXI_DENSE_STATISTICS
	if(metric<16) {
		stats->metainfo = (stats->metainfo&65505)|(metric<<1);
		return 1;
	} else {
		return 0;
	}
#else
	stats->metric = metric;
	return 1;
#endif
}

uint16_t plexi_get_statistics_window(plexi_stats* stats) {
	if(!stats) return (uint16_t)(-1);
#if PLEXI_STATISTICS_MODE & PLEXI_DENSE_STATISTICS == PLEXI_DENSE_STATISTICS
	return (uint16_t)(stats->metainfo>>10);
#else
	return stats->window;
#endif
}

int plexi_set_statistics_window(plexi_stats* stats, uint16_t window) {
	if(!stats) return 0;
#if PLEXI_STATISTICS_MODE & PLEXI_DENSE_STATISTICS == PLEXI_DENSE_STATISTICS
	if(window<64) {
		stats->metainfo = (stats->metainfo&1023)|(window<<10);
		return 1;
	} else {
		return 0;
	}
#else
	stats->window = window;
	return 1;
#endif
}

void printubin(plexi_stats_value_t a) {
	char buf[128];
	int i=0;
    for (i = 8*sizeof(plexi_stats_value_t)-1; i >= 0; i--) {
        buf[i] = (a & 1) + '0';
        a >>= 1;
    }
	for (i = 0; i<8*sizeof(plexi_stats_value_t); i++) {
		printf("%c",buf[i]);
	}
}

void printsbin(plexi_stats_value_st a) {
	char buf[128];
	int i=0;
    for (i = 8*sizeof(plexi_stats_value_t)-1; i >= 0; i--) {
        buf[i] = (a & 1) + '0';
        a >>= 1;
    }
	for (i = 0; i<8*sizeof(plexi_stats_value_t); i++) {
		printf("%c",buf[i]);
	}
}

#endif

#if PLEXI_WITH_QUEUE_STATISTICS

/**************************************************************************************************/
/** Observable queuelist resource and event handler to obtain txqlength 						  */ 
/**************************************************************************************************/

PARENT_PERIODIC_RESOURCE(resource_6top_queue,					//* name
               "obs;title=\"6TOP Queue statistics\"",			//* attributes
               plexi_get_queue_handler,							//* GET handler
               NULL,											//* POST handler
               NULL,											//* PUT handler
               NULL,											//* DELETE handler
               PLEXI_QUEUE_UPDATE_INTERVAL,
               plexi_queue_event_handler);


/* Responds to GET with a JSON object with the following format:
 * {
 * 		"215:8d00:57:6466":5,
 * 		"215:8d00:57:6499":1
 * }
 * Each item in the object has the format: "EUI 64 address":<# of packets in tx queue>
 * */
static void plexi_get_queue_handler(void *request, void *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset) {
	content_len = 0;
	unsigned int accept = -1;
	REST.get_header_accept(request, &accept);
	if(accept == -1 || accept == REST.type.APPLICATION_JSON) {
		char *end;
		char *uri_path = NULL;
		int uri_len = REST.get_url(request, (const char**)(&uri_path));
		/* If you need to handle subresources or queries edit the commented code below 
		const char *query = NULL;
		int base_len = 0, query_len = 0, query_value_len = 0;
		char *uri_subresource = NULL, *query_value = NULL;
		uint8_t id = 0;

		if(uri_len>0) {
			*(uri_path+uri_len) = '\0';
			base_len = strlen(resource_6top_queue.url);
			uri_subresource = uri_path+base_len;
			if(*uri_subresource == '/') {
				uri_subresource++;
			}
			query_len = REST.get_query(request, &query);
			query_value_len = REST.get_query_variable(request, QUEUE_ID_LABEL, (const char**)(&query_value));
			if(query_value) {
				*(query_value+query_value_len) = '\0';
				id = (uint8_t)strtoul(query_value, &end, 10);
			}
		}
		if((uri_len > base_len + 1 && strcmp(QUEUE_ID_LABEL,uri_subresource) \
			  && strcmp(QUEUE_TXLEN_LABEL,uri_subresource) \
			  ) || (query && !query_value)) {
			coap_set_status_code(response, BAD_REQUEST_4_00);
			coap_set_payload(response, "Supports only queries on queue id", 33);
			return;
		}
		*/
		// Run through all the neighbors. Each neighbor has one queue. There are two extra for the EBs and the broadcast messages.
		// The function tsch_queue_get_nbr_next is defined by George in tsch_queue.h/.c
		int first_item = 1;
		char buf[32];
		struct tsch_neighbor *neighbor = NULL;
		for(neighbor = (struct tsch_neighbor *)tsch_queue_get_nbr_next(NULL);
			  neighbor != NULL;
			  neighbor = (struct tsch_neighbor *)tsch_queue_get_nbr_next(neighbor)) {
			linkaddr_t tna = neighbor->addr; // get the link layer address of neighbor
			int txlength = tsch_queue_packet_count(&tna); // get the size of his queue
			int success = linkaddr_to_na(buf, &tna); //convert his address to string
			if(success) { // if the address was valid
				if(first_item) {
					CONTENT_PRINTF("{");
				} else {
					CONTENT_PRINTF(",");
				}
				first_item = 0;
				CONTENT_PRINTF("\"%s\":%d", buf, txlength); // put to the output string the new JSON item
			}
		}
		if(!first_item) { //if you found at least one queue
			CONTENT_PRINTF("}");
			REST.set_header_content_type(response, REST.type.APPLICATION_JSON);
			REST.set_response_payload(response, (uint8_t *)content, content_len);
		} else { // if no queues
			coap_set_status_code(response, NOT_FOUND_4_04);
			coap_set_payload(response, "No neighbor was found", 21);
			return;
		}
	} else {
		coap_set_status_code(response, NOT_ACCEPTABLE_4_06);
		return;
	}
}

static void plexi_queue_event_handler(void) {
	REST.notify_subscribers(&resource_6top_queue);
}

static void plexi_queue_changed(uint8_t event, struct tsch_neighbor* n) {
	// There are two events coming from queues TSCH_QUEUE_EVENT_SHRINK and TSCH_QUEUE_EVENT_GROW
	// For now we do not treat them separately and we let plexi return the complete list of queues when there is a change even on one.
	plexi_queue_event_handler();
	// For handling better the events work on the following
	/*
	if(event == TSCH_QUEUE_EVENT_SHRINK) {
		
	} else if(event == TSCH_QUEUE_EVENT_GROW) {
		
	}
	*/
}

#endif

#if PLEXI_WITH_TRAFFIC_GENERATOR

	PROCESS(plexi_traffic_process, "plexi");

	/* The logging process */
	PROCESS_THREAD(plexi_traffic_process, ev, data)
	{
		static struct etimer periodic;
		PROCESS_BEGIN();
		etimer_set(&periodic, PLEXI_TRAFFIC_STEP);
		while(1) {
			PROCESS_WAIT_UNTIL(etimer_expired(&periodic));
			etimer_reset(&periodic);
			plexi_generate_traffic(NULL);
		}
		PROCESS_END();
	}

#endif

void plexi_init() {
	static struct uip_ds6_notification n;
	printf("\n*** PLEXI: initializing scheduler interface ***\n");

#if PLEXI_WITH_TRAFFIC_GENERATOR
	process_start(&plexi_traffic_process, NULL);
#endif

	rest_init_engine();
	rest_activate_resource(&resource_rpl_dag, DAG_RESOURCE);
	rest_activate_resource(&resource_6top_nbrs, NEIGHBORS_RESOURCE);
	rest_activate_resource(&resource_6top_slotframe, FRAME_RESOURCE);
	rest_activate_resource(&resource_6top_links, LINK_RESOURCE);
	/* A callback for routing table changes */
	uip_ds6_notification_add(&n, route_changed_callback);
#if PLEXI_WITH_LINK_STATISTICS
	rime_sniffer_add(&plexi_sniffer);
	memb_init(&plexi_stats_mem);
	memb_init(&plexi_enhanced_stats_mem);
	
	rest_activate_resource(&resource_6top_stats, STATS_RESOURCE);
	#if PLEXI_WITH_VICINITY_MONITOR
		memb_init(&plexi_vicinity_mem);
		list_init(plexi_vicinity);
		ctimer_set(&ct, 10*PLEXI_PHEROMONE_WINDOW, plexi_vicinity_updater, NULL);
		rest_activate_resource(&resource_mac_vicinity, VICINITY_RESOURCE);
	#endif
#endif

#if PLEXI_WITH_QUEUE_STATISTICS
	rest_activate_resource(&resource_6top_queue, QUEUE_RESOURCE);
#endif
}

/* Utility function for json parsing */
int jsonparse_find_field(struct jsonparse_state *js, char *field_buf, int field_buf_len) {
	int state=jsonparse_next(js);
	while(state) {
		switch(state) {
			case JSON_TYPE_PAIR_NAME:
				jsonparse_copy_value(js, field_buf, field_buf_len);
				/* Move to ":" */
				jsonparse_next(js);
				/* Move to value and return its type */
				return jsonparse_next(js);
			default:
				return state;
		}
		state=jsonparse_next(js);
	}
	return 0;
}
/* Utility function. Converts na field (string containing the lower 64bit of the IPv6) to
 * 64-bit MAC. */
static int na_to_linkaddr(const char *na_inbuf, int bufsize, linkaddr_t *linkaddress) {
	int i;
	char next_end_char = ':';
	const char *na_inbuf_end = na_inbuf + bufsize - 1;
	char *end;
	unsigned val;
	for(i=0; i<4; i++) {
		if(na_inbuf >= na_inbuf_end) {
			return 0;
		}
		if(i == 3) {
			next_end_char = '\0';
		}
		val = (unsigned)strtoul(na_inbuf, &end, 16);
		/* Check conversion */
		if(end != na_inbuf && *end == next_end_char && errno != ERANGE) {
			linkaddress->u8[2*i] = val >> 8;
			linkaddress->u8[2*i+1] = val;
			na_inbuf = end+1;
		} else {
			return 0;
		}
	}
	/* We consider only links with IEEE EUI-64 identifier */
	linkaddress->u8[0] ^= 0x02;
	return 1;
}

static int linkaddr_to_na(char *buf, linkaddr_t *addr) {
	char *pointer = buf;
	unsigned int i;
	for(i = 0; i < sizeof(linkaddr_t); i++) {
		if(i > 1 && i!=3 && i!=4 && i!=7) {
			*pointer = ':';
			pointer++;
		}
		if(i==4){
			continue;
		}
		if(i==0) {
			sprintf(pointer, "%x", addr->u8[i]^0x02);
			pointer++;
		} else {
			sprintf(pointer, "%02x", addr->u8[i]);
			pointer+=2;
		}
	}
	sprintf(pointer, "\0");
	return strlen(buf);
}
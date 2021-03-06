/*
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER
 *
 * Copyright (c) 2016, Juniper Networks, Inc. All rights reserved.
 *
 *
 * The contents of this file are subject to the terms of the BSD 3 clause
 * License (the "License"). You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license at
 * https://github.com/Juniper/warp17/blob/master/LICENSE.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * File name:
 *     tpg_pktloop.c
 *
 * Description:
 *     Packet send/receive functions / loop
 *
 * Author:
 *     Dumitru Ceara, Eelco Chaudron
 *
 * Initial Created:
 *     02/27/2015
 *
 * Notes:
 *
 */

/*****************************************************************************
 * Include files
 ****************************************************************************/
#include <rte_arp.h>

#include "tcp_generator.h"

/*****************************************************************************
 * Definitions
 ****************************************************************************/
/* Information about a port + queue that would be polled by the local core. */
typedef struct local_port_info_s {

    uint32_t     lpi_port_id;
    uint32_t     lpi_queue_id;
    port_info_t *lpi_port_info;

} local_port_info_t;


/*****************************************************************************
 * Static per lcore globals.
 ****************************************************************************/

/* TODO: improve this. we don't need so many entries */
/* Buffered packets to send (per port). */
static RTE_DEFINE_PER_LCORE(struct rte_mbuf *, pkt_tx_q)[TPG_ETH_DEV_MAX][TPG_TX_BURST_SIZE];
/* Flags specifying wether we need to trace the packets at TX or not. */
static RTE_DEFINE_PER_LCORE(bool, pkt_tx_q_trace)[TPG_ETH_DEV_MAX][TPG_TX_BURST_SIZE];
/* Number of packets buffered for TX (per port). */
static RTE_DEFINE_PER_LCORE(uint32_t, pkt_tx_q_len)[TPG_ETH_DEV_MAX];

/* Drop one packet at tx every 'pkt_send_simulate_drop_rate' sends. */
static RTE_DEFINE_PER_LCORE(uint32_t, pkt_send_simulate_drop_rate);

/*****************************************************************************
 * pkt_trace_tx()
 ****************************************************************************/
static void pkt_trace_tx(packet_control_block_t *pcb, int32_t tx_queue_id,
                         struct rte_mbuf *mbuf, bool failed)
{
    struct ether_hdr *eth_hdr;

    PKT_TRACE(pcb, PKT_TX, DEBUG,
              "port=%d qid=%d data_len=%u, buf_len = %u failed = %d",
              pcb->pcb_port, tx_queue_id, rte_pktmbuf_data_len(mbuf),
              mbuf->buf_len, failed);
    if (!PKT_TRACE_ENABLED(pcb))
        return;

    /*
     * Only do this is the segment header is big enough to hold longest
     * decode train. eth/ip/tcp in this case.
     */
    if (mbuf->buf_len < (sizeof(struct ether_hdr) +
                          sizeof(struct ipv4_hdr) +
                          sizeof(struct tcp_hdr))) {
        return;
    }

    eth_hdr = rte_pktmbuf_mtod(mbuf, struct ether_hdr *);

    PKT_TRACE(pcb, ETH, DEBUG, "dst=%02X:%02X:%02X:%02X:%02X:%02X, src=%02X:%02X:%02X:%02X:%02X:%02X, etype=0x%4.4X",
              eth_hdr->d_addr.addr_bytes[0],
              eth_hdr->d_addr.addr_bytes[1],
              eth_hdr->d_addr.addr_bytes[2],
              eth_hdr->d_addr.addr_bytes[3],
              eth_hdr->d_addr.addr_bytes[4],
              eth_hdr->d_addr.addr_bytes[5],
              eth_hdr->s_addr.addr_bytes[0],
              eth_hdr->s_addr.addr_bytes[1],
              eth_hdr->s_addr.addr_bytes[2],
              eth_hdr->s_addr.addr_bytes[3],
              eth_hdr->s_addr.addr_bytes[4],
              eth_hdr->s_addr.addr_bytes[5],
              rte_be_to_cpu_16(eth_hdr->ether_type));

    switch (rte_be_to_cpu_16(eth_hdr->ether_type)) {
    case ETHER_TYPE_IPv4:
        if (true) {
            struct ipv4_hdr *ip_hdr = (struct ipv4_hdr *) (eth_hdr + 1);

            PKT_TRACE(pcb, IPV4, DEBUG, "src/dst=%8.8X/%8.8X, prot=%u, ver_len=0x%2.2X, len=%u",
                      rte_be_to_cpu_32(ip_hdr->src_addr),
                      rte_be_to_cpu_32(ip_hdr->dst_addr),
                      ip_hdr->next_proto_id,
                      ip_hdr->version_ihl,
                      rte_be_to_cpu_16(ip_hdr->total_length));

            PKT_TRACE(pcb, IPV4, DEBUG, " ttl=%u, tos=%u, frag=0x%4.4X[%c%c%c], id=0x%4.4X, csum=0x%4.4X",
                      ip_hdr->time_to_live,
                      ip_hdr->type_of_service,
                      rte_be_to_cpu_16(ip_hdr->fragment_offset) & IPV4_HDR_OFFSET_MASK,
                      (rte_be_to_cpu_16(ip_hdr->fragment_offset) & 1<<15) == 0 ? '-' : 'R',
                      (rte_be_to_cpu_16(ip_hdr->fragment_offset) & IPV4_HDR_DF_FLAG) == 0 ? '-' : 'd',
                      (rte_be_to_cpu_16(ip_hdr->fragment_offset) & IPV4_HDR_MF_FLAG) == 0 ? '-' : 'm',
                      rte_be_to_cpu_16(ip_hdr->packet_id),
                      rte_be_to_cpu_16(ip_hdr->hdr_checksum));

            switch (ip_hdr->next_proto_id) {
            case IPPROTO_TCP:
                if (true) {
                    unsigned int    i;
                    struct tcp_hdr *tcp_hdr = (struct tcp_hdr *) ip_hdr + 1;
                    uint32_t       *options = (uint32_t *) (tcp_hdr + 1);

                    PKT_TRACE(pcb, TCP, DEBUG, "sport=%u, dport=%u, hdrlen=%u, flags=%c%c%c%c%c%c, csum=0x%4.4X",
                              rte_be_to_cpu_16(tcp_hdr->src_port),
                              rte_be_to_cpu_16(tcp_hdr->dst_port),
                              (tcp_hdr->data_off >> 4) << 2,
                              (tcp_hdr->tcp_flags & TCP_URG_FLAG) == 0 ? '-' : 'u',
                              (tcp_hdr->tcp_flags & TCP_ACK_FLAG) == 0 ? '-' : 'a',
                              (tcp_hdr->tcp_flags & TCP_PSH_FLAG) == 0 ? '-' : 'p',
                              (tcp_hdr->tcp_flags & TCP_RST_FLAG) == 0 ? '-' : 'r',
                              (tcp_hdr->tcp_flags & TCP_SYN_FLAG) == 0 ? '-' : 's',
                              (tcp_hdr->tcp_flags & TCP_FIN_FLAG) == 0 ? '-' : 'f',
                              rte_be_to_cpu_16(tcp_hdr->cksum));

                    PKT_TRACE(pcb, TCP, DEBUG, " seq=%u, ack=%u, window=%u, urgent=%u",
                              rte_be_to_cpu_32(tcp_hdr->sent_seq),
                              rte_be_to_cpu_32(tcp_hdr->recv_ack),
                              rte_be_to_cpu_16(tcp_hdr->rx_win),
                              rte_be_to_cpu_16(tcp_hdr->tcp_urp));

                    for (i = 0;
                         i < (((((tcp_hdr->data_off >> 4) << 2) - sizeof(struct tcp_hdr)) / sizeof(uint32_t)));
                         i++) {

                        PKT_TRACE(pcb, TCP, DEBUG, "  option word 0x%2.2X: 0x%8.8X",
                                  i,
                                  rte_be_to_cpu_32(options[i]));
                    }
                }
                break;
            }
        }
        break;

    case ETHER_TYPE_ARP:
        if (true) {

            struct arp_hdr *arp_hdr = (struct arp_hdr *) (eth_hdr + 1);

            PKT_TRACE(pcb, ARP, DEBUG, "hrd=%u, pro=0x%4.4X, hln=%u, pln=%u, op=%u",
                      rte_be_to_cpu_16(arp_hdr->arp_hrd),
                      rte_be_to_cpu_16(arp_hdr->arp_pro),
                      arp_hdr->arp_hln, arp_hdr->arp_pln,
                      rte_be_to_cpu_16(arp_hdr->arp_op));

            PKT_TRACE(pcb, ARP, DEBUG, "  sha=%02X:%02X:%02X:%02X:%02X:%02X spa=" TPG_IPV4_PRINT_FMT,
                      arp_hdr->arp_data.arp_sha.addr_bytes[0],
                      arp_hdr->arp_data.arp_sha.addr_bytes[1],
                      arp_hdr->arp_data.arp_sha.addr_bytes[2],
                      arp_hdr->arp_data.arp_sha.addr_bytes[3],
                      arp_hdr->arp_data.arp_sha.addr_bytes[4],
                      arp_hdr->arp_data.arp_sha.addr_bytes[5],
                      TPG_IPV4_PRINT_ARGS(rte_be_to_cpu_32(arp_hdr->arp_data.arp_sip)));

            PKT_TRACE(pcb, ARP, DEBUG, "  tha=%02X:%02X:%02X:%02X:%02X:%02X, tpa=" TPG_IPV4_PRINT_FMT,
                      arp_hdr->arp_data.arp_tha.addr_bytes[0],
                      arp_hdr->arp_data.arp_tha.addr_bytes[1],
                      arp_hdr->arp_data.arp_tha.addr_bytes[2],
                      arp_hdr->arp_data.arp_tha.addr_bytes[3],
                      arp_hdr->arp_data.arp_tha.addr_bytes[4],
                      arp_hdr->arp_data.arp_tha.addr_bytes[5],
                      TPG_IPV4_PRINT_ARGS(rte_be_to_cpu_32(arp_hdr->arp_data.arp_tip)));
        }
        break;
    }
}

/*****************************************************************************
 * pkt_flush_tx_q()
 ****************************************************************************/
void pkt_flush_tx_q(uint32_t port, port_statistics_t *stats)
{
    int32_t                tx_queue_id;
    int                    lcore_id = rte_lcore_id();
    uint32_t               pkt_sent_cnt;
    packet_control_block_t pcb;
    uint32_t               i;

    if (RTE_PER_LCORE(pkt_tx_q_len)[port] == 0)
        return;

    pcb.pcb_port = port;
    pcb.pcb_core_index = rte_lcore_index(lcore_id);

    tx_queue_id = port_get_tx_queue_id(lcore_id, port);

    if (unlikely(tx_queue_id == CORE_PORT_QINVALID)) {
        TPG_ERROR_ABORT("[%d:%s()] CRIT: invalid core port configuration for port %d\n",
                        rte_lcore_index(lcore_id), __func__, port);
    }

    /* First trace what we're about to send because rte_eth_tx_burst will free
     * the mbufs that are successfully queued on the transmit ring.
     * Also increment the stats but don't forget to decrement when we actually
     * fail to send.
     */
    for (i = 0; i < RTE_PER_LCORE(pkt_tx_q_len)[port]; i++) {
        pcb.pcb_trace = RTE_PER_LCORE(pkt_tx_q_trace)[port][i];
        pkt_trace_tx(&pcb, tx_queue_id, RTE_PER_LCORE(pkt_tx_q)[port][i], false);

        INC_STATS(stats, ps_send_pkts);
        INC_STATS_VAL(stats, ps_send_bytes,
                      rte_pktmbuf_data_len(RTE_PER_LCORE(pkt_tx_q)[port][i]));
    }

    pkt_sent_cnt = rte_eth_tx_burst(port, tx_queue_id, &RTE_PER_LCORE(pkt_tx_q)[port][0],
                                    RTE_PER_LCORE(pkt_tx_q_len)[port]);

    /* We can't really notify the initial sender that we failed. Just increase
     * stats so we know that something went wrong.
     * Free the ones we couldn't send but first log them.
     */
    for (i = pkt_sent_cnt; i < RTE_PER_LCORE(pkt_tx_q_len)[port]; i++) {
        INC_STATS(stats, ps_send_failure);
        DEC_STATS(stats, ps_send_pkts);
        DEC_STATS_VAL(stats, ps_send_bytes,
                      rte_pktmbuf_data_len(RTE_PER_LCORE(pkt_tx_q)[port][i]));

        pcb.pcb_trace = RTE_PER_LCORE(pkt_tx_q_trace)[port][i];
        pkt_trace_tx(&pcb, tx_queue_id, RTE_PER_LCORE(pkt_tx_q)[port][i], true);

        rte_pktmbuf_free(RTE_PER_LCORE(pkt_tx_q)[port][i]);
    }

    /* Reinitialize the burst tx queue. */
    RTE_PER_LCORE(pkt_tx_q_len)[port] = 0;
}


/*****************************************************************************
 * pkt_send()
 *
 * NOTE: mbuf should not be used after this call...
 ****************************************************************************/
int pkt_send(uint32_t port, struct rte_mbuf *mbuf, bool trace)
{
    port_statistics_t *stats;

    stats = STATS_LOCAL(port_statistics_t, port);

    /* If we should simulate a packet drop, do it here! */
    if (unlikely(RTE_PER_LCORE(pkt_send_simulate_drop_rate) != 0)) {
        if (unlikely(rte_rand() %
                RTE_PER_LCORE(pkt_send_simulate_drop_rate) == 0)) {
            INC_STATS(stats, ps_send_sim_failure);
            rte_pktmbuf_free(mbuf);
            return false;
        }
    }

    if (RTE_PER_LCORE(pkt_tx_q_len)[port] == TPG_TX_BURST_SIZE)
        pkt_flush_tx_q(port, stats);

    RTE_PER_LCORE(pkt_tx_q)[port][RTE_PER_LCORE(pkt_tx_q_len)[port]] = mbuf;
    RTE_PER_LCORE(pkt_tx_q_trace)[port][RTE_PER_LCORE(pkt_tx_q_len)[port]] = trace;
    RTE_PER_LCORE(pkt_tx_q_len)[port]++;

    return true;
}

/*****************************************************************************
 * pkt_rx_burst()
 ****************************************************************************/
static uint16_t pkt_rx_burst(uint8_t port_id, uint16_t queue_id,
                             struct rte_mbuf **rx_pkts,
                             const uint16_t nb_pkts,
                             port_info_t *port_info,
                             port_statistics_t *stats)
{
    uint16_t no_rx_buffers;

    no_rx_buffers = rte_eth_rx_burst(port_id, queue_id, rx_pkts, nb_pkts);

#if defined(TPG_RING_IF)
    if (unlikely(port_info->pi_ring_if)) {

        uint16_t i;

        i = 0;
        while (i < no_rx_buffers) {
            struct rte_mbuf *orig_mbuf = rx_pkts[i];

            rx_pkts[i] = data_copy_chain(orig_mbuf, mem_get_mbuf_local_pool());
            if (unlikely(rx_pkts[i] == NULL)) {
                INC_STATS(stats, ps_rx_ring_if_failed);

                rx_pkts[i] = rx_pkts[no_rx_buffers - 1];
                no_rx_buffers--;
            } else {
                i++;
            }
            rte_pktmbuf_free(orig_mbuf);
        }
    }
#endif /* defined(TPG_RING_IF) */

    /* Trick the compiler into shutting up about the unused parameter.. */
    (void)port_info;
    (void)stats;

    return no_rx_buffers;
}

/*****************************************************************************
 * pkt_receive_loop()
 ****************************************************************************/
int pkt_receive_loop(void *arg __rte_unused)
{
    int                     lcore_id    = rte_lcore_id();
    int                     lcore_index = rte_lcore_index(lcore_id);
    int32_t                 queue_id;
    global_config_t        *cfg;
    port_statistics_t      *port_stats;
    packet_control_block_t *pcb;
    uint32_t                port;

    /* Local port info array indexed by queue idx. */
    local_port_info_t      *local_port_info;
    uint32_t                local_port_count;

    cfg = cfg_get_config();
    if (cfg == NULL) {
        RTE_LOG(ERR, USER1, "ERROR: Can't get global config on lcore %d, core index %d\n",
                lcore_id, lcore_index);
        return 0;
    }

    RTE_LOG(INFO, USER1, "Starting receive packet loop on lcore %d, core index %d\n",
            lcore_id, lcore_index);

    /*
     * Call per core initialization functions
     */
    mem_lcore_init(lcore_id);
    trace_filter_lcore_init(lcore_id);
    timer_lcore_init(lcore_id);
    msg_sys_lcore_init(lcore_id);
    tsm_lcore_init(lcore_id);
    test_lcore_init(lcore_id);
    port_lcore_init(lcore_id);
    eth_lcore_init(lcore_id);
    arp_lcore_init(lcore_id);
    ipv4_lcore_init(lcore_id);
    tcp_lcore_init(lcore_id);
    udp_lcore_init(lcore_id);
    tlkp_tcp_lcore_init(lcore_id);
    tlkp_udp_lcore_init(lcore_id);
    route_lcore_init(lcore_id);
    raw_lcore_init(lcore_id);
    http_lcore_init(lcore_id);

    /*
     * Initialize local configuration. We will also potentially drop initial
     * control packets like ARPs.
     */
    RTE_PER_LCORE(pkt_send_simulate_drop_rate) = cfg->gcfg_pkt_send_drop_rate;

    /*
     * Get per core port stats pointer.
     */
    port_stats = STATS_LOCAL_NAME(port_statistics_t);
    if (port_stats == NULL) {
        TPG_ERROR_ABORT("Can't get port stats pointer on lcore %d, lindex %d\n",
                        lcore_id,
                        lcore_index);
    }

    /* Allocate commonly used variables from local socket memory. The pcb,
     * and core/port mapping information are used in every loop iteration.
     * Try to make it as fast as possible by packing the ports/queues we need
     * to poll (in local_ports and local_qs respectively).
     */
    pcb = rte_zmalloc_socket("local_pcb_pktloop", sizeof(*pcb),
                             RTE_CACHE_LINE_SIZE,
                             rte_lcore_to_socket_id(lcore_id));

    if (pcb == NULL)
        TPG_ERROR_ABORT("[%d] Cannot allocate pcb!\n", lcore_index);

    local_port_info = rte_zmalloc_socket("local_port_info_pktloop",
                                         rte_eth_dev_count() *
                                            sizeof(*local_port_info),
                                         RTE_CACHE_LINE_SIZE,
                                         rte_lcore_to_socket_id(lcore_id));

    if (local_port_info == NULL)
        TPG_ERROR_ABORT("[%d] Cannot allocate pktloop port info!\n",
                        lcore_index);

    local_port_count = 0;
    for (port = 0; port < rte_eth_dev_count(); port++) {
        queue_id = port_get_rx_queue_id(lcore_id, port);
        if (queue_id != CORE_PORT_QINVALID) {
            local_port_info_t *pi = &local_port_info[local_port_count];

            pi->lpi_port_id = port;
            pi->lpi_queue_id = queue_id;
            pi->lpi_port_info = &port_dev_info[port];
            local_port_count++;
        }
    }

    /*
     * Main processing loop...
     */

    while (!tpg_exit) {

        int              i;
        uint32_t         qidx;
        int              error;
        int              no_rx_buffers;
        struct rte_mbuf *buf[TPG_RX_BURST_SIZE];
        struct rte_mbuf *ret_mbuf;

        /* Check for the RTE timers too. There shouldn't be too many of them. */
        rte_timer_manage();

        /* Work some TPG timers. */
        time_advance();

        /* Poll for messages from other modules/cores. */
        error = msg_poll();
        if (error)
            RTE_LOG(ERR, USER1, "[%d:%s()] Failed to poll for messages: %s(%d)\n",
                    lcore_index, __func__,
                    rte_strerror(-error), -error);

        for (qidx = 0; qidx < local_port_count; qidx++) {
            port = local_port_info[qidx].lpi_port_id;
            queue_id = local_port_info[qidx].lpi_queue_id;

            no_rx_buffers = pkt_rx_burst(port, queue_id, buf, TPG_RX_BURST_SIZE,
                                         local_port_info[qidx].lpi_port_info,
                                         &port_stats[port]);
            if (unlikely(no_rx_buffers <= 0)) {
                /* Flush the bulk tx queue in case we have packets pending. */
                pkt_flush_tx_q(port, &port_stats[port]);
                continue;
            }

            for (i = 0; i < no_rx_buffers; i++) {
                /*
                 * setup PCB
                 */
                pcb_minimal_init(pcb, lcore_index, port, buf[i]);

                /*
                 * Hand off packet to ethernet driver, as we only support ethernet
                 */
                INC_STATS(&port_stats[port], ps_received_pkts);
                INC_STATS_VAL(&port_stats[port], ps_received_bytes,
                              rte_pktmbuf_pkt_len(buf[i]));

                PKT_TRACE(pcb, PKT_RX, DEBUG,
                          "port=%d qid=%d len=%u, ol_flags=0x%16.16"PRIX64", nb_segs=%u",
                          port,
                          queue_id,
                          rte_pktmbuf_pkt_len(buf[i]),
                          buf[i]->ol_flags,
                          buf[i]->nb_segs);

                PKT_TRACE(pcb, PKT_RX, DEBUG,
                          "  data_len=%u/%u, rss_hash=0x%8.8X",
                          rte_pktmbuf_data_len(buf[i]),
                          buf[i]->buf_len,
                          buf[i]->hash.rss);

                ret_mbuf = eth_receive_pkt(pcb, buf[i]);

                if (ret_mbuf != NULL)
                    rte_pktmbuf_free(ret_mbuf);
            }

            /* Flush the bulk tx queue in case we still have packets pending. */
            pkt_flush_tx_q(port, &port_stats[port]);
        }

    }

    return 0;
}

/*****************************************************************************
 * PktLoop Message Handlers.
 ****************************************************************************/
/*****************************************************************************
 * pkt_loop_init_wait_cb()
 ****************************************************************************/
static int pkt_loop_init_wait_cb(uint16_t msgid, uint16_t lcore __rte_unused,
                                 void *msg)
{
    if (MSG_INVALID(msgid, msg, MSG_PKTLOOP_INIT_WAIT))
        return -EINVAL;

    /* No need to do anything. The sender should just block until we're done
     * processing the message.
     */

    return 0;
}

/*****************************************************************************
 * pkt_handle_cmdline_opt()
 * --pkt-send-drop-rate - if set then one packet every 'pkt-send-drop-rate' will
 *      be dropped at TX. (per lcore)
 ****************************************************************************/
bool pkt_handle_cmdline_opt(const char *opt_name, char *opt_arg)
{
    global_config_t *cfg = cfg_get_config();

    if (!cfg)
        TPG_ERROR_ABORT("ERROR: Unable to get config!\n");

    if (strcmp(opt_name, "pkt-send-drop-rate") == 0) {
        cfg->gcfg_pkt_send_drop_rate = atoi(opt_arg);
        return true;
    }

    return false;
}

/*****************************************************************************
 * pkt_loop_init()
 ****************************************************************************/
bool pkt_loop_init(void)
{
    int error;

    /*
     * Register the handlers for our message types.
     */

    error = msg_register_handler(MSG_PKTLOOP_INIT_WAIT,
                                 pkt_loop_init_wait_cb);
    if (error) {
        RTE_LOG(ERR, USER1, "Failed to register PktLoop msg handler: %s(%d)\n",
                rte_strerror(-error), -error);
        return false;
    }

    return true;
}


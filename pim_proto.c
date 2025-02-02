/*
 * Copyright (c) 1998-2001
 * University of Southern California/Information Sciences Institute.
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
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
/*
 *  $Id: pim_proto.c,v 1.47 2003/05/28 22:57:16 pavlin Exp $
 */

#include <arpa/inet.h>
#include "defs.h"

/*
 * Local functions definitions.
 */
static int parse_pim_hello         (char *pim_message, size_t datalen, u_int32 src, u_int16 *holdtime);
static int send_pim_register_stop  (u_int32 reg_src, u_int32 reg_dst, u_int32 inner_source, u_int32 inner_grp);
static build_jp_message_t *get_jp_working_buff (void);
static void return_jp_working_buff (pim_nbr_entry_t *pim_nbr);
static void pack_jp_message        (pim_nbr_entry_t *pim_nbr);
static void send_jp_message        (pim_nbr_entry_t *pim_nbr);
static int compare_metrics         (u_int32 local_preference,
                                    u_int32 local_metric,
                                    u_int32 local_address,
                                    u_int32 remote_preference,
                                    u_int32 remote_metric,
                                    u_int32 remote_address);

build_jp_message_t *build_jp_message_pool;
int build_jp_message_pool_counter;

/************************************************************************
 *                        PIM_HELLO
 ************************************************************************/
int receive_pim_hello(u_int32 src, u_int32 dst __attribute__((unused)), char *pim_message, size_t datalen)
{
    vifi_t vifi;
    struct uvif *v;
    pim_nbr_entry_t *nbr, *prev_nbr, *new_nbr;
    u_int16 holdtime = 0;
    int     bsr_length;
    u_int8  *data_ptr __attribute__((unused));
    srcentry_t *srcentry_ptr;
    mrtentry_t *mrtentry_ptr;

    /* Checksum */
    if (inet_cksum((u_int16 *)pim_message, datalen))
        return FALSE;

    if ((vifi = find_vif_direct(src)) == NO_VIF) {
        /* Either a local vif or somehow received PIM_HELLO from
         * non-directly connected router. Ignore it.
         */
        if (local_address(src) == NO_VIF)
            logit(LOG_INFO, 0, "Ignoring PIM_HELLO from non-neighbor router %s",
                  inet_fmt(src, s1, sizeof(s1)));
        return FALSE;
    }

    v = &uvifs[vifi];
    if (v->uv_flags & (VIFF_DOWN | VIFF_DISABLED | VIFF_REGISTER))
        return FALSE;    /* Shoudn't come on this interface */

    data_ptr = (u_int8 *)(pim_message + sizeof(pim_header_t));

    /* Get the Holdtime (in seconds) from the message. Return if error. */
    if (parse_pim_hello(pim_message, datalen, src, &holdtime) == FALSE)
        return FALSE;

    IF_DEBUG(DEBUG_PIM_HELLO | DEBUG_PIM_TIMER) {
        logit(LOG_DEBUG, 0, "PIM HELLO holdtime from %s is %u",
              inet_fmt(src, s1, sizeof(s1)), holdtime);
    }

    for (prev_nbr = (pim_nbr_entry_t *)NULL, nbr = v->uv_pim_neighbors;
         nbr != (pim_nbr_entry_t *)NULL;
         prev_nbr = nbr, nbr = nbr->next) {
        /* The PIM neighbors are sorted in decreasing order of the
         * network addresses (note that to be able to compare them
         * correctly we must translate the addresses in host order.
         */
        if (ntohl(src) < ntohl(nbr->address))
            continue;

        if (src == nbr->address) {
            /* We already have an entry for this host */
            if (0 == holdtime) {
                /* Looks like we have a nice neighbor who is going down
                 * and wants to inform us by sending "holdtime=0". Thanks
                 * buddy and see you again!
                 */
                logit(LOG_INFO, 0, "PIM HELLO received: neighbor %s going down",
                      inet_fmt(src, s1, sizeof(s1)));
                delete_pim_nbr(nbr);

                return TRUE;
            }
            SET_TIMER(nbr->timer, holdtime);

            return TRUE;
        }
        else {
            /*
             * No entry for this neighbor. Exit the loop and create an
             * entry for it.
             */
            break;
	}
    }

    /*
     * This is a new neighbor. Create a new entry for it.
     * It must be added right after `prev_nbr`
     */
    new_nbr = (pim_nbr_entry_t *)calloc(1, sizeof(pim_nbr_entry_t));
    if (!new_nbr)
	logit(LOG_ERR, 0, "Ran out of memory in receive_pim_hello()");
    new_nbr->address          = src;
    new_nbr->vifi             = vifi;
    SET_TIMER(new_nbr->timer, holdtime);
    new_nbr->build_jp_message = (build_jp_message_t *)NULL;
    new_nbr->next             = nbr;
    new_nbr->prev             = prev_nbr;

    if (prev_nbr != (pim_nbr_entry_t *)NULL)
        prev_nbr->next  = new_nbr;
    else
        v->uv_pim_neighbors = new_nbr;
    if (new_nbr->next != (pim_nbr_entry_t *)NULL)
        new_nbr->next->prev = new_nbr;

    v->uv_flags &= ~VIFF_NONBRS;
    v->uv_flags |= VIFF_PIM_NBR;

    /* Since a new neighbour has come up, let it know your existence */
    /* XXX: TODO: not in the spec,
     * but probably should send the message after a short random period?
     */
    send_pim_hello(v, PIM_TIMER_HELLO_HOLDTIME);

    if (v->uv_flags & VIFF_DR) {
        /*
         * If I am the current DR on that interface, so
         * send an RP-Set message to the new neighbor.
         */
        if ((bsr_length = create_pim_bootstrap_message(pim_send_buf)))
            send_pim_unicast(pim_send_buf, v->uv_lcl_addr, src, PIM_BOOTSTRAP, bsr_length);

        /* The router with highest network address is the elected DR */
        if (ntohl(v->uv_lcl_addr) < ntohl(src)) {
            /* I was the DR, but not anymore. Remove all register_vif from
             * oif list for all directly connected sources (for vifi).
             */
            /* TODO: XXX: first entry is not used! */
            for (srcentry_ptr = srclist->next;
                 srcentry_ptr != (srcentry_t *)NULL;
                 srcentry_ptr = srcentry_ptr->next) {

                /* If not directly connected source for vifi */
                if ((srcentry_ptr->incoming != vifi)
                    || (srcentry_ptr->upstream != (pim_nbr_entry_t *)NULL))
                    continue;

                for (mrtentry_ptr = srcentry_ptr->mrtlink;
                     mrtentry_ptr != (mrtentry_t *)NULL;
                     mrtentry_ptr = mrtentry_ptr->srcnext) {

                    if (!(mrtentry_ptr->flags & MRTF_SG))
                        continue;  /* This is not (S,G) entry */

                    /* Remove the register oif */
                    VIFM_CLR(reg_vif_num, mrtentry_ptr->joined_oifs);
                    change_interfaces(mrtentry_ptr,
                                      mrtentry_ptr->incoming,
                                      mrtentry_ptr->joined_oifs,
                                      mrtentry_ptr->pruned_oifs,
                                      mrtentry_ptr->leaves,
                                      mrtentry_ptr->asserted_oifs, 0);
                }
            }
            v->uv_flags &= ~VIFF_DR;
        }
    }

    /*
     * TODO: XXX: does a new neighbor change any routing entries info?
     * Need to trigger joins?
     */

    IF_DEBUG(DEBUG_PIM_HELLO)
        dump_vifs(stderr);      /* Show we got a new neighbor */

    return TRUE;
}


void delete_pim_nbr(pim_nbr_entry_t *nbr_delete)
{
    srcentry_t *srcentry_ptr;
    srcentry_t *srcentry_ptr_next;
    mrtentry_t *mrtentry_ptr;
    mrtentry_t *mrtentry_srcs;
    grpentry_t *grpentry_ptr;
    pim_nbr_entry_t *new_nbr __attribute__((unused));
    cand_rp_t *cand_rp_ptr;
    rp_grp_entry_t *rp_grp_entry_ptr;
    rpentry_t  *rpentry_ptr;
    struct uvif *v;

    v = &uvifs[nbr_delete->vifi];

    /* Delete the entry from the pim_nbrs chain */
    if (nbr_delete->prev != (pim_nbr_entry_t *)NULL)
        nbr_delete->prev->next = nbr_delete->next;
    else
        v->uv_pim_neighbors = nbr_delete->next;
    if (nbr_delete->next != (pim_nbr_entry_t *)NULL)
        nbr_delete->next->prev = nbr_delete->prev;

    return_jp_working_buff(nbr_delete);

    if (v->uv_pim_neighbors == (pim_nbr_entry_t *)NULL) {
        /* This was our last neighbor. */
        v->uv_flags &= ~VIFF_PIM_NBR;
        v->uv_flags |= (VIFF_NONBRS | VIFF_DR);
    }
    else {
        if (ntohl(v->uv_lcl_addr) >
            ntohl(v->uv_pim_neighbors->address))
            /* The first address is the new potential remote
             * DR address, but the local address is the winner.
             */
            v->uv_flags |= VIFF_DR;
    }

    /* Update the source entries */
    for (srcentry_ptr = srclist; srcentry_ptr != (srcentry_t *)NULL;
         srcentry_ptr = srcentry_ptr_next) {
        srcentry_ptr_next = srcentry_ptr->next;

        if (srcentry_ptr->upstream != nbr_delete)
            continue;

        /* Reset the next hop (PIM) router */
        if (set_incoming(srcentry_ptr, PIM_IIF_SOURCE) == FALSE) {
            /* Coudn't reset it. Sorry, the hext hop router toward that
             * source is probably not a PIM router, or cannot find route
             * at all, hence I cannot handle this source and have to
             * delete it.
             */
            delete_srcentry(srcentry_ptr);
        }
        else if (srcentry_ptr->upstream != (pim_nbr_entry_t *)NULL) {
            /* Ignore the local or directly connected sources */
            /* Browse all MRT entries for this source and reset the
             * upstream router. Note that the upstream router is not always
             * toward the source: it could be toward the RP for example.
             */
            new_nbr = srcentry_ptr->upstream;
            for (mrtentry_ptr = srcentry_ptr->mrtlink;
                 mrtentry_ptr != (mrtentry_t *)NULL;
                 mrtentry_ptr = mrtentry_ptr->srcnext) {
                if (!(mrtentry_ptr->flags & MRTF_RP)) {
                    mrtentry_ptr->upstream   = srcentry_ptr->upstream;
                    mrtentry_ptr->metric     = srcentry_ptr->metric;
                    mrtentry_ptr->preference = srcentry_ptr->preference;
                    change_interfaces(mrtentry_ptr, srcentry_ptr->incoming,
                                      mrtentry_ptr->joined_oifs,
                                      mrtentry_ptr->pruned_oifs,
                                      mrtentry_ptr->leaves,
                                      mrtentry_ptr->asserted_oifs, 0);
                }
            }
        }
    }

    /* Update the RP entries */
    for (cand_rp_ptr = cand_rp_list; cand_rp_ptr != (cand_rp_t *)NULL;
         cand_rp_ptr = cand_rp_ptr->next) {
        if (cand_rp_ptr->rpentry->upstream != nbr_delete)
            continue;
        rpentry_ptr = cand_rp_ptr->rpentry;
        /* Reset the RP entry iif */
        /* TODO: check if error setting the iif! */
        if (local_address(rpentry_ptr->address) == NO_VIF)
            set_incoming(rpentry_ptr, PIM_IIF_RP);
        else {
            rpentry_ptr->incoming = reg_vif_num;
            rpentry_ptr->upstream = (pim_nbr_entry_t *)NULL;
        }
        mrtentry_ptr = rpentry_ptr->mrtlink;
        if (mrtentry_ptr != (mrtentry_t *)NULL) {
            mrtentry_ptr->upstream   = rpentry_ptr->upstream;
            mrtentry_ptr->metric     = rpentry_ptr->metric;
            mrtentry_ptr->preference = rpentry_ptr->preference;
            change_interfaces(mrtentry_ptr,
                              rpentry_ptr->incoming,
                              mrtentry_ptr->joined_oifs,
                              mrtentry_ptr->pruned_oifs,
                              mrtentry_ptr->leaves,
                              mrtentry_ptr->asserted_oifs, 0);
        }
        /* Update the group entries for this RP */
        for (rp_grp_entry_ptr = cand_rp_ptr->rp_grp_next;
             rp_grp_entry_ptr != (rp_grp_entry_t *)NULL;
             rp_grp_entry_ptr = rp_grp_entry_ptr->rp_grp_next) {
            for (grpentry_ptr = rp_grp_entry_ptr->grplink;
                 grpentry_ptr != (grpentry_t *)NULL;
                 grpentry_ptr = grpentry_ptr->rpnext) {
                mrtentry_ptr = grpentry_ptr->grp_route;
                if (mrtentry_ptr != (mrtentry_t *)NULL) {
                    mrtentry_ptr->upstream = rpentry_ptr->upstream;
                    mrtentry_ptr->metric     = rpentry_ptr->metric;
                    mrtentry_ptr->preference = rpentry_ptr->preference;
                    change_interfaces(mrtentry_ptr,
                                      rpentry_ptr->incoming,
                                      mrtentry_ptr->joined_oifs,
                                      mrtentry_ptr->pruned_oifs,
                                      mrtentry_ptr->leaves,
                                      mrtentry_ptr->asserted_oifs, 0);
                }
                /* Update only the (S,G)RPbit entries for this group */
                for (mrtentry_srcs = grpentry_ptr->mrtlink;
                     mrtentry_srcs != (mrtentry_t *)NULL;
                     mrtentry_srcs = mrtentry_srcs->grpnext) {
                    if (mrtentry_srcs->flags & MRTF_RP) {
                        mrtentry_ptr->upstream = rpentry_ptr->upstream;
                        mrtentry_ptr->metric = rpentry_ptr->metric;
                        mrtentry_ptr->preference = rpentry_ptr->preference;
                        change_interfaces(mrtentry_srcs,
                                          rpentry_ptr->incoming,
                                          mrtentry_srcs->joined_oifs,
                                          mrtentry_srcs->pruned_oifs,
                                          mrtentry_srcs->leaves,
                                          mrtentry_srcs->asserted_oifs, 0);
                    }
                }
            }
        }
    }

    free((char *)nbr_delete);
}


/* TODO: simplify it! */
static int parse_pim_hello(char *pim_message, size_t datalen, u_int32 src, u_int16 *holdtime)
{
    u_int8 *pim_hello_message;
    u_int8 *data_ptr;
    u_int16 option_type;
    u_int16 option_length;
    int holdtime_received_ok = FALSE;
    int option_total_length;

    pim_hello_message = (u_int8 *)(pim_message + sizeof(pim_header_t));
    datalen -= sizeof(pim_header_t);
    for ( ; datalen >= sizeof(pim_hello_t); ) {
        /* Ignore any data if shorter than (pim_hello header) */
        data_ptr = pim_hello_message;
        GET_HOSTSHORT(option_type, data_ptr);
        GET_HOSTSHORT(option_length, data_ptr);

        switch (option_type) {
        case PIM_MESSAGE_HELLO_HOLDTIME:
            if (PIM_MESSAGE_HELLO_HOLDTIME_LENGTH != option_length) {
                IF_DEBUG(DEBUG_PIM_HELLO) {
                    logit(LOG_DEBUG, 0, "PIM HELLO HOldtime from %s: invalid OptionLength = %u",
                            inet_fmt(src, s1, sizeof(s1)), option_length);
                }

                return FALSE;
            }

	    GET_HOSTSHORT(*holdtime, data_ptr);
	    holdtime_received_ok = TRUE;
	    break;
        default:
            /* Ignore any unknown options */
	    break;
        }

        /* Move to the next option */
        /* XXX: TODO: If we are padding to the end of the 32 bit boundary,
         * use the first method to move to the next option, otherwise
         * simply (sizeof(pim_hello_t) + option_length).
         */
#ifdef BOUNDARY_32_BIT
        option_total_length = (sizeof(pim_hello_t) + (option_length & ~0x3) +
                               ((option_length & 0x3) ? 4 : 0));
#else
        option_total_length = (sizeof(pim_hello_t) + option_length);
#endif /* BOUNDARY_32_BIT */

        datalen -= option_total_length;
        pim_hello_message += option_total_length;
    }

    return holdtime_received_ok;
}


int send_pim_hello(struct uvif *v, u_int16 holdtime)
{
    char   *buf;
    u_int8 *data_ptr;
    int     datalen;

    buf = pim_send_buf + sizeof(struct ip) + sizeof(pim_header_t);
    data_ptr = (u_int8 *)buf;
    PUT_HOSTSHORT(PIM_MESSAGE_HELLO_HOLDTIME, data_ptr);
    PUT_HOSTSHORT(PIM_MESSAGE_HELLO_HOLDTIME_LENGTH, data_ptr);
    PUT_HOSTSHORT(holdtime, data_ptr);

    datalen = data_ptr - (u_int8 *)buf;
    send_pim(pim_send_buf, v->uv_lcl_addr, allpimrouters_group, PIM_HELLO, datalen);
    SET_TIMER(v->uv_pim_hello_timer, PIM_TIMER_HELLO_PERIOD);

    return TRUE;
}


/************************************************************************
 *                        PIM_REGISTER
 ************************************************************************/
/* TODO: XXX: IF THE BORDER BIT IS SET, THEN
 * FORWARD THE WHOLE PACKET FROM USER SPACE
 * AND AT THE SAME TIME IGNORE ANY CACHE_MISS
 * SIGNALS FROM THE KERNEL.
 */
int receive_pim_register(u_int32 reg_src, u_int32 reg_dst, char *pim_message, size_t datalen)
{
    u_int32 inner_src, inner_grp;
    pim_register_t *register_p;
    struct ip *ip;
    u_int32 borderBit, nullRegisterBit;
    mrtentry_t *mrtentry_ptr;
    mrtentry_t *mrtentry_ptr2;
    vifbitmap_t oifs;

    /*
     * Message length validation.
     * This is suppose to be done in the kernel, but some older kernel
     * versions do not pefrorm the check for the NULL register messages.
     */
    if (datalen < sizeof(pim_header_t) + sizeof(pim_register_t) + sizeof(struct ip)) {
        IF_DEBUG(DEBUG_PIM_REGISTER) {
            logit(LOG_INFO, 0, "PIM register: short packet (len = %d) from %s",
                  datalen, inet_fmt(reg_src, s1, sizeof(s1)));
	}

        return FALSE;
    }

    /*
     * XXX: For PIM_REGISTER the checksum does not include
     * the inner IP packet. However, some older routers might
     * create the checksum over the whole packet. Hence,
     * verify the checksum over the first 8 bytes, and if fails,
     * then over the whole Register
     */
    if ((inet_cksum((u_int16 *)pim_message, sizeof(pim_header_t) + sizeof(pim_register_t)))
        && (inet_cksum((u_int16 *)pim_message, datalen))) {
        IF_DEBUG(DEBUG_PIM_REGISTER) {
            logit(LOG_DEBUG, 0, "PIM REGISTER from DR %s: invalid PIM header checksum",
                  inet_fmt(reg_src, s1, sizeof(s1)));
	}

        return FALSE;
    }

    register_p = (pim_register_t *)(pim_message + sizeof(pim_header_t));
    borderBit       = ntohl(register_p->reg_flags) & PIM_MESSAGE_REGISTER_BORDER_BIT;
    nullRegisterBit = ntohl(register_p->reg_flags) & PIM_MESSAGE_REGISTER_NULL_REGISTER_BIT;

    /* initialize the pointer to the encapsulated packet */
    ip = (struct ip *)(register_p + 1);

    /* check the IP version (especially for the NULL register...see above) */
    if (ip->ip_v != IPVERSION && (! nullRegisterBit)) {
        IF_DEBUG(DEBUG_PIM_REGISTER)
            logit(LOG_INFO, 0, "PIM register: incorrect IP version (%d) of the inner packet from %s",
                  ip->ip_v, inet_fmt(reg_src, s1, sizeof(s1)));

        return FALSE;
    }

    /* We are keeping all addresses in network order, so no need for ntohl()*/
    inner_src = ip->ip_src.s_addr;
    inner_grp = ip->ip_dst.s_addr;

    /*
     * inner_src and inner_grp must be valid IP unicast and multicast address
     * respectively. XXX: not in the spec.
     */
    if ((!inet_valid_host(inner_src)) || (!IN_MULTICAST(ntohl(inner_grp)))) {
        if (!inet_valid_host(inner_src)) {
            logit(LOG_WARNING, 0, "Inner source address of register message by %s is invalid: %s",
                  inet_fmt(reg_src, s1, sizeof(s1)), inet_fmt(inner_src, s2, sizeof(s2)));
        }
        if (!IN_MULTICAST(ntohl(inner_grp))) {
            logit(LOG_WARNING, 0, "Inner group address of register message by %s is invalid: %s",
                  inet_fmt(reg_src, s1, sizeof(s1)), inet_fmt(inner_grp, s2, sizeof(s2)));
        }
        send_pim_register_stop(reg_dst, reg_src, inner_grp, inner_src);

        return FALSE;
    }

    mrtentry_ptr = find_route(inner_src, inner_grp, MRTF_SG | MRTF_WC | MRTF_PMBR, DONT_CREATE);
    if (!mrtentry_ptr) {
        /* No routing entry. Send REGISTER_STOP and return. */
        IF_DEBUG(DEBUG_PIM_REGISTER) {
            logit(LOG_DEBUG, 0, "No routing entry for source %s and/or group %s" ,
                  inet_fmt(inner_src, s1, sizeof(s1)), inet_fmt(inner_grp, s2, sizeof(s2)));
	}
        /* TODO: XXX: shoudn't be inner_src=INADDR_ANY? Not in the spec. */
        send_pim_register_stop(reg_dst, reg_src, inner_grp, inner_src);

        return TRUE;
    }

    /* XXX: not in the spec: check if I am the RP for that group */
    if ((local_address(reg_dst) == NO_VIF) ||
        (check_mrtentry_rp(mrtentry_ptr, reg_dst) == FALSE)) {
        send_pim_register_stop(reg_dst, reg_src, inner_grp, inner_src);

        return TRUE;
    }

    if (mrtentry_ptr->flags & MRTF_SG) {
        /* (S,G) found */
        /* TODO: check the timer again */
        SET_TIMER(mrtentry_ptr->timer, PIM_DATA_TIMEOUT); /* restart timer */
        if (!(mrtentry_ptr->flags & MRTF_SPT)) { /* The SPT bit is not set */
            if (!nullRegisterBit) {
                calc_oifs(mrtentry_ptr, &oifs);
                if (VIFM_ISEMPTY(oifs) && (mrtentry_ptr->incoming == reg_vif_num)) {
                    send_pim_register_stop(reg_dst, reg_src, inner_grp, inner_src);
                    return TRUE;
                }

                /*
                 * TODO: XXX: BUG!!!
                 * The data will be forwarded by the kernel MFC!!!
                 * Need to set a special flag for this routing entry so after
                 * a cache miss occur, the multicast packet will be forwarded
                 * from user space and won't install entry in the kernel MFC.
                 * The problem is that the kernel MFC doesn't know the
                 * PMBR address and simply sets the multicast forwarding
                 * cache to accept/forward all data coming from the
                 * register_vif.
                 */
                if (borderBit) {
                    if (mrtentry_ptr->pmbr_addr != reg_src) {
                        send_pim_register_stop(reg_dst, reg_src, inner_grp, inner_src);

                        return TRUE;
                    }
                }

                return TRUE;
            }

            /* TODO: XXX: if NULL_REGISTER and has (S,G) with SPT=0, then..?*/
            return TRUE;
        }
        else {
            /* The SPT bit is set */
            send_pim_register_stop(reg_dst, reg_src, inner_grp, inner_src);
            return TRUE;
        }
    }
    if (mrtentry_ptr->flags & (MRTF_WC | MRTF_PMBR)) {
        if (borderBit) {
            /* Create (S,G) state. The oifs will be the copied from the
             * existing (*,G) or (*,*,RP) entry. */
            mrtentry_ptr2 = find_route(inner_src, inner_grp, MRTF_SG, CREATE);
            if (mrtentry_ptr2) {
                mrtentry_ptr2->pmbr_addr = reg_src;
                /* Clear the SPT flag */
                mrtentry_ptr2->flags &= ~(MRTF_SPT | MRTF_NEW);
                SET_TIMER(mrtentry_ptr2->timer, PIM_DATA_TIMEOUT);
                /* TODO: explicitly call the Join/Prune send function? */
                FIRE_TIMER(mrtentry_ptr2->jp_timer); /* Send the Join immediately */
                /* TODO: explicitly call this function?
                   send_pim_join_prune(mrtentry_ptr2->upstream->vifi,
                   mrtentry_ptr2->upstream,
                   PIM_JOIN_PRUNE_HOLDTIME);
                */
            }
        }
    }

    if (mrtentry_ptr->flags & MRTF_WC) {
        /* (*,G) entry */
        calc_oifs(mrtentry_ptr, &oifs);
        if (VIFM_ISEMPTY(oifs)) {
            send_pim_register_stop(reg_dst, reg_src, inner_grp, INADDR_ANY_N);

            return FALSE;
        }
        /* XXX: TODO: check with the spec again */
        else {
            if (!nullRegisterBit) {
                /* Install cache entry in the kernel */
                /* TODO: XXX: probably redundant here, because the
                 * decapsulated mcast packet in the kernel will
                 * result in CACHE_MISS
                 */
                u_int32 mfc_source = inner_src;
#ifdef KERNEL_MFC_WC_G
                if (!(mrtentry_ptr->flags & MRTF_MFC_CLONE_SG))
                    mfc_source = INADDR_ANY_N;
#endif /* KERNEL_MFC_WC_G */
                add_kernel_cache(mrtentry_ptr, mfc_source, inner_grp, 0);
                k_chg_mfc(igmp_socket, mfc_source, inner_grp,
                          mrtentry_ptr->incoming, mrtentry_ptr->oifs,
                          mrtentry_ptr->group->rpaddr);

                return TRUE;
            }
        }

        return TRUE;
    }

    if (mrtentry_ptr->flags & MRTF_PMBR) {
        /* (*,*,RP) entry */
        if (!nullRegisterBit) {
            u_int32 mfc_source = inner_src;
            /* XXX: have to create either (S,G) or (*,G).
             * The choice below is (*,G)
             */
            mrtentry_ptr2 = find_route(INADDR_ANY_N, inner_grp, MRTF_WC, CREATE);
            if (!mrtentry_ptr2)
                return FALSE;
            if (mrtentry_ptr2->flags & MRTF_NEW) {
                /* TODO: something else? Have the feeling sth is missing */
                mrtentry_ptr2->flags &= ~MRTF_NEW;
                /* TODO: XXX: copy the timer from the (*,*,RP) entry? */
                COPY_TIMER(mrtentry_ptr->timer, mrtentry_ptr2->timer);
            }
            /* Install cache entry in the kernel */
#ifdef KERNEL_MFC_WC_G
            if (!(mrtentry_ptr->flags & MRTF_MFC_CLONE_SG))
                mfc_source = INADDR_ANY_N;
#endif /* KERNEL_MFC_WC_G */
            add_kernel_cache(mrtentry_ptr, mfc_source, inner_grp, 0);
            k_chg_mfc(igmp_socket, mfc_source, inner_grp,
                      mrtentry_ptr->incoming, mrtentry_ptr->oifs,
                      mrtentry_ptr2->group->rpaddr);

            return TRUE;
        }
    }

    /* Shoudn't happen: invalid routing entry? */
    /* XXX: TODO: shoudn't be inner_src=INADDR_ANY? Not in the spec. */
    send_pim_register_stop(reg_dst, reg_src, inner_grp, inner_src);

    return TRUE;
}


int send_pim_register(char *packet)
{
    struct ip  *ip;
    u_int32     source, group;
    vifi_t	vifi;
    rpentry_t  *rpentry_ptr;
    mrtentry_t *mrtentry_ptr;
    mrtentry_t *mrtentry_ptr2;
    u_int32     reg_src, reg_dst;
    int         pktlen = 0;
    char       *buf;

    ip = (struct ip *)packet;
    source = ip->ip_src.s_addr;
    group = ip->ip_dst.s_addr;

    if ((vifi = find_vif_direct_local(source)) == NO_VIF)
        return FALSE;

    if (!(uvifs[vifi].uv_flags & VIFF_DR))
        return FALSE;		/* I am not the DR for that subnet */

    rpentry_ptr = rp_match(group);
    if (!rpentry_ptr)
        return FALSE;		/* No RP for this group */

    if (local_address(rpentry_ptr->address) != NO_VIF) {
        /* TODO: XXX: not sure it is working! */
        return FALSE;		/* I am the RP for this group */
    }

    mrtentry_ptr = find_route(source, group, MRTF_SG, CREATE);
    if (!mrtentry_ptr)
        return FALSE;		/* Cannot create (S,G) state */

    if (mrtentry_ptr->flags & MRTF_NEW) {
        /* A new entry */
        mrtentry_ptr->flags &= ~MRTF_NEW;
        RESET_TIMER(mrtentry_ptr->rs_timer); /* Reset the Register-Suppression timer */
	mrtentry_ptr2 = mrtentry_ptr->group->grp_route;
        if (!mrtentry_ptr2)
            mrtentry_ptr2 = mrtentry_ptr->group->active_rp_grp->rp->rpentry->mrtlink;
        if (mrtentry_ptr2) {
            FIRE_TIMER(mrtentry_ptr2->jp_timer); /* Timeout the Join/Prune timer */
            /* TODO: explicitly call this function?
               send_pim_join_prune(mrtentry_ptr2->upstream->vifi,
               mrtentry_ptr2->upstream,
               PIM_JOIN_PRUNE_HOLDTIME);
            */
        }
    }
    /* Restart the (S,G) Entry-timer */
    SET_TIMER(mrtentry_ptr->timer, PIM_DATA_TIMEOUT);

    IF_TIMER_NOT_SET(mrtentry_ptr->rs_timer) {
        /* The Register-Suppression Timer is not running.
         * Encapsulate the data and send to the RP.
         */

#ifdef PIM_OLD_KERNEL
        /* TODO: verify again! */
        /* recompute the checksum; as checksum may fail when decapsulating
         * at the RP if we don't do this.
         */
#ifdef old_Linux
        ip->ip_csum = 0;
#else
        ip->ip_sum = 0;
#endif /* old_Linux */
#ifndef RAW_INPUT_IS_RAW
        /* TODO: htons??? */
        ip->ip_len = htons(ip->ip_len);
        ip->ip_off = htons(ip->ip_off);
#endif /* !RAW_INPUT_IS_RAW */

#ifdef old_Linux
        ip->ip_csum = inet_cksum((u_int16 *)ip, ip->ip_hl << 2);
#else
        ip->ip_sum = inet_cksum((u_int16 *)ip, ip->ip_hl << 2);
#endif /* old_Linux */

#endif /* PIM_OLD_KERNEL */

        buf = pim_send_buf + sizeof(struct ip) + sizeof(pim_header_t);
        memset(buf, 0, sizeof(pim_register_t)); /* No flags set */
        buf += sizeof(pim_register_t);

        /* Copy the data packet at the back of the register packet */
        /* TODO: check pktlen. ntohs? */
        pktlen = ntohs(ip->ip_len);
        memcpy(buf, ip, pktlen);
        pktlen += sizeof(pim_register_t);
        reg_src = uvifs[vifi].uv_lcl_addr;
        reg_dst = mrtentry_ptr->group->rpaddr;
        send_pim_unicast(pim_send_buf, reg_src, reg_dst,
                         PIM_REGISTER, pktlen);

        return TRUE;
    }

    return TRUE;
}


int send_pim_null_register(mrtentry_t *mrtentry_ptr)
{
    struct ip *ip;
    pim_register_t *pim_register;
    int pktlen =0;
    vifi_t vifi;
    u_int32 reg_source, dest;

    /* No directly connected source; no local address */
    if ((vifi = find_vif_direct_local(mrtentry_ptr->source->address))== NO_VIF)
        return FALSE;

    pim_register = (pim_register_t *)(pim_send_buf + sizeof(struct ip) +
                                      sizeof(pim_header_t));
    memset(pim_register, 0, sizeof(pim_register_t));
    pim_register->reg_flags = htonl(pim_register->reg_flags
                                    | PIM_MESSAGE_REGISTER_NULL_REGISTER_BIT);

    ip = (struct ip *)(pim_register + 1);
    /* set src/dst in dummy hdr */
    ip->ip_v     = IPVERSION;
    ip->ip_hl    = (sizeof(struct ip) >> 2);
    ip->ip_tos   = 0;
    ip->ip_id    = 0;
    ip->ip_off   = 0;
    ip->ip_p     = IPPROTO_UDP;			/* XXX: bogus */
    ip->ip_len   = htons(sizeof(struct ip));
    ip->ip_ttl   = MINTTL; /* TODO: XXX: check whether need to setup the ttl */
    ip->ip_src.s_addr = mrtentry_ptr->source->address;
    ip->ip_dst.s_addr = mrtentry_ptr->group->group;
#ifdef old_Linux
    ip->ip_csum   = 0;
    ip->ip_csum   = inet_cksum((u_int16 *)ip, sizeof(struct ip));
#else
    ip->ip_sum   = 0;
    ip->ip_sum   = inet_cksum((u_int16 *)ip, sizeof(struct ip));
#endif /* old_Linux */

    /* include the dummy ip header */
    pktlen = sizeof(pim_register_t) + sizeof(struct ip);

    dest = mrtentry_ptr->group->rpaddr;
    reg_source = uvifs[vifi].uv_lcl_addr;

    send_pim_unicast(pim_send_buf, reg_source, dest, PIM_REGISTER, pktlen);

    return TRUE;
}


/************************************************************************
 *                        PIM_REGISTER_STOP
 ************************************************************************/
int receive_pim_register_stop(u_int32 reg_src, u_int32 reg_dst, char *pim_message, size_t datalen)
{
    pim_register_stop_t *pim_regstop_p;
    pim_encod_grp_addr_t encod_grp;
    pim_encod_uni_addr_t encod_unisrc;
    u_int8 *data_ptr;
    mrtentry_t *mrtentry_ptr;
    vifbitmap_t pruned_oifs;

    /* Checksum */
    if (inet_cksum((u_int16 *)pim_message, datalen))
        return FALSE;

    pim_regstop_p = (pim_register_stop_t *)(pim_message +
                                            sizeof(pim_header_t));
    data_ptr = (u_int8 *)&pim_regstop_p->encod_grp;
    GET_EGADDR(&encod_grp, data_ptr);
    GET_EUADDR(&encod_unisrc, data_ptr);

    IF_DEBUG(DEBUG_PIM_REGISTER) {
        logit(LOG_DEBUG, 0, "Received PIM_REGISTER_STOP from RP %s to %s for src = %s and group = %s", inet_fmt(reg_src, s1, sizeof(s1)), inet_fmt(reg_dst, s2, sizeof(s2)),
              inet_fmt(encod_unisrc.unicast_addr, s3, sizeof(s3)),
              inet_fmt(encod_grp.mcast_addr, s4, sizeof(s4)));
    }

    /* TODO: apply the group mask and do register_stop for all grp addresses */
    /* TODO: check for SourceAddress == 0 */
    mrtentry_ptr = find_route(encod_unisrc.unicast_addr, encod_grp.mcast_addr, MRTF_SG, DONT_CREATE);
    if (!mrtentry_ptr)
        return FALSE;

    /* XXX: not in the spec: check if the PIM_REGISTER_STOP originator is
     * really the RP
     */
    if (check_mrtentry_rp(mrtentry_ptr, reg_src) == FALSE)
        return FALSE;

    /* restart the Register-Suppression timer */
    SET_TIMER(mrtentry_ptr->rs_timer, (0.5 * PIM_REGISTER_SUPPRESSION_TIMEOUT)
              + (RANDOM() % (PIM_REGISTER_SUPPRESSION_TIMEOUT + 1)));
    /* Prune the register_vif from the outgoing list */
    VIFM_COPY(mrtentry_ptr->pruned_oifs, pruned_oifs);
    VIFM_SET(reg_vif_num, pruned_oifs);
    change_interfaces(mrtentry_ptr, mrtentry_ptr->incoming,
                      mrtentry_ptr->joined_oifs, pruned_oifs,
                      mrtentry_ptr->leaves,
                      mrtentry_ptr->asserted_oifs, 0);

    return TRUE;
}


/* TODO: optional rate limiting is not implemented yet */
/* Unicasts a REGISTER_STOP message to the DR */
static int
send_pim_register_stop(u_int32 reg_src, u_int32 reg_dst, u_int32 inner_grp, u_int32 inner_src)
{
    char   *buf;
    u_int8 *data_ptr;

    buf      = pim_send_buf + sizeof(struct ip) + sizeof(pim_header_t);
    data_ptr = (u_int8 *)buf;
    PUT_EGADDR(inner_grp, SINGLE_GRP_MSKLEN, 0, data_ptr);
    PUT_EUADDR(inner_src, data_ptr);
    send_pim_unicast(pim_send_buf, reg_src, reg_dst, PIM_REGISTER_STOP, data_ptr - (u_int8 *)buf);

    return TRUE;
}


/************************************************************************
 *                        PIM_JOIN_PRUNE
 ************************************************************************/
int join_or_prune(mrtentry_t *mrtentry_ptr, pim_nbr_entry_t *upstream_router)
{
    vifbitmap_t entry_oifs;
    mrtentry_t *mrtentry_grp;

    if (!mrtentry_ptr || !upstream_router)
        return PIM_ACTION_NOTHING;

    calc_oifs(mrtentry_ptr, &entry_oifs);
    if (mrtentry_ptr->flags & (MRTF_PMBR | MRTF_WC)) {
        /* (*,*,RP) or (*,G) entry */
        /* The (*,*,RP) or (*,G) J/P messages are sent only toward the RP */
        if (upstream_router != mrtentry_ptr->upstream)
            return PIM_ACTION_NOTHING;

        /* TODO: XXX: Can we have (*,*,RP) prune? */
        if (VIFM_ISEMPTY(entry_oifs)) {
            /* NULL oifs */

            if (!(uvifs[mrtentry_ptr->incoming].uv_flags & VIFF_DR))
                /* I am not the DR for that subnet. */
                return PIM_ACTION_PRUNE;

            if (VIFM_ISSET(mrtentry_ptr->incoming, mrtentry_ptr->leaves))
                /* I am the DR and have local leaves */
                return PIM_ACTION_JOIN;

            /* Probably the last local member hast timeout */
            return PIM_ACTION_PRUNE;
        }

        return PIM_ACTION_JOIN;
    }

    if (mrtentry_ptr->flags & MRTF_SG) {
        /* (S,G) entry */
        /* TODO: check again */
        if (mrtentry_ptr->upstream == upstream_router) {
            if (!(mrtentry_ptr->flags & MRTF_RP)) {
                /* Upstream router toward S */
                if (VIFM_ISEMPTY(entry_oifs)) {
                    if (mrtentry_ptr->group->active_rp_grp &&
			mrtentry_ptr->group->rpaddr == my_cand_rp_address) {
                        /* (S,G) at the RP. Don't send Join/Prune
                         * (see the end of Section 3.3.2)
                         */
                        return PIM_ACTION_NOTHING;
                    }

                    return PIM_ACTION_PRUNE;
                }
                else {
                    return PIM_ACTION_JOIN;
		}
            }
            else {
                /* Upstream router toward RP */
                if (VIFM_ISEMPTY(entry_oifs))
                    return PIM_ACTION_PRUNE;
            }
        }

        /* Looks like the case when the upstream router toward S is
         * different from the upstream router toward RP
         */
        if (!mrtentry_ptr->group->active_rp_grp)
            return PIM_ACTION_NOTHING;

        mrtentry_grp = mrtentry_ptr->group->grp_route;
        if (!mrtentry_grp) {
            mrtentry_grp = mrtentry_ptr->group->active_rp_grp->rp->rpentry->mrtlink;
	    if (!mrtentry_grp)
		return PIM_ACTION_NOTHING;
	}

        if (mrtentry_grp->upstream != upstream_router)
            return PIM_ACTION_NOTHING; /* XXX: shoudn't happen */

        if (!(mrtentry_ptr->flags & MRTF_RP) && (mrtentry_ptr->flags & MRTF_SPT))
            return PIM_ACTION_PRUNE;
    }

    return PIM_ACTION_NOTHING;
}


/* TODO: when parsing, check if we go beyong message size */
/* TODO: too long, simplify it! */
#define PIM_JOIN_PRUNE_MINLEN (4 + PIM_ENCODE_UNI_ADDR_LEN + 4)
int receive_pim_join_prune(u_int32 src, u_int32 dst __attribute__((unused)), char *pim_message, int datalen)
{
    vifi_t vifi;
    struct uvif *v;
    pim_encod_uni_addr_t uni_target_addr;
    pim_encod_grp_addr_t encod_group;
    pim_encod_src_addr_t encod_src;
    u_int8 *data_ptr;
    u_int8 *data_ptr_start;
    u_int8 *data_ptr_group_end;
    u_int8 num_groups;
    u_int8 num_groups_tmp;
    int star_star_rp_found;
    u_int16 holdtime;
    u_int16 num_j_srcs;
    u_int16 num_j_srcs_tmp;
    u_int16 num_p_srcs;
    u_int32 source;
    u_int32 group;
    u_int32 s_mask;
    u_int32 g_mask;
    u_int8 s_flags;
    u_int8 reserved __attribute__((unused));
    rpentry_t *rpentry_ptr;
    mrtentry_t *mrtentry_ptr;
    mrtentry_t *mrtentry_srcs;
    mrtentry_t *mrtentry_rp;
    grpentry_t *grpentry_ptr;
    u_int16 jp_value;
    pim_nbr_entry_t *upstream_router;
    int my_action;
    int ignore_group;
    rp_grp_entry_t *rp_grp_entry_ptr;
    u_int8 *data_ptr_group_j_start;
    u_int8 *data_ptr_group_p_start;

    if ((vifi = find_vif_direct(src)) == NO_VIF) {
        /* Either a local vif or somehow received PIM_JOIN_PRUNE from
         * non-directly connected router. Ignore it.
         */
        if (local_address(src) == NO_VIF) {
            logit(LOG_INFO, 0, "Ignoring PIM_JOIN_PRUNE from non-neighbor router %s",
                  inet_fmt(src, s1, sizeof(s1)));
	}

        return FALSE;
    }

    /* Checksum */
    if (inet_cksum((u_int16 *)pim_message, datalen))
        return FALSE;

    v = &uvifs[vifi];
    if (uvifs[vifi].uv_flags & (VIFF_DOWN | VIFF_DISABLED | VIFF_NONBRS | VIFF_REGISTER))
        return FALSE;    /* Shoudn't come on this interface */

    /* sanity check for the minimum length */
    if (datalen < PIM_JOIN_PRUNE_MINLEN) {
        logit(LOG_NOTICE, 0, "receive_pim_join_prune: Join/Prune message size(%u) is too short from %s on %s",
              datalen, inet_fmt(src, s1, sizeof(s1)), v->uv_name);

        return FALSE;
    }
    /*

     * TODO: Sanity check for the message length through all the groups
     * (see Kame's pim6sd).
     */

    data_ptr = (u_int8 *)(pim_message + sizeof(pim_header_t));
    /* Get the target address */
    GET_EUADDR(&uni_target_addr, data_ptr);
    GET_BYTE(reserved, data_ptr);
    GET_BYTE(num_groups, data_ptr);
    if (num_groups == 0)
        return FALSE;    /* No indication for groups in the message */
    GET_HOSTSHORT(holdtime, data_ptr);

    if (uni_target_addr.unicast_addr != v->uv_lcl_addr) {
        /* if I am not the targer of the join message */
        /* Join/Prune suppression code. This either modifies the J/P timers
         * or triggers an overriding Join.
         */
        /* Note that if we have (S,G) prune and (*,G) Join, we must send
         * them in the same message. We don't bother to modify both timers
         * here. The Join/Prune sending function will take care of that.
         */
        upstream_router = find_pim_nbr(uni_target_addr.unicast_addr);
        if (!upstream_router)
            return FALSE;   /* I have no such neighbor */

        while (num_groups--) {
            GET_EGADDR(&encod_group, data_ptr);
            GET_HOSTSHORT(num_j_srcs, data_ptr);
            GET_HOSTSHORT(num_p_srcs, data_ptr);
            MASKLEN_TO_MASK(encod_group.masklen, g_mask);
            group = encod_group.mcast_addr;
            if (!IN_MULTICAST(ntohl(group))) {
                data_ptr += (num_j_srcs + num_p_srcs) * sizeof(pim_encod_src_addr_t);
                continue; /* Ignore this group and jump to the next */
            }

            if ((ntohl(group) == CLASSD_PREFIX) && (encod_group.masklen == STAR_STAR_RP_MSKLEN)) {
                /* (*,*,RP) Join suppression */

                while(num_j_srcs--) {
                    GET_ESADDR(&encod_src, data_ptr);
                    source = encod_src.src_addr;
                    if (!inet_valid_host(source))
                        continue;

                    s_flags = encod_src.flags;
                    MASKLEN_TO_MASK(encod_src.masklen, s_mask);
                    if ((s_flags & USADDR_RP_BIT) && (s_flags & USADDR_WC_BIT)) {
                        /* This is the RP address. */
                        rpentry_ptr = rp_find(source);
                        if (!rpentry_ptr)
                            continue; /* Don't have such RP. Ignore */

                        mrtentry_rp = rpentry_ptr->mrtlink;
                        my_action = join_or_prune(mrtentry_rp, upstream_router);
                        if (my_action != PIM_ACTION_JOIN)
                            continue;

                        /* Check the holdtime */
                        /* TODO: XXX: TIMER implem. dependency! */
                        if (mrtentry_rp->jp_timer > holdtime)
                            continue;

                        if ((mrtentry_rp->jp_timer == holdtime) && (ntohl(src) > ntohl(v->uv_lcl_addr)))
                            continue;

                        /* Set the Join/Prune suppression timer for this
                         * routing entry by increasing the current
                         * Join/Prune timer.
                         */
                        jp_value = PIM_JOIN_PRUNE_PERIOD + 0.5 * (RANDOM() % PIM_JOIN_PRUNE_PERIOD);
                        /* TODO: XXX: TIMER implem. dependency! */
                        if (mrtentry_rp->jp_timer < jp_value)
                            SET_TIMER(mrtentry_rp->jp_timer, jp_value);
                    }
                } /* num_j_srcs */

                while (num_p_srcs--) {
                    /* TODO: XXX: Can we have (*,*,RP) prune message?
                     * Not in the spec, but anyway, the code below
                     * can handle them: either suppress
                     * the local (*,*,RP) prunes or override the prunes by
                     * sending (*,*,RP) and/or (*,G) and/or (S,G) Join.
                     */
                    GET_ESADDR(&encod_src, data_ptr);
                    source = encod_src.src_addr;
                    if (!inet_valid_host(source))
                        continue;
                    s_flags = encod_src.flags;
                    MASKLEN_TO_MASK(encod_src.masklen, s_mask);
                    if ((s_flags & USADDR_RP_BIT) && (s_flags & USADDR_WC_BIT)) {
                        /* This is the RP address. */
                        rpentry_ptr = rp_find(source);
                        if (!rpentry_ptr)
                            continue; /* Don't have such RP. Ignore */

                        mrtentry_rp = rpentry_ptr->mrtlink;
                        my_action = join_or_prune(mrtentry_rp, upstream_router);
                        if (my_action == PIM_ACTION_PRUNE) {
                            /* TODO: XXX: TIMER implem. dependency! */
                            if ((mrtentry_rp->jp_timer < holdtime)
				|| ((mrtentry_rp->jp_timer == holdtime) &&
				    (ntohl(src) > ntohl(v->uv_lcl_addr)))) {
                                /* Suppress the Prune */
                                jp_value = PIM_JOIN_PRUNE_PERIOD + 0.5 * (RANDOM() % PIM_JOIN_PRUNE_PERIOD);
                                if (mrtentry_rp->jp_timer < jp_value)
                                    SET_TIMER(mrtentry_rp->jp_timer, jp_value);
                            }
                        } else if (my_action == PIM_ACTION_JOIN) {
                            /* Override the Prune by scheduling a Join */
                            jp_value = (RANDOM() % (int)(10 * PIM_RANDOM_DELAY_JOIN_TIMEOUT)) / 10;
                            /* TODO: XXX: TIMER implem. dependency! */
                            if (mrtentry_rp->jp_timer > jp_value)
                                SET_TIMER(mrtentry_rp->jp_timer, jp_value);
                        }

                        /* Check all (*,G) and (S,G) matching to this RP.
                         * If my_action == JOIN, then send a Join and override
                         * the (*,*,RP) Prune.
                         */
                        for (grpentry_ptr = rpentry_ptr->cand_rp->rp_grp_next->grplink;
                             grpentry_ptr != (grpentry_t *)NULL;
                             grpentry_ptr = grpentry_ptr->rpnext) {
                            my_action = join_or_prune(grpentry_ptr->grp_route, upstream_router);
                            if (my_action == PIM_ACTION_JOIN) {
                                jp_value = (RANDOM() % (int)(10 * PIM_RANDOM_DELAY_JOIN_TIMEOUT)) / 10;
                                /* TODO: XXX: TIMER implem. dependency! */
                                if (grpentry_ptr->grp_route->jp_timer > jp_value)
                                    SET_TIMER(grpentry_ptr->grp_route->jp_timer, jp_value);
                            }
                            for (mrtentry_srcs = grpentry_ptr->mrtlink;
                                 mrtentry_srcs != (mrtentry_t *)NULL;
                                 mrtentry_srcs = mrtentry_srcs->grpnext) {
                                my_action = join_or_prune(mrtentry_srcs, upstream_router);
                                if (my_action == PIM_ACTION_JOIN) {
                                    jp_value = (RANDOM() % (int)(10 * PIM_RANDOM_DELAY_JOIN_TIMEOUT)) / 10;
                                    /* TODO: XXX: TIMER implem. dependency! */
                                    if (mrtentry_srcs->jp_timer > jp_value)
                                        SET_TIMER(mrtentry_srcs->jp_timer, jp_value);
                                }
                            } /* For all (S,G) */
                        } /* For all (*,G) */
                    }
                } /* num_p_srcs */
                continue;  /* This was (*,*,RP) suppression */
            }

            /* (*,G) or (S,G) suppression */
            /* TODO: XXX: currently, accumulated groups
             * (i.e. group_masklen < group_address_lengt) are not
             * implemented. Just need to create a loop and apply the
             * procedure below for all groups matching the prefix.
             */
            while (num_j_srcs--) {
                GET_ESADDR(&encod_src, data_ptr);
                source = encod_src.src_addr;
                if (!inet_valid_host(source))
                    continue;

                s_flags = encod_src.flags;
                MASKLEN_TO_MASK(encod_src.masklen, s_mask);

                if ((s_flags & USADDR_RP_BIT) && (s_flags & USADDR_WC_BIT)) {
                    /* (*,G) JOIN_REQUEST (toward the RP) */
                    mrtentry_ptr = find_route(INADDR_ANY_N, group, MRTF_WC, DONT_CREATE);
                    if (!mrtentry_ptr)
                        continue;

                    my_action = join_or_prune(mrtentry_ptr, upstream_router);
                    if (my_action != PIM_ACTION_JOIN)
                        continue;

                    /* (*,G) Join suppresion */
                    if (source != mrtentry_ptr->group->active_rp_grp->rp->rpentry->address)
                        continue;  /* The RP address doesn't match. Ignore. */

                    /* Check the holdtime */
                    /* TODO: XXX: TIMER implem. dependency! */
                    if (mrtentry_ptr->jp_timer > holdtime)
                        continue;

                    if ((mrtentry_ptr->jp_timer == holdtime) && (ntohl(src) > ntohl(v->uv_lcl_addr)))
                        continue;

                    jp_value = PIM_JOIN_PRUNE_PERIOD + 0.5 * (RANDOM() % PIM_JOIN_PRUNE_PERIOD);
                    if (mrtentry_ptr->jp_timer < jp_value)
                        SET_TIMER(mrtentry_ptr->jp_timer, jp_value);
                    continue;
                } /* End of (*,G) Join suppression */

                /* (S,G) Join suppresion */
                mrtentry_ptr = find_route(source, group, MRTF_SG, DONT_CREATE);
                if (!mrtentry_ptr)
                    continue;

                my_action = join_or_prune(mrtentry_ptr, upstream_router);
                if (my_action != PIM_ACTION_JOIN)
                    continue;

                /* Check the holdtime */
                /* TODO: XXX: TIMER implem. dependency! */
                if (mrtentry_ptr->jp_timer > holdtime)
                    continue;

                if ((mrtentry_ptr->jp_timer == holdtime) && (ntohl(src) > ntohl(v->uv_lcl_addr)))
                    continue;

                jp_value = PIM_JOIN_PRUNE_PERIOD + 0.5 * (RANDOM() % PIM_JOIN_PRUNE_PERIOD);
                if (mrtentry_ptr->jp_timer < jp_value)
                    SET_TIMER(mrtentry_ptr->jp_timer, jp_value);
                continue;
            }

            /* Prunes suppression */
            while (num_p_srcs--) {
                GET_ESADDR(&encod_src, data_ptr);
                source = encod_src.src_addr;
                if (!inet_valid_host(source))
                    continue;

                s_flags = encod_src.flags;
                MASKLEN_TO_MASK(encod_src.masklen, s_mask);
                if ((s_flags & USADDR_RP_BIT) && (s_flags & USADDR_WC_BIT)) {
                    /* (*,G) prune suppression */
                    rpentry_ptr = rp_match(group);
                    if (!rpentry_ptr || (rpentry_ptr->address != source))
                        continue;  /* No such RP or it is different. Ignore */

                    mrtentry_ptr = find_route(INADDR_ANY_N, group, MRTF_WC, DONT_CREATE);
                    if (!mrtentry_ptr)
                        continue;

                    my_action = join_or_prune(mrtentry_ptr, upstream_router);
                    if (my_action == PIM_ACTION_PRUNE) {
                        /* TODO: XXX: TIMER implem. dependency! */
                        if ((mrtentry_ptr->jp_timer < holdtime)
                            || ((mrtentry_ptr->jp_timer == holdtime)
                                && (ntohl(src) > ntohl(v->uv_lcl_addr)))) {
                            /* Suppress the Prune */
                            jp_value = PIM_JOIN_PRUNE_PERIOD + 0.5 * (RANDOM() % PIM_JOIN_PRUNE_PERIOD);
                            if (mrtentry_ptr->jp_timer < jp_value)
                                SET_TIMER(mrtentry_ptr->jp_timer, jp_value);
                        }
                    }
                    else if (my_action == PIM_ACTION_JOIN) {
                        /* Override the Prune by scheduling a Join */
                        jp_value = (RANDOM() % (int)(10 * PIM_RANDOM_DELAY_JOIN_TIMEOUT)) / 10;
                        /* TODO: XXX: TIMER implem. dependency! */
                        if (mrtentry_ptr->jp_timer > jp_value)
                            SET_TIMER(mrtentry_ptr->jp_timer, jp_value);
                    }

                    /* Check all (S,G) entries for this group.
                     * If my_action == JOIN, then send the Join and override
                     * the (*,G) Prune.
                     */
                    for (mrtentry_srcs = mrtentry_ptr->group->mrtlink;
                         mrtentry_srcs != (mrtentry_t *)NULL;
                         mrtentry_srcs = mrtentry_srcs->grpnext) {
                        my_action = join_or_prune(mrtentry_srcs, upstream_router);
                        if (my_action == PIM_ACTION_JOIN) {
                            jp_value = (RANDOM() % (int)(10 * PIM_RANDOM_DELAY_JOIN_TIMEOUT)) / 10;
                            /* TODO: XXX: TIMER implem. dependency! */
                            if (mrtentry_ptr->jp_timer > jp_value)
                                SET_TIMER(mrtentry_ptr->jp_timer, jp_value);
                        }
                    } /* For all (S,G) */
                    continue;  /* End of (*,G) prune suppression */
                }

                /* (S,G) prune suppression */
                mrtentry_ptr = find_route(source, group, MRTF_SG, DONT_CREATE);
                if (mrtentry_ptr == NULL)
                    continue;

                my_action = join_or_prune(mrtentry_ptr, upstream_router);
                if (my_action == PIM_ACTION_PRUNE) {
                    /* Suppress the (S,G) Prune */
                    /* TODO: XXX: TIMER implem. dependency! */
                    if ((mrtentry_ptr->jp_timer < holdtime)
                        || ((mrtentry_ptr->jp_timer == holdtime)
                            && (ntohl(src) > ntohl(v->uv_lcl_addr)))) {
                        jp_value = PIM_JOIN_PRUNE_PERIOD + 0.5 * (RANDOM() % PIM_JOIN_PRUNE_PERIOD);
                        if (mrtentry_ptr->jp_timer < jp_value)
                            SET_TIMER(mrtentry_ptr->jp_timer, jp_value);
                    }
                }
                else if (my_action == PIM_ACTION_JOIN) {
                    /* Override the Prune by scheduling a Join */
                    jp_value = (RANDOM() % (int)(10 * PIM_RANDOM_DELAY_JOIN_TIMEOUT)) / 10;
                    /* TODO: XXX: TIMER implem. dependency! */
                    if (mrtentry_ptr->jp_timer > jp_value)
                        SET_TIMER(mrtentry_ptr->jp_timer, jp_value);
                }
            }  /* while (num_p_srcs--) */
        }  /* while (num_groups--) */
        return TRUE;
    }   /* End of Join/Prune suppression code */

    /* I am the target of this join, so process the message */

    /* The spec says that if there is (*,G) Join, it has priority over
     * old existing ~(S,G) prunes in the routing table.
     * However, if the (*,G) Join and the ~(S,G) prune are in
     * the same message, ~(S,G) has the priority. The spec doesn't say it,
     * but I think the same is true for (*,*,RP) and ~(S,G) prunes.
     *
     * The code below do:
     *  (1) Check the whole message for (*,*,RP) Joins.
     *  (1.1) If found, clean all pruned_oifs for all (*,G) and all (S,G)
     *        for each RP in the list, but do not update the kernel cache.
     *        Then go back to the beginning of the message and start
     *        processing for each group:
     *  (2) Check for Prunes. If no prunes, process the Joins.
     *  (3) If there are Prunes:
     *  (3.1) Scan the Join part for existing (*,G) Join.
     *  (3.1.1) If there is (*,G) Join, clear join interface from
     *          the pruned_oifs for all (S,G), but DO NOT flush the
     *          change to the kernel (by using change_interfaces()
     *          for example)
     *  (3.2) After the pruned_oifs are eventually cleared in (3.1.1),
     *        process the Prune part of the message normally
     *        (setting the prune_oifs and flashing the changes to the (kernel).
     *  (3.3) After the Prune part is processed, process the Join part
     *        normally (by applying any changes to the kernel)
     *  (4) If there were (*,*,RP) Join/Prune, process them.
     *
     *   If the Join/Prune list is too long, it may result in long processing
     *   overhead. The idea above is not to place any wrong info in the
     *   kernel, because it may result in short-time existing traffic
     *   forwarding on wrong interface.
     *   Hopefully, in the future will find a better way to implement it.
     */
    num_groups_tmp = num_groups;
    data_ptr_start = data_ptr;
    star_star_rp_found = FALSE; /* Indicating whether we have (*,*,RP) join */
    while (num_groups_tmp--) {
        /* Search for (*,*,RP) Join */
        GET_EGADDR(&encod_group, data_ptr);
        GET_HOSTSHORT(num_j_srcs, data_ptr);
        GET_HOSTSHORT(num_p_srcs, data_ptr);
        group = encod_group.mcast_addr;
        if ((ntohl(group) != CLASSD_PREFIX) || (encod_group.masklen != STAR_STAR_RP_MSKLEN)) {
            /* This is not (*,*,RP). Jump to the next group. */
            data_ptr += (num_j_srcs + num_p_srcs) * sizeof(pim_encod_src_addr_t);
            continue;
        }

        /* (*,*,RP) found. For each RP and each (*,G) and each (S,G) clear
         * the pruned oif, but do not update the kernel.
         */
        star_star_rp_found = TRUE;
        while (num_j_srcs--) {
            GET_ESADDR(&encod_src, data_ptr);
            rpentry_ptr = rp_find(encod_src.src_addr);
            if (rpentry_ptr == (rpentry_t *)NULL)
                continue;
            for (rp_grp_entry_ptr = rpentry_ptr->cand_rp->rp_grp_next;
                 rp_grp_entry_ptr != (rp_grp_entry_t *)NULL;
                 rp_grp_entry_ptr = rp_grp_entry_ptr->rp_grp_next) {
                for (grpentry_ptr = rp_grp_entry_ptr->grplink;
                     grpentry_ptr != (grpentry_t *)NULL;
                     grpentry_ptr = grpentry_ptr->rpnext) {
                    if (grpentry_ptr->grp_route != (mrtentry_t *)NULL)
                        VIFM_CLR(vifi, grpentry_ptr->grp_route->pruned_oifs);
                    for (mrtentry_ptr = grpentry_ptr->mrtlink;
                         mrtentry_ptr != (mrtentry_t *)NULL;
                         mrtentry_ptr = mrtentry_ptr->grpnext)
                        VIFM_CLR(vifi, mrtentry_ptr->pruned_oifs);
                }
            }
        }
        data_ptr += (num_p_srcs) * sizeof(pim_encod_src_addr_t);
    }

    /*
     * Start processing the groups. If this is (*,*,RP), skip it, but process
     * it at the end.
     */
    data_ptr = data_ptr_start;
    num_groups_tmp = num_groups;
    while (num_groups_tmp--) {
        GET_EGADDR(&encod_group, data_ptr);
        GET_HOSTSHORT(num_j_srcs, data_ptr);
        GET_HOSTSHORT(num_p_srcs, data_ptr);
        group = encod_group.mcast_addr;
        if (!IN_MULTICAST(ntohl(group))) {
            data_ptr += (num_j_srcs + num_p_srcs) * sizeof(pim_encod_src_addr_t);
            continue;		/* Ignore this group and jump to the next one */
        }

        if ((ntohl(group) == CLASSD_PREFIX)
            && (encod_group.masklen == STAR_STAR_RP_MSKLEN)) {
            /* This is (*,*,RP). Jump to the next group. */
            data_ptr += (num_j_srcs + num_p_srcs) * sizeof(pim_encod_src_addr_t);
            continue;
        }

        rpentry_ptr = rp_match(group);
        if (rpentry_ptr == (rpentry_t *)NULL) {
            data_ptr += (num_j_srcs + num_p_srcs) * sizeof(pim_encod_src_addr_t);
            continue;
        }

        data_ptr_group_j_start = data_ptr;
        data_ptr_group_p_start = data_ptr + num_j_srcs * sizeof(pim_encod_src_addr_t);
        data_ptr_group_end = data_ptr + (num_j_srcs + num_p_srcs) * sizeof(pim_encod_src_addr_t);

        /* Scan the Join part for (*,G) Join and then clear the
         * particular interface from pruned_oifs for all (S,G).
         * If the RP address in the Join message is different from
         * the local match, ignore the whole group.
         */
        num_j_srcs_tmp = num_j_srcs;
        ignore_group = FALSE;
        while (num_j_srcs_tmp--) {
            GET_ESADDR(&encod_src, data_ptr);
            if ((encod_src.flags & USADDR_RP_BIT) && (encod_src.flags & USADDR_WC_BIT)) {
                /* This is the RP address, i.e. (*,G) Join.
                 * Check if the RP-mapping is consistent and if "yes",
                 * then Reset the pruned_oifs for all (S,G) entries.
                 */
                if (rpentry_ptr->address != encod_src.src_addr) {
                    ignore_group = TRUE;
                    break;
                }

                mrtentry_ptr = find_route(INADDR_ANY_N, group, MRTF_WC, DONT_CREATE);
                if (mrtentry_ptr) {
                    for (mrtentry_srcs = mrtentry_ptr->group->mrtlink;
                         mrtentry_srcs != (mrtentry_t *)NULL;
                         mrtentry_srcs = mrtentry_srcs->grpnext)
                        VIFM_CLR(vifi, mrtentry_srcs->pruned_oifs);
                }
                break;
            }
        }

        if (ignore_group == TRUE) {
            data_ptr += (num_j_srcs_tmp + num_p_srcs) * sizeof(pim_encod_src_addr_t);
            continue;
        }

        data_ptr = data_ptr_group_p_start;
        /* Process the Prune part first */
        while (num_p_srcs--) {
            GET_ESADDR(&encod_src, data_ptr);
            source = encod_src.src_addr;
            if (!inet_valid_host(source))
                continue;

            s_flags = encod_src.flags;
            if (!(s_flags & (USADDR_WC_BIT | USADDR_RP_BIT))) {
                /* (S,G) prune sent toward S */
                mrtentry_ptr = find_route(source, group, MRTF_SG, DONT_CREATE);
                if (!mrtentry_ptr)
                    continue;   /* I don't have (S,G) to prune. Ignore. */

                /* If the link is point-to-point, timeout the oif
                 * immediately, otherwise decrease the timer to allow
                 * other downstream routers to override the prune.
                 */
                /* TODO: XXX: increase the entry timer? */
                if (v->uv_flags & VIFF_POINT_TO_POINT) {
                    FIRE_TIMER(mrtentry_ptr->vif_timers[vifi]);
                } else {
                    /* TODO: XXX: TIMER implem. dependency! */
                    if (mrtentry_ptr->vif_timers[vifi] > mrtentry_ptr->vif_deletion_delay[vifi])
                        SET_TIMER(mrtentry_ptr->vif_timers[vifi],
				  mrtentry_ptr->vif_deletion_delay[vifi]);
                }
                IF_TIMER_NOT_SET(mrtentry_ptr->vif_timers[vifi]) {
                    VIFM_CLR(vifi, mrtentry_ptr->joined_oifs);
                    VIFM_SET(vifi, mrtentry_ptr->pruned_oifs);
                    change_interfaces(mrtentry_ptr,
                                      mrtentry_ptr->incoming,
                                      mrtentry_ptr->joined_oifs,
                                      mrtentry_ptr->pruned_oifs,
                                      mrtentry_ptr->leaves,
                                      mrtentry_ptr->asserted_oifs, 0);
                }
                continue;
            }

            if ((s_flags & USADDR_RP_BIT) && (!(s_flags & USADDR_WC_BIT))) {
                /* ~(S,G)RPbit prune sent toward the RP */
                mrtentry_ptr = find_route(source, group, MRTF_SG, DONT_CREATE);
                if (mrtentry_ptr) {
                    SET_TIMER(mrtentry_ptr->timer, holdtime);
                    if (v->uv_flags & VIFF_POINT_TO_POINT) {
                        FIRE_TIMER(mrtentry_ptr->vif_timers[vifi]);
                    } else {
                        /* TODO: XXX: TIMER implem. dependency! */
                        if (mrtentry_ptr->vif_timers[vifi] > mrtentry_ptr->vif_deletion_delay[vifi])
                            SET_TIMER(mrtentry_ptr->vif_timers[vifi],
                                      mrtentry_ptr->vif_deletion_delay[vifi]);
                    }
                    IF_TIMER_NOT_SET(mrtentry_ptr->vif_timers[vifi]) {
                        VIFM_CLR(vifi, mrtentry_ptr->joined_oifs);
                        VIFM_SET(vifi, mrtentry_ptr->pruned_oifs);
                        change_interfaces(mrtentry_ptr,
                                          mrtentry_ptr->incoming,
                                          mrtentry_ptr->joined_oifs,
                                          mrtentry_ptr->pruned_oifs,
                                          mrtentry_ptr->leaves,
                                          mrtentry_ptr->asserted_oifs, 0);
                    }
                    continue;
                }

                /* There is no (S,G) entry. Check for (*,G) or (*,*,RP) */
                mrtentry_ptr = find_route(INADDR_ANY_N, group, MRTF_WC | MRTF_PMBR, DONT_CREATE);
                if (mrtentry_ptr) {
                    mrtentry_ptr = find_route(source, group, MRTF_SG | MRTF_RP, CREATE);
                    if (!mrtentry_ptr)
                        continue;

                    mrtentry_ptr->flags &= ~MRTF_NEW;
                    RESET_TIMER(mrtentry_ptr->vif_timers[vifi]);
                    /* TODO: XXX: The spec doens't say what value to use for
                     * the entry time. Use the J/P holdtime.
                     */
                    SET_TIMER(mrtentry_ptr->timer, holdtime);
                    /* TODO: XXX: The spec says to delete the oif. However,
                     * its timer only should be lowered, so the prune can be
                     * overwritten on multiaccess LAN. Spec BUG.
                     */
                    VIFM_CLR(vifi, mrtentry_ptr->joined_oifs);
                    VIFM_SET(vifi, mrtentry_ptr->pruned_oifs);
                    change_interfaces(mrtentry_ptr,
                                      mrtentry_ptr->incoming,
                                      mrtentry_ptr->joined_oifs,
                                      mrtentry_ptr->pruned_oifs,
                                      mrtentry_ptr->leaves,
                                      mrtentry_ptr->asserted_oifs, 0);
                }
                continue;
            }

            if ((s_flags & USADDR_RP_BIT) && (s_flags & USADDR_WC_BIT)) {
                /* (*,G) Prune */
                mrtentry_ptr = find_route(INADDR_ANY_N, group, MRTF_WC | MRTF_PMBR, DONT_CREATE);
                if (mrtentry_ptr) {
                    if (mrtentry_ptr->flags & MRTF_WC) {
                        /* TODO: XXX: Should check the whole Prune list in
                         * advance for (*,G) prune and if the RP address
                         * does not match the local RP-map, then ignore the
                         * whole group, not only this particular (*,G) prune.
                         */
                        if (mrtentry_ptr->group->active_rp_grp->rp->rpentry->address != source)
                            continue; /* The RP address doesn't match. */

                        if (v->uv_flags & VIFF_POINT_TO_POINT) {
                            FIRE_TIMER(mrtentry_ptr->vif_timers[vifi]);
                        } else {
                            /* TODO: XXX: TIMER implem. dependency! */
                            if (mrtentry_ptr->vif_timers[vifi] > mrtentry_ptr->vif_deletion_delay[vifi])
                                SET_TIMER(mrtentry_ptr->vif_timers[vifi],
                                          mrtentry_ptr->vif_deletion_delay[vifi]);
                        }
                        IF_TIMER_NOT_SET(mrtentry_ptr->vif_timers[vifi]) {
                            VIFM_CLR(vifi, mrtentry_ptr->joined_oifs);
                            VIFM_SET(vifi, mrtentry_ptr->pruned_oifs);
                            change_interfaces(mrtentry_ptr,
                                              mrtentry_ptr->incoming,
                                              mrtentry_ptr->joined_oifs,
                                              mrtentry_ptr->pruned_oifs,
                                              mrtentry_ptr->leaves,
                                              mrtentry_ptr->asserted_oifs, 0);
                        }
                        continue;
                    }

                    /* No (*,G) entry, but found (*,*,RP). Create (*,G) */
                    if (mrtentry_ptr->source->address != source)
                        continue; /* The RP address doesn't match. */

                    mrtentry_ptr = find_route(INADDR_ANY_N, group, MRTF_WC, CREATE);
                    if (!mrtentry_ptr)
                        continue;

                    mrtentry_ptr->flags &= ~MRTF_NEW;
                    RESET_TIMER(mrtentry_ptr->vif_timers[vifi]);
                    /* TODO: XXX: should only lower the oif timer, so it can
                     * be overwritten on multiaccess LAN. Spec bug.
                     */
                    VIFM_CLR(vifi, mrtentry_ptr->joined_oifs);
                    VIFM_SET(vifi, mrtentry_ptr->pruned_oifs);
                    change_interfaces(mrtentry_ptr,
                                      mrtentry_ptr->incoming,
                                      mrtentry_ptr->joined_oifs,
                                      mrtentry_ptr->pruned_oifs,
                                      mrtentry_ptr->leaves,
                                      mrtentry_ptr->asserted_oifs, 0);
                } /* (*,G) or (*,*,RP) found */
            } /* (*,G) prune */
        } /* while(num_p_srcs--) */
        /* End of (S,G) and (*,G) Prune handling */

        /* Jump back to the Join part and process it */
        data_ptr = data_ptr_group_j_start;
        while (num_j_srcs--) {
            GET_ESADDR(&encod_src, data_ptr);
            source = encod_src.src_addr;
            if (!inet_valid_host(source))
                continue;

            s_flags = encod_src.flags;
            MASKLEN_TO_MASK(encod_src.masklen, s_mask);
            if ((s_flags & USADDR_WC_BIT) && (s_flags & USADDR_RP_BIT)) {
                /* (*,G) Join toward RP */
                /* It has been checked already that this RP address is
                 * the same as the local RP-maping.
                 */
                mrtentry_ptr = find_route(INADDR_ANY_N, group, MRTF_WC, CREATE);
                if (!mrtentry_ptr)
                    continue;

                VIFM_SET(vifi, mrtentry_ptr->joined_oifs);
                VIFM_CLR(vifi, mrtentry_ptr->pruned_oifs);
                VIFM_CLR(vifi, mrtentry_ptr->asserted_oifs);
                /* TODO: XXX: TIMER implem. dependency! */
                if (mrtentry_ptr->vif_timers[vifi] < holdtime) {
                    SET_TIMER(mrtentry_ptr->vif_timers[vifi], holdtime);
                    mrtentry_ptr->vif_deletion_delay[vifi] = holdtime/3;
                }
                if (mrtentry_ptr->timer < holdtime)
                    SET_TIMER(mrtentry_ptr->timer, holdtime);
                mrtentry_ptr->flags &= ~MRTF_NEW;
                change_interfaces(mrtentry_ptr,
                                  mrtentry_ptr->incoming,
                                  mrtentry_ptr->joined_oifs,
                                  mrtentry_ptr->pruned_oifs,
                                  mrtentry_ptr->leaves,
                                  mrtentry_ptr->asserted_oifs, 0);
                /* Need to update the (S,G) entries, because of the previous
                 * cleaning of the pruned_oifs. The reason is that if the
                 * oifs for (*,G) weren't changed, the (S,G) entries won't
                 * be updated by change_interfaces()
                 */
                for (mrtentry_srcs = mrtentry_ptr->group->mrtlink;
                     mrtentry_srcs != (mrtentry_t *)NULL;
                     mrtentry_srcs = mrtentry_srcs->grpnext)
                    change_interfaces(mrtentry_srcs,
                                      mrtentry_srcs->incoming,
                                      mrtentry_srcs->joined_oifs,
                                      mrtentry_srcs->pruned_oifs,
                                      mrtentry_srcs->leaves,
                                      mrtentry_srcs->asserted_oifs, 0);
                continue;
            }

            if (!(s_flags & (USADDR_WC_BIT | USADDR_RP_BIT))) {
                /* (S,G) Join toward S */
                if (vifi == get_iif(source))
                    continue;  /* Ignore this (S,G) Join */

                mrtentry_ptr = find_route(source, group, MRTF_SG, CREATE);
                if (!mrtentry_ptr)
                    continue;

                VIFM_SET(vifi, mrtentry_ptr->joined_oifs);
                VIFM_CLR(vifi, mrtentry_ptr->pruned_oifs);
                VIFM_CLR(vifi, mrtentry_ptr->asserted_oifs);
                /* TODO: XXX: TIMER implem. dependency! */
                if (mrtentry_ptr->vif_timers[vifi] < holdtime) {
                    SET_TIMER(mrtentry_ptr->vif_timers[vifi], holdtime);
                    mrtentry_ptr->vif_deletion_delay[vifi] = holdtime/3;
                }
                if (mrtentry_ptr->timer < holdtime)
                    SET_TIMER(mrtentry_ptr->timer, holdtime);
                /* TODO: if this is a new entry, send immediately the
                 * Join message toward S. The Join/Prune timer for new
                 * entries is 0, but it does not means the message will
                 * be sent immediately.
                 */
                mrtentry_ptr->flags &= ~MRTF_NEW;
                /* Note that we must create (S,G) without the RPbit set.
                 * If we already had such entry, change_interfaces() will
                 * reset the RPbit propertly.
                 */
                change_interfaces(mrtentry_ptr,
                                  mrtentry_ptr->source->incoming,
                                  mrtentry_ptr->joined_oifs,
                                  mrtentry_ptr->pruned_oifs,
                                  mrtentry_ptr->leaves,
                                  mrtentry_ptr->asserted_oifs, 0);
                continue;
            }
        } /* while(num_j_srcs--) */
        data_ptr = data_ptr_group_end;
    } /* for all groups */

    /* Now process the (*,*,RP) Join/Prune */
    if (star_star_rp_found != TRUE)
        return TRUE;

    data_ptr = data_ptr_start;
    while (num_groups--) {
        /* The conservative approach is to scan again the whole message,
         * just in case if we have more than one (*,*,RP) requests.
         */
        GET_EGADDR(&encod_group, data_ptr);
        GET_HOSTSHORT(num_j_srcs, data_ptr);
        GET_HOSTSHORT(num_p_srcs, data_ptr);
        group = encod_group.mcast_addr;
        if ((ntohl(group) != CLASSD_PREFIX)
            || (encod_group.masklen != STAR_STAR_RP_MSKLEN)) {
            /* This is not (*,*,RP). Jump to the next group. */
            data_ptr +=
                (num_j_srcs + num_p_srcs) * sizeof(pim_encod_src_addr_t);
            continue;
        }
        /* (*,*,RP) found */
        while (num_j_srcs--) {
            /* TODO: XXX: check that the iif is different from the Join oifs */
            GET_ESADDR(&encod_src, data_ptr);
            source = encod_src.src_addr;
            if (!inet_valid_host(source))
                continue;

            s_flags = encod_src.flags;
            MASKLEN_TO_MASK(encod_src.masklen, s_mask);
            mrtentry_ptr = find_route(source, INADDR_ANY_N, MRTF_PMBR, CREATE);
            if (mrtentry_ptr == (mrtentry_t *)NULL)
                continue;

            VIFM_SET(vifi, mrtentry_ptr->joined_oifs);
            VIFM_CLR(vifi, mrtentry_ptr->pruned_oifs);
            VIFM_CLR(vifi, mrtentry_ptr->asserted_oifs);
            /* TODO: XXX: TIMER implem. dependency! */
            if (mrtentry_ptr->vif_timers[vifi] < holdtime) {
                SET_TIMER(mrtentry_ptr->vif_timers[vifi], holdtime);
                mrtentry_ptr->vif_deletion_delay[vifi] = holdtime/3;
            }
            if (mrtentry_ptr->timer < holdtime)
                SET_TIMER(mrtentry_ptr->timer, holdtime);
            mrtentry_ptr->flags &= ~MRTF_NEW;
            change_interfaces(mrtentry_ptr,
                              mrtentry_ptr->incoming,
                              mrtentry_ptr->joined_oifs,
                              mrtentry_ptr->pruned_oifs,
                              mrtentry_ptr->leaves,
                              mrtentry_ptr->asserted_oifs, 0);

            /* Need to update the (S,G) and (*,G) entries, because of
             * the previous cleaning of the pruned_oifs. The reason is
             * that if the oifs for (*,*,RP) weren't changed, the
             * (*,G) and (S,G) entries won't be updated by change_interfaces()
             */
            for (rp_grp_entry_ptr = mrtentry_ptr->source->cand_rp->rp_grp_next;
                 rp_grp_entry_ptr != (rp_grp_entry_t *)NULL;
                 rp_grp_entry_ptr = rp_grp_entry_ptr->rp_grp_next) {
                for (grpentry_ptr = rp_grp_entry_ptr->grplink;
                     grpentry_ptr != (grpentry_t *)NULL;
                     grpentry_ptr = grpentry_ptr->rpnext) {
                    /* Update the (*,G) entry */
                    if (grpentry_ptr->grp_route != NULL) {
                        change_interfaces(grpentry_ptr->grp_route,
                                          grpentry_ptr->grp_route->incoming,
                                          grpentry_ptr->grp_route->joined_oifs,
                                          grpentry_ptr->grp_route->pruned_oifs,
                                          grpentry_ptr->grp_route->leaves,
                                          grpentry_ptr->grp_route->asserted_oifs, 0);
                    }
                    /* Update the (S,G) entries */
                    for (mrtentry_srcs = grpentry_ptr->mrtlink;
                         mrtentry_srcs != (mrtentry_t *)NULL;
                         mrtentry_srcs = mrtentry_srcs->grpnext)
                        change_interfaces(mrtentry_srcs,
                                          mrtentry_srcs->incoming,
                                          mrtentry_srcs->joined_oifs,
                                          mrtentry_srcs->pruned_oifs,
                                          mrtentry_srcs->leaves,
                                          mrtentry_srcs->asserted_oifs, 0);
                }
	    }
            continue;
        }

        while (num_p_srcs--) {
            /* TODO: XXX: can we have (*,*,RP) Prune? */
            GET_ESADDR(&encod_src, data_ptr);
            source = encod_src.src_addr;
            if (!inet_valid_host(source))
                continue;

            s_flags = encod_src.flags;
            MASKLEN_TO_MASK(encod_src.masklen, s_mask);
            mrtentry_ptr = find_route(source, INADDR_ANY_N, MRTF_PMBR, DONT_CREATE);
            if (mrtentry_ptr == (mrtentry_t *)NULL)
                continue;

            /* If the link is point-to-point, timeout the oif
             * immediately, otherwise decrease the timer to allow
             * other downstream routers to override the prune.
             */
            /* TODO: XXX: increase the entry timer? */
            if (v->uv_flags & VIFF_POINT_TO_POINT) {
                FIRE_TIMER(mrtentry_ptr->vif_timers[vifi]);
            } else {
                /* TODO: XXX: TIMER implem. dependency! */
                if (mrtentry_ptr->vif_timers[vifi] > mrtentry_ptr->vif_deletion_delay[vifi])
                    SET_TIMER(mrtentry_ptr->vif_timers[vifi],
                              mrtentry_ptr->vif_deletion_delay[vifi]);
            }
            IF_TIMER_NOT_SET(mrtentry_ptr->vif_timers[vifi]) {
                VIFM_CLR(vifi, mrtentry_ptr->joined_oifs);
                VIFM_SET(vifi, mrtentry_ptr->pruned_oifs);
                VIFM_SET(vifi, mrtentry_ptr->asserted_oifs);
                change_interfaces(mrtentry_ptr,
                                  mrtentry_ptr->incoming,
                                  mrtentry_ptr->joined_oifs,
                                  mrtentry_ptr->pruned_oifs,
                                  mrtentry_ptr->leaves,
                                  mrtentry_ptr->asserted_oifs, 0);
            }

        }
    } /* For all groups processing (*,*,R) */

    return TRUE;
}


/*
 * TODO: NOT USED, probably buggy, but may need it in the future.
 */
/*
 * TODO: create two functions: periodic which timeout the timers
 * and non-periodic which only check but don't timeout the timers.
 */
/*
 * Create and send Join/Prune messages per interface.
 * Only the entries which have the Join/Prune timer expired are included.
 * In the special case when we have ~(S,G)RPbit Prune entry, we must
 * include any (*,G) or (*,*,RP)
 * Currently the whole table is scanned. In the future will have all
 * routing entries linked in a chain with the corresponding upstream
 * pim_nbr_entry.
 *
 * If pim_nbr is not NULL, then send to only this particular PIM neighbor,
 */
int send_periodic_pim_join_prune(vifi_t vifi, pim_nbr_entry_t *pim_nbr, u_int16 holdtime)
{
    grpentry_t *grpentry_ptr;
    mrtentry_t *mrtentry_ptr;
    rpentry_t  *rpentry_ptr;
    u_int32 src_addr;
    struct uvif *v;
    pim_nbr_entry_t *pim_nbr_ptr;
    cand_rp_t *cand_rp_ptr;

    /* Walk through all routing entries. The iif must match to include the
     * entry. Check first the (*,G) entry and then all associated (S,G).
     * At the end of the message will add any (*,*,RP) entries.
     * TODO: check other PIM-SM implementations and decide the more
     * appropriate place to put the (*,*,RP) entries: in the beginning of the
     * message or at the end.
     */

    v = &uvifs[vifi];

    /* Check the (*,G) and (S,G) entries */
    for(grpentry_ptr = grplist; grpentry_ptr; grpentry_ptr = grpentry_ptr->next) {
        mrtentry_ptr = grpentry_ptr->grp_route;
        /* TODO: XXX: TIMER implem. dependency! */
        if (mrtentry_ptr
	    && (mrtentry_ptr->incoming == vifi)
            && (mrtentry_ptr->jp_timer <= TIMER_INTERVAL)) {

            /* If join/prune to a particular neighbor only was specified */
            if (pim_nbr && mrtentry_ptr->upstream != pim_nbr)
                continue;

            /* TODO: XXX: The J/P suppression timer is not in the spec! */
            if (!VIFM_ISEMPTY(mrtentry_ptr->joined_oifs) || (v->uv_flags & VIFF_DR)) {
                add_jp_entry(mrtentry_ptr->upstream, holdtime,
                             grpentry_ptr->group,
                             SINGLE_GRP_MSKLEN,
                             grpentry_ptr->rpaddr,
                             SINGLE_SRC_MSKLEN, 0, PIM_ACTION_JOIN);
            }
            /* TODO: XXX: TIMER implem. dependency! */
            if (VIFM_ISEMPTY(mrtentry_ptr->joined_oifs)
                && (!(v->uv_flags & VIFF_DR))
                && (mrtentry_ptr->jp_timer <= TIMER_INTERVAL)) {
                add_jp_entry(mrtentry_ptr->upstream, holdtime,
                             grpentry_ptr->group, SINGLE_GRP_MSKLEN,
                             grpentry_ptr->rpaddr,
                             SINGLE_SRC_MSKLEN, 0, PIM_ACTION_PRUNE);
            }
        }

        /* Check the (S,G) entries */
        for (mrtentry_ptr = grpentry_ptr->mrtlink; mrtentry_ptr; mrtentry_ptr = mrtentry_ptr->grpnext) {
            /* If join/prune to a particular neighbor only was specified */
            if (pim_nbr && mrtentry_ptr->upstream != pim_nbr)
                continue;

            if (mrtentry_ptr->flags & MRTF_RP) {
                /* RPbit set */
                src_addr = mrtentry_ptr->source->address;
                if (VIFM_ISEMPTY(mrtentry_ptr->joined_oifs)
                    || ((find_vif_direct_local(src_addr) != NO_VIF)
                        && grpentry_ptr->grp_route))
                    /* TODO: XXX: TIMER implem. dependency! */
                    if ((grpentry_ptr->grp_route->incoming == vifi)
                        && (grpentry_ptr->grp_route->jp_timer <= TIMER_INTERVAL))
                        /* S is directly connected. Send toward RP */
                        add_jp_entry(grpentry_ptr->grp_route->upstream,
                                     holdtime,
                                     grpentry_ptr->group, SINGLE_GRP_MSKLEN,
                                     src_addr, SINGLE_SRC_MSKLEN,
                                     MRTF_RP, PIM_ACTION_PRUNE);
            }
            else {
                /* RPbit cleared */
                if (VIFM_ISEMPTY(mrtentry_ptr->joined_oifs)) {
                    /* TODO: XXX: TIMER implem. dependency! */
                    if ((mrtentry_ptr->incoming == vifi)
                        && (mrtentry_ptr->jp_timer <= TIMER_INTERVAL))
                        add_jp_entry(mrtentry_ptr->upstream, holdtime,
                                     grpentry_ptr->group, SINGLE_GRP_MSKLEN,
                                     mrtentry_ptr->source->address,
                                     SINGLE_SRC_MSKLEN, 0, PIM_ACTION_PRUNE);
                }
                else {
                    /* TODO: XXX: TIMER implem. dependency! */
                    if ((mrtentry_ptr->incoming == vifi)
                        && (mrtentry_ptr->jp_timer <= TIMER_INTERVAL))
                        add_jp_entry(mrtentry_ptr->upstream, holdtime,
                                     grpentry_ptr->group, SINGLE_GRP_MSKLEN,
                                     mrtentry_ptr->source->address,
                                     SINGLE_SRC_MSKLEN, 0, PIM_ACTION_JOIN);
                }
                /* TODO: XXX: TIMER implem. dependency! */
                if ((mrtentry_ptr->flags & MRTF_SPT)
                    && grpentry_ptr->grp_route
                    && (mrtentry_ptr->incoming != grpentry_ptr->grp_route->incoming)
                    && (grpentry_ptr->grp_route->incoming == vifi)
                    && (grpentry_ptr->grp_route->jp_timer <= TIMER_INTERVAL))
                    add_jp_entry(grpentry_ptr->grp_route->upstream, holdtime,
                                 grpentry_ptr->group, SINGLE_GRP_MSKLEN,
                                 mrtentry_ptr->source->address,
                                 SINGLE_SRC_MSKLEN, MRTF_RP,
                                 PIM_ACTION_PRUNE);
            }
        }
    }

    /* Check the (*,*,RP) entries */
    for (cand_rp_ptr = cand_rp_list; cand_rp_ptr; cand_rp_ptr = cand_rp_ptr->next) {
        rpentry_ptr = cand_rp_ptr->rpentry;

        /* If join/prune to a particular neighbor only was specified */
        if (pim_nbr && rpentry_ptr->upstream != pim_nbr)
            continue;

        /* TODO: XXX: TIMER implem. dependency! */
        if (rpentry_ptr->mrtlink
            && (rpentry_ptr->incoming == vifi)
            && (rpentry_ptr->mrtlink->jp_timer <= TIMER_INTERVAL)) {
            add_jp_entry(rpentry_ptr->upstream, holdtime, htonl(CLASSD_PREFIX), STAR_STAR_RP_MSKLEN,
                         rpentry_ptr->address, SINGLE_SRC_MSKLEN, MRTF_RP | MRTF_WC, PIM_ACTION_JOIN);
        }
    }

    /* Send all pending Join/Prune messages */
    for (pim_nbr_ptr = v->uv_pim_neighbors; pim_nbr_ptr; pim_nbr_ptr = pim_nbr->next) {
        /* If join/prune to a particular neighbor only was specified */
        if (pim_nbr && (pim_nbr_ptr != pim_nbr))
            continue;

        pack_and_send_jp_message(pim_nbr_ptr);
    }

    return TRUE;
}


int add_jp_entry(pim_nbr_entry_t *pim_nbr, u_int16 holdtime, u_int32 group,
		 u_int8 grp_msklen, u_int32 source, u_int8 src_msklen,
		 u_int16 addr_flags, u_int8 join_prune)
{
    build_jp_message_t *bjpm;
    u_int8 *data_ptr;
    u_int8 flags = 0;
    int rp_flag;

    bjpm = pim_nbr->build_jp_message;
    if (bjpm) {
        if ((bjpm->jp_message_size + bjpm->join_list_size +
             bjpm->prune_list_size + bjpm->rp_list_join_size +
             bjpm->rp_list_prune_size >= MAX_JP_MESSAGE_SIZE)
            || (bjpm->join_list_size >= MAX_JOIN_LIST_SIZE)
            || (bjpm->prune_list_size >= MAX_PRUNE_LIST_SIZE)
            || (bjpm->rp_list_join_size >= MAX_JOIN_LIST_SIZE)
            || (bjpm->rp_list_prune_size >= MAX_PRUNE_LIST_SIZE)) {
            /* TODO: XXX: BUG: If the list is getting too large, must
             * be careful with the fragmentation.
             */
            pack_and_send_jp_message(pim_nbr);
            bjpm = pim_nbr->build_jp_message;	/* The buffer will be freed */
        }
    }

    if (bjpm) {
        if ((bjpm->curr_group != group)
            || (bjpm->curr_group_msklen != grp_msklen)
            || (bjpm->holdtime != holdtime)) {
            pack_jp_message(pim_nbr);
        }
    }

    if (!bjpm) {
        bjpm = get_jp_working_buff();
	if (!bjpm) {
	    logit(LOG_ERR, 0, "Failed allocating working buffer in add_jp_entry()\n");
	    exit (-1);
	}

        pim_nbr->build_jp_message = bjpm;
        data_ptr = bjpm->jp_message;
        PUT_EUADDR(pim_nbr->address, data_ptr);
        PUT_BYTE(0, data_ptr);			/* Reserved */
        bjpm->num_groups_ptr = data_ptr++;	/* The pointer for numgroups */
        *(bjpm->num_groups_ptr) = 0;		/* Zero groups */
        PUT_HOSTSHORT(holdtime, data_ptr);
        bjpm->holdtime = holdtime;
        bjpm->jp_message_size = data_ptr - bjpm->jp_message;
    }

    /* TODO: move somewhere else, only when it is a new group */
    bjpm->curr_group = group;
    bjpm->curr_group_msklen = grp_msklen;

    if (group == htonl(CLASSD_PREFIX) && grp_msklen == STAR_STAR_RP_MSKLEN)
        rp_flag = TRUE;
    else
        rp_flag = FALSE;

    switch (join_prune) {
        case PIM_ACTION_JOIN:
            if (rp_flag == TRUE)
                data_ptr = bjpm->rp_list_join + bjpm->rp_list_join_size;
            else
                data_ptr = bjpm->join_list + bjpm->join_list_size;
            break;

        case PIM_ACTION_PRUNE:
            if (rp_flag == TRUE)
                data_ptr = bjpm->rp_list_prune + bjpm->rp_list_prune_size;
            else
                data_ptr = bjpm->prune_list + bjpm->prune_list_size;
            break;

        default:
            return FALSE;
    }

    flags |= USADDR_S_BIT;   /* Mandatory for PIMv2 */
    if (addr_flags & MRTF_RP)
        flags |= USADDR_RP_BIT;
    if (addr_flags & MRTF_WC)
        flags |= USADDR_WC_BIT;
    PUT_ESADDR(source, src_msklen, flags, data_ptr);

    switch (join_prune) {
        case PIM_ACTION_JOIN:
            if (rp_flag == TRUE) {
                bjpm->rp_list_join_size = data_ptr - bjpm->rp_list_join;
                bjpm->rp_list_join_number++;
            } else {
                bjpm->join_list_size = data_ptr - bjpm->join_list;
                bjpm->join_addr_number++;
            }
            break;

        case PIM_ACTION_PRUNE:
            if (rp_flag == TRUE) {
                bjpm->rp_list_prune_size = data_ptr - bjpm->rp_list_prune;
                bjpm->rp_list_prune_number++;
            } else {
                bjpm->prune_list_size = data_ptr - bjpm->prune_list;
                bjpm->prune_addr_number++;
            }
            break;

        default:
            return FALSE;
    }

    return TRUE;
}


/* TODO: check again the size of the buffers */
static build_jp_message_t *get_jp_working_buff(void)
{
    build_jp_message_t *bjpm_ptr;

    if (build_jp_message_pool_counter == 0) {
        bjpm_ptr = (build_jp_message_t *)calloc(1, sizeof(build_jp_message_t));
	if (!bjpm_ptr)
	    return NULL;

        bjpm_ptr->next = (build_jp_message_t *)NULL;
        bjpm_ptr->jp_message = (u_int8 *)calloc(1, MAX_JP_MESSAGE_SIZE +
						sizeof(pim_jp_encod_grp_t) +
						2 * sizeof(pim_encod_src_addr_t));
	if (!bjpm_ptr->jp_message) {
	    free(bjpm_ptr);
	    return NULL;
	}

        bjpm_ptr->jp_message_size = 0;
        bjpm_ptr->join_list_size = 0;
        bjpm_ptr->join_addr_number = 0;
        bjpm_ptr->join_list = (u_int8 *)calloc(1, MAX_JOIN_LIST_SIZE +
                                               sizeof(pim_encod_src_addr_t));
	if (!bjpm_ptr->join_list) {
	    free(bjpm_ptr->jp_message);
	    free(bjpm_ptr);
	    return NULL;
	}
        bjpm_ptr->prune_list_size = 0;
        bjpm_ptr->prune_addr_number = 0;
        bjpm_ptr->prune_list = (u_int8 *)calloc(1, MAX_PRUNE_LIST_SIZE +
                                                sizeof(pim_encod_src_addr_t));
	if (!bjpm_ptr->prune_list) {
	    free(bjpm_ptr->join_list);
	    free(bjpm_ptr->jp_message);
	    free(bjpm_ptr);
	    return NULL;
	}
        bjpm_ptr->rp_list_join_size = 0;
        bjpm_ptr->rp_list_join_number = 0;
        bjpm_ptr->rp_list_join = (u_int8 *)calloc(1, MAX_JOIN_LIST_SIZE +
                                                  sizeof(pim_encod_src_addr_t));
	if (!bjpm_ptr->rp_list_join) {
	    free(bjpm_ptr->prune_list);
	    free(bjpm_ptr->join_list);
	    free(bjpm_ptr->jp_message);
	    free(bjpm_ptr);
	    return NULL;
	}
        bjpm_ptr->rp_list_prune_size = 0;
        bjpm_ptr->rp_list_prune_number = 0;
        bjpm_ptr->rp_list_prune = (u_int8 *)calloc(1, MAX_PRUNE_LIST_SIZE +
                                                   sizeof(pim_encod_src_addr_t));
	if (!bjpm_ptr->rp_list_prune) {
	    free(bjpm_ptr->rp_list_join);
	    free(bjpm_ptr->prune_list);
	    free(bjpm_ptr->join_list);
	    free(bjpm_ptr->jp_message);
	    free(bjpm_ptr);
	    return NULL;
	}
        bjpm_ptr->curr_group = INADDR_ANY_N;
        bjpm_ptr->curr_group_msklen = 0;
        bjpm_ptr->holdtime = 0;

        return bjpm_ptr;
    }

    bjpm_ptr = build_jp_message_pool;
    build_jp_message_pool = build_jp_message_pool->next;
    build_jp_message_pool_counter--;
    bjpm_ptr->jp_message_size   = 0;
    bjpm_ptr->join_list_size    = 0;
    bjpm_ptr->join_addr_number  = 0;
    bjpm_ptr->prune_list_size   = 0;
    bjpm_ptr->prune_addr_number = 0;
    bjpm_ptr->curr_group        = INADDR_ANY_N;
    bjpm_ptr->curr_group_msklen = 0;

    return bjpm_ptr;
}


static void return_jp_working_buff(pim_nbr_entry_t *pim_nbr)
{
    build_jp_message_t *bjpm_ptr = pim_nbr->build_jp_message;

    if (!bjpm_ptr)
        return;

    /* Don't waste memory by keeping too many free buffers */
    /* TODO: check/modify the definitions for POOL_NUMBER and size */
    if (build_jp_message_pool_counter >= MAX_JP_MESSAGE_POOL_NUMBER) {
        free((void *)bjpm_ptr->jp_message);
        free((void *)bjpm_ptr->join_list);
        free((void *)bjpm_ptr->prune_list);
        free((void *)bjpm_ptr->rp_list_join);
        free((void *)bjpm_ptr->rp_list_prune);
        free((void *)bjpm_ptr);
    } else {
        bjpm_ptr->next = build_jp_message_pool;
        build_jp_message_pool = bjpm_ptr;
        build_jp_message_pool_counter++;
    }
    pim_nbr->build_jp_message = (build_jp_message_t *)NULL;
}


/* TODO: XXX: Currently, the (*,*,RP) stuff goes at the end of the
 * Join/Prune message. However, this particular implementation of PIM
 * processes the Join/Prune messages faster if (*,*,RP) is at the beginning.
 * Modify some of the functions below such that the
 * outgoing messages place (*,*,RP) at the beginning, not at the end.
 */
static void pack_jp_message(pim_nbr_entry_t *pim_nbr)
{
    build_jp_message_t *bjpm;
    u_int8 *data_ptr;

    bjpm = pim_nbr->build_jp_message;
    if (!bjpm || (bjpm->curr_group == INADDR_ANY_N))
        return;

    data_ptr = bjpm->jp_message + bjpm->jp_message_size;
    PUT_EGADDR(bjpm->curr_group, bjpm->curr_group_msklen, 0, data_ptr);
    PUT_HOSTSHORT(bjpm->join_addr_number, data_ptr);
    PUT_HOSTSHORT(bjpm->prune_addr_number, data_ptr);
    memcpy(data_ptr, bjpm->join_list, bjpm->join_list_size);
    data_ptr += bjpm->join_list_size;
    memcpy(data_ptr, bjpm->prune_list, bjpm->prune_list_size);
    data_ptr += bjpm->prune_list_size;
    bjpm->jp_message_size = (data_ptr - bjpm->jp_message);
    bjpm->curr_group = INADDR_ANY_N;
    bjpm->curr_group_msklen = 0;
    bjpm->join_list_size = 0;
    bjpm->join_addr_number = 0;
    bjpm->prune_list_size = 0;
    bjpm->prune_addr_number = 0;
    (*bjpm->num_groups_ptr)++;

    if (*bjpm->num_groups_ptr == ((u_int8)~0 - 1)) {
        if (bjpm->rp_list_join_number + bjpm->rp_list_prune_number) {
            /* Add the (*,*,RP) at the end */
            data_ptr = bjpm->jp_message + bjpm->jp_message_size;
            PUT_EGADDR(htonl(CLASSD_PREFIX), STAR_STAR_RP_MSKLEN, 0, data_ptr);
            PUT_HOSTSHORT(bjpm->rp_list_join_number, data_ptr);
            PUT_HOSTSHORT(bjpm->rp_list_prune_number, data_ptr);
            memcpy(data_ptr, bjpm->rp_list_join, bjpm->rp_list_join_size);
            data_ptr += bjpm->rp_list_join_size;
            memcpy(data_ptr, bjpm->rp_list_prune, bjpm->rp_list_prune_size);
            data_ptr += bjpm->rp_list_prune_size;
            bjpm->jp_message_size = (data_ptr - bjpm->jp_message);
            bjpm->rp_list_join_size = 0;
            bjpm->rp_list_join_number = 0;
            bjpm->rp_list_prune_size = 0;
            bjpm->rp_list_prune_number = 0;
            (*bjpm->num_groups_ptr)++;
        }
        send_jp_message(pim_nbr);
    }
}


void pack_and_send_jp_message(pim_nbr_entry_t *pim_nbr)
{
    u_int8 *data_ptr;
    build_jp_message_t *bjpm;

    if (!pim_nbr || !pim_nbr->build_jp_message)
        return;

    pack_jp_message(pim_nbr);

    bjpm = pim_nbr->build_jp_message;
    if (bjpm->rp_list_join_number + bjpm->rp_list_prune_number) {
        /* Add the (*,*,RP) at the end */
        data_ptr = bjpm->jp_message + bjpm->jp_message_size;
        PUT_EGADDR(htonl(CLASSD_PREFIX), STAR_STAR_RP_MSKLEN, 0, data_ptr);
        PUT_HOSTSHORT(bjpm->rp_list_join_number, data_ptr);
        PUT_HOSTSHORT(bjpm->rp_list_prune_number, data_ptr);
        memcpy(data_ptr, bjpm->rp_list_join, bjpm->rp_list_join_size);
        data_ptr += bjpm->rp_list_join_size;
        memcpy(data_ptr, bjpm->rp_list_prune, bjpm->rp_list_prune_size);
        data_ptr += bjpm->rp_list_prune_size;
        bjpm->jp_message_size = (data_ptr - bjpm->jp_message);
        bjpm->rp_list_join_size = 0;
        bjpm->rp_list_join_number = 0;
        bjpm->rp_list_prune_size = 0;
        bjpm->rp_list_prune_number = 0;
        (*bjpm->num_groups_ptr)++;
    }
    send_jp_message(pim_nbr);
}


static void send_jp_message(pim_nbr_entry_t *pim_nbr)
{
    u_int16 datalen;
    vifi_t vifi;

    datalen = pim_nbr->build_jp_message->jp_message_size;
    vifi = pim_nbr->vifi;
    memcpy(pim_send_buf + sizeof(struct ip) + sizeof(pim_header_t),
	   pim_nbr->build_jp_message->jp_message, datalen);
    send_pim(pim_send_buf, uvifs[vifi].uv_lcl_addr, allpimrouters_group,
             PIM_JOIN_PRUNE, datalen);
    return_jp_working_buff(pim_nbr);
}


/************************************************************************
 *                        PIM_ASSERT
 ************************************************************************/
int receive_pim_assert(u_int32 src, u_int32 dst __attribute__((unused)), char *pim_message, int datalen)
{
    vifi_t vifi;
    pim_encod_uni_addr_t eusaddr;
    pim_encod_grp_addr_t egaddr;
    u_int32 source, group;
    mrtentry_t *mrtentry_ptr, *mrtentry_ptr2;
    u_int8 *data_ptr;
    struct uvif *v;
    u_int32 assert_preference;
    u_int32 assert_metric;
    u_int32 assert_rptbit;
    u_int32 local_metric;
    u_int32 local_preference;
    u_int8  local_rptbit;
    u_int8  local_wins;
    pim_nbr_entry_t *original_upstream_router;

    vifi = find_vif_direct(src);
    if (vifi == NO_VIF) {
        /* Either a local vif or somehow received PIM_ASSERT from
         * non-directly connected router. Ignore it.
         */
        if (local_address(src) == NO_VIF)
            logit(LOG_INFO, 0, "Ignoring PIM_ASSERT from non-neighbor router %s",
                  inet_fmt(src, s1, sizeof(s1)));

        return FALSE;
    }

    /* Checksum */
    if (inet_cksum((u_int16 *)pim_message, datalen))
        return FALSE;

    v = &uvifs[vifi];
    if (uvifs[vifi].uv_flags & (VIFF_DOWN | VIFF_DISABLED | VIFF_NONBRS | VIFF_REGISTER))
        return FALSE;    /* Shoudn't come on this interface */

    data_ptr = (u_int8 *)(pim_message + sizeof(pim_header_t));

    /* Get the group and source addresses */
    GET_EGADDR(&egaddr, data_ptr);
    GET_EUADDR(&eusaddr, data_ptr);

    /* Get the metric related info */
    GET_HOSTLONG(assert_preference, data_ptr);
    GET_HOSTLONG(assert_metric, data_ptr);
    assert_rptbit = assert_preference & PIM_ASSERT_RPT_BIT;

    source = eusaddr.unicast_addr;
    group = egaddr.mcast_addr;
    /* Find the longest "active" entry, i.e. the one with a kernel mirror */
    if (assert_rptbit) {
        mrtentry_ptr = find_route(INADDR_ANY_N, group, MRTF_WC | MRTF_PMBR, DONT_CREATE);
        if (mrtentry_ptr)
            if (!(mrtentry_ptr->flags & MRTF_KERNEL_CACHE))
                if (mrtentry_ptr->flags & MRTF_WC) {
                    mrtentry_ptr = mrtentry_ptr->group->active_rp_grp->rp->rpentry->mrtlink;
                }
    } else {
        mrtentry_ptr = find_route(source, group, MRTF_SG | MRTF_WC | MRTF_PMBR, DONT_CREATE);
        if (mrtentry_ptr)
            if (!(mrtentry_ptr->flags & MRTF_KERNEL_CACHE)) {
                if (mrtentry_ptr->flags & MRTF_SG) {
                    mrtentry_ptr2 = mrtentry_ptr->group->grp_route;
                    if (mrtentry_ptr2 && (mrtentry_ptr2->flags & MRTF_KERNEL_CACHE))
                        mrtentry_ptr = mrtentry_ptr2;
                    else
                        mrtentry_ptr = mrtentry_ptr->group->active_rp_grp->rp->rpentry->mrtlink;
                } else {
                    if (mrtentry_ptr->flags & MRTF_WC)
                        mrtentry_ptr = mrtentry_ptr->group->active_rp_grp->rp->rpentry->mrtlink;
		}
            }
    }

    if (!mrtentry_ptr || !(mrtentry_ptr->flags & MRTF_KERNEL_CACHE)) {
        /* No routing entry or not "active" entry. Ignore the assert */
        return FALSE;
    }

    /* Prepare the local preference and metric */
    if ((mrtentry_ptr->flags & MRTF_PMBR)
        || ((mrtentry_ptr->flags & MRTF_SG)
            && !(mrtentry_ptr->flags & MRTF_RP))) {
        /* Either (S,G) (toward S) or (*,*,RP). */
        /* TODO: XXX: get the info from mrtentry, or source or from kernel ? */
        /*
          local_metric = mrtentry_ptr->source->metric;
          local_preference = mrtentry_ptr->source->preference;
        */
        local_metric = mrtentry_ptr->metric;
        local_preference = mrtentry_ptr->preference;
    } else {
        /* Should be (*,G) or (S,G)RPbit entry.
         * Get what we need from the RP info.
         */
        /* TODO: get the info from mrtentry, RP-entry or kernel? */
        /*
          local_metric =
          mrtentry_ptr->group->active_rp_grp->rp->rpentry->metric;
          local_preference =
          mrtentry_ptr->group->active_rp_grp->rp->rpentry->preference;
        */
        local_metric = mrtentry_ptr->metric;
        local_preference = mrtentry_ptr->preference;
    }

    local_rptbit = (mrtentry_ptr->flags & MRTF_RP);
    if (local_rptbit) {
        /* Make the RPT bit the most significant one */
        local_preference |= PIM_ASSERT_RPT_BIT;
    }

    if (VIFM_ISSET(vifi, mrtentry_ptr->oifs)) {
        /* The ASSERT has arrived on oif */

        /* TODO: XXX: here the processing order is different from the spec.
         * The spec requires first eventually to create a routing entry
         * (see 3.5.2.1(1) and then compare the metrics. Here we compare
         * first the metrics with the existing longest match entry and
         * if we lose then create a new entry and compare again. This saves
         * us the unnecessary creating of a routing entry if we anyway are
         * going to lose: for example the local (*,*,RP) vs the remote
         * (*,*,RP) or (*,G)
         */

        local_wins = compare_metrics(local_preference, local_metric,
                                     v->uv_lcl_addr, assert_preference,
                                     assert_metric, src);

        if (local_wins == TRUE) {
            /* TODO: verify the parameters */
            send_pim_assert(source, group, vifi, mrtentry_ptr);
            return TRUE;
        }

        /* Create a "better" routing entry and try again */
        if (assert_rptbit && (mrtentry_ptr->flags & MRTF_PMBR)) {
            /* The matching entry was (*,*,RP). Create (*,G) */
            mrtentry_ptr2 = find_route(INADDR_ANY_N, group, MRTF_WC, CREATE);
        } else if (!assert_rptbit && (mrtentry_ptr->flags & (MRTF_WC | MRTF_PMBR))) {
            /* create (S,G) */
            mrtentry_ptr2 = find_route(source, group, MRTF_SG, CREATE);
        } else {
            /* We have no chance to win. Give up and prune the oif */
            mrtentry_ptr2 = (mrtentry_t *)NULL;
        }

        if (mrtentry_ptr2) {
            mrtentry_ptr2->flags &= ~MRTF_NEW;
            /* TODO: XXX: The spec doesn't say what entry timer value
             * to use when the routing entry is created because of asserts.
             */
            SET_TIMER(mrtentry_ptr2->timer, PIM_DATA_TIMEOUT);
            if (mrtentry_ptr2->flags & MRTF_RP) {
                /* Either (*,G) or (S,G)RPbit entry.
                 * Get what we need from the RP info.
                 */
                /* TODO: where to get the metric+preference from? */
                /*
                  local_metric =
                  mrtentry_ptr->group->active_rp_grp->rp->rpentry->metric;
                  local_preference =
                  mrtentry_ptr->group->active_rp_grp->rp->rpentry->preference;
                */
                local_metric = mrtentry_ptr->metric;
                local_preference = mrtentry_ptr->preference;
                local_preference |= PIM_ASSERT_RPT_BIT;
            } else {
                /* (S,G) toward the source */
                /* TODO: where to get the metric from ? */
                /*
                  local_metric = mrtentry_ptr->source->metric;
                  local_preference = mrtentry_ptr->source->preference;
                */
                local_metric = mrtentry_ptr->metric;
                local_preference = mrtentry_ptr->preference;
            }

            local_wins = compare_metrics(local_preference, local_metric,
                                         v->uv_lcl_addr, assert_preference,
                                         assert_metric, src);
            if (local_wins == TRUE) {
                /* TODO: verify the parameters */
                send_pim_assert(source, group, vifi, mrtentry_ptr);
                return TRUE;
            }
            /* We lost, but have created the entry which has to be pruned */
            mrtentry_ptr = mrtentry_ptr2;
        }

        /* Have to remove that outgoing vifi from mrtentry_ptr */
        VIFM_SET(vifi, mrtentry_ptr->asserted_oifs);
        /* TODO: XXX: TIMER implem. dependency! */
        if (mrtentry_ptr->timer < PIM_ASSERT_TIMEOUT)
            SET_TIMER(mrtentry_ptr->timer, PIM_ASSERT_TIMEOUT);
        /* TODO: XXX: check that the timer of all affected routing entries
         * has been restarted.
         */
        change_interfaces(mrtentry_ptr,
                          mrtentry_ptr->incoming,
                          mrtentry_ptr->joined_oifs,
                          mrtentry_ptr->pruned_oifs,
                          mrtentry_ptr->leaves,
                          mrtentry_ptr->asserted_oifs, 0);

        return FALSE;  /* Doesn't matter the return value */
    } /* End of assert received on oif */


    if (mrtentry_ptr->incoming == vifi) {
        /* Assert received on iif */
        if (assert_rptbit) {
            if (!(mrtentry_ptr->flags & MRTF_RP))
                return TRUE;       /* The locally used upstream router will
                                     * win the assert, so don't change it.
                                     */
        }

        /* TODO: where to get the local metric and preference from?
         * system call or mrtentry is fine?
         */
        local_metric = mrtentry_ptr->metric;
        local_preference = mrtentry_ptr->preference;
        if (mrtentry_ptr->flags & MRTF_RP)
            local_preference |= PIM_ASSERT_RPT_BIT;

        local_wins = compare_metrics(local_preference, local_metric,
                                     mrtentry_ptr->upstream->address,
                                     assert_preference, assert_metric, src);

        if (local_wins == TRUE)
            return TRUE; /* return whatever */

        /* The upstream must be changed to the winner */
        mrtentry_ptr->preference = assert_preference;
        mrtentry_ptr->metric = assert_metric;
        mrtentry_ptr->upstream = find_pim_nbr(src);

        /* Check if the upstream router is different from the original one */
        if (mrtentry_ptr->flags & MRTF_PMBR)
            original_upstream_router = mrtentry_ptr->source->upstream;
        else
            if (mrtentry_ptr->flags & MRTF_RP)
                original_upstream_router = mrtentry_ptr->group->active_rp_grp->rp->rpentry->upstream;
            else
                original_upstream_router = mrtentry_ptr->source->upstream;
        if (mrtentry_ptr->upstream != original_upstream_router) {
            mrtentry_ptr->flags |= MRTF_ASSERTED;
            SET_TIMER(mrtentry_ptr->assert_timer, PIM_ASSERT_TIMEOUT);
        } else {
            mrtentry_ptr->flags &= ~MRTF_ASSERTED;
	}
    }

    return TRUE;
}


int send_pim_assert(u_int32 source, u_int32 group, vifi_t vifi, mrtentry_t *mrtentry_ptr)
{
    u_int8 *data_ptr;
    u_int8 *data_start_ptr;
    u_int32 local_preference;
    u_int32 local_metric;
    srcentry_t *srcentry_ptr __attribute__((unused));

    /* Don't send assert if the outgoing interface a tunnel or register vif */
    /* TODO: XXX: in the code above asserts are accepted over VIFF_TUNNEL.
     * Check if anything can go wrong if asserts are accepted and/or
     * sent over VIFF_TUNNEL.
     */
    if (uvifs[vifi].uv_flags & (VIFF_REGISTER | VIFF_TUNNEL))
        return FALSE;

    data_ptr = (u_int8 *)(pim_send_buf + sizeof(struct ip) + sizeof(pim_header_t));
    data_start_ptr = data_ptr;
    PUT_EGADDR(group, SINGLE_GRP_MSKLEN, 0, data_ptr);
    PUT_EUADDR(source, data_ptr);

    /* TODO: XXX: where to get the metric from: srcentry_ptr or mrtentry_ptr
     * or from the kernel?
     */
    if (mrtentry_ptr->flags & MRTF_PMBR) {
        /* (*,*,RP) */
        srcentry_ptr = mrtentry_ptr->source;
        /* TODO:
           set_incoming(srcentry_ptr, PIM_IIF_RP);
        */
    } else if (mrtentry_ptr->flags & MRTF_RP) {
        /* (*,G) or (S,G)RPbit (iif toward RP) */
        srcentry_ptr = mrtentry_ptr->group->active_rp_grp->rp->rpentry;
        /* TODO:
           set_incoming(srcentry_ptr, PIM_IIF_RP);
        */
    } else {
        /* (S,G) toward S */
        srcentry_ptr = mrtentry_ptr->source;
        /* TODO:
           set_incoming(srcentry_ptr, PIM_IIF_SOURCE);
        */
    }

    /* TODO: check again!
       local_metric = srcentry_ptr->metric;
       local_preference = srcentry_ptr->preference;
    */
    local_metric = mrtentry_ptr->metric;
    local_preference = mrtentry_ptr->preference;

    if (mrtentry_ptr->flags & MRTF_RP)
        local_preference |= PIM_ASSERT_RPT_BIT;
    PUT_HOSTLONG(local_preference, data_ptr);
    PUT_HOSTLONG(local_metric, data_ptr);
    send_pim(pim_send_buf, uvifs[vifi].uv_lcl_addr, allpimrouters_group,
             PIM_ASSERT, data_ptr - data_start_ptr);

    return TRUE;
}


/* Return TRUE if the local win, otherwise FALSE */
static int compare_metrics(u_int32 local_preference, u_int32 local_metric, u_int32 local_address,
			   u_int32 remote_preference, u_int32 remote_metric, u_int32 remote_address)
{
    /* Now lets see who has a smaller gun (aka "asserts war") */
    /* FYI, the smaller gun...err metric wins, but if the same
     * caliber, then the bigger network address wins. The order of
     * threatment is: preference, metric, address.
     */
    /* The RPT bits are already included as the most significant bits
     * of the preferences.
     */
    if (remote_preference > local_preference)
        return TRUE;

    if (remote_preference < local_preference)
        return FALSE;

    if (remote_metric > local_metric)
        return TRUE;

    if (remote_metric < local_metric)
        return FALSE;

    if (ntohl(local_address) > ntohl(remote_address))
        return TRUE;

    return FALSE;
}


/************************************************************************
 *                        PIM_BOOTSTRAP
 ************************************************************************/
#define PIM_BOOTSTRAP_MINLEN (PIM_MINLEN + PIM_ENCODE_UNI_ADDR_LEN)
int receive_pim_bootstrap(u_int32 src, u_int32 dst, char *pim_message, int datalen)
{
    u_int8               *data_ptr;
    u_int8               *max_data_ptr;
    u_int16              new_bsr_fragment_tag;
    u_int8               new_bsr_hash_masklen;
    u_int8               new_bsr_priority;
    pim_encod_uni_addr_t new_bsr_uni_addr;
    u_int32              new_bsr_address;
    struct rpfctl        rpfc;
    pim_nbr_entry_t      *n, *rpf_neighbor __attribute__((unused));
    u_int32              neighbor_addr;
    vifi_t               vifi, incoming = NO_VIF;
    int                  min_datalen;
    pim_encod_grp_addr_t curr_group_addr;
    pim_encod_uni_addr_t curr_rp_addr;
    u_int8               curr_rp_count;
    u_int8               curr_frag_rp_count;
    u_int16              reserved_short __attribute__((unused));
    u_int16              curr_rp_holdtime;
    u_int8               curr_rp_priority;
    u_int8               reserved_byte __attribute__((unused));
    u_int32              curr_group_mask;
    u_int32              prefix_h;
    grp_mask_t           *grp_mask_ptr;
    grp_mask_t           *grp_mask_next;
    rp_grp_entry_t       *grp_rp_entry_ptr;
    rp_grp_entry_t       *grp_rp_entry_next;

    /* Checksum */
    if (inet_cksum((u_int16 *)pim_message, datalen))
        return FALSE;

    if (find_vif_direct(src) == NO_VIF) {
        /* Either a local vif or somehow received PIM_BOOTSTRAP from
         * non-directly connected router. Ignore it.
         */
        if (local_address(src) == NO_VIF)
            logit(LOG_INFO, 0, "Ignoring PIM_BOOTSTRAP from non-neighbor router %s",
                  inet_fmt(src, s1, sizeof(s1)));

        return FALSE;
    }

    /* sanity check for the minimum length */
    if (datalen < PIM_BOOTSTRAP_MINLEN) {
        logit(LOG_NOTICE, 0, "receive_pim_bootstrap: Bootstrap message size(%u) is too short from %s",
              datalen, inet_fmt(src, s1, sizeof(s1)));

        return FALSE;
    }

    data_ptr = (u_int8 *)(pim_message + sizeof(pim_header_t));

    /* Parse the PIM_BOOTSTRAP message */
    GET_HOSTSHORT(new_bsr_fragment_tag, data_ptr);
    GET_BYTE(new_bsr_hash_masklen, data_ptr);
    GET_BYTE(new_bsr_priority, data_ptr);
    GET_EUADDR(&new_bsr_uni_addr, data_ptr);
    new_bsr_address = new_bsr_uni_addr.unicast_addr;

    if (local_address(new_bsr_address) != NO_VIF)
        return FALSE; /* The new BSR is one of my local addresses */

    /*
     * Compare the current BSR priority with the priority of the BSR
     * included in the message.
     */
    /* TODO: if I am just starting and will become the BSR,
     * I should accept the message coming from the current BSR and get the
     * current Cand-RP-Set.
     */
    if ((curr_bsr_priority > new_bsr_priority) ||
        ((curr_bsr_priority == new_bsr_priority)
         && (ntohl(curr_bsr_address) > ntohl(new_bsr_address)))) {
        /* The message's BSR is less preferred than the current BSR */
        return FALSE;  /* Ignore the received BSR message */
    }

    /* Check the iif, if this was PIM-ROUTERS multicast */
    if (dst == allpimrouters_group) {
        k_req_incoming(new_bsr_address, &rpfc);
        if (rpfc.iif == NO_VIF || rpfc.rpfneighbor.s_addr == INADDR_ANY_N) {
            /* coudn't find a route to the BSR */
            return FALSE;
        }

        neighbor_addr = rpfc.rpfneighbor.s_addr;
        incoming = rpfc.iif;
        if (uvifs[incoming].uv_flags & (VIFF_DISABLED | VIFF_DOWN | VIFF_REGISTER))
            return FALSE;	/* Shoudn't arrive on that interface */

        /* Find the upstream router */
        for (n = uvifs[incoming].uv_pim_neighbors; n != NULL; n = n->next) {
            if (ntohl(neighbor_addr) < ntohl(n->address))
                continue;

            if (neighbor_addr == n->address) {
                rpf_neighbor = n;
                break;
            }

            return FALSE;	/* No neighbor toward BSR found */
        }

        if (!n || n->address != src)
            return FALSE;	/* Sender of this message is not the RPF neighbor */

    } else {
        if (local_address(dst) == NO_VIF) {
            /* TODO: XXX: this situation should be handled earlier:
             * The destination is neither ALL_PIM_ROUTERS neither me
             */
            return FALSE;
	}

        /* Probably unicasted from the current DR */
        if (cand_rp_list != (cand_rp_t *)NULL) {
            /* Hmmm, I do have a Cand-RP-list, but some neighbor has a
             * different opinion and is unicasting it to me. Ignore this guy.
             */
            return FALSE;
        }

        for (vifi = 0; vifi < numvifs; vifi++) {
            if (uvifs[vifi].uv_flags & (VIFF_DISABLED | VIFF_DOWN | VIFF_REGISTER))
                continue;

            if (uvifs[vifi].uv_lcl_addr == dst) {
                incoming = vifi;
                break;
            }
        }

        if (incoming == NO_VIF) {
            /* Cannot find the receiving iif toward that DR */
            IF_DEBUG(DEBUG_RPF | DEBUG_PIM_BOOTSTRAP)
                logit(LOG_DEBUG, 0, "Unicast boostrap message from %s to ignored: cannot find iif", inet_fmt(src, s1, sizeof(s1)), inet_fmt(dst, s2, sizeof(s2)));
            return FALSE;
        }
        /* TODO: check the sender is directly connected and I am really the DR */
    }

    if (cand_rp_flag == TRUE) {
        /* If change in the BSR address, schedule immediate Cand-RP-Adv */
        /* TODO: use some random delay? */
        if (new_bsr_address != curr_bsr_address) {
            SET_TIMER(pim_cand_rp_adv_timer, 0);
        }
    }

    /* Forward the BSR Message first and then update the RP-set list */
    /* TODO: if the message was unicasted to me, resend? */
    for (vifi = 0; vifi < numvifs; vifi++) {
        if (vifi == incoming)
            continue;

        if (uvifs[vifi].uv_flags & (VIFF_DISABLED | VIFF_DOWN | VIFF_REGISTER | VIFF_NONBRS))
            continue;

        memcpy(pim_send_buf + sizeof(struct ip), pim_message, datalen);
        send_pim(pim_send_buf, uvifs[vifi].uv_lcl_addr, allpimrouters_group,
                 PIM_BOOTSTRAP, datalen - sizeof(pim_header_t));
    }

    max_data_ptr = (u_int8 *)pim_message + datalen;
    /* TODO: XXX: this 22 is HARDCODING!!! Do a bunch of definitions
     * and make it stylish!
     */
    min_datalen = 22;

    if (new_bsr_fragment_tag != curr_bsr_fragment_tag || new_bsr_address != curr_bsr_address) {
        /* Throw away the old segment */
        delete_rp_list(&segmented_cand_rp_list, &segmented_grp_mask_list);
    }

    curr_bsr_address      = new_bsr_address;
    curr_bsr_priority     = new_bsr_priority;
    curr_bsr_fragment_tag = new_bsr_fragment_tag;
    MASKLEN_TO_MASK(new_bsr_hash_masklen, curr_bsr_hash_mask);
    SET_TIMER(pim_bootstrap_timer, PIM_BOOTSTRAP_TIMEOUT);

    while (data_ptr + min_datalen <= max_data_ptr) {
        GET_EGADDR(&curr_group_addr, data_ptr);
        GET_BYTE(curr_rp_count, data_ptr);
        GET_BYTE(curr_frag_rp_count, data_ptr);
        GET_HOSTSHORT(reserved_short, data_ptr);
        MASKLEN_TO_MASK(curr_group_addr.masklen, curr_group_mask);
        if (curr_rp_count == 0) {
            delete_grp_mask(&cand_rp_list, &grp_mask_list,
                            curr_group_addr.mcast_addr, curr_group_mask);
            continue;
        }

        if (curr_rp_count == curr_frag_rp_count) {
            /* Add all RPs */
            while(curr_frag_rp_count--) {
                GET_EUADDR(&curr_rp_addr, data_ptr);
                GET_HOSTSHORT(curr_rp_holdtime, data_ptr);
                GET_BYTE(curr_rp_priority, data_ptr);
                GET_BYTE(reserved_byte, data_ptr);
                MASKLEN_TO_MASK(curr_group_addr.masklen, curr_group_mask);
                add_rp_grp_entry(&cand_rp_list, &grp_mask_list,
                                 curr_rp_addr.unicast_addr, curr_rp_priority,
                                 curr_rp_holdtime, curr_group_addr.mcast_addr,
                                 curr_group_mask,
                                 curr_bsr_hash_mask,
                                 curr_bsr_fragment_tag);
            }
            continue;
        }

        /*
         * This is a partial list of the RPs for this group prefix.
         * Save until all segments arrive.
         */
        prefix_h = ntohl(curr_group_addr.mcast_addr & curr_group_mask);
        for (grp_mask_ptr = segmented_grp_mask_list;
             grp_mask_ptr != (grp_mask_t *)NULL;
             grp_mask_ptr = grp_mask_ptr->next) {
            if (ntohl(grp_mask_ptr->group_addr
                      & grp_mask_ptr->group_mask) > prefix_h)
                continue;
            else
                break;
        }

        if ((grp_mask_ptr != (grp_mask_t *)NULL)
            && (grp_mask_ptr->group_addr == curr_group_addr.mcast_addr)
            && (grp_mask_ptr->group_mask == curr_group_mask)
            && (grp_mask_ptr->group_rp_number + curr_frag_rp_count == curr_rp_count)) {
            /* All missing PRs have arrived. Add all RP entries */
            while(curr_frag_rp_count--) {
                GET_EUADDR(&curr_rp_addr, data_ptr);
                GET_HOSTSHORT(curr_rp_holdtime, data_ptr);
                GET_BYTE(curr_rp_priority, data_ptr);
                GET_BYTE(reserved_byte, data_ptr);
                MASKLEN_TO_MASK(curr_group_addr.masklen, curr_group_mask);
                add_rp_grp_entry(&cand_rp_list,
                                 &grp_mask_list,
                                 curr_rp_addr.unicast_addr,
                                 curr_rp_priority,
                                 curr_rp_holdtime,
                                 curr_group_addr.mcast_addr,
                                 curr_group_mask,
                                 curr_bsr_hash_mask,
                                 curr_bsr_fragment_tag);
            }

            /* Add the rest from the previously saved segments */
            for(grp_rp_entry_ptr = grp_mask_ptr->grp_rp_next;
                grp_rp_entry_ptr != (rp_grp_entry_t *)NULL;
                grp_rp_entry_ptr = grp_rp_entry_ptr->grp_rp_next) {
                add_rp_grp_entry(&cand_rp_list,
                                 &grp_mask_list,
                                 grp_rp_entry_ptr->rp->rpentry->address,
                                 grp_rp_entry_ptr->priority,
                                 grp_rp_entry_ptr->holdtime,
                                 curr_group_addr.mcast_addr,
                                 curr_group_mask,
                                 curr_bsr_hash_mask,
                                 curr_bsr_fragment_tag);
            }
            delete_grp_mask(&segmented_cand_rp_list,
                            &segmented_grp_mask_list,
                            curr_group_addr.mcast_addr,
                            curr_group_mask);
        } else {
            /*Add the partially received RP-list to the group of pending RPs*/
            while(curr_frag_rp_count--) {
                GET_EUADDR(&curr_rp_addr, data_ptr);
                GET_HOSTSHORT(curr_rp_holdtime, data_ptr);
                GET_BYTE(curr_rp_priority, data_ptr);
                GET_BYTE(reserved_byte, data_ptr);
                MASKLEN_TO_MASK(curr_group_addr.masklen, curr_group_mask);
                add_rp_grp_entry(&segmented_cand_rp_list,
                                 &segmented_grp_mask_list,
                                 curr_rp_addr.unicast_addr,
                                 curr_rp_priority,
                                 curr_rp_holdtime,
                                 curr_group_addr.mcast_addr,
                                 curr_group_mask,
                                 curr_bsr_hash_mask,
                                 curr_bsr_fragment_tag);
            }
        }
    }

    /* Garbage collection. Check all group prefixes and if the
     * fragment_tag for a group_prefix is the same as curr_bsr_fragment_tag,
     * then remove all RPs for this group_prefix which have different
     * fragment tag.
     */
    for (grp_mask_ptr = grp_mask_list;
         grp_mask_ptr != (grp_mask_t *)NULL;
         grp_mask_ptr = grp_mask_next) {
        grp_mask_next = grp_mask_ptr->next;
        if (grp_mask_ptr->fragment_tag == curr_bsr_fragment_tag) {
            for (grp_rp_entry_ptr = grp_mask_ptr->grp_rp_next;
                 grp_rp_entry_ptr != (rp_grp_entry_t *)NULL;
                 grp_rp_entry_ptr = grp_rp_entry_next) {
                grp_rp_entry_next = grp_rp_entry_ptr->grp_rp_next;
                if (grp_rp_entry_ptr->fragment_tag != curr_bsr_fragment_tag)
                    delete_rp_grp_entry(&cand_rp_list, &grp_mask_list, grp_rp_entry_ptr);
            }
        }
    }

    /* Cleanup also the list used by incompleted segments */
    for (grp_mask_ptr = segmented_grp_mask_list;
         grp_mask_ptr != (grp_mask_t *)NULL;
         grp_mask_ptr = grp_mask_next) {
        grp_mask_next = grp_mask_ptr->next;
        if (grp_mask_ptr->fragment_tag == curr_bsr_fragment_tag) {
            for (grp_rp_entry_ptr = grp_mask_ptr->grp_rp_next;
                 grp_rp_entry_ptr != (rp_grp_entry_t *)NULL;
                 grp_rp_entry_ptr = grp_rp_entry_next) {
                grp_rp_entry_next = grp_rp_entry_ptr->grp_rp_next;
                if (grp_rp_entry_ptr->fragment_tag != curr_bsr_fragment_tag)
                    delete_rp_grp_entry(&segmented_cand_rp_list,
                                        &segmented_grp_mask_list,
                                        grp_rp_entry_ptr);
            }
        }
    }

    return TRUE;
}


void send_pim_bootstrap(void)
{
    int datalen;
    vifi_t vifi;

    if ((datalen = create_pim_bootstrap_message(pim_send_buf))) {
        for (vifi = 0; vifi < numvifs; vifi++) {
            if (uvifs[vifi].uv_flags & (VIFF_DISABLED | VIFF_DOWN | VIFF_REGISTER))
                continue;
            send_pim(pim_send_buf, uvifs[vifi].uv_lcl_addr,
                     allpimrouters_group, PIM_BOOTSTRAP, datalen);
        }
    }
}


/************************************************************************
 *                        PIM_CAND_RP_ADV
 ************************************************************************/
/*
 * If I am the Bootstrap router, process the advertisement, otherwise
 * ignore it.
 */
#define PIM_CAND_RP_ADV_MINLEN (PIM_MINLEN + PIM_ENCODE_UNI_ADDR_LEN)
int receive_pim_cand_rp_adv(u_int32 src, u_int32 dst __attribute__((unused)), char *pim_message, int datalen)
{
    u_int8 prefix_cnt;
    u_int8 priority;
    u_int16 holdtime;
    pim_encod_uni_addr_t cand_rp_addr;
    pim_encod_grp_addr_t encod_grp_addr;
    u_int8 *data_ptr;
    u_int32 grp_mask;

    /* Checksum */
    if (inet_cksum((u_int16 *)pim_message, datalen))
        return FALSE;

    /* if I am not the bootstrap RP, then do not accept the message */
    if (cand_bsr_flag == FALSE || curr_bsr_address != my_bsr_address)
        return FALSE;

    /* sanity check for the minimum length */
    if (datalen < PIM_CAND_RP_ADV_MINLEN) {
        logit(LOG_NOTICE, 0, "receive_pim_cand_rp_adv: cand_RP message size(%u) is too short from %s",
              datalen, inet_fmt(src, s1, sizeof(s1)));

        return FALSE;
    }

    data_ptr = (u_int8 *)(pim_message + sizeof(pim_header_t));
    /* Parse the CAND_RP_ADV message */
    /* TODO: XXX: check datalen whether it is at least the minimum */
    GET_BYTE(prefix_cnt, data_ptr);
    GET_BYTE(priority, data_ptr);
    GET_HOSTSHORT(holdtime, data_ptr);
    GET_EUADDR(&cand_rp_addr, data_ptr);
    if (prefix_cnt == 0) {
        /* The default 224.0.0.0 and masklen of 4 */
        MASKLEN_TO_MASK(ALL_MCAST_GROUPS_LENGTH, grp_mask);
        add_rp_grp_entry(&cand_rp_list, &grp_mask_list,
                         cand_rp_addr.unicast_addr, priority, holdtime,
                         htonl(ALL_MCAST_GROUPS_ADDR), grp_mask,
                         my_bsr_hash_mask,
                         curr_bsr_fragment_tag);

        return TRUE;
    }

    while (prefix_cnt--) {
        GET_EGADDR(&encod_grp_addr, data_ptr);
        MASKLEN_TO_MASK(encod_grp_addr.masklen, grp_mask);
        add_rp_grp_entry(&cand_rp_list, &grp_mask_list,
                         cand_rp_addr.unicast_addr, priority, holdtime,
                         encod_grp_addr.mcast_addr, grp_mask,
                         my_bsr_hash_mask,
                         curr_bsr_fragment_tag);
        /* TODO: Check for datalen */
    }

    return TRUE;
}


int send_pim_cand_rp_adv(void)
{
    u_int8 prefix_cnt;
    u_int32 grp_mask;
    pim_encod_grp_addr_t encod_grp_addr;
    u_int8 *data_ptr;

    if (!inet_valid_host(curr_bsr_address))
        return FALSE;  /* No BSR yet */

    if (curr_bsr_address == my_bsr_address) {
        /* I am the BSR and have to include my own group_prefix stuff */
        prefix_cnt = *cand_rp_adv_message.prefix_cnt_ptr;
        if (prefix_cnt == 0) {
            /* The default 224.0.0.0 and masklen of 4 */
            MASKLEN_TO_MASK(ALL_MCAST_GROUPS_LENGTH, grp_mask);
            add_rp_grp_entry(&cand_rp_list, &grp_mask_list,
                             my_cand_rp_address, my_cand_rp_priority,
                             my_cand_rp_holdtime,
                             htonl(ALL_MCAST_GROUPS_ADDR),
                             my_bsr_hash_mask,
                             grp_mask, curr_bsr_fragment_tag);
            return TRUE;
        }

        /* TODO: hardcoding!! */
        data_ptr = cand_rp_adv_message.buffer + (4 + 6);
        while (prefix_cnt--) {
            GET_EGADDR(&encod_grp_addr, data_ptr);
            MASKLEN_TO_MASK(encod_grp_addr.masklen, grp_mask);
            add_rp_grp_entry(&cand_rp_list,
                             &grp_mask_list,
                             my_cand_rp_address, my_cand_rp_priority,
                             my_cand_rp_holdtime,
                             encod_grp_addr.mcast_addr, grp_mask,
                             my_bsr_hash_mask,
                             curr_bsr_fragment_tag);
            /* TODO: Check for datalen */
        }

        return TRUE;
    }

    data_ptr = (u_int8 *)(pim_send_buf + sizeof(struct ip) + sizeof(pim_header_t));
    memcpy(data_ptr, cand_rp_adv_message.buffer, cand_rp_adv_message.message_size);
    send_pim_unicast(pim_send_buf, my_cand_rp_address, curr_bsr_address,
                     PIM_CAND_RP_ADV, cand_rp_adv_message.message_size);

    return TRUE;
}

/**
 * Local Variables:
 *  version-control: t
 *  indent-tabs-mode: t
 *  c-file-style: "ellemtel"
 *  c-basic-offset: 4
 * End:
 */

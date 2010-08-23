/*
 * Soft:        Keepalived is a failover program for the LVS project
 *              <www.linuxvirtualserver.org>. It monitor & manipulate
 *              a loadbalanced server pool using multi-layer checks.
 *
 * Part:        SNMP agent
 *
 * Version:     $Id$
 *
 * Author:      Vincent Bernat <bernat@luffy.cx>
 *
 *              This program is distributed in the hope that it will be useful,
 *              but WITHOUT ANY WARRANTY; without even the implied warranty of
 *              MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *              See the GNU General Public License for more details.
 *
 *              This program is free software; you can redistribute it and/or
 *              modify it under the terms of the GNU General Public License
 *              as published by the Free Software Foundation; either version
 *              2 of the License, or (at your option) any later version.
 *
 * Copyright (C) 2001-2009 Alexandre Cassen, <acassen@freebox.fr>
 */

#include "check_snmp.h"
#include "check_data.h"
#include "list.h"
#include "ipvswrapper.h"

/* Magic */
#define CHECK_SNMP_VSGROUPNAME 1
#define CHECK_SNMP_VSGROUPMEMBERTYPE 3
#define CHECK_SNMP_VSGROUPMEMBERFWMARK 4
#define CHECK_SNMP_VSGROUPMEMBERADDRTYPE 5
#define CHECK_SNMP_VSGROUPMEMBERADDRESS 6
#define CHECK_SNMP_VSGROUPMEMBERADDR1 7
#define CHECK_SNMP_VSGROUPMEMBERADDR2 8
#define CHECK_SNMP_VSGROUPMEMBERPORT 9
#define CHECK_SNMP_VSTYPE 10
#define CHECK_SNMP_VSNAMEGROUP 14
#define CHECK_SNMP_VSFWMARK 11
#define CHECK_SNMP_VSADDRTYPE 12
#define CHECK_SNMP_VSADDRESS 13
#define CHECK_SNMP_VSPORT 16
#define CHECK_SNMP_VSPROTOCOL 17
#define CHECK_SNMP_VSLOADBALANCINGALGO 18
#define CHECK_SNMP_VSLOADBALANCINGKIND 19
#define CHECK_SNMP_VSSTATUS 20
#define CHECK_SNMP_VSVIRTUALHOST 21
#define CHECK_SNMP_VSPERSIST 22
#define CHECK_SNMP_VSPERSISTTIMEOUT 23
#define CHECK_SNMP_VSPERSISTGRANULARITY 24
#define CHECK_SNMP_VSDELAYLOOP 25
#define CHECK_SNMP_VSHASUSPEND 26
#define CHECK_SNMP_VSALPHA 27
#define CHECK_SNMP_VSOMEGA 28
#define CHECK_SNMP_VSQUORUM 29
#define CHECK_SNMP_VSQUORUMSTATUS 30
#define CHECK_SNMP_VSQUORUMUP 31
#define CHECK_SNMP_VSQUORUMDOWN 32
#define CHECK_SNMP_VSHYSTERESIS 33
#define CHECK_SNMP_VSREALTOTAL 34
#define CHECK_SNMP_VSREALUP 35
#define CHECK_SNMP_VSSTATSCONNS 61
#define CHECK_SNMP_VSSTATSINPKTS 62
#define CHECK_SNMP_VSSTATSOUTPKTS 63
#define CHECK_SNMP_VSSTATSINBYTES 64
#define CHECK_SNMP_VSSTATSOUTBYTES 65
#define CHECK_SNMP_VSRATECPS 66
#define CHECK_SNMP_VSRATEINPPS 67
#define CHECK_SNMP_VSRATEOUTPPS 68
#define CHECK_SNMP_VSRATEINBPS 69
#define CHECK_SNMP_VSRATEOUTBPS 70
#define CHECK_SNMP_RSTYPE 36
#define CHECK_SNMP_RSADDRTYPE 37
#define CHECK_SNMP_RSADDRESS 38
#define CHECK_SNMP_RSPORT 39
#define CHECK_SNMP_RSSTATUS 40
#define CHECK_SNMP_RSWEIGHT 41
#define CHECK_SNMP_RSUPPERCONNECTIONLIMIT 42
#define CHECK_SNMP_RSLOWERCONNECTIONLIMIT 43
#define CHECK_SNMP_RSACTIONWHENDOWN 44
#define CHECK_SNMP_RSNOTIFYUP 45
#define CHECK_SNMP_RSNOTIFYDOWN 46
#define CHECK_SNMP_RSFAILEDCHECKS 47
#define CHECK_SNMP_RSSTATSCONNS 48
#define CHECK_SNMP_RSSTATSACTIVECONNS 49
#define CHECK_SNMP_RSSTATSINACTIVECONNS 50
#define CHECK_SNMP_RSSTATSPERSISTENTCONNS 51
#define CHECK_SNMP_RSSTATSINPKTS 52
#define CHECK_SNMP_RSSTATSOUTPKTS 53
#define CHECK_SNMP_RSSTATSINBYTES 54
#define CHECK_SNMP_RSSTATSOUTBYTES 55
#define CHECK_SNMP_RSRATECPS 56
#define CHECK_SNMP_RSRATEINPPS 57
#define CHECK_SNMP_RSRATEOUTPPS 58
#define CHECK_SNMP_RSRATEINBPS 59
#define CHECK_SNMP_RSRATEOUTBPS 60

static u_char*
check_snmp_vsgroup(struct variable *vp, oid *name, size_t *length,
		   int exact, size_t *var_len, WriteMethod **write_method)
{
	virtual_server_group *g;

	if ((g = (virtual_server_group *)
	     snmp_header_list_table(vp, name, length, exact,
				    var_len, write_method,
				    check_data->vs_group)) == NULL)
		return NULL;

	switch (vp->magic) {
	case CHECK_SNMP_VSGROUPNAME:
		*var_len = strlen(g->gname);
		return (u_char *)g->gname;
	default:
		break;
        }
        return NULL;
}

static u_char*
check_snmp_vsgroupmember(struct variable *vp, oid *name, size_t *length,
			 int exact, size_t *var_len, WriteMethod **write_method)
{
	static unsigned long long_ret;
	static uint32_t ip;
        oid *target, current[2], best[2];
        int result, target_len;
	int curgroup = 0, curentry;
	element e1, e2;
	virtual_server_group *group;
	virtual_server_group_entry *e, *be = NULL;
#define STATE_VSGM_FWMARK 1
#define STATE_VSGM_ADDRESS 2
#define STATE_VSGM_RANGE 3
#define STATE_VSGM_END 4
	int state;
	list l;


        if ((result = snmp_oid_compare(name, *length, vp->name, vp->namelen)) < 0) {
                memcpy(name, vp->name, sizeof(oid) * vp->namelen);
                *length = vp->namelen;
        }

	*write_method = 0;
	*var_len = sizeof(long);

	if (LIST_ISEMPTY(check_data->vs_group))
		return NULL;

	/* We search the best match: equal if exact, the lower OID in
	   the set of the OID strictly superior to the target
	   otherwise. */
        best[0] = best[1] = MAX_SUBID; /* Our best match */
        target = &name[vp->namelen];   /* Our target match */
        target_len = *length - vp->namelen;
	for (e1 = LIST_HEAD(check_data->vs_group); e1; ELEMENT_NEXT(e1)) {
		group = ELEMENT_DATA(e1);
		curgroup++;
		curentry = 0;
		if (target_len && (curgroup < target[0]))
			continue; /* Optimization: cannot be part of our set */
		if (be)
			break; /* Optimization: cannot be the lower anymore */
		state = STATE_VSGM_FWMARK;
		while (state != STATE_VSGM_END) {
			switch (state) {
			case STATE_VSGM_FWMARK:
				l = group->vfwmark;
				break;
			case STATE_VSGM_ADDRESS:
				l = group->addr_ip;
				break;
			case STATE_VSGM_RANGE:
				l = group->range;
				break;
			default:
				/* Dunno? */
				return NULL;
			}
			state++;
			if (LIST_ISEMPTY(l))
				continue;
			for (e2 = LIST_HEAD(l); e2; ELEMENT_NEXT(e2)) {
				e = ELEMENT_DATA(e2);
				curentry++;
				/* We build our current match */
				current[0] = curgroup;
				current[1] = curentry;
				/* And compare it to our target match */
				if ((result = snmp_oid_compare(current, 2, target,
							       target_len)) < 0)
					continue;
				if ((result == 0) && !exact)
					continue;
				if (result == 0) {
					/* Got an exact match and asked for it */
					be = e;
					goto vsgmember_found;
				}
				if (snmp_oid_compare(current, 2, best, 2) < 0) {
					/* This is our best match */
					memcpy(best, current, sizeof(oid) * 2);
					be = e;
					goto vsgmember_be_found;
				}
			}
		}
	}
	if (be == NULL)
		/* No best match */
		return NULL;
	if (exact)
		/* No exact match */
		return NULL;
 vsgmember_be_found:
	/* Let's use our best match */
        memcpy(target, best, sizeof(oid) * 2);
        *length = vp->namelen + 2;
 vsgmember_found:
	switch (vp->magic) {
	case CHECK_SNMP_VSGROUPMEMBERTYPE:
		if (be->vfwmark)
			long_ret = 1;
		else if (be->range)
			long_ret = 3;
		else
			long_ret = 2;
		return (u_char *)&long_ret;
	case CHECK_SNMP_VSGROUPMEMBERFWMARK:
		if (!be->vfwmark) break;
		long_ret = be->vfwmark;
		return (u_char *)&long_ret;
	case CHECK_SNMP_VSGROUPMEMBERADDRTYPE:
		if (be->vfwmark) break;
		long_ret = 1;	/* IPv4 */
		return (u_char *)&long_ret;
	case CHECK_SNMP_VSGROUPMEMBERADDRESS:
		if (be->vfwmark || be->range) break;
		*var_len = 4;
		return (u_char *)&be->addr_ip;
	case CHECK_SNMP_VSGROUPMEMBERADDR1:
		if (!be->range) break;
		*var_len = 4;
		return (u_char *)&be->addr_ip;
	case CHECK_SNMP_VSGROUPMEMBERADDR2:
		if (!be->range) break;
		*var_len = 4;
		ip = be->addr_ip + htonl(be->range);
		return (u_char *)&ip;
	case CHECK_SNMP_VSGROUPMEMBERPORT:
		if (be->vfwmark) break;
		long_ret = htons(be->addr_port);
		return (u_char *)&long_ret;
	default:
		return NULL;
	}
	/* If we are here, we asked for a non existent data. Try the
	   next one. */
	if (!exact && (name[*length-1] < MAX_SUBID))
		return check_snmp_vsgroupmember(vp, name, length,
						exact, var_len, write_method);
        return NULL;
}

static u_char*
check_snmp_virtualserver(struct variable *vp, oid *name, size_t *length,
			 int exact, size_t *var_len, WriteMethod **write_method)
{
	static unsigned long long_ret;
#ifdef _KRNL_2_6_
	static U64 counter64_ret;
#endif
	virtual_server *v;
	element e;

	if ((v = (virtual_server *)
	     snmp_header_list_table(vp, name, length, exact,
				    var_len, write_method,
				    check_data->vs)) == NULL)
		return NULL;

	switch (vp->magic) {
	case CHECK_SNMP_VSTYPE:
		if (v->vsgname)
			long_ret = 3;
		else if (v->vfwmark)
			long_ret = 1;
		else
			long_ret = 2;
		return (u_char*)&long_ret;
	case CHECK_SNMP_VSNAMEGROUP:
		if (!v->vsgname) break;
		*var_len = strlen(v->vsgname);
		return (u_char*)v->vsgname;
	case CHECK_SNMP_VSFWMARK:
		if (!v->vfwmark) break;
		long_ret = v->vfwmark;
		return (u_char*)&long_ret;
	case CHECK_SNMP_VSADDRTYPE:
		if (v->vfwmark || v->vsgname) break;
		long_ret = 1;	/* IPv4 */
		return (u_char*)&long_ret;
	case CHECK_SNMP_VSADDRESS:
		if (v->vfwmark || v->vsgname) break;
		*var_len = 4;
		return (u_char*)&v->addr_ip;
	case CHECK_SNMP_VSPORT:
		if (v->vfwmark || v->vsgname) break;
		long_ret = htons(v->addr_port);
		return (u_char*)&long_ret;
	case CHECK_SNMP_VSPROTOCOL:
		long_ret = (v->service_type == IPPROTO_TCP)?1:2;
		return (u_char*)&long_ret;
	case CHECK_SNMP_VSLOADBALANCINGALGO:
		if (strncmp(v->sched, "rr", SCHED_MAX_LENGTH) == 0)
			long_ret = 1;
		else if (strncmp(v->sched, "wrr", SCHED_MAX_LENGTH) == 0)
			long_ret = 2;
		else if (strncmp(v->sched, "lc", SCHED_MAX_LENGTH) == 0)
			long_ret = 3;
		else if (strncmp(v->sched, "wlc", SCHED_MAX_LENGTH) == 0)
			long_ret = 4;
		else if (strncmp(v->sched, "lblc", SCHED_MAX_LENGTH) == 0)
			long_ret = 5;
		else if (strncmp(v->sched, "lblcr", SCHED_MAX_LENGTH) == 0)
			long_ret = 6;
		else if (strncmp(v->sched, "dh", SCHED_MAX_LENGTH) == 0)
			long_ret = 7;
		else if (strncmp(v->sched, "sh", SCHED_MAX_LENGTH) == 0)
			long_ret = 8;
		else if (strncmp(v->sched, "sed", SCHED_MAX_LENGTH) == 0)
			long_ret = 9;
		else if (strncmp(v->sched, "nq", SCHED_MAX_LENGTH) == 0)
			long_ret = 10;
		else long_ret = 99;
		return (u_char*)&long_ret;
	case CHECK_SNMP_VSLOADBALANCINGKIND:
		long_ret = 0;
		switch (v->loadbalancing_kind) {
#ifdef _WITH_LVS_
#ifdef _KRNL_2_2_
		case 0:
			long_ret = 1;
			break;
		case IP_MASQ_F_VS_DROUTE:
			long_ret = 2;
			break;
		case IP_MASQ_F_VS_TUNNEL:
			long_ret = 3;
			break;
#else
		case IP_VS_CONN_F_MASQ:
			long_ret = 1;
			break;
		case IP_VS_CONN_F_DROUTE:
			long_ret = 2;
			break;
		case IP_VS_CONN_F_TUNNEL:
			long_ret = 3;
			break;
#endif
#endif
		}
		if (!long_ret) break;
		return (u_char*)&long_ret;
	case CHECK_SNMP_VSSTATUS:
		long_ret = v->alive?1:2;
		return (u_char*)&long_ret;
	case CHECK_SNMP_VSVIRTUALHOST:
		if (!v->virtualhost) break;
		*var_len = strlen(v->virtualhost);
		return (u_char*)v->virtualhost;
	case CHECK_SNMP_VSPERSIST:
		long_ret = (v->timeout_persistence > 0)?1:2;
		return (u_char*)&long_ret;
	case CHECK_SNMP_VSPERSISTTIMEOUT:
		if (v->timeout_persistence <= 0) break;
		long_ret = atol(v->timeout_persistence);
		return (u_char*)&long_ret;
	case CHECK_SNMP_VSPERSISTGRANULARITY:
		if (v->timeout_persistence <= 0) break;
		if (!v->granularity_persistence) break;
		*var_len = 4;
		return (u_char*)&v->granularity_persistence;
	case CHECK_SNMP_VSDELAYLOOP:
		if (v->delay_loop >= TIMER_MAX_SEC)
			long_ret = v->delay_loop/TIMER_HZ;
		else
			long_ret = v->delay_loop;
		return (u_char*)&long_ret;
	case CHECK_SNMP_VSHASUSPEND:
		long_ret = v->ha_suspend?1:2;
		return (u_char*)&long_ret;
	case CHECK_SNMP_VSALPHA:
		long_ret = v->alpha?1:2;
		return (u_char*)&long_ret;
	case CHECK_SNMP_VSOMEGA:
		long_ret = v->omega?1:2;
		return (u_char*)&long_ret;
	case CHECK_SNMP_VSQUORUM:
		long_ret = v->quorum;
		return (u_char*)&long_ret;
	case CHECK_SNMP_VSQUORUMSTATUS:
		long_ret = v->quorum_state?1:2;
		return (u_char*)&long_ret;
	case CHECK_SNMP_VSQUORUMUP:
		if (!v->quorum_up) break;
		*var_len = strlen(v->quorum_up);
		return (u_char*)v->quorum_up;
	case CHECK_SNMP_VSQUORUMDOWN:
		if (!v->quorum_down) break;
		*var_len = strlen(v->quorum_down);
		return (u_char*)v->quorum_down;
	case CHECK_SNMP_VSHYSTERESIS:
		long_ret = v->hysteresis;
		return (u_char*)&long_ret;
	case CHECK_SNMP_VSREALTOTAL:
		if (LIST_ISEMPTY(v->rs))
			long_ret = 0;
		else
			long_ret = LIST_SIZE(v->rs);
		return (u_char*)&long_ret;
	case CHECK_SNMP_VSREALUP:
		long_ret = 0;
		if (!LIST_ISEMPTY(v->rs))
			for (e = LIST_HEAD(v->rs); e; ELEMENT_NEXT(e))
				if (((real_server *)ELEMENT_DATA(e))->alive)
					long_ret++;
		return (u_char*)&long_ret;
#ifdef _KRNL_2_6_
	case CHECK_SNMP_VSSTATSCONNS:
		ipvs_update_stats(v);
		long_ret = v->stats.conns;
		return (u_char*)&long_ret;
	case CHECK_SNMP_VSSTATSINPKTS:
		ipvs_update_stats(v);
		long_ret = v->stats.inpkts;
		return (u_char*)&long_ret;
	case CHECK_SNMP_VSSTATSOUTPKTS:
		ipvs_update_stats(v);
		long_ret = v->stats.outpkts;
		return (u_char*)&long_ret;
	case CHECK_SNMP_VSSTATSINBYTES:
		ipvs_update_stats(v);
		counter64_ret.low = v->stats.inbytes & 0xffffffff;
		counter64_ret.high = v->stats.inbytes >> 32;
		*var_len = sizeof(U64);
		return (u_char*)&counter64_ret;
	case CHECK_SNMP_VSSTATSOUTBYTES:
		ipvs_update_stats(v);
		counter64_ret.low = v->stats.outbytes & 0xffffffff;
		counter64_ret.high = v->stats.outbytes >> 32;
		*var_len = sizeof(U64);
		return (u_char*)&counter64_ret;
	case CHECK_SNMP_VSRATECPS:
		ipvs_update_stats(v);
		long_ret = v->stats.cps;
		return (u_char*)&long_ret;
	case CHECK_SNMP_VSRATEINPPS:
		ipvs_update_stats(v);
		long_ret = v->stats.inpps;
		return (u_char*)&long_ret;
	case CHECK_SNMP_VSRATEOUTPPS:
		ipvs_update_stats(v);
		long_ret = v->stats.outpps;
		return (u_char*)&long_ret;
	case CHECK_SNMP_VSRATEINBPS:
		ipvs_update_stats(v);
		long_ret = v->stats.inbps;
		return (u_char*)&long_ret;
	case CHECK_SNMP_VSRATEOUTBPS:
		ipvs_update_stats(v);
		long_ret = v->stats.outbps;
		return (u_char*)&long_ret;
#endif
	default:
		return NULL;
        }
	if (!exact && (name[*length-1] < MAX_SUBID))
		return check_snmp_virtualserver(vp, name, length,
						exact, var_len, write_method);
        return NULL;
}

static u_char*
check_snmp_realserver(struct variable *vp, oid *name, size_t *length,
		      int exact, size_t *var_len, WriteMethod **write_method)
{
	static unsigned long long_ret;
#ifdef _KRNL_2_6_
	static U64 counter64_ret;
#endif
        oid *target, current[2], best[2];
        int result, target_len;
	int curvirtual = 0, curreal;
	real_server *e = NULL, *be = NULL;
	element e1, e2 = NULL;
	virtual_server *vs, *bvs = NULL;
#define STATE_RS_SORRY 1
#define STATE_RS_REGULAR_FIRST 2
#define STATE_RS_REGULAR_NEXT 3
#define STATE_RS_END 4
	int state;
	int type, btype;

        if ((result = snmp_oid_compare(name, *length, vp->name, vp->namelen)) < 0) {
                memcpy(name, vp->name, sizeof(oid) * vp->namelen);
                *length = vp->namelen;
        }

	*write_method = 0;
	*var_len = sizeof(long);

	if (LIST_ISEMPTY(check_data->vs))
		return NULL;

	/* We search the best match: equal if exact, the lower OID in
	   the set of the OID strictly superior to the target
	   otherwise. */
        best[0] = best[1] = MAX_SUBID; /* Our best match */
        target = &name[vp->namelen];   /* Our target match */
        target_len = *length - vp->namelen;
	for (e1 = LIST_HEAD(check_data->vs); e1; ELEMENT_NEXT(e1)) {
		vs = ELEMENT_DATA(e1);
		curvirtual++;
		curreal = 0;
		if (target_len && (curvirtual < target[0]))
			continue; /* Optimization: cannot be part of our set */
		if (be)
			break; /* Optimization: cannot be the lower anymore */
		state = STATE_RS_SORRY;
		while (state != STATE_RS_END) {
			switch (state) {
			case STATE_RS_SORRY:
				e = vs->s_svr;
				type = state++;
				break;
			case STATE_RS_REGULAR_FIRST:
				if (LIST_ISEMPTY(vs->rs)) {
					e = NULL;
					state = STATE_RS_END;
					break;
				}
				e2 = LIST_HEAD(vs->rs);
				e = ELEMENT_DATA(e2);
				type = state++;
				break;
			case STATE_RS_REGULAR_NEXT:
				type = state;
				ELEMENT_NEXT(e2);
				if (!e2) {
					e = NULL;
					state++;
					break;
				}
				e = ELEMENT_DATA(e2);
				break;
			default:
				/* Dunno? */
				return NULL;
			}
			if (!e)
				continue;
			curreal++;
			/* We build our current match */
			current[0] = curvirtual;
			current[1] = curreal;
			/* And compare it to our target match */
			if ((result = snmp_oid_compare(current, 2, target,
						       target_len)) < 0)
				continue;
			if ((result == 0) && !exact)
				continue;
			if (result == 0) {
				/* Got an exact match and asked for it */
				be = e;
				bvs = vs;
				btype = type;
				goto real_found;
			}
			if (snmp_oid_compare(current, 2, best, 2) < 0) {
				/* This is our best match */
				memcpy(best, current, sizeof(oid) * 2);
				be = e;
				bvs = vs;
				btype = type;
				goto real_be_found;
			}
		}
	}
	if (be == NULL)
		/* No best match */
		return NULL;
	if (exact)
		/* No exact match */
		return NULL;
 real_be_found:
	/* Let's use our best match */
        memcpy(target, best, sizeof(oid) * 2);
        *length = vp->namelen + 2;
 real_found:
	switch (vp->magic) {
	case CHECK_SNMP_RSTYPE:
		long_ret = (btype == STATE_RS_SORRY)?2:1;
		return (u_char*)&long_ret;
	case CHECK_SNMP_RSADDRTYPE:
		long_ret = 1;	/* IPv4 */
		return (u_char*)&long_ret;
	case CHECK_SNMP_RSADDRESS:
		*var_len = 4;
		return (u_char*)&be->addr_ip;
	case CHECK_SNMP_RSPORT:
		long_ret = htons(be->addr_port);
		return (u_char*)&long_ret;
	case CHECK_SNMP_RSSTATUS:
		if (btype == STATE_RS_SORRY) break;
		long_ret = be->alive?1:2;
		return (u_char*)&long_ret;
	case CHECK_SNMP_RSWEIGHT:
		if (btype == STATE_RS_SORRY) break;
		long_ret = be->weight;
		return (u_char*)&long_ret;
#ifdef _KRNL_2_6_
	case CHECK_SNMP_RSUPPERCONNECTIONLIMIT:
		if (btype == STATE_RS_SORRY) break;
		if (!be->u_threshold) break;
		long_ret = be->u_threshold;
		return (u_char*)&long_ret;
	case CHECK_SNMP_RSLOWERCONNECTIONLIMIT:
		if (btype == STATE_RS_SORRY) break;
		if (!be->l_threshold) break;
		long_ret = be->l_threshold;
		return (u_char*)&long_ret;
#endif
	case CHECK_SNMP_RSACTIONWHENDOWN:
		if (btype == STATE_RS_SORRY) break;
		long_ret = be->inhibit?2:1;
		return (u_char*)&long_ret;
	case CHECK_SNMP_RSNOTIFYUP:
		if (btype == STATE_RS_SORRY) break;
		if (!be->notify_up) break;
		*var_len = strlen(be->notify_up);
		return (u_char*)be->notify_up;
	case CHECK_SNMP_RSNOTIFYDOWN:
		if (btype == STATE_RS_SORRY) break;
		if (!be->notify_down) break;
		*var_len = strlen(be->notify_down);
		return (u_char*)be->notify_down;
	case CHECK_SNMP_RSFAILEDCHECKS:
		if (btype == STATE_RS_SORRY) break;
		if (LIST_ISEMPTY(be->failed_checkers))
			long_ret = 0;
		else
			long_ret = LIST_SIZE(be->failed_checkers);
		return (u_char*)&long_ret;
#ifdef _KRNL_2_6_
	case CHECK_SNMP_RSSTATSCONNS:
		ipvs_update_stats(bvs);
		long_ret = be->stats.conns;
		return (u_char*)&long_ret;
	case CHECK_SNMP_RSSTATSACTIVECONNS:
		ipvs_update_stats(bvs);
		long_ret = be->activeconns;
		return (u_char*)&long_ret;
	case CHECK_SNMP_RSSTATSINACTIVECONNS:
		ipvs_update_stats(bvs);
		long_ret = be->inactconns;
		return (u_char*)&long_ret;
	case CHECK_SNMP_RSSTATSPERSISTENTCONNS:
		ipvs_update_stats(bvs);
		long_ret = be->persistconns;
		return (u_char*)&long_ret;
	case CHECK_SNMP_RSSTATSINPKTS:
		ipvs_update_stats(bvs);
		long_ret = be->stats.inpkts;
		return (u_char*)&long_ret;
	case CHECK_SNMP_RSSTATSOUTPKTS:
		ipvs_update_stats(bvs);
		long_ret = be->stats.outpkts;
		return (u_char*)&long_ret;
	case CHECK_SNMP_RSSTATSINBYTES:
		ipvs_update_stats(bvs);
		counter64_ret.low = be->stats.inbytes & 0xffffffff;
		counter64_ret.high = be->stats.inbytes >> 32;
		*var_len = sizeof(U64);
		return (u_char*)&counter64_ret;
	case CHECK_SNMP_RSSTATSOUTBYTES:
		ipvs_update_stats(bvs);
		counter64_ret.low = be->stats.outbytes & 0xffffffff;
		counter64_ret.high = be->stats.outbytes >> 32;
		*var_len = sizeof(U64);
		return (u_char*)&counter64_ret;
	case CHECK_SNMP_RSRATECPS:
		ipvs_update_stats(bvs);
		long_ret = be->stats.cps;
		return (u_char*)&long_ret;
	case CHECK_SNMP_RSRATEINPPS:
		ipvs_update_stats(bvs);
		long_ret = be->stats.inpps;
		return (u_char*)&long_ret;
	case CHECK_SNMP_RSRATEOUTPPS:
		ipvs_update_stats(bvs);
		long_ret = be->stats.outpps;
		return (u_char*)&long_ret;
	case CHECK_SNMP_RSRATEINBPS:
		ipvs_update_stats(bvs);
		long_ret = be->stats.inbps;
		return (u_char*)&long_ret;
	case CHECK_SNMP_RSRATEOUTBPS:
		ipvs_update_stats(bvs);
		long_ret = be->stats.outbps;
		return (u_char*)&long_ret;
#endif
	default:
		return NULL;
	}
	/* If we are here, we asked for a non existent data. Try the
	   next one. */
	if (!exact && (name[*length-1] < MAX_SUBID))
		return check_snmp_realserver(vp, name, length,
					     exact, var_len, write_method);
        return NULL;
}

static oid check_oid[] = CHECK_OID;
static struct variable8 check_vars[] = {
	/* virtualServerGroupTable */
	{CHECK_SNMP_VSGROUPNAME, ASN_OCTET_STR, RONLY,
	 check_snmp_vsgroup, 3, {1, 1, 2}},
	/* virtualServerGroupMemberTable */
	{CHECK_SNMP_VSGROUPMEMBERTYPE, ASN_INTEGER, RONLY,
	 check_snmp_vsgroupmember, 3, {2, 1, 2}},
	{CHECK_SNMP_VSGROUPMEMBERFWMARK, ASN_UNSIGNED, RONLY,
	 check_snmp_vsgroupmember, 3, {2, 1, 3}},
	{CHECK_SNMP_VSGROUPMEMBERADDRTYPE, ASN_INTEGER, RONLY,
	 check_snmp_vsgroupmember, 3, {2, 1, 4}},
	{CHECK_SNMP_VSGROUPMEMBERADDRESS, ASN_OCTET_STR, RONLY,
	 check_snmp_vsgroupmember, 3, {2, 1, 5}},
	{CHECK_SNMP_VSGROUPMEMBERADDR1, ASN_OCTET_STR, RONLY,
	 check_snmp_vsgroupmember, 3, {2, 1, 6}},
	{CHECK_SNMP_VSGROUPMEMBERADDR2, ASN_OCTET_STR, RONLY,
	 check_snmp_vsgroupmember, 3, {2, 1, 7}},
	{CHECK_SNMP_VSGROUPMEMBERPORT, ASN_UNSIGNED, RONLY,
	 check_snmp_vsgroupmember, 3, {2, 1, 8}},
	/* virtualServerTable */
	{CHECK_SNMP_VSTYPE, ASN_INTEGER, RONLY,
	 check_snmp_virtualserver, 3, {3, 1, 2}},
	{CHECK_SNMP_VSNAMEGROUP, ASN_OCTET_STR, RONLY,
	 check_snmp_virtualserver, 3, {3, 1, 3}},
	{CHECK_SNMP_VSFWMARK, ASN_UNSIGNED, RONLY,
	 check_snmp_virtualserver, 3, {3, 1, 4}},
	{CHECK_SNMP_VSADDRTYPE, ASN_INTEGER, RONLY,
	 check_snmp_virtualserver, 3, {3, 1, 5}},
	{CHECK_SNMP_VSADDRESS, ASN_OCTET_STR, RONLY,
	 check_snmp_virtualserver, 3, {3, 1, 6}},
	{CHECK_SNMP_VSPORT, ASN_UNSIGNED, RONLY,
	 check_snmp_virtualserver, 3, {3, 1, 7}},
	{CHECK_SNMP_VSPROTOCOL, ASN_INTEGER, RONLY,
	 check_snmp_virtualserver, 3, {3, 1, 8}},
	{CHECK_SNMP_VSLOADBALANCINGALGO, ASN_INTEGER, RONLY,
	 check_snmp_virtualserver, 3, {3, 1, 9}},
	{CHECK_SNMP_VSLOADBALANCINGKIND, ASN_INTEGER, RONLY,
	 check_snmp_virtualserver, 3, {3, 1, 10}},
	{CHECK_SNMP_VSSTATUS, ASN_INTEGER, RONLY,
	 check_snmp_virtualserver, 3, {3, 1, 11}},
	{CHECK_SNMP_VSVIRTUALHOST, ASN_OCTET_STR, RONLY,
	 check_snmp_virtualserver, 3, {3, 1, 12}},
	{CHECK_SNMP_VSPERSIST, ASN_INTEGER, RONLY,
	 check_snmp_virtualserver, 3, {3, 1, 13}},
	{CHECK_SNMP_VSPERSISTTIMEOUT, ASN_UNSIGNED, RONLY,
	 check_snmp_virtualserver, 3, {3, 1, 14}},
	{CHECK_SNMP_VSPERSISTGRANULARITY, ASN_OCTET_STR, RONLY,
	 check_snmp_virtualserver, 3, {3, 1, 15}},
	{CHECK_SNMP_VSDELAYLOOP, ASN_UNSIGNED, RONLY,
	 check_snmp_virtualserver, 3, {3, 1, 16}},
	{CHECK_SNMP_VSHASUSPEND, ASN_INTEGER, RONLY,
	 check_snmp_virtualserver, 3, {3, 1, 17}},
	{CHECK_SNMP_VSALPHA, ASN_INTEGER, RONLY,
	 check_snmp_virtualserver, 3, {3, 1, 18}},
	{CHECK_SNMP_VSOMEGA, ASN_INTEGER, RONLY,
	 check_snmp_virtualserver, 3, {3, 1, 19}},
	{CHECK_SNMP_VSREALTOTAL, ASN_UNSIGNED, RONLY,
	 check_snmp_virtualserver, 3, {3, 1, 20}},
	{CHECK_SNMP_VSREALUP, ASN_UNSIGNED, RONLY,
	 check_snmp_virtualserver, 3, {3, 1, 21}},
	{CHECK_SNMP_VSQUORUM, ASN_UNSIGNED, RONLY,
	 check_snmp_virtualserver, 3, {3, 1, 22}},
	{CHECK_SNMP_VSQUORUMSTATUS, ASN_INTEGER, RONLY,
	 check_snmp_virtualserver, 3, {3, 1, 23}},
	{CHECK_SNMP_VSQUORUMUP, ASN_OCTET_STR, RONLY,
	 check_snmp_virtualserver, 3, {3, 1, 24}},
	{CHECK_SNMP_VSQUORUMDOWN, ASN_OCTET_STR, RONLY,
	 check_snmp_virtualserver, 3, {3, 1, 25}},
	{CHECK_SNMP_VSHYSTERESIS, ASN_UNSIGNED, RONLY,
	 check_snmp_virtualserver, 3, {3, 1, 26}},
	{CHECK_SNMP_VSSTATSCONNS, ASN_GAUGE, RONLY,
	 check_snmp_virtualserver, 3, {3, 1, 27}},
	{CHECK_SNMP_VSSTATSINPKTS, ASN_COUNTER, RONLY,
	 check_snmp_virtualserver, 3, {3, 1, 28}},
	{CHECK_SNMP_VSSTATSOUTPKTS, ASN_COUNTER, RONLY,
	 check_snmp_virtualserver, 3, {3, 1, 29}},
	{CHECK_SNMP_VSSTATSINBYTES, ASN_COUNTER64, RONLY,
	 check_snmp_virtualserver, 3, {3, 1, 30}},
	{CHECK_SNMP_VSSTATSOUTBYTES, ASN_COUNTER64, RONLY,
	 check_snmp_virtualserver, 3, {3, 1, 31}},
	{CHECK_SNMP_VSRATECPS, ASN_GAUGE, RONLY,
	 check_snmp_virtualserver, 3, {3, 1, 32}},
	{CHECK_SNMP_VSRATEINPPS, ASN_GAUGE, RONLY,
	 check_snmp_virtualserver, 3, {3, 1, 33}},
	{CHECK_SNMP_VSRATEOUTPPS, ASN_GAUGE, RONLY,
	 check_snmp_virtualserver, 3, {3, 1, 34}},
	{CHECK_SNMP_VSRATEINBPS, ASN_GAUGE, RONLY,
	 check_snmp_virtualserver, 3, {3, 1, 35}},
	{CHECK_SNMP_VSRATEOUTBPS, ASN_GAUGE, RONLY,
	 check_snmp_virtualserver, 3, {3, 1, 36}},
	/* realServerTable */
	{CHECK_SNMP_RSTYPE, ASN_INTEGER, RONLY,
	 check_snmp_realserver, 3, {4, 1, 2}},
	{CHECK_SNMP_RSADDRTYPE, ASN_INTEGER, RONLY,
	 check_snmp_realserver, 3, {4, 1, 3}},
	{CHECK_SNMP_RSADDRESS, ASN_OCTET_STR, RONLY,
	 check_snmp_realserver, 3, {4, 1, 4}},
	{CHECK_SNMP_RSPORT, ASN_UNSIGNED, RONLY,
	 check_snmp_realserver, 3, {4, 1, 5}},
	{CHECK_SNMP_RSSTATUS, ASN_INTEGER, RONLY,
	 check_snmp_realserver, 3, {4, 1, 6}},
	{CHECK_SNMP_RSWEIGHT, ASN_INTEGER, RONLY,
	 check_snmp_realserver, 3, {4, 1, 7}},
#ifdef _KRNL_2_6_
	{CHECK_SNMP_RSUPPERCONNECTIONLIMIT, ASN_UNSIGNED, RONLY,
	 check_snmp_realserver, 3, {4, 1, 8}},
	{CHECK_SNMP_RSLOWERCONNECTIONLIMIT, ASN_UNSIGNED, RONLY,
	 check_snmp_realserver, 3, {4, 1, 9}},
#endif
	{CHECK_SNMP_RSACTIONWHENDOWN, ASN_INTEGER, RONLY,
	 check_snmp_realserver, 3, {4, 1, 10}},
	{CHECK_SNMP_RSNOTIFYUP, ASN_OCTET_STR, RONLY,
	 check_snmp_realserver, 3, {4, 1, 11}},
	{CHECK_SNMP_RSNOTIFYDOWN, ASN_OCTET_STR, RONLY,
	 check_snmp_realserver, 3, {4, 1, 12}},
	{CHECK_SNMP_RSFAILEDCHECKS, ASN_UNSIGNED, RONLY,
	 check_snmp_realserver, 3, {4, 1, 13}},
#ifdef _KRNL_2_6_
	{CHECK_SNMP_RSSTATSCONNS, ASN_GAUGE, RONLY,
	 check_snmp_realserver, 3, {4, 1, 14}},
	{CHECK_SNMP_RSSTATSACTIVECONNS, ASN_GAUGE, RONLY,
	 check_snmp_realserver, 3, {4, 1, 15}},
	{CHECK_SNMP_RSSTATSINACTIVECONNS, ASN_GAUGE, RONLY,
	 check_snmp_realserver, 3, {4, 1, 16}},
	{CHECK_SNMP_RSSTATSPERSISTENTCONNS, ASN_GAUGE, RONLY,
	 check_snmp_realserver, 3, {4, 1, 17}},
	{CHECK_SNMP_RSSTATSINPKTS, ASN_COUNTER, RONLY,
	 check_snmp_realserver, 3, {4, 1, 18}},
	{CHECK_SNMP_RSSTATSOUTPKTS, ASN_COUNTER, RONLY,
	 check_snmp_realserver, 3, {4, 1, 19}},
	{CHECK_SNMP_RSSTATSINBYTES, ASN_COUNTER64, RONLY,
	 check_snmp_realserver, 3, {4, 1, 20}},
	{CHECK_SNMP_RSSTATSOUTBYTES, ASN_COUNTER64, RONLY,
	 check_snmp_realserver, 3, {4, 1, 21}},
	{CHECK_SNMP_RSRATECPS, ASN_GAUGE, RONLY,
	 check_snmp_realserver, 3, {4, 1, 22}},
	{CHECK_SNMP_RSRATEINPPS, ASN_GAUGE, RONLY,
	 check_snmp_realserver, 3, {4, 1, 23}},
	{CHECK_SNMP_RSRATEOUTPPS, ASN_GAUGE, RONLY,
	 check_snmp_realserver, 3, {4, 1, 24}},
	{CHECK_SNMP_RSRATEINBPS, ASN_GAUGE, RONLY,
	 check_snmp_realserver, 3, {4, 1, 25}},
	{CHECK_SNMP_RSRATEOUTBPS, ASN_GAUGE, RONLY,
	 check_snmp_realserver, 3, {4, 1, 26}},
#endif
};

void
check_snmp_agent_init()
{
	snmp_agent_init(check_oid, OID_LENGTH(check_oid), "Healthchecker",
			(struct variable *)check_vars,
			sizeof(struct variable8),
			sizeof(check_vars)/sizeof(struct variable8));
}

void
check_snmp_agent_close()
{
	snmp_agent_close(check_oid, OID_LENGTH(check_oid), "Healthchecker");
}
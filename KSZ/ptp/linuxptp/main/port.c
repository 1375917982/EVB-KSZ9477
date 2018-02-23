/**
 * @file port.c
 * @note Copyright (C) 2011 Richard Cochran <richardcochran@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/queue.h>
#if 1
#include <linux/if_ether.h>
#endif

#include "bmc.h"
#include "clock.h"
#include "filter.h"
#include "missing.h"
#include "msg.h"
#include "phc.h"
#include "port.h"
#include "print.h"
#include "sk.h"
#include "tlv.h"
#include "tmv.h"
#include "tsproc.h"
#include "util.h"

#ifdef KSZ_1588_PTP

#if 0
#define KSZ_DBG_HOST
#define KSZ_DBG_TIMER
#endif

#define _ptp_second
#define _ptp_text

#define USE_NET_IOCTL
#define USE_TIMESTAMP_OPER

#include "ksz_ptp.c"

struct dev_info ptpdev;
#endif

#define ALLOWED_LOST_RESPONSES 3
#define ANNOUNCE_SPAN 1

enum syfu_state {
	SF_EMPTY,
	SF_HAVE_SYNC,
	SF_HAVE_FUP,
};

enum syfu_event {
	SYNC_MISMATCH,
	SYNC_MATCH,
	FUP_MISMATCH,
	FUP_MATCH,
};

struct nrate_estimator {
	double ratio;
	tmv_t origin1;
	tmv_t ingress1;
	unsigned int max_count;
	unsigned int count;
	int ratio_valid;
};

struct port {
	LIST_ENTRY(port) list;
	char *name;
	struct clock *clock;
	struct transport *trp;
	enum timestamp_type timestamping;
	struct fdarray fda;
	int fault_fd;
	int phc_index;
	int jbod;
	struct foreign_clock *best;
	enum syfu_state syfu;
	struct ptp_message *last_syncfup;
	struct ptp_message *delay_req;
	struct ptp_message *peer_delay_req;
	struct ptp_message *peer_delay_resp;
	struct ptp_message *peer_delay_fup;
#ifdef KSZ_1588_PTP
	struct ptp_message *sync;
	struct ptp_message *follow_up;
	struct ptp_message *delay_resp;
	struct ptp_message *pdelay_resp;
	struct ptp_message *pdelay_resp_fup;
	struct port *host_port;
	int index;
	int dest_port;
	int forward_port;
	int receive_port;
	int pdelay_resp_port;
	int pdelay_resp_fup_port;
	int phys_port;
	int port_mask;
	int new_state;
	char *basename;
	char *devname;
	u32 p2p_sec;
	u32 p2p_nsec;
	int followUpReceiptTimeout;
	int syncTxContTimeout;
	int syncTxCont;
	u32 ann_rx_timeout:1;
	u32 ann_tx_timeout:1;
	u32 sync_rx_timeout:1;
	u32 sync_tx_timeout:1;
	u32 fup_rx_timeout:1;
	u32 fup_tx_timeout:1;
	u32 gm_change:1;
	u32 tx_ann:1;
	u32 sync_rx_tx:1;
	u32 multiple_pdr:1;
	u32 last_rx_sec;
	u32 last_tx_sec;
	u32 rx_sec;
	u32 tx_sec;
	struct timespec sync_ts;
	struct timestamp sync_timestamp;
	struct timestamp fup_timestamp;
	Integer64 sync_correction;
	Integer64 fup_correction;
#endif
	int peer_portid_valid;
	struct PortIdentity peer_portid;
	struct {
		UInteger16 announce;
		UInteger16 delayreq;
		UInteger16 sync;
	} seqnum;
	tmv_t peer_delay;
	struct tsproc *tsproc;
	int log_sync_interval;
	struct nrate_estimator nrate;
	unsigned int pdr_missing;
	unsigned int multiple_seq_pdr_count;
	unsigned int multiple_pdr_detected;
	enum port_state (*state_machine)(enum port_state state,
					 enum fsm_event event, int mdiff);
	/* portDS */
	struct PortIdentity portIdentity;
	enum port_state     state; /*portState*/
	Integer64           asymmetry;
	int                 asCapable;
	Integer8            logMinDelayReqInterval;
	TimeInterval        peerMeanPathDelay;
	Integer8            logAnnounceInterval;
	UInteger8           announceReceiptTimeout;
	int                 announce_span;
	UInteger8           syncReceiptTimeout;
	UInteger8           transportSpecific;
	Integer8            logSyncInterval;
	Enumeration8        delayMechanism;
	Integer8            logMinPdelayReqInterval;
	UInteger32          neighborPropDelayThresh;
	int                 follow_up_info;
	int                 freq_est_interval;
	int                 hybrid_e2e;
	int                 min_neighbor_prop_delay;
	int                 path_trace_enabled;
	int                 rx_timestamp_offset;
	int                 tx_timestamp_offset;
	int                 link_status;
	struct fault_interval flt_interval_pertype[FT_CNT];
	enum fault_type     last_fault_type;
	unsigned int        versionNumber; /*UInteger4*/
	/* foreignMasterDS */
	LIST_HEAD(fm, foreign_clock) foreign_masters;
};

#define portnum(p) (p->portIdentity.portNumber)

#ifdef KSZ_1588_PTP
#define portdst(p) (1 << ((p) - 1))
#endif

#define NSEC2SEC 1000000000LL

#ifdef KSZ_1588_PTP
int port_set_peer_delay(struct port *p)
{
	int port = p->phys_port;

#ifdef KSZ_DBG_HOST
if (!is_peer_port(p->clock, p))
printf(" !! %s %d %p\n", __func__, portnum(p), p);
#endif
	if (need_dest_port(p->clock))
		port = p->pdelay_resp_port;
	if (!port)
		return -ENODEV;
	return set_peer_delay(&ptpdev,
		port - 1, (int) p->peer_delay);
}

int port_get_msg_info(struct port *p, struct ptp_header *header, int *tx,
	u32 *port, u32 *sec, u32 *nsec)
{
	int rc;

	rc = get_msg_info(&ptpdev, header, tx,
		port, sec, nsec);
	return rc;
}

int port_set_msg_info(struct port *p, struct ptp_header *header, u32 port,
	u32 sec, u32 nsec)
{
	int rc;

	rc = set_msg_info(&ptpdev, header,
		port, sec, nsec);
	return rc;
}

int port_set_port_cfg(struct port *p, int enable, int asCapable)
{
	int port = p->phys_port;

	if (!port)
		return -ENODEV;
	return set_port_cfg(&ptpdev,
		port - 1, enable, asCapable);
}

int port_exit_ptp(struct clock *c)
{
	int rc;
	struct timePropertiesDS *tp = clock_time_properties(c);

	if (tp->flags & UTC_OFF_VALID)
		rc = set_utc_offset(&ptpdev,
			tp->currentUtcOffset);
	rc = set_global_cfg(&ptpdev,
		0, 0, 0, 0);
	rc = ptp_dev_exit(&ptpdev);
	close(ptpdev.sock);
	return rc;
}

int port_init_ptp(struct port *p, int cap, int *drift, UInteger8 *version,
	UInteger8 *ports, UInteger32 *access_delay)
{
	int offset;
	int rc;
	int master;
	int two_step;
	int p2p;
	int as;
	int unicast;
	int alternate;
	int csum;
	int check;
	int delay_assoc;
	int pdelay_assoc;
	int sync_assoc;
	int drop_sync;
	int priority;
	int started;
	UInteger8 domain;

	strncpy(ptpdev.name, p->devname, sizeof(ptpdev.name));
	ptpdev.sock = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_ALL));

	rc = ptp_dev_init(&ptpdev, cap,
		drift, version, ports, &ptp_host_port);
	pr_info("  version=%d ports=%d",
		*version, *ports);
	ptp_drift = *drift;
	ptp_version = *version;
	ptp_ports = *ports;
	rc = get_global_cfg(&ptpdev,
		&master, &two_step, &p2p, &as,
		&unicast, &alternate, &csum, &check,
		&delay_assoc, &pdelay_assoc, &sync_assoc, &drop_sync,
		&priority, &domain, access_delay, &started);
	pr_info("  access_delay=%u", *access_delay);
	if (!get_min_sync_interval(p->clock)) {
		UInteger32 min_sync_interval;

		if (*access_delay >= 1000000)
			/* About ^-2. */
			min_sync_interval = 200000000;
		else if (*access_delay >= 100000)
			min_sync_interval = 60000000;
		else
			min_sync_interval = 30000000;
		set_min_sync_interval(p->clock, min_sync_interval);
	}
	rc = set_global_cfg(&ptpdev,
		0, clock_two_step(p->clock), (p->delayMechanism == DM_P2P),
		need_stop_forwarding(p->clock) | boundary_clock(p->clock));
	rc = set_hw_domain(&ptpdev,
		clock_domain_number(p->clock));
	rc = get_utc_offset(&ptpdev,
		&offset);
	pr_info("  UTC offset: %d", offset);
	if (offset) {
		struct timePropertiesDS *tp = clock_time_properties(p->clock);

		if (offset > tp->currentUtcOffset) {
			set_master_utc_offset(p->clock, offset);
			tp->currentUtcOffset = offset;
		} else if (!(tp->flags & UTC_OFF_VALID)) {
			if (tp->currentUtcOffset)
				set_master_utc_offset(p->clock,
					tp->currentUtcOffset);
		}
	}
	return rc;
}

int new_state(struct port *p)
{
	return p->new_state;
}

void port_set_host_port(struct port *p, struct port *host_port)
{
#if 0
printf("%s %d %d; %p %p\n", __func__, portnum(p), portnum(host_port),
p, host_port);
#endif
	p->host_port = host_port;
}

void port_set_port_state(struct port *p, enum fsm_event event)
{
#if 0
printf("%s %d %d %d\n", __func__, portnum(p), p->state, p->host_port->state);
#endif
	port_dispatch(p, event, 0);
}
#endif

static int port_capable(struct port *p);
static int port_is_ieee8021as(struct port *p);
static void port_nrate_initialize(struct port *p);

static int announce_compare(struct ptp_message *m1, struct ptp_message *m2)
{
	struct announce_msg *a = &m1->announce, *b = &m2->announce;
	int len =
		sizeof(a->grandmasterPriority1) +
		sizeof(a->grandmasterClockQuality) +
		sizeof(a->grandmasterPriority2) +
		sizeof(a->grandmasterIdentity) +
		sizeof(a->stepsRemoved);

	return memcmp(&a->grandmasterPriority1, &b->grandmasterPriority1, len);
}

static void announce_to_dataset(struct ptp_message *m, struct port *p,
				struct dataset *out)
{
	struct announce_msg *a = &m->announce;
	out->priority1    = a->grandmasterPriority1;
	out->identity     = a->grandmasterIdentity;
	out->quality      = a->grandmasterClockQuality;
	out->priority2    = a->grandmasterPriority2;
	out->stepsRemoved = a->stepsRemoved;
	out->sender       = m->header.sourcePortIdentity;
	out->receiver     = p->portIdentity;
}

static int clear_fault_asap(struct fault_interval *faint)
{
	switch (faint->type) {
	case FTMO_LINEAR_SECONDS:
		return faint->val == 0 ? 1 : 0;
	case FTMO_LOG2_SECONDS:
		return faint->val == FRI_ASAP ? 1 : 0;
	case FTMO_CNT:
		return 0;
	}
	return 0;
}

static int msg_current(struct ptp_message *m, struct timespec now)
{
	int64_t t1, t2, tmo;

	t1 = m->ts.host.tv_sec * NSEC2SEC + m->ts.host.tv_nsec;
	t2 = now.tv_sec * NSEC2SEC + now.tv_nsec;

	if (m->header.logMessageInterval < -63) {
		tmo = 0;
	} else if (m->header.logMessageInterval > 31) {
		tmo = INT64_MAX;
	} else if (m->header.logMessageInterval < 0) {
		tmo = 4LL * NSEC2SEC / (1 << -m->header.logMessageInterval);
	} else {
		tmo = 4LL * (1 << m->header.logMessageInterval) * NSEC2SEC;
	}

	return t2 - t1 < tmo;
}

static int msg_source_equal(struct ptp_message *m1, struct foreign_clock *fc)
{
	struct PortIdentity *id1, *id2;
	id1 = &m1->header.sourcePortIdentity;
	id2 = &fc->dataset.sender;
	return 0 == memcmp(id1, id2, sizeof(*id1));
}

static int pid_eq(struct PortIdentity *a, struct PortIdentity *b)
{
	return 0 == memcmp(a, b, sizeof(*a));
}

static int source_pid_eq(struct ptp_message *m1, struct ptp_message *m2)
{
	return pid_eq(&m1->header.sourcePortIdentity,
		      &m2->header.sourcePortIdentity);
}

enum fault_type last_fault_type(struct port *port)
{
	return port->last_fault_type;
}

void fault_interval(struct port *port, enum fault_type ft,
		    struct fault_interval *i)
{
	i->type = port->flt_interval_pertype[ft].type;
	i->val = port->flt_interval_pertype[ft].val;
}

int port_fault_fd(struct port *port)
{
	return port->fault_fd;
}

struct fdarray *port_fda(struct port *port)
{
	return &port->fda;
}

int set_tmo_log(int fd, unsigned int scale, int log_seconds)
{
	struct itimerspec tmo = {
		{0, 0}, {0, 0}
	};
	uint64_t ns;
	int i;

	if (log_seconds < 0) {

		log_seconds *= -1;
		for (i = 1, ns = scale * 500000000ULL; i < log_seconds; i++) {
			ns >>= 1;
		}
		while (ns >= NS_PER_SEC) {
			ns -= NS_PER_SEC;
			tmo.it_value.tv_sec++;
		}
		tmo.it_value.tv_nsec = ns;

	} else
		tmo.it_value.tv_sec = scale * (1 << log_seconds);

	return timerfd_settime(fd, 0, &tmo, NULL);
}

int set_tmo_lin(int fd, int seconds)
{
	struct itimerspec tmo = {
		{0, 0}, {0, 0}
	};

	tmo.it_value.tv_sec = seconds;
	return timerfd_settime(fd, 0, &tmo, NULL);
}

#ifdef KSZ_1588_PTP
int set_tmo_ms(int fd, int milliseconds)
{
	struct itimerspec tmo = {
		{0, 0}, {0, 0}
	};

	tmo.it_value.tv_nsec = milliseconds * (NS_PER_SEC / 1000);
	while (tmo.it_value.tv_nsec >= NS_PER_SEC) {
		tmo.it_value.tv_nsec -= NS_PER_SEC;
		tmo.it_value.tv_sec++;
	}
	return timerfd_settime(fd, 0, &tmo, NULL);
}

#endif

int set_tmo_random(int fd, int min, int span, int log_seconds)
{
	uint64_t value_ns, min_ns, span_ns;
	struct itimerspec tmo = {
		{0, 0}, {0, 0}
	};

	if (log_seconds >= 0) {
		min_ns = min * NS_PER_SEC << log_seconds;
		span_ns = span * NS_PER_SEC << log_seconds;
	} else {
		min_ns = min * NS_PER_SEC >> -log_seconds;
		span_ns = span * NS_PER_SEC >> -log_seconds;
	}

	value_ns = min_ns + (span_ns * (random() % (1 << 15) + 1) >> 15);

	tmo.it_value.tv_sec = value_ns / NS_PER_SEC;
	tmo.it_value.tv_nsec = value_ns % NS_PER_SEC;

	return timerfd_settime(fd, 0, &tmo, NULL);
}

int port_set_fault_timer_log(struct port *port,
			     unsigned int scale, int log_seconds)
{
	return set_tmo_log(port->fault_fd, scale, log_seconds);
}

int port_set_fault_timer_lin(struct port *port, int seconds)
{
	return set_tmo_lin(port->fault_fd, seconds);
}

static void fc_clear(struct foreign_clock *fc)
{
	struct ptp_message *m;

	while (fc->n_messages) {
		m = TAILQ_LAST(&fc->messages, messages);
		TAILQ_REMOVE(&fc->messages, m, list);
		fc->n_messages--;
		msg_put(m);
	}
}

static void fc_prune(struct foreign_clock *fc)
{
	struct timespec now;
	struct ptp_message *m;

	clock_gettime(CLOCK_MONOTONIC, &now);

	while (fc->n_messages > FOREIGN_MASTER_THRESHOLD) {
		m = TAILQ_LAST(&fc->messages, messages);
		TAILQ_REMOVE(&fc->messages, m, list);
		fc->n_messages--;
		msg_put(m);
	}

	while (!TAILQ_EMPTY(&fc->messages)) {
		m = TAILQ_LAST(&fc->messages, messages);
		if (msg_current(m, now))
			break;
		TAILQ_REMOVE(&fc->messages, m, list);
		fc->n_messages--;
		msg_put(m);
	}
}

#ifndef KSZ_1588_PTP
static void ts_add(struct timespec *ts, int ns)
{
	if (!ns) {
		return;
	}
	ts->tv_nsec += ns;
	while (ts->tv_nsec < 0) {
		ts->tv_nsec += (long) NS_PER_SEC;
		ts->tv_sec--;
	}
	while (ts->tv_nsec >= (long) NS_PER_SEC) {
		ts->tv_nsec -= (long) NS_PER_SEC;
		ts->tv_sec++;
	}
}
#endif

#ifdef KSZ_1588_PTP
static void tspdu_to_timestamp(struct timestamp *src, struct Timestamp *dst)
{
	dst->seconds_lsb = (uint32_t) src->sec;
	dst->seconds_msb = src->sec >> 32;
	dst->nanoseconds = src->nsec;
}
#endif

static void ts_to_timestamp(struct timespec *src, struct Timestamp *dst)
{
	dst->seconds_lsb = src->tv_sec;
	dst->seconds_msb = 0;
	dst->nanoseconds = src->tv_nsec;
}

/*
 * Returns non-zero if the announce message is different than last.
 */
static int add_foreign_master(struct port *p, struct ptp_message *m)
{
	struct foreign_clock *fc;
	struct ptp_message *tmp;
	int broke_threshold = 0, diff = 0;

	LIST_FOREACH(fc, &p->foreign_masters, list) {
		if (msg_source_equal(m, fc))
			break;
	}
	if (!fc) {
		pr_notice("port %hu: new foreign master %s", portnum(p),
			pid2str(&m->header.sourcePortIdentity));

		fc = malloc(sizeof(*fc));
		if (!fc) {
			pr_err("low memory, failed to add foreign master");
			return 0;
		}
		memset(fc, 0, sizeof(*fc));
		TAILQ_INIT(&fc->messages);
		LIST_INSERT_HEAD(&p->foreign_masters, fc, list);
		fc->port = p;
		fc->dataset.sender = m->header.sourcePortIdentity;
		/* We do not count this first message, see 9.5.3(b) */
#ifdef KSZ_1588_PTP
		if (!port_is_ieee8021as(p))
#endif
		return 0;
	}

	/*
	 * If this message breaks the threshold, that is an important change.
	 */
	fc_prune(fc);
	if (FOREIGN_MASTER_THRESHOLD - 1 == fc->n_messages)
		broke_threshold = 1;
#ifdef KSZ_1588_PTP
	else if (port_is_ieee8021as(p))
		broke_threshold = 1;
#endif

	/*
	 * Okay, go ahead and add this announcement.
	 */
	msg_get(m);
	fc->n_messages++;
	TAILQ_INSERT_HEAD(&fc->messages, m, list);

	/*
	 * Test if this announcement contains changed information.
	 */
	if (fc->n_messages > 1) {
		tmp = TAILQ_NEXT(m, list);
		diff = announce_compare(m, tmp);
	}

	return broke_threshold || diff;
}

static int follow_up_info_append(struct port *p, struct ptp_message *m)
{
	struct follow_up_info_tlv *fui;
	fui = (struct follow_up_info_tlv *) m->follow_up.suffix;
	fui->type = TLV_ORGANIZATION_EXTENSION;
	fui->length = sizeof(*fui) - sizeof(fui->type) - sizeof(fui->length);
	memcpy(fui->id, ieee8021_id, sizeof(ieee8021_id));
	fui->subtype[2] = 1;
	clock_get_follow_up_info(p->clock, fui);
	m->tlv_count = 1;
	return sizeof(*fui);
}

static struct follow_up_info_tlv *follow_up_info_extract(struct ptp_message *m)
{
	struct follow_up_info_tlv *f;
#ifdef KSZ_1588_PTP
	struct TLV *tlv;
	uint8_t *ptr;
	int i;

	ptr = (uint8_t *) m->follow_up.suffix;
	for (i = 0; i < m->tlv_count; i++) {
		tlv = (struct TLV *) ptr;
		f = (struct follow_up_info_tlv *) tlv;
		if (f->type == TLV_ORGANIZATION_EXTENSION &&
		    f->length >= sizeof(*f) - sizeof(struct TLV) &&
		    !memcmp(f->id, ieee8021_id, sizeof(ieee8021_id)) &&
		    f->subtype[0] == 0 && f->subtype[1] == 0 &&
		    f->subtype[2] == 1)
			return f;
		ptr += sizeof(struct TLV) + tlv->length;
	}
	return NULL;
#else
	f = (struct follow_up_info_tlv *) m->follow_up.suffix;

	if (m->tlv_count != 1 ||
	    f->type != TLV_ORGANIZATION_EXTENSION ||
	    f->length != sizeof(*f) - sizeof(f->type) - sizeof(f->length) ||
//	    memcmp(f->id, ieee8021_id, sizeof(ieee8021_id)) ||
	    f->subtype[0] || f->subtype[1] || f->subtype[2] != 1) {
		return NULL;
	}
#endif
	return f;
}

static void free_foreign_masters(struct port *p)
{
	struct foreign_clock *fc;
	while ((fc = LIST_FIRST(&p->foreign_masters)) != NULL) {
		LIST_REMOVE(fc, list);
		fc_clear(fc);
		free(fc);
	}
}

static int fup_sync_ok(struct ptp_message *fup, struct ptp_message *sync)
{
	int64_t tfup, tsync;
	tfup = tmv_to_nanoseconds(timespec_to_tmv(fup->hwts.sw));
	tsync = tmv_to_nanoseconds(timespec_to_tmv(sync->hwts.sw));
	/*
	 * NB - If the sk_check_fupsync option is not enabled, then
	 * both of these time stamps will be zero.
	 */
	if (tfup < tsync) {
		return 0;
	}
	return 1;
}

static int incapable_ignore(struct port *p, struct ptp_message *m)
{
	if (port_capable(p)) {
		return 0;
	}
	if (msg_type(m) == ANNOUNCE || msg_type(m) == SYNC) {
		return 1;
	}
	return 0;
}

static int path_trace_append(struct port *p, struct ptp_message *m,
			     struct parent_ds *dad)
{
	struct path_trace_tlv *ptt;
	int length = 1 + dad->path_length;

	if (length > PATH_TRACE_MAX) {
		return 0;
	}
	ptt = (struct path_trace_tlv *) m->announce.suffix;
	ptt->type = TLV_PATH_TRACE;
	ptt->length = length * sizeof(struct ClockIdentity);
	memcpy(ptt->cid, dad->ptl, ptt->length);
	ptt->cid[length - 1] = clock_identity(p->clock);
	m->tlv_count = 1;
	return ptt->length + sizeof(ptt->type) + sizeof(ptt->length);
}

static int path_trace_ignore(struct port *p, struct ptp_message *m)
{
	struct ClockIdentity cid;
	struct path_trace_tlv *ptt;
#ifdef KSZ_1588_PTP
	struct TLV *tlv;
	uint8_t *ptr;
#endif
	int i, cnt;

	if (!p->path_trace_enabled) {
		return 0;
	}
	if (msg_type(m) != ANNOUNCE) {
		return 0;
	}
#ifndef KSZ_1588_PTP
	if (m->tlv_count != 1) {
		return 1;
	}
	ptt = (struct path_trace_tlv *) m->announce.suffix;
	if (ptt->type != TLV_PATH_TRACE) {
		return 1;
	}
#else
	ptr = (uint8_t *) m->announce.suffix;
	for (i = 0; i < m->tlv_count; i++) {
		tlv = (struct TLV *) ptr;
		if (tlv->type == TLV_PATH_TRACE)
			break;
		ptr += sizeof(struct TLV) + tlv->length;
	}
	if (i >= m->tlv_count)
		return 1;
	ptt = (struct path_trace_tlv *) tlv;
#endif
	cnt = path_length(ptt);
	cid = clock_identity(p->clock);
	for (i = 0; i < cnt; i++) {
		if (0 == memcmp(&ptt->cid[i], &cid, sizeof(cid)))
			return 1;
	}
	return 0;
}

static int peer_prepare_and_send(struct port *p, struct ptp_message *msg,
				 int event)
{
	int cnt;
#ifdef KSZ_1588_PTP
	int port = 0;
#endif
	if (msg_pre_send(msg)) {
		return -1;
	}

#ifdef KSZ_1588_PTP
	if (need_dest_port(p->clock)) {
		switch (msg_type(msg)) {
		case PDELAY_RESP:
		case PDELAY_RESP_FOLLOW_UP:
			if (get_hw_version(p->clock) >= 2)
				port = p->receive_port;
			break;
		default:
			port = get_master_port(p->clock);

			if (get_hw_version(p->clock) < 1)
				msg->header.sourcePortIdentity.portNumber = 0;
			if (port) {
				if (get_hw_version(p->clock) < 2) {
					msg->header.reserved1 = port;
					port = 0;
				}
			} else {
				if (get_hw_version(p->clock) < 2)
					msg->header.reserved1 =
						all_ports(p->clock);
			}
			break;
		}
	}
	if (p->p2p_sec || p->p2p_nsec || port) {
		port_set_msg_info(p, &msg->header,
			portdst(port), p->p2p_sec, p->p2p_nsec);
		p->p2p_sec = p->p2p_nsec = 0;
	}
#endif
	cnt = transport_peer(p->trp, &p->fda, event, msg);
	if (cnt <= 0) {
		return -1;
	}
#ifndef KSZ_1588_PTP
	if (msg_sots_valid(msg)) {
		ts_add(&msg->hwts.ts, p->tx_timestamp_offset);
	}
#endif
	return 0;
}

#ifdef KSZ_1588_PTP
static int c37_238_append(struct port *p, struct ptp_message *m)
{
	struct ieee_c37_238_info_tlv *c37;
	struct ieee_c37_238_data *data;
	struct alternate_time_offset_tlv *alt;
	int length;

	c37 = (struct ieee_c37_238_info_tlv *) m->announce.suffix;
	c37->org.type = TLV_ORGANIZATION_EXTENSION;
	c37->org.length = sizeof(*c37) - sizeof(c37->org.type) -
		sizeof(c37->org.length);
	memcpy(c37->org.id, ieee_c37_238_id, sizeof(ieee_c37_238_id));
	c37->org.subtype[2] = 1;
	data = &c37->data;
	data->grandmasterID = 0x00FE;
	data->grandmasterTimeInaccuracy = 50;
	data->networkTimeInaccuracy = 200;
	data->reserved = 0;
	length = sizeof(*c37);

	alt = (struct alternate_time_offset_tlv *)(data + 1);
	alt->hdr.type = TLV_ALTERNATE_TIME_OFFSET_INDICATOR;
	alt->keyField = 0;
	alt->currentOffset = 0;
	alt->jumpSeconds = 0;
	alt->timeOfNextJump.seconds_msb = 0;
	alt->timeOfNextJump.seconds_lsb = 0;
	alt->displayName.lengthField = 3;
	alt->displayName.textField[0] = 'P';
	alt->displayName.textField[1] = 'S';
	alt->displayName.textField[2] = 'T';
	alt->hdr.length = sizeof(*alt) - sizeof(struct tlv_hdr) - 1 +
		alt->displayName.lengthField;
	if (alt->hdr.length & 1) {
		uint8_t *pad = (uint8_t *) alt;

		pad[alt->hdr.length + sizeof(struct tlv_hdr)] = '\0';
		alt->hdr.length++;
	}
	length += alt->hdr.length + sizeof(struct tlv_hdr);
	m->tlv_count = 2;
	return length;
}

static int c37_238_extract(struct ptp_message *m,
	struct ieee_c37_238_data **c37, struct alternate_time_offset_tlv **alt)
{
	struct ieee_c37_238_info_tlv *c;
	struct alternate_time_offset_tlv *a;

	c = (struct ieee_c37_238_info_tlv *) m->announce.suffix;

	if (m->tlv_count != 2 ||
	    c->org.type != TLV_ORGANIZATION_EXTENSION ||
	    c->org.length < sizeof(*c) - sizeof(c->org.type) -
	    sizeof(c->org.length) ||
	    memcmp(c->org.id, ieee_c37_238_id, sizeof(ieee_c37_238_id)) ||
	    c->org.subtype[0] || c->org.subtype[1] || c->org.subtype[2] != 1) {
		return FALSE;
	}
	*c37 = &c->data;
	a = (struct alternate_time_offset_tlv *)(*c37 + 1);
	if (a->hdr.type != TLV_ALTERNATE_TIME_OFFSET_INDICATOR ||
	    a->hdr.length < sizeof(*a) - sizeof(struct tlv_hdr))
		return FALSE;
	*alt = a;
	return TRUE;
}
#endif

static int port_capable(struct port *p)
{
	if (!port_is_ieee8021as(p)) {
		/* Normal 1588 ports are always capable. */
		goto capable;
	}
#if 0
if (port_is_ieee8021as(p)) {
	goto capable;
}
#endif

	if (tmv_to_nanoseconds(p->peer_delay) >	p->neighborPropDelayThresh) {
		if (p->asCapable)
			pr_debug("port %hu: peer_delay (%" PRId64 ") > neighborPropDelayThresh "
				"(%" PRId32 "), resetting asCapable", portnum(p),
				tmv_to_nanoseconds(p->peer_delay),
				p->neighborPropDelayThresh);
		goto not_capable;
	}

	if (tmv_to_nanoseconds(p->peer_delay) <	p->min_neighbor_prop_delay) {
		if (p->asCapable)
			pr_debug("port %hu: peer_delay (%" PRId64 ") < min_neighbor_prop_delay "
				"(%" PRId32 "), resetting asCapable", portnum(p),
				tmv_to_nanoseconds(p->peer_delay),
				p->min_neighbor_prop_delay);
		goto not_capable;
	}

	if (p->pdr_missing > ALLOWED_LOST_RESPONSES) {
		if (p->asCapable)
			pr_debug("port %hu: missed %d peer delay resp, "
				"resetting asCapable", portnum(p), p->pdr_missing);
		goto not_capable;
	}

	if (p->multiple_seq_pdr_count) {
		if (p->asCapable)
			pr_debug("port %hu: multiple sequential peer delay resp, "
				"resetting asCapable", portnum(p));
		goto not_capable;
	}

#ifdef KSZ_1588_PTP
	if (p->multiple_pdr) {
		if (p->asCapable)
			pr_debug("port %hu: multiple peer delay resp, "
				"resetting asCapable", portnum(p));
		goto not_capable;
	}
#endif

	if (!p->peer_portid_valid) {
		if (p->asCapable)
			pr_debug("port %hu: invalid peer port id, "
				"resetting asCapable", portnum(p));
		goto not_capable;
	}

	if (!p->nrate.ratio_valid) {
		if (p->asCapable)
			pr_debug("port %hu: invalid nrate, "
				"resetting asCapable", portnum(p));
		goto not_capable;
	}

capable:
	if (!p->asCapable)
		pr_debug("port %hu: setting asCapable", portnum(p));
#ifdef KSZ_1588_PTP
	/* Port 0 can be IEEE 802.1AS port. */
	if (!p->asCapable && port_is_ieee8021as(p) && portnum(p) > 0) {
		if (p->state == PS_MASTER || p->state == PS_GRAND_MASTER) {
			/* Want to send Announce as fast as possible. */
			if (p->gm_change && !p->tx_ann) {
				set_tmo_log(p->fda.fd[FD_MANNO_TIMER], 1, -10);
				if (!p->ann_tx_timeout) {
					p->ann_tx_timeout = 1;
				}
			}
		}
		port_set_port_cfg(p, 1, 1);
#if 0
printf(" as: %d\n", portnum(p));
#endif
	}
#endif
	p->asCapable = 1;
	return 1;

not_capable:
#ifdef KSZ_1588_PTP
	/* Port 0 can be IEEE 802.1AS port. */
	if (p->asCapable && port_is_ieee8021as(p) && portnum(p) > 0) {
		port_set_port_cfg(p, 1, 0);
	}
#endif
	if (p->asCapable)
		port_nrate_initialize(p);
	p->asCapable = 0;
	return 0;
}

static int port_clr_tmo(int fd)
{
	struct itimerspec tmo = {
		{0, 0}, {0, 0}
	};
	return timerfd_settime(fd, 0, &tmo, NULL);
}

static int port_ignore(struct port *p, struct ptp_message *m)
{
	struct ClockIdentity c1, c2;

	if (incapable_ignore(p, m)) {
		return 1;
	}
	if (path_trace_ignore(p, m)) {
		return 1;
	}
	if (msg_transport_specific(m) != p->transportSpecific) {
		return 1;
	}
	if (pid_eq(&m->header.sourcePortIdentity, &p->portIdentity)) {
		return 1;
	}
	if (m->header.domainNumber != clock_domain_number(p->clock)) {
		return 1;
	}

	c1 = clock_identity(p->clock);
	c2 = m->header.sourcePortIdentity.clockIdentity;

	if (0 == memcmp(&c1, &c2, sizeof(c1))) {
#ifdef KSZ_1588_PTP
		/* Allow own Pdelay_Resp to detect multiple responses. */
		if (msg_type(m) == PDELAY_RESP)
			return 0;
#if 0
		if (msg_type(m) == PDELAY_REQ ||
		    msg_type(m) == PDELAY_RESP_FOLLOW_UP)
			return 0;
#endif
#endif
		return 1;
	}
	return 0;
}

/*
 * Test whether a 802.1AS port may transmit a sync message.
 */
static int port_sync_incapable(struct port *p)
{
	struct ClockIdentity cid;
	struct PortIdentity pid;

	if (!port_is_ieee8021as(p)) {
		return 0;
	}
	if (clock_gm_capable(p->clock)) {
		return 0;
	}
	cid = clock_identity(p->clock);
	pid = clock_parent_identity(p->clock);
	if (!memcmp(&cid, &pid.clockIdentity, sizeof(cid))) {
		/*
		 * We are the GM, but without gmCapable set.
		 */
		return 1;
	}
	return 0;
}

static int port_is_ieee8021as(struct port *p)
{
	return p->follow_up_info ? 1 : 0;
}

static void port_management_send_error(struct port *p, struct port *ingress,
				       struct ptp_message *msg, int error_id)
{
	if (port_management_error(p->portIdentity, ingress, msg, error_id))
		pr_err("port %hu: management error failed", portnum(p));
}

static const Octet profile_id_drr[] = {0x00, 0x1B, 0x19, 0x00, 0x01, 0x00};
static const Octet profile_id_p2p[] = {0x00, 0x1B, 0x19, 0x00, 0x02, 0x00};

static int port_management_fill_response(struct port *target,
					 struct ptp_message *rsp, int id)
{
	int datalen = 0, respond = 0;
	struct management_tlv *tlv;
	struct management_tlv_datum *mtd;
	struct portDS *pds;
	struct port_ds_np *pdsnp;
	struct port_properties_np *ppn;
	struct clock_description *desc;
	struct mgmt_clock_description *cd;
	uint8_t *buf;
	uint16_t u16;

	tlv = (struct management_tlv *) rsp->management.suffix;
	tlv->type = TLV_MANAGEMENT;
	tlv->id = id;

	switch (id) {
	case TLV_NULL_MANAGEMENT:
		datalen = 0;
		respond = 1;
		break;
	case TLV_CLOCK_DESCRIPTION:
		cd = &rsp->last_tlv.cd;
		buf = tlv->data;
		cd->clockType = (UInteger16 *) buf;
		buf += sizeof(*cd->clockType);
		*cd->clockType = clock_type(target->clock);
		cd->physicalLayerProtocol = (struct PTPText *) buf;
		switch(transport_type(target->trp)) {
		case TRANS_UDP_IPV4:
		case TRANS_UDP_IPV6:
		case TRANS_IEEE_802_3:
			ptp_text_set(cd->physicalLayerProtocol, "IEEE 802.3");
			break;
		default:
			ptp_text_set(cd->physicalLayerProtocol, NULL);
			break;
		}
		buf += sizeof(struct PTPText) + cd->physicalLayerProtocol->length;

		cd->physicalAddress = (struct PhysicalAddress *) buf;
		u16 = transport_physical_addr(target->trp,
                                              cd->physicalAddress->address);
		memcpy(&cd->physicalAddress->length, &u16, 2);
		buf += sizeof(struct PhysicalAddress) + u16;

		cd->protocolAddress = (struct PortAddress *) buf;
		u16 = transport_type(target->trp);
		memcpy(&cd->protocolAddress->networkProtocol, &u16, 2);
		u16 = transport_protocol_addr(target->trp,
                                              cd->protocolAddress->address);
		memcpy(&cd->protocolAddress->addressLength, &u16, 2);
		buf += sizeof(struct PortAddress) + u16;

		desc = clock_description(target->clock);
		cd->manufacturerIdentity = buf;
		memcpy(cd->manufacturerIdentity,
                       desc->manufacturerIdentity, OUI_LEN);
		buf += OUI_LEN;
		*(buf++) = 0; /* reserved */

		cd->productDescription = (struct PTPText *) buf;
		ptp_text_copy(cd->productDescription, &desc->productDescription);
		buf += sizeof(struct PTPText) + cd->productDescription->length;

		cd->revisionData = (struct PTPText *) buf;
		ptp_text_copy(cd->revisionData, &desc->revisionData);
		buf += sizeof(struct PTPText) + cd->revisionData->length;

		cd->userDescription = (struct PTPText *) buf;
		ptp_text_copy(cd->userDescription, &desc->userDescription);
		buf += sizeof(struct PTPText) + cd->userDescription->length;

		if (target->delayMechanism == DM_P2P) {
			memcpy(buf, profile_id_p2p, PROFILE_ID_LEN);
		} else {
			memcpy(buf, profile_id_drr, PROFILE_ID_LEN);
		}
		buf += PROFILE_ID_LEN;
		datalen = buf - tlv->data;
		respond = 1;
		break;
	case TLV_PORT_DATA_SET:
		pds = (struct portDS *) tlv->data;
		pds->portIdentity            = target->portIdentity;
		if (target->state == PS_GRAND_MASTER) {
			pds->portState = PS_MASTER;
		} else {
			pds->portState = target->state;
		}
		pds->logMinDelayReqInterval  = target->logMinDelayReqInterval;
		pds->peerMeanPathDelay       = target->peerMeanPathDelay;
		pds->logAnnounceInterval     = target->logAnnounceInterval;
		pds->announceReceiptTimeout  = target->announceReceiptTimeout;
		pds->logSyncInterval         = target->logSyncInterval;
		if (target->delayMechanism) {
			pds->delayMechanism = target->delayMechanism;
#ifdef KSZ_1588_PTP
			if (target->delayMechanism == DM_NONE)
				pds->delayMechanism = 0xFE;
#endif
		} else {
			pds->delayMechanism = DM_E2E;
		}
		pds->logMinPdelayReqInterval = target->logMinPdelayReqInterval;
		pds->versionNumber           = target->versionNumber;
		datalen = sizeof(*pds);
		respond = 1;
		break;
#ifdef KSZ_1588_PTP
	case TLV_TRANSPARENT_CLOCK_PORT_DATA_SET:
	{
		struct transparent_clock_port_data_set *pds =
			(struct transparent_clock_port_data_set *) tlv->data;

		memcpy(&pds->portIdentity, &target->portIdentity,
			sizeof(struct PortIdentity));
		pds->faultyFlag = FALSE;
		pds->peerMeanPathDelay = target->peerMeanPathDelay;
		pds->logMinPdelayReqInterval = target->logMinPdelayReqInterval;
		datalen = sizeof(*pds);
		respond = 1;
		break;
	}
	case TLV_DISABLE_PORT:
	case TLV_ENABLE_PORT:
		respond = 1;
		break;
#endif
	case TLV_LOG_ANNOUNCE_INTERVAL:
		mtd = (struct management_tlv_datum *) tlv->data;
		mtd->val = target->logAnnounceInterval;
		datalen = sizeof(*mtd);
		respond = 1;
		break;
	case TLV_ANNOUNCE_RECEIPT_TIMEOUT:
		mtd = (struct management_tlv_datum *) tlv->data;
		mtd->val = target->announceReceiptTimeout;
		datalen = sizeof(*mtd);
		respond = 1;
		break;
	case TLV_LOG_SYNC_INTERVAL:
		mtd = (struct management_tlv_datum *) tlv->data;
		mtd->val = target->logSyncInterval;
		datalen = sizeof(*mtd);
		respond = 1;
		break;
	case TLV_VERSION_NUMBER:
		mtd = (struct management_tlv_datum *) tlv->data;
		mtd->val = target->versionNumber;
		datalen = sizeof(*mtd);
		respond = 1;
		break;
	case TLV_DELAY_MECHANISM:
		mtd = (struct management_tlv_datum *) tlv->data;
		if (target->delayMechanism)
			mtd->val = target->delayMechanism;
		else
			mtd->val = DM_E2E;
		datalen = sizeof(*mtd);
		respond = 1;
		break;
	case TLV_LOG_MIN_PDELAY_REQ_INTERVAL:
		mtd = (struct management_tlv_datum *) tlv->data;
		mtd->val = target->logMinPdelayReqInterval;
		datalen = sizeof(*mtd);
		respond = 1;
		break;
	case TLV_PORT_DATA_SET_NP:
		pdsnp = (struct port_ds_np *) tlv->data;
		pdsnp->neighborPropDelayThresh = target->neighborPropDelayThresh;
		pdsnp->asCapable = target->asCapable;
		datalen = sizeof(*pdsnp);
		respond = 1;
		break;
	case TLV_PORT_PROPERTIES_NP:
		ppn = (struct port_properties_np *)tlv->data;
		ppn->portIdentity = target->portIdentity;
		if (target->state == PS_GRAND_MASTER)
			ppn->port_state = PS_MASTER;
		else
			ppn->port_state = target->state;
		ppn->timestamping = target->timestamping;
		ptp_text_set(&ppn->interface, target->name);
		datalen = sizeof(*ppn) + ppn->interface.length;
		respond = 1;
		break;
	}
	if (respond) {
		if (datalen % 2) {
			tlv->data[datalen] = 0;
			datalen++;
		}
		tlv->length = sizeof(tlv->id) + datalen;
		rsp->header.messageLength += sizeof(*tlv) + datalen;
		rsp->tlv_count = 1;
	}
	return respond;
}

static int port_management_get_response(struct port *target,
					struct port *ingress, int id,
					struct ptp_message *req)
{
	struct PortIdentity pid = port_identity(target);
	struct ptp_message *rsp;
	int respond;

	rsp = port_management_reply(pid, ingress, req);
	if (!rsp) {
		return 0;
	}
	respond = port_management_fill_response(target, rsp, id);
#ifdef KSZ_1588_PTP
	ingress->dest_port = ingress->receive_port;
#endif
	if (respond)
		port_prepare_and_send(ingress, rsp, 0);
	msg_put(rsp);
	return respond;
}

static int port_management_set(struct port *target,
			       struct port *ingress, int id,
			       struct ptp_message *req)
{
	int respond = 0;
	struct management_tlv *tlv;
	struct port_ds_np *pdsnp;

	tlv = (struct management_tlv *) req->management.suffix;

	switch (id) {
	case TLV_PORT_DATA_SET_NP:
		pdsnp = (struct port_ds_np *) tlv->data;
		target->neighborPropDelayThresh = pdsnp->neighborPropDelayThresh;
		respond = 1;
		break;
#ifdef KSZ_1588_PTP
	case TLV_DISABLE_PORT:
		if (PS_DISABLED != target->state) {
			port_dispatch(target, EV_DESIGNATED_DISABLED, 0);
		}
		respond = 1;
		break;
	case TLV_ENABLE_PORT:
		if (PS_DISABLED == target->state) {
			port_dispatch(target, EV_DESIGNATED_ENABLED, 0);
		}
		respond = 1;
		break;
#endif
	}
	if (respond && !port_management_get_response(target, ingress, id, req))
		pr_err("port %hu: failed to send management set response", portnum(target));
	return respond ? 1 : 0;
}

static void port_nrate_calculate(struct port *p, tmv_t origin, tmv_t ingress)
{
	struct nrate_estimator *n = &p->nrate;

	/*
	 * We experienced a successful exchanges of peer delay request
	 * and response, reset pdr_missing for this port.
	 */
	p->pdr_missing = 0;

	if (!n->ingress1) {
		n->ingress1 = ingress;
		n->origin1 = origin;
		return;
	}
	n->count++;
	if (n->count < n->max_count) {
		return;
	}
	if (tmv_eq(ingress, n->ingress1)) {
		pr_warning("bad timestamps in nrate calculation");
		return;
	}
	n->ratio =
		tmv_dbl(tmv_sub(origin, n->origin1)) /
		tmv_dbl(tmv_sub(ingress, n->ingress1));
	n->ingress1 = ingress;
	n->origin1 = origin;
	n->count = 0;
	n->ratio_valid = 1;
}

static void port_nrate_initialize(struct port *p)
{
	int shift = p->freq_est_interval - p->logMinPdelayReqInterval;

	if (shift < 0)
		shift = 0;
	else if (shift >= sizeof(int) * 8) {
		shift = sizeof(int) * 8 - 1;
		pr_warning("freq_est_interval is too long");
	}

	/* We start in the 'incapable' state. */
	p->pdr_missing = ALLOWED_LOST_RESPONSES + 1;
	p->asCapable = 0;

	p->peer_portid_valid = 0;

	p->nrate.origin1 = tmv_zero();
	p->nrate.ingress1 = tmv_zero();
	p->nrate.max_count = (1 << shift);
	p->nrate.count = 0;
	p->nrate.ratio = 1.0;
	p->nrate.ratio_valid = 0;
}

static int port_set_announce_tmo(struct port *p)
{
#ifdef KSZ_DBG_HOST
if (!is_peer_port(p->clock, p))
printf("  !! %s\n", __func__);
#endif
	if (!p->ann_rx_timeout) {
#ifdef KSZ_DBG_TIMER
#if 0 
if (portnum(p) == 5)
#endif
printf(" %s %d\n", __func__, portnum(p));
#endif
		p->ann_rx_timeout = 1;
	}
	return set_tmo_random(p->fda.fd[FD_ANNOUNCE_TIMER],
			      p->announceReceiptTimeout,
			      p->announce_span, p->logAnnounceInterval);
}

static int port_set_delay_tmo(struct port *p)
{
#ifdef KSZ_DBG_HOST
if (!is_peer_port(p->clock, p))
printf("  !! %s %d\n", __func__, portnum(p));
#endif
	if (p->delayMechanism == DM_P2P) {
		return set_tmo_log(p->fda.fd[FD_DELAY_TIMER], 1,
			       p->logMinPdelayReqInterval);
	} else {

#ifdef KSZ_1588_PTP
		if (p->delayMechanism == DM_NONE)
			return 0;
#endif
		return set_tmo_random(p->fda.fd[FD_DELAY_TIMER], 0, 2,
				p->logMinDelayReqInterval);
	}
}

static int port_set_manno_tmo(struct port *p)
{
#ifdef KSZ_DBG_HOST
if (!is_host_port(p->clock, p))
printf("  !! %s\n", __func__);
#endif
	if (!p->ann_tx_timeout) {
#ifdef KSZ_DBG_TIMER
#if 0 
if (portnum(p) == 5)
#endif
printf(" %s %d\n", __func__, portnum(p));
#endif
		p->ann_tx_timeout = 1;
	}
	return set_tmo_log(p->fda.fd[FD_MANNO_TIMER], 1, p->logAnnounceInterval);
}

static int port_set_qualification_tmo(struct port *p)
{
	return set_tmo_log(p->fda.fd[FD_QUALIFICATION_TIMER],
		       1+clock_steps_removed(p->clock), p->logAnnounceInterval);
}

static int port_set_sync_rx_tmo(struct port *p)
{
#ifdef KSZ_DBG_HOST
if (!is_peer_port(p->clock, p))
printf("  !! %s\n", __func__);
#endif
	if (!p->sync_rx_timeout) {
#ifdef KSZ_DBG_TIMER
#if 0 
if (portnum(p) == 5)
#endif
printf(" %s %d\n", __func__, portnum(p));
#endif
		p->sync_rx_timeout = 1;
	}
	return set_tmo_log(p->fda.fd[FD_SYNC_RX_TIMER],
			   p->syncReceiptTimeout, p->logSyncInterval);
}

static int port_set_sync_tx_tmo(struct port *p)
{
#ifdef KSZ_DBG_HOST
if (!is_host_port(p->clock, p))
printf("  !! %s\n", __func__);
#endif
	if (!p->sync_tx_timeout) {
#ifdef KSZ_DBG_TIMER
#if 0 
if (portnum(p) == 5)
#endif
printf(" %s %d\n", __func__, portnum(p));
#endif
		p->sync_tx_timeout = 1;
	}
	return set_tmo_log(p->fda.fd[FD_SYNC_TX_TIMER], 1, p->logSyncInterval);
}

#ifdef KSZ_1588_PTP
static int port_set_sync_fup_tx_tmo(struct port *p)
{
	if (!p->sync_tx_timeout) {
#ifdef KSZ_DBG_TIMER
#if 0
if (portnum(p) == 5)
#endif
printf(" %s %d\n", __func__, portnum(p));
#endif
		p->sync_tx_timeout = 1;
	}
	return set_tmo_ms(p->fda.fd[FD_SYNC_TX_TIMER], 50);
}

static int port_set_fup_rx_tmo(struct port *p)
{
	if (!p->fup_rx_timeout) {
#ifdef KSZ_DBG_TIMER
#if 1
if (portnum(p) == 5)
#endif
printf(" %s %d\n", __func__, portnum(p));
#endif
		p->fup_rx_timeout = 1;
	}
	return set_tmo_log(p->fda.fd[FD_FUP_RX_TIMER],
			   p->followUpReceiptTimeout, p->logSyncInterval);
}

static int port_set_sync_cont_tmo(struct port *p)
{
	if (!p->fup_tx_timeout) {
#ifdef KSZ_DBG_TIMER
if (portnum(p) == 5)
printf(" %s %d\n", __func__, portnum(p));
#endif
		p->fup_tx_timeout = 1;
	}
	return set_tmo_ms(p->fda.fd[FD_SYNC_CONT_TIMER],
		p->syncTxContTimeout);
}

void port_clear_sync_fup(struct port *p, int n)
{
	memset(&p->sync_ts, 0, sizeof(struct timespec));
	p->sync_correction = p->fup_correction = 0;
	memset(&p->sync_timestamp, 0, sizeof(struct timestamp));
	memset(&p->fup_timestamp, 0, sizeof(struct timestamp));
	if (p->sync_rx_tx) {
#ifdef KSZ_DBG_TIMER
#if 0
if (portnum(p) == 5)
#endif
printf(" %s sync_rx_tx %d\n", __func__, portnum(p));
#endif
		p->sync_rx_tx = 0;
	}
	if (p->index != n) {
		p->gm_change = 1;

		/* Want to send Announce for new grandmaster. */
		set_tmo_log(p->fda.fd[FD_MANNO_TIMER], 1, -10); /*~1ms*/
		if (!p->ann_tx_timeout) {
			p->ann_tx_timeout = 1;
		}
		port_set_sync_tx_tmo(p);
	}
}

void port_clear_sync_tx(struct port *p, int n)
{
	if (p->index != n) {
		if (p->syncTxCont) {
			port_clr_tmo(p->fda.fd[FD_SYNC_TX_TIMER]);
			if (p->sync_tx_timeout) {
#ifdef KSZ_DBG_TIMER
#if 0
if (portnum(p) == 5)
#endif
printf(" %s sync_tx %d\n", __func__, portnum(p));
#endif
				p->sync_tx_timeout = 0;
			}
		}
		p->syncTxCont = 0;
	}
}

void port_update_sync(struct port *p, struct timespec *ts, Integer64 corr,
	struct timestamp *timestamp)
{
#if 0
if (portnum(p) == 5)
printf(" update sync: %d\n", portnum(p));
#endif
	if (!p->sync_ts.tv_sec) {
		port_clr_tmo(p->fda.fd[FD_SYNC_TX_TIMER]);
		if (p->sync_tx_timeout) {
#ifdef KSZ_DBG_TIMER
#if 0
if (portnum(p) == 5)
#endif
printf(" %s sync_tx %d\n", __func__, portnum(p));
#endif
			p->sync_tx_timeout = 0;
		}
	}
	memcpy(&p->sync_ts, ts, sizeof(struct timespec));
	p->sync_correction = corr;
	memcpy(&p->sync_timestamp, timestamp, sizeof(struct timestamp));
	if (!p->sync_rx_tx) {
#ifdef KSZ_DBG_TIMER
if (portnum(p) == 5)
printf(" %s sync_rx_tx %d\n", __func__, portnum(p));
#endif
		p->sync_rx_tx = 1;
	}
}

void port_update_fup(struct port *p, Integer64 corr,
	struct timestamp *timestamp)
{
	if (p->state != PS_MASTER && p->state != PS_GRAND_MASTER)
		return;
	p->fup_correction = corr;
	memcpy(&p->fup_timestamp, timestamp, sizeof(struct timestamp));
	port_set_sync_fup_tx_tmo(p);
}

void port_update_sync_tx(struct port *p)
{
	if (p->state != PS_MASTER && p->state != PS_GRAND_MASTER)
		return;
	p->syncTxCont = 2;
	port_set_sync_fup_tx_tmo(p);
#ifdef KSZ_DBG_TIMER
if (portnum(p) < 3)
printf(" update_sync: %d\n", portnum(p));
#endif
}

void port_update_grandmaster(struct port *p)
{
	if (!p->gm_change) {
#if 0
printf(" %s %p:%d\n", __func__, p, portnum(p));
#endif
		p->gm_change = 1;
		if (port_is_ieee8021as(p) && p->sync_tx_timeout) {
#ifdef KSZ_DBG_TIMER
#if 0
if (portnum(p) == 5)
#endif
printf(" %s sync_tx %d\n", __func__, portnum(p));
#endif
			p->sync_tx_timeout = 0;
			port_clr_tmo(p->fda.fd[FD_SYNC_TX_TIMER]);
		}
	}
}

int port_get_info(struct port *p)
{
	int rc;
	u8 phys_port;
	u32 port_mask;

	rc = ptp_port_info(&ptpdev, p->basename, &phys_port, &port_mask);
	if (!rc) {
		p->phys_port = phys_port;
		p->port_mask = port_mask;
		if (!is_host_port(p->clock, p) || port_is_ieee8021as(p))
			p->portIdentity.portNumber = p->phys_port;
		else if (p->portIdentity.portNumber != 1)
			p->portIdentity.portNumber = ptp_host_port;
	}
	return rc;
}

int port_matched(struct port *p, int n)
{
	if (p->phys_port == n)
		return TRUE;
	return FALSE;
}
#endif

static void port_show_transition(struct port *p,
				 enum port_state next, enum fsm_event event)
{
	if (event == EV_FAULT_DETECTED) {
		pr_notice("port %hu: %s to %s on %s (%s)", portnum(p),
			  ps_str[p->state], ps_str[next], ev_str[event],
			  ft_str(last_fault_type(p)));
	} else {
		pr_notice("port %hu: %s to %s on %s", portnum(p),
			  ps_str[p->state], ps_str[next], ev_str[event]);
	}
}

static void port_slave_priority_warning(struct port *p)
{
	UInteger16 n = portnum(p);
	pr_warning("port %hu: master state recommended in slave only mode", n);
	pr_warning("port %hu: defaultDS.priority1 probably misconfigured", n);
}

static int port_delay_request(struct port *p);
static void port_synchronize(struct port *p,
			     struct timespec ingress_ts,
			     struct timestamp origin_ts,
			     Integer64 correction1, Integer64 correction2)
{
	enum servo_state state;
	tmv_t t1, t1c, t2, c1, c2;

#ifdef KSZ_DBG_HOST
if (!is_peer_port(p->clock, p))
printf("  !! %s %d\n", __func__, portnum(p));
#endif
#if 0
printf("%s %d\n", __func__, portnum(p));
#endif
	port_set_sync_rx_tmo(p);

	t1 = timestamp_to_tmv(origin_ts);
	t2 = timespec_to_tmv(ingress_ts);
	c1 = correction_to_tmv(correction1);
	c2 = correction_to_tmv(correction2);
	t1c = tmv_add(t1, tmv_add(c1, c2));

	state = clock_synchronize(p->clock, t2, t1c);
#if 0
printf("%s %d %d\n", __func__, portnum(p), state);
#endif
	switch (state) {
	case SERVO_UNLOCKED:
		port_dispatch(p, EV_SYNCHRONIZATION_FAULT, 0);
		break;
	case SERVO_JUMP:
		port_dispatch(p, EV_SYNCHRONIZATION_FAULT, 0);
		if (p->delay_req) {
			msg_put(p->delay_req);
			p->delay_req = NULL;
		}
		if (p->peer_delay_req) {
			msg_put(p->peer_delay_req);
			p->peer_delay_req = NULL;
		}
		break;
	case SERVO_LOCKED:
		port_dispatch(p, EV_MASTER_CLOCK_SELECTED, 0);
#ifdef KSZ_1588_PTP
		if (p->best && p->best->bad_cnt) {
			if (p->rx_sec != p->last_rx_sec) {
				p->last_rx_sec = p->rx_sec;
				p->best->good_cnt++;
				if (p->best->good_cnt > 4) {
					p->best->good_cnt = 0;
					p->best->bad_cnt--;
					if (!p->best->bad_cnt)
						p->best->bad_master = 0;
				}
			}
		}
#endif
		break;
#ifdef KSZ_1588_PTP
	case SERVO_LOCKING:
		if (p->delay_req) {
			msg_put(p->delay_req);
			p->delay_req = NULL;
		}
		if (p->peer_delay_req) {
			msg_put(p->peer_delay_req);
			p->peer_delay_req = NULL;
		}
#if 1
		if (p->delayMechanism == DM_E2E)
			port_delay_request(p);
#endif
		break;
#endif
	}
}

/*
 * Handle out of order packets. The network stack might
 * provide the follow up _before_ the sync message. After all,
 * they can arrive on two different ports. In addition, time
 * stamping in PHY devices might delay the event packets.
 */
static void port_syfufsm(struct port *p, enum syfu_event event,
			 struct ptp_message *m)
{
	struct ptp_message *syn, *fup;

	switch (p->syfu) {
	case SF_EMPTY:
		switch (event) {
		case SYNC_MISMATCH:
			msg_get(m);
			p->last_syncfup = m;
			p->syfu = SF_HAVE_SYNC;
			break;
		case FUP_MISMATCH:
#ifdef KSZ_1588_PTP
			/*
			 * Unlikely to receive Sync/Follow_Up messages not
			 * in sequence in 802.1AS.
			 */
			if (port_is_ieee8021as(p)) {
				break;
			}
#endif
			msg_get(m);
			p->last_syncfup = m;
			p->syfu = SF_HAVE_FUP;
			break;
		case SYNC_MATCH:
			break;
		case FUP_MATCH:
			break;
		}
		break;

	case SF_HAVE_SYNC:
		switch (event) {
		case SYNC_MISMATCH:
			msg_put(p->last_syncfup);
#ifdef KSZ_1588_PTP
			/*
			 * Unlikely to receive Sync/Follow_Up messages not
			 * in sequence in 802.1AS.
			 */
			if (port_is_ieee8021as(p)) {
				p->syfu = SF_EMPTY;
				break;
			}
#endif
			msg_get(m);
			p->last_syncfup = m;
			break;
		case SYNC_MATCH:
			break;
		case FUP_MISMATCH:
			msg_put(p->last_syncfup);
#ifdef KSZ_1588_PTP
			/*
			 * Unlikely to receive Sync/Follow_Up messages not
			 * in sequence in 802.1AS.
			 */
			if (port_is_ieee8021as(p)) {
				p->syfu = SF_EMPTY;
				break;
			}
#endif
			msg_get(m);
			p->last_syncfup = m;
			p->syfu = SF_HAVE_FUP;
			break;
		case FUP_MATCH:
			syn = p->last_syncfup;
			port_synchronize(p, syn->hwts.ts, m->ts.pdu,
					 syn->header.correction,
					 m->header.correction);
			msg_put(p->last_syncfup);
			p->syfu = SF_EMPTY;
			break;
		}
		break;

	case SF_HAVE_FUP:
		switch (event) {
		case SYNC_MISMATCH:
			msg_put(p->last_syncfup);
			msg_get(m);
			p->last_syncfup = m;
			p->syfu = SF_HAVE_SYNC;
			break;
		case SYNC_MATCH:
			fup = p->last_syncfup;
			port_synchronize(p, m->hwts.ts, fup->ts.pdu,
					 m->header.correction,
					 fup->header.correction);
			msg_put(p->last_syncfup);
			p->syfu = SF_EMPTY;
			break;
		case FUP_MISMATCH:
			msg_put(p->last_syncfup);
			msg_get(m);
			p->last_syncfup = m;
			break;
		case FUP_MATCH:
			break;
		}
		break;
	}
}

static int port_pdelay_request(struct port *p)
{
	struct ptp_message *msg;
	int err;

#ifdef KSZ_DBG_HOST
if (!is_peer_port(p->clock, p))
printf("  !! %s\n", __func__);
#endif
#ifndef KSZ_1588_PTP
	/* If multiple pdelay resp were not detected the counter can be reset */
	if (!p->multiple_pdr_detected)
		p->multiple_seq_pdr_count = 0;
	p->multiple_pdr_detected = 0;
#endif
#ifdef KSZ_1588_PTP
	if (!p->multiple_pdr_detected && p->multiple_seq_pdr_count)
		p->multiple_seq_pdr_count--;
	p->multiple_pdr_detected = 0;
#endif

	msg = msg_allocate();
	if (!msg)
		return -1;

	msg->hwts.type = p->timestamping;

	msg->header.tsmt               = PDELAY_REQ | p->transportSpecific;
	msg->header.ver                = PTP_VERSION;
	msg->header.messageLength      = sizeof(struct pdelay_req_msg);
	msg->header.domainNumber       = clock_domain_number(p->clock);
	msg->header.correction         = -p->asymmetry;
	msg->header.sourcePortIdentity = p->portIdentity;
	msg->header.sequenceId         = p->seqnum.delayreq++;
	msg->header.control            = CTL_OTHER;
	msg->header.logMessageInterval = port_is_ieee8021as(p) ?
		p->logMinPdelayReqInterval : 0x7f;

#ifdef KSZ_1588_PTP
	msg->header.flagField[1] |= PTP_TIMESCALE;
#endif
	err = peer_prepare_and_send(p, msg, 1);
	if (err) {
		pr_err("port %hu: send peer delay request failed", portnum(p));
		goto out;
	}
#ifndef KSZ_1588_PTP
	if (msg_sots_missing(msg)) {
		pr_err("missing timestamp on transmitted peer delay request");
		goto out;
	}
#endif

	if (p->peer_delay_req) {
		if (port_capable(p)) {
			p->pdr_missing++;
		}
		msg_put(p->peer_delay_req);
	}
	p->peer_delay_req = msg;
	return 0;
out:
	msg_put(msg);
	return -1;
}

static int port_delay_request(struct port *p)
{
	struct ptp_message *msg;

	/* Time to send a new request, forget current pdelay resp and fup */
	if (p->peer_delay_resp) {
		msg_put(p->peer_delay_resp);
		p->peer_delay_resp = NULL;
	}
	if (p->peer_delay_fup) {
		msg_put(p->peer_delay_fup);
		p->peer_delay_fup = NULL;
	}

	if (p->delayMechanism == DM_P2P)
		return port_pdelay_request(p);

#ifdef KSZ_DBG_HOST
if (!is_peer_port(p->clock, p))
printf("  !! %s\n", __func__);
#endif
	msg = msg_allocate();
	if (!msg)
		return -1;

	msg->hwts.type = p->timestamping;

	msg->header.tsmt               = DELAY_REQ | p->transportSpecific;
	msg->header.ver                = PTP_VERSION;
	msg->header.messageLength      = sizeof(struct delay_req_msg);
	msg->header.domainNumber       = clock_domain_number(p->clock);
	msg->header.correction         = -p->asymmetry;
	msg->header.sourcePortIdentity = p->portIdentity;
	msg->header.sequenceId         = p->seqnum.delayreq++;
	msg->header.control            = CTL_DELAY_REQ;
	msg->header.logMessageInterval = 0x7f;

	if (p->hybrid_e2e) {
		struct ptp_message *dst = TAILQ_FIRST(&p->best->messages);
		msg->address = dst->address;
		msg->header.flagField[0] |= UNICAST;
	}

#ifdef KSZ_1588_PTP
	msg->header.flagField[1] |= PTP_TIMESCALE;
	p->dest_port = get_master_port(p->clock);
#endif
	if (port_prepare_and_send(p, msg, 1)) {
		pr_err("port %hu: send delay request failed", portnum(p));
		goto out;
	}
#ifndef KSZ_1588_PTP
	if (msg_sots_missing(msg)) {
		pr_err("missing timestamp on transmitted delay request");
		goto out;
	}
#endif

	if (p->delay_req)
		msg_put(p->delay_req);

	p->delay_req = msg;
	return 0;
out:
	msg_put(msg);
	return -1;
}

#if 0
static unsigned char test_data[] = {
	0x0d, 0x00, 0x16, 0x00, 0xab, 0x56, 0x78, 0xab,
	0xcd, 0x00, 0x8b, 0xa0, 0xf9, 0x52, 0xf4, 0x40,
	0xff, 0x52, 0xad, 0xd1, 0x96, 0xee, 0x93, 0x55,
	0xe1, 0xad
};

static int test_append(struct port *p, struct ptp_message *m)
{
	u8 *tlv = m->announce.suffix;
	memcpy(tlv, test_data, sizeof(test_data));
	m->tlv_count = 1;
	return sizeof(test_data);
}
#endif

static int port_tx_announce(struct port *p)
{
	struct parent_ds *dad = clock_parent_ds(p->clock);
	struct timePropertiesDS *tp = clock_time_properties(p->clock);
	struct ptp_message *msg;
	int err, pdulen;

#ifdef KSZ_DBG_HOST
if (!is_host_port(p->clock, p))
printf("  !! %s\n", __func__);
#endif
#if 0
if (portnum(p) == 5)
printf("tx ann: %d=%x\n", portnum(p), p->seqnum.announce);
#endif
	p->tx_ann = 1;
	if (!port_capable(p)) {
		return 0;
	}
	msg = msg_allocate();
	if (!msg)
		return -1;

	pdulen = sizeof(struct announce_msg);
	msg->hwts.type = p->timestamping;

	if (p->path_trace_enabled)
		pdulen += path_trace_append(p, msg, dad);
#ifdef KSZ_1588_PTP
	if (clock_c37_238(p->clock))
		pdulen += c37_238_append(p, msg);
#if 0
pdulen += test_append(p, msg);
#endif
#endif

	msg->header.tsmt               = ANNOUNCE | p->transportSpecific;
	msg->header.ver                = PTP_VERSION;
	msg->header.messageLength      = pdulen;
	msg->header.domainNumber       = clock_domain_number(p->clock);
	msg->header.sourcePortIdentity = p->portIdentity;
	msg->header.sequenceId         = p->seqnum.announce++;
	msg->header.control            = CTL_OTHER;
	msg->header.logMessageInterval = p->logAnnounceInterval;

	msg->header.flagField[1] = tp->flags;

	msg->announce.currentUtcOffset        = tp->currentUtcOffset;
	msg->announce.grandmasterPriority1    = dad->pds.grandmasterPriority1;
	msg->announce.grandmasterClockQuality = dad->pds.grandmasterClockQuality;
	msg->announce.grandmasterPriority2    = dad->pds.grandmasterPriority2;
	msg->announce.grandmasterIdentity     = dad->pds.grandmasterIdentity;
	msg->announce.stepsRemoved            = clock_steps_removed(p->clock);
	msg->announce.timeSource              = tp->timeSource;

#ifdef KSZ_1588_PTP
	p->dest_port = 0;
	if (p->gm_change) {
		p->gm_change = 0;
	}
#endif
	err = port_prepare_and_send(p, msg, 0);
	if (err)
		pr_err("port %hu: send announce failed", portnum(p));
	msg_put(msg);
	return err;
}

static int port_tx_sync(struct port *p)
{
	struct ptp_message *msg, *fup;
	int err, pdulen;
	int event = p->timestamping == TS_ONESTEP ? TRANS_ONESTEP : TRANS_EVENT;

#ifdef KSZ_DBG_HOST
if (!is_host_port(p->clock, p))
printf("  !! %s\n", __func__);
#endif
	p->tx_ann = 0;
	if (!port_capable(p)) {
		return 0;
	}
	if (port_sync_incapable(p)) {
		return 0;
	}
#ifdef KSZ_1588_PTP
	if (p->sync) {
		msg = p->sync;
		p->sync = NULL;
		msg_put(msg);
	}
	if (p->follow_up) {
		msg = p->follow_up;
		p->follow_up = NULL;
		msg_put(msg);
	}
#endif
	msg = msg_allocate();
	if (!msg)
		return -1;
	fup = msg_allocate();
	if (!fup) {
		msg_put(msg);
		return -1;
	}

	pdulen = sizeof(struct sync_msg);
	msg->hwts.type = p->timestamping;

	msg->header.tsmt               = SYNC | p->transportSpecific;
	msg->header.ver                = PTP_VERSION;
	msg->header.messageLength      = pdulen;
	msg->header.domainNumber       = clock_domain_number(p->clock);
	msg->header.sourcePortIdentity = p->portIdentity;
	msg->header.sequenceId         = p->seqnum.sync++;
	msg->header.control            = CTL_SYNC;
	msg->header.logMessageInterval = p->logSyncInterval;
#if 0
if (p->syncTxCont)
printf(" tx cont: %d %d %d; %d\n", portnum(p), p->seqnum.sync, p->logSyncInterval,
p->syncTxCont);
#endif
#if 0
else if (portnum(p) == 5)
printf(" tx sync: %d\n", portnum(p));
#endif

	if (p->timestamping != TS_ONESTEP)
		msg->header.flagField[0] |= TWO_STEP;

#ifdef KSZ_1588_PTP
	msg->header.flagField[1] |= PTP_TIMESCALE;
	if (get_hw_version(p->clock) < 2)
		p->dest_port = all_ports(p->clock);
	if (port_is_ieee8021as(p) && p->sync_ts.tv_sec) {
		msg->header.correction = p->sync_correction;
		tspdu_to_timestamp(&p->sync_timestamp,
			&msg->sync.originTimestamp);
	}
#endif
	err = port_prepare_and_send(p, msg, event);
	if (err) {
		pr_err("port %hu: send sync failed", portnum(p));
		goto out;
	}
	if (p->timestamping == TS_ONESTEP) {
		goto out;
	} else if (msg_sots_missing(msg)) {
#ifdef KSZ_1588_PTP
		err = 0;
		msg_get(msg);
		if (p->sync)
			msg_put(p->sync);
		p->sync = msg;
		msg_get(fup);
		if (p->follow_up)
			msg_put(p->follow_up);
		p->follow_up = fup;
#else
		pr_err("missing timestamp on transmitted sync");
		err = -1;
#endif
		goto out;
	}

	/*
	 * Send the follow up message right away.
	 */
	pdulen = sizeof(struct follow_up_msg);
	fup->hwts.type = p->timestamping;

	if (p->follow_up_info)
		pdulen += follow_up_info_append(p, fup);

	fup->header.tsmt               = FOLLOW_UP | p->transportSpecific;
	fup->header.ver                = PTP_VERSION;
	fup->header.messageLength      = pdulen;
	fup->header.domainNumber       = clock_domain_number(p->clock);
	fup->header.sourcePortIdentity = p->portIdentity;
	fup->header.sequenceId         = p->seqnum.sync - 1;
	fup->header.control            = CTL_FOLLOW_UP;
	fup->header.logMessageInterval = p->logSyncInterval;

	ts_to_timestamp(&msg->hwts.ts, &fup->follow_up.preciseOriginTimestamp);

	err = port_prepare_and_send(p, fup, 0);
	if (err)
		pr_err("port %hu: send follow up failed", portnum(p));
out:
	msg_put(msg);
	msg_put(fup);
	return err;
}

/*
 * port initialize and disable
 */
static int port_is_enabled(struct port *p)
{
	switch (p->state) {
	case PS_INITIALIZING:
	case PS_FAULTY:
	case PS_DISABLED:
		return 0;
	case PS_LISTENING:
	case PS_PRE_MASTER:
	case PS_MASTER:
	case PS_GRAND_MASTER:
	case PS_PASSIVE:
	case PS_UNCALIBRATED:
	case PS_SLAVE:
		break;
	}
	return 1;
}

static void flush_last_sync(struct port *p)
{
	if (p->syfu != SF_EMPTY) {
		msg_put(p->last_syncfup);
		p->syfu = SF_EMPTY;
	}
}

static void flush_delay_req(struct port *p)
{
	if (p->delay_req) {
		msg_put(p->delay_req);
		p->delay_req = NULL;
	}
}

static void flush_peer_delay(struct port *p)
{
	if (p->peer_delay_req) {
		msg_put(p->peer_delay_req);
		p->peer_delay_req = NULL;
	}
	if (p->peer_delay_resp) {
		msg_put(p->peer_delay_resp);
		p->peer_delay_resp = NULL;
	}
	if (p->peer_delay_fup) {
		msg_put(p->peer_delay_fup);
		p->peer_delay_fup = NULL;
	}
#ifdef KSZ_1588_PTP
	if (p->sync) {
		msg_put(p->sync);
		p->sync = NULL;
	}
	if (p->follow_up) {
		msg_put(p->follow_up);
		p->follow_up = NULL;
	}
	if (p->delay_resp) {
		msg_put(p->delay_resp);
		p->delay_resp = NULL;
	}
	if (p->pdelay_resp) {
		msg_put(p->pdelay_resp);
		p->pdelay_resp = NULL;
	}
	if (p->pdelay_resp_fup) {
		msg_put(p->pdelay_resp_fup);
		p->pdelay_resp_fup = NULL;
	}
#endif
}

static void port_clear_fda(struct port *p, int count)
{
	int i;

	for (i = 0; i < count; i++)
		p->fda.fd[i] = -1;
}

static void port_disable(struct port *p)
{
	int i;

#ifdef KSZ_1588_PTP
	/* Port 0 can be IEEE 802.1AS port. */
	if (p->asCapable && port_is_ieee8021as(p) && portnum(p) > 0)
		port_set_port_cfg(p, 1, 0);
#endif
	flush_last_sync(p);
	flush_delay_req(p);
	flush_peer_delay(p);

	p->best = NULL;
	free_foreign_masters(p);
	transport_close(p->trp, &p->fda);

	for (i = 0; i < N_TIMER_FDS; i++) {
		close(p->fda.fd[FD_ANNOUNCE_TIMER + i]);
	}
	port_clear_fda(p, N_POLLFD);
	clock_fda_changed(p->clock);
}

static int port_initialize(struct port *p)
{
	struct config *cfg = clock_config(p->clock);
	int fd[N_TIMER_FDS], i;

	p->multiple_seq_pdr_count  = 0;
	p->multiple_pdr_detected   = 0;
	p->last_fault_type         = FT_UNSPECIFIED;
	p->logMinDelayReqInterval  = config_get_int(cfg, p->name, "logMinDelayReqInterval");
	p->peerMeanPathDelay       = 0;
	p->logAnnounceInterval     = config_get_int(cfg, p->name, "logAnnounceInterval");
	p->announceReceiptTimeout  = config_get_int(cfg, p->name, "announceReceiptTimeout");
	p->syncReceiptTimeout      = config_get_int(cfg, p->name, "syncReceiptTimeout");
	p->transportSpecific       = config_get_int(cfg, p->name, "transportSpecific");
	p->transportSpecific     <<= 4;
	p->logSyncInterval         = config_get_int(cfg, p->name, "logSyncInterval");
	p->logMinPdelayReqInterval = config_get_int(cfg, p->name, "logMinPdelayReqInterval");
	p->neighborPropDelayThresh = config_get_int(cfg, p->name, "neighborPropDelayThresh");
	p->min_neighbor_prop_delay = config_get_int(cfg, p->name, "min_neighbor_prop_delay");
#ifdef KSZ_1588_PTP
	p->followUpReceiptTimeout  = config_get_int(cfg, p->name, "followUpReceiptTimeout");
	p->syncTxContTimeout  = config_get_int(cfg, p->name, "syncTxContTimeout");
	if (p->syncTxContTimeout < 140)
		p->syncTxContTimeout = 140;
#endif

	for (i = 0; i < N_TIMER_FDS; i++) {
		fd[i] = -1;
	}
	for (i = 0; i < N_TIMER_FDS; i++) {
		fd[i] = timerfd_create(CLOCK_MONOTONIC, 0);
		if (fd[i] < 0) {
			pr_err("timerfd_create: %s", strerror(errno));
			goto no_timers;
		}
	}
	if (transport_open(p->trp, p->name, &p->fda, p->timestamping))
		goto no_tropen;

	for (i = 0; i < N_TIMER_FDS; i++) {
		p->fda.fd[FD_ANNOUNCE_TIMER + i] = fd[i];
	}

#ifdef KSZ_1588_PTP
	if (is_peer_port(p->clock, p))
#endif
	if (port_set_announce_tmo(p))
		goto no_tmo;

	port_nrate_initialize(p);

	clock_fda_changed(p->clock);
	return 0;

no_tmo:
	transport_close(p->trp, &p->fda);
no_tropen:
no_timers:
	for (i = 0; i < N_TIMER_FDS; i++) {
		if (fd[i] >= 0)
			close(fd[i]);
	}
	return -1;
}

static int port_renew_transport(struct port *p)
{
	int res;

	if (!port_is_enabled(p)) {
		return 0;
	}
	transport_close(p->trp, &p->fda);
	port_clear_fda(p, FD_ANNOUNCE_TIMER);
	res = transport_open(p->trp, p->name, &p->fda, p->timestamping);
	/* Need to call clock_fda_changed even if transport_open failed in
	 * order to update clock to the now closed descriptors. */
	clock_fda_changed(p->clock);
	return res;
}

/*
 * Returns non-zero if the announce message is different than last.
 */
static int update_current_master(struct port *p, struct ptp_message *m)
{
	struct foreign_clock *fc = p->best;
	struct ptp_message *tmp;
#ifndef KSZ_1588_PTP
	struct parent_ds *dad;
	struct path_trace_tlv *ptt;
#endif
	struct timePropertiesDS tds;

#ifdef KSZ_DBG_HOST
if (!is_peer_port(p->clock, p))
printf(" !! %s\n", __func__);
#endif
	if (!msg_source_equal(m, fc))
		return add_foreign_master(p, m);

	if (p->state != PS_PASSIVE) {
		tds.currentUtcOffset = m->announce.currentUtcOffset;
		tds.flags = m->header.flagField[1];
		tds.timeSource = m->announce.timeSource;
		clock_update_time_properties(p->clock, tds);
	}
#ifndef KSZ_1588_PTP
	/* Update pathTrace in clock_update_slave(). */
	if (p->path_trace_enabled) {
		ptt = (struct path_trace_tlv *) m->announce.suffix;
		dad = clock_parent_ds(p->clock);
		memcpy(dad->ptl, ptt->cid, ptt->length);
		dad->path_length = path_length(ptt);
	}
#endif
	port_set_announce_tmo(p);
	fc_prune(fc);
	msg_get(m);
	fc->n_messages++;
	TAILQ_INSERT_HEAD(&fc->messages, m, list);
	if (fc->n_messages > 1) {
		tmp = TAILQ_NEXT(m, list);
		return announce_compare(m, tmp);
	}
	return 0;
}

struct dataset *port_best_foreign(struct port *port)
{
	return port->best ? &port->best->dataset : NULL;
}

/* message processing routines */

/*
 * Returns non-zero if the announce message is both qualified and different.
 */
static int process_announce(struct port *p, struct ptp_message *m)
{
	int result = 0;

#ifdef KSZ_1588_PTP
	if (clock_c37_238(p->clock)) {
		struct ieee_c37_238_data *c37;
		struct alternate_time_offset_tlv *alt;

		if (!c37_238_extract(m, &c37, &alt))
			return result;
	}

	/* Grandmaster cannot be self. */
	if (!memcmp(&p->portIdentity.clockIdentity,
	    &m->announce.grandmasterIdentity, sizeof(struct ClockIdentity)))
		return result;
#endif
	/* Do not qualify announce messages with stepsRemoved >= 255, see
	 * IEEE1588-2008 section 9.3.2.5 (d)
	 */
	if (m->announce.stepsRemoved >= 255)
		return result;

	switch (p->state) {
	case PS_INITIALIZING:
	case PS_FAULTY:
	case PS_DISABLED:
		break;
	case PS_LISTENING:
	case PS_PRE_MASTER:
	case PS_MASTER:
	case PS_GRAND_MASTER:
		result = add_foreign_master(p, m);
		break;
	case PS_PASSIVE:
	case PS_UNCALIBRATED:
	case PS_SLAVE:
		result = update_current_master(p, m);
		break;
	}
	return result;
}

static int process_delay_req(struct port *p, struct ptp_message *m)
{
	struct ptp_message *msg;
	int err;

	if (p->state != PS_MASTER && p->state != PS_GRAND_MASTER)
		return 0;

	if (p->delayMechanism == DM_P2P) {
		pr_warning("port %hu: delay request on P2P port", portnum(p));
		return 0;
	}

	msg = msg_allocate();
	if (!msg)
		return -1;

	msg->hwts.type = p->timestamping;

	msg->header.tsmt               = DELAY_RESP | p->transportSpecific;
	msg->header.ver                = PTP_VERSION;
	msg->header.messageLength      = sizeof(struct delay_resp_msg);
	msg->header.domainNumber       = m->header.domainNumber;
	msg->header.correction         = m->header.correction;
	msg->header.sourcePortIdentity = p->portIdentity;
	msg->header.sourcePortIdentity = p->host_port->portIdentity;
	msg->header.sequenceId         = m->header.sequenceId;
	msg->header.control            = CTL_DELAY_RESP;
	msg->header.logMessageInterval = p->logMinDelayReqInterval;

	ts_to_timestamp(&m->hwts.ts, &msg->delay_resp.receiveTimestamp);

	msg->delay_resp.requestingPortIdentity = m->header.sourcePortIdentity;

	if (p->hybrid_e2e && m->header.flagField[0] & UNICAST) {
		msg->address = m->address;
		msg->header.flagField[0] |= UNICAST;
		msg->header.logMessageInterval = 0x7f;
	}

#ifdef KSZ_1588_PTP
	msg->header.flagField[1] |= PTP_TIMESCALE;
	p->dest_port = p->receive_port;
#endif
	err = port_prepare_and_send(p, msg, 0);
	if (err)
		pr_err("port %hu: send delay response failed", portnum(p));
	msg_put(msg);
	return err;
}

static void process_delay_resp(struct port *p, struct ptp_message *m)
{
	struct delay_req_msg *req;
	struct delay_resp_msg *rsp = &m->delay_resp;
	struct PortIdentity master;
	tmv_t c3, t3, t4, t4c;

	if (!p->delay_req)
		return;

	master = clock_parent_identity(p->clock);
	req = &p->delay_req->delay_req;

	if (p->state != PS_UNCALIBRATED && p->state != PS_SLAVE)
		return;
	if (!pid_eq(&rsp->requestingPortIdentity, &req->hdr.sourcePortIdentity))
		return;
	if (rsp->hdr.sequenceId != ntohs(req->hdr.sequenceId))
		return;
	if (!pid_eq(&master, &m->header.sourcePortIdentity))
		return;

#ifdef KSZ_1588_PTP
	if (msg_sots_missing(p->delay_req)) {
		if (p->delay_resp)
			msg_put(p->delay_resp);
		p->delay_resp = m;
	} else
		p->delay_resp = NULL;
	if (msg_sots_missing(p->delay_req))
		goto next;
#endif
	c3 = correction_to_tmv(m->header.correction);
	t3 = timespec_to_tmv(p->delay_req->hwts.ts);
	t4 = timestamp_to_tmv(m->ts.pdu);
	t4c = tmv_sub(t4, c3);

	clock_path_delay(p->clock, t3, t4c);

#ifdef KSZ_1588_PTP
next:
#endif
	if (p->logMinDelayReqInterval == rsp->hdr.logMessageInterval) {
		return;
	}
	if (m->header.flagField[0] & UNICAST) {
		/* Unicast responses have logMinDelayReqInterval set to 0x7F. */
		return;
	}
	if (rsp->hdr.logMessageInterval < -10 ||
	    rsp->hdr.logMessageInterval > 22) {
		pl_info(300, "port %hu: ignore bogus delay request interval 2^%d",
			portnum(p), rsp->hdr.logMessageInterval);
		return;
	}
	p->logMinDelayReqInterval = rsp->hdr.logMessageInterval;
	pr_notice("port %hu: minimum delay request interval 2^%d",
		  portnum(p), p->logMinDelayReqInterval);
}

static void process_follow_up(struct port *p, struct ptp_message *m)
{
	enum syfu_event event;
	struct PortIdentity master;
	switch (p->state) {
	case PS_INITIALIZING:
	case PS_FAULTY:
	case PS_DISABLED:
	case PS_LISTENING:
	case PS_PRE_MASTER:
	case PS_MASTER:
	case PS_GRAND_MASTER:
	case PS_PASSIVE:
		return;
	case PS_UNCALIBRATED:
	case PS_SLAVE:
		break;
	}
	master = clock_parent_identity(p->clock);
#ifdef KSZ_1588_PTP
	if (!skip_sync_check(p->clock))
#endif
	if (memcmp(&master, &m->header.sourcePortIdentity, sizeof(master)))
		return;

	if (p->follow_up_info) {
		struct follow_up_info_tlv *fui = follow_up_info_extract(m);
		if (!fui)
			return;
		clock_follow_up_info(p->clock, fui);
	}

	if (p->syfu == SF_HAVE_SYNC &&
	    p->last_syncfup->header.sequenceId == m->header.sequenceId) {
		event = FUP_MATCH;
	} else {
		event = FUP_MISMATCH;
	}
#ifdef KSZ_1588_PTP
	/* Out of sequence Follow_Up does not satisfy receive timer. */
	if (event == FUP_MATCH && port_is_ieee8021as(p)) {
		port_clr_tmo(p->fda.fd[FD_FUP_RX_TIMER]);
		if (p->fup_rx_timeout) {
#ifdef KSZ_DBG_TIMER
#if 1
if (portnum(p) == 5)
#endif
printf(" %s fup_rx %d\n", __func__, portnum(p));
#endif
			p->fup_rx_timeout = 0;
		}
		if (PS_SLAVE == p->state) {
			p->fup_correction = m->header.correction;
			memcpy(&p->fup_timestamp, &m->ts.pdu,
				sizeof(struct timestamp));
			clock_update_fup(p->clock, p->index,
				p->fup_correction, &p->fup_timestamp);
		}
	}
#endif
	port_syfufsm(p, event, m);
}

static int process_pdelay_req(struct port *p, struct ptp_message *m)
{
	struct ptp_message *rsp, *fup;
	int err;

	if (p->delayMechanism == DM_E2E) {
		pr_warning("port %hu: pdelay_req on E2E port", portnum(p));
		return 0;
	}
	if (p->delayMechanism == DM_AUTO) {
		pr_info("port %hu: peer detected, switch to P2P", portnum(p));
		p->delayMechanism = DM_P2P;
#ifdef KSZ_1588_PTP
		set_hw_p2p(&ptpdev, 1);
#endif
		port_set_delay_tmo(p);
	}
	if (p->peer_portid_valid) {
		if (!pid_eq(&p->peer_portid, &m->header.sourcePortIdentity)) {
			pr_err("port %hu: received pdelay_req msg with "
				"unexpected peer port id %s",
				portnum(p),
				pid2str(&m->header.sourcePortIdentity));
			p->peer_portid_valid = 0;
			port_capable(p);
		}
	} else {
		p->peer_portid_valid = 1;
		p->peer_portid = m->header.sourcePortIdentity;
		pr_debug("port %hu: peer port id set to %s", portnum(p),
			pid2str(&p->peer_portid));
	}

	rsp = msg_allocate();
	if (!rsp)
		return -1;
	fup = msg_allocate();
	if (!fup) {
		msg_put(rsp);
		return -1;
	}

	rsp->hwts.type = p->timestamping;

	rsp->header.tsmt               = PDELAY_RESP | p->transportSpecific;
	rsp->header.ver                = PTP_VERSION;
	rsp->header.messageLength      = sizeof(struct pdelay_resp_msg);
	rsp->header.domainNumber       = m->header.domainNumber;
	rsp->header.sourcePortIdentity = p->portIdentity;
	rsp->header.sequenceId         = m->header.sequenceId;
	rsp->header.control            = CTL_OTHER;
	rsp->header.logMessageInterval = 0x7f;

#ifdef KSZ_1588_PTP
	if (p->timestamping != TS_ONESTEP)
#endif
	/*
	 * NB - There is no kernel support for one step P2P messaging,
	 * so we always send a follow up message.
	 */
	rsp->header.flagField[0] |= TWO_STEP;

	/*
	 * NB - We do not have any fraction nanoseconds for the correction
	 * fields, neither in the response or the follow up.
	 */
#ifdef KSZ_1588_PTP
	if (need_dest_port(p->clock))
		rsp->header.sourcePortIdentity.portNumber = p->receive_port;
	rsp->header.flagField[1] |= PTP_TIMESCALE;
	p->p2p_sec = p->p2p_nsec = 0;
	if (!clock_two_step(p->clock)) {
		if (get_hw_version(p->clock) < 2) {
			rsp->header.reserved2 = ((m->hwts.ts.tv_sec & 3) << 30)
				| m->hwts.ts.tv_nsec;
			rsp->header.reserved2 = htonl(rsp->header.reserved2);
		}
		if (get_hw_version(p->clock) >= 2) {
			p->p2p_sec = m->hwts.ts.tv_sec;
			p->p2p_nsec = m->hwts.ts.tv_nsec;
		}
		rsp->header.correction = m->header.correction;
	} else
#endif
	ts_to_timestamp(&m->hwts.ts, &rsp->pdelay_resp.requestReceiptTimestamp);
	rsp->pdelay_resp.requestingPortIdentity = m->header.sourcePortIdentity;

	fup->hwts.type = p->timestamping;

	fup->header.tsmt               = PDELAY_RESP_FOLLOW_UP | p->transportSpecific;
	fup->header.ver                = PTP_VERSION;
	fup->header.messageLength      = sizeof(struct pdelay_resp_fup_msg);
	fup->header.domainNumber       = m->header.domainNumber;
	fup->header.correction         = m->header.correction;
	fup->header.sourcePortIdentity = p->portIdentity;
	fup->header.sequenceId         = m->header.sequenceId;
	fup->header.control            = CTL_OTHER;
	fup->header.logMessageInterval = 0x7f;

	fup->pdelay_resp_fup.requestingPortIdentity = m->header.sourcePortIdentity;

#ifdef KSZ_1588_PTP
	if (need_dest_port(p->clock))
		fup->header.sourcePortIdentity.portNumber = p->receive_port;
	fup->header.flagField[1] |= PTP_TIMESCALE;
#endif
	err = peer_prepare_and_send(p, rsp, 1);
	if (err) {
		pr_err("port %hu: send peer delay response failed", portnum(p));
		goto out;
	}
	if (msg_sots_missing(rsp)) {
#ifdef KSZ_1588_PTP
		err = 0;
		if (p->timestamping == TS_ONESTEP)
			goto out;
		msg_get(rsp);
		if (p->pdelay_resp)
			msg_put(p->pdelay_resp);
		p->pdelay_resp = rsp;
		msg_get(fup);
		if (p->pdelay_resp_fup)
			msg_put(p->pdelay_resp_fup);
		p->pdelay_resp_fup = fup;
#else
		pr_err("missing timestamp on transmitted peer delay response");
#endif
		goto out;
	}

	ts_to_timestamp(&rsp->hwts.ts,
			&fup->pdelay_resp_fup.responseOriginTimestamp);

	err = peer_prepare_and_send(p, fup, 0);
	if (err)
		pr_err("port %hu: send pdelay_resp_fup failed", portnum(p));

out:
	msg_put(rsp);
	msg_put(fup);
	return err;
}

static void port_peer_delay(struct port *p)
{
	tmv_t c1, c2, t1, t2, t3, t3c, t4;
	struct ptp_message *req = p->peer_delay_req;
	struct ptp_message *rsp = p->peer_delay_resp;
	struct ptp_message *fup = p->peer_delay_fup;
#ifdef KSZ_1588_PTP
	struct PortIdentity *portIdentity = &p->portIdentity;
#endif

	/* Check for response, validate port and sequence number. */

	if (!rsp)
		return;

#ifdef KSZ_1588_PTP
	if (need_dest_port(p->clock)) {
		struct PortIdentity pid;

		pid = p->portIdentity;
		pid.portNumber = p->receive_port;
		portIdentity = &pid;
	}
	if (!pid_eq(&rsp->pdelay_resp.requestingPortIdentity, portIdentity))
#else
	if (!pid_eq(&rsp->pdelay_resp.requestingPortIdentity, &p->portIdentity))
#endif
		return;

	if (rsp->header.sequenceId != ntohs(req->header.sequenceId))
		return;

	t1 = timespec_to_tmv(req->hwts.ts);
	t4 = timespec_to_tmv(rsp->hwts.ts);
	c1 = correction_to_tmv(rsp->header.correction + p->asymmetry);

	/* Process one-step response immediately. */
	if (one_step(rsp)) {
		t2 = tmv_zero();
		t3 = tmv_zero();
		c2 = tmv_zero();
		goto calc;
	}

	/* Check for follow up, validate port and sequence number. */

	if (!fup)
		return;

	if (!pid_eq(&fup->pdelay_resp_fup.requestingPortIdentity, &p->portIdentity))
		return;

	if (fup->header.sequenceId != rsp->header.sequenceId)
		return;

	if (!source_pid_eq(fup, rsp))
		return;

	/* Process follow up response. */
	t2 = timestamp_to_tmv(rsp->ts.pdu);
	t3 = timestamp_to_tmv(fup->ts.pdu);
	c2 = correction_to_tmv(fup->header.correction);
calc:
	t3c = tmv_add(t3, tmv_add(c1, c2));

	if (p->follow_up_info)
		port_nrate_calculate(p, t3c, t4);

	tsproc_set_clock_rate_ratio(p->tsproc, p->nrate.ratio *
				    clock_rate_ratio(p->clock));
	tsproc_up_ts(p->tsproc, t1, t2);
	tsproc_down_ts(p->tsproc, t3c, t4);
	if (tsproc_update_delay(p->tsproc, &p->peer_delay))
		return;

	p->peerMeanPathDelay = tmv_to_TimeInterval(p->peer_delay);

	if (p->state == PS_UNCALIBRATED || p->state == PS_SLAVE) {
		clock_peer_delay(p->clock, p->peer_delay, t1, t2,
				 p->nrate.ratio);
	}
#ifdef KSZ_1588_PTP
	port_set_peer_delay(p);
	msg_put(p->peer_delay_resp);
	p->peer_delay_resp = NULL;
	if (p->peer_delay_fup) {
		msg_put(p->peer_delay_fup);
		p->peer_delay_fup = NULL;
	}
#endif

	msg_put(p->peer_delay_req);
	p->peer_delay_req = NULL;
}

static int process_pdelay_resp(struct port *p, struct ptp_message *m)
{
#ifdef KSZ_1588_PTP
	p->multiple_pdr = 0;
	if (0 == memcmp(&p->portIdentity, &m->header.sourcePortIdentity,
		        sizeof(struct ClockIdentity))) {
		p->multiple_pdr = 1;
		return 0;
	}
#endif
	if (p->peer_delay_resp) {
		p->multiple_pdr = 1;
		if (!source_pid_eq(p->peer_delay_resp, m)) {
			pr_err("port %hu: multiple peer responses", portnum(p));
			if (!p->multiple_pdr_detected) {
				p->multiple_pdr_detected = 1;
				p->multiple_seq_pdr_count++;
			}
			if (p->multiple_seq_pdr_count >= 3) {
				p->last_fault_type = FT_BAD_PEER_NETWORK;
				return -1;
			}
		}
	}
	if (!p->peer_delay_req) {
		pr_err("port %hu: rogue peer delay response", portnum(p));
		return -1;
	}
	if (p->peer_portid_valid) {
		if (!pid_eq(&p->peer_portid, &m->header.sourcePortIdentity)) {
			pr_err("port %hu: received pdelay_resp msg with "
				"unexpected peer port id %s",
				portnum(p),
				pid2str(&m->header.sourcePortIdentity));
			p->peer_portid_valid = 0;
			port_capable(p);
		}
	} else {
		p->peer_portid_valid = 1;
		p->peer_portid = m->header.sourcePortIdentity;
		pr_debug("port %hu: peer port id set to %s", portnum(p),
			pid2str(&p->peer_portid));
	}

	if (p->peer_delay_resp) {
		msg_put(p->peer_delay_resp);
	}
	msg_get(m);
	p->peer_delay_resp = m;
#ifdef KSZ_1588_PTP
	if (msg_sots_missing(p->peer_delay_req))
		return 0;
#endif
	port_peer_delay(p);
	return 0;
}

static void process_pdelay_resp_fup(struct port *p, struct ptp_message *m)
{
#ifdef KSZ_1588_PTP
	if (0 == memcmp(&p->portIdentity, &m->header.sourcePortIdentity,
		        sizeof(struct ClockIdentity))) {
		return;
	}
#endif
	if (!p->peer_delay_req)
		return;

	if (p->peer_delay_fup)
		msg_put(p->peer_delay_fup);

	msg_get(m);
	p->peer_delay_fup = m;
#ifdef KSZ_1588_PTP
	if (msg_sots_missing(p->peer_delay_req))
		return;
#endif
	port_peer_delay(p);
}

static void process_sync(struct port *p, struct ptp_message *m)
{
	enum syfu_event event;
	struct PortIdentity master;
	switch (p->state) {
	case PS_INITIALIZING:
	case PS_FAULTY:
	case PS_DISABLED:
	case PS_LISTENING:
	case PS_PRE_MASTER:
	case PS_MASTER:
	case PS_GRAND_MASTER:
	case PS_PASSIVE:
		return;
	case PS_UNCALIBRATED:
	case PS_SLAVE:
		break;
	}
	master = clock_parent_identity(p->clock);
#ifdef KSZ_1588_PTP
	if (!skip_sync_check(p->clock))
#endif
	if (memcmp(&master, &m->header.sourcePortIdentity, sizeof(master))) {
		return;
	}

	if (m->header.logMessageInterval != p->log_sync_interval) {
		p->log_sync_interval = m->header.logMessageInterval;
		clock_sync_interval(p->clock, p->log_sync_interval);
	}

	m->header.correction += p->asymmetry;

#ifdef KSZ_1588_PTP
	p->rx_sec = m->hwts.ts.tv_sec;
	if (need_dest_port(p->clock)) {
		if (get_hw_version(p->clock) < 2)
			set_master_port(p->clock, m->header.reserved1);
		if (get_hw_version(p->clock) >= 2)
			set_master_port(p->clock, p->receive_port);
	}
	if (port_is_ieee8021as(p) && p->state == PS_SLAVE) {
		set_master_port(p->clock, portnum(p));
		memcpy(&p->sync_ts, &m->hwts.ts, sizeof(struct timespec));
		p->sync_correction = m->header.correction;
		memcpy(&p->sync_timestamp, &m->ts.pdu,
			sizeof(struct timestamp));
		clock_update_sync(p->clock, p->index, &p->sync_ts,
			p->sync_correction, &p->sync_timestamp);
		port_set_sync_cont_tmo(p);
	}
#endif
	if (one_step(m)) {
		port_synchronize(p, m->hwts.ts, m->ts.pdu,
				 m->header.correction, 0);
		flush_last_sync(p);
		return;
	}

	if (p->syfu == SF_HAVE_FUP &&
	    fup_sync_ok(p->last_syncfup, m) &&
	    p->last_syncfup->header.sequenceId == m->header.sequenceId) {
		event = SYNC_MATCH;
	} else {
		event = SYNC_MISMATCH;
#ifdef KSZ_1588_PTP
		if (p->followUpReceiptTimeout && !p->fup_rx_timeout)
			port_set_fup_rx_tmo(p);
#endif
	}
	port_syfufsm(p, event, m);
}

/* public methods */

void port_close(struct port *p)
{
	if (port_is_enabled(p)) {
		port_disable(p);
	}
	transport_destroy(p->trp);
	tsproc_destroy(p->tsproc);
	if (p->fault_fd >= 0)
		close(p->fault_fd);
	free(p);
}

struct foreign_clock *port_compute_best(struct port *p)
{
	struct foreign_clock *fc;
	struct ptp_message *tmp;

	p->best = NULL;

	LIST_FOREACH(fc, &p->foreign_masters, list) {
		tmp = TAILQ_FIRST(&fc->messages);
		if (!tmp)
			continue;

		announce_to_dataset(tmp, p, &fc->dataset);

		fc_prune(fc);

#ifdef KSZ_1588_PTP
		if (fc->bad_master)
			continue;
		if (!port_is_ieee8021as(p))
#endif
		if (fc->n_messages < FOREIGN_MASTER_THRESHOLD)
			continue;

		if (!p->best)
			p->best = fc;
		else if (dscmp(&fc->dataset, &p->best->dataset) > 0)
			p->best = fc;
		else
			fc_clear(fc);
	}

	return p->best;
}

static void port_e2e_transition(struct port *p, enum port_state next)
{
	port_clr_tmo(p->fda.fd[FD_ANNOUNCE_TIMER]);
	port_clr_tmo(p->fda.fd[FD_SYNC_RX_TIMER]);
	port_clr_tmo(p->fda.fd[FD_DELAY_TIMER]);
	port_clr_tmo(p->fda.fd[FD_QUALIFICATION_TIMER]);
	port_clr_tmo(p->fda.fd[FD_MANNO_TIMER]);
	port_clr_tmo(p->fda.fd[FD_SYNC_TX_TIMER]);
#ifdef KSZ_1588_PTP
	p->ann_rx_timeout = 0;
	p->ann_tx_timeout = 0;
	p->sync_rx_timeout = 0;
	p->sync_tx_timeout = 0;
#endif

	switch (next) {
	case PS_INITIALIZING:
		break;
	case PS_FAULTY:
	case PS_DISABLED:
		port_disable(p);
		break;
	case PS_LISTENING:
		if (p == get_slave_port(p->clock))
			set_slave_port(p->clock, NULL);
		if (!is_peer_port(p->clock, p) ||
		    clock_slave_only(p->clock) ||
		    PS_MASTER == p->state ||
		    PS_GRAND_MASTER == p->state)
			break;
		port_set_announce_tmo(p);
		break;
	case PS_PRE_MASTER:
		if (!is_host_port(p->clock, p))
			break;
		port_set_qualification_tmo(p);
		break;
	case PS_MASTER:
	case PS_GRAND_MASTER:
#ifdef KSZ_1588_PTP
		free_foreign_masters(p);
		if (p == get_slave_port(p->clock))
			set_slave_port(p->clock, NULL);

		/* Only host port handles master clock operation. */
		if (!is_host_port(p->clock, p))
			break;
#endif
		set_tmo_log(p->fda.fd[FD_MANNO_TIMER], 1, -10); /*~1ms*/
		if (!p->ann_tx_timeout) {
#ifdef KSZ_DBG_TIMER
#if 0 
if (portnum(p) == 5)
#endif
printf(" %s ann_tx %d\n", __func__, portnum(p));
#endif
			p->ann_tx_timeout = 1;
		}
		port_set_sync_tx_tmo(p);
		break;
	case PS_PASSIVE:

		/* Host port does not handle Announce. */
		if (!is_peer_port(p->clock, p))
			break;
		port_set_announce_tmo(p);
		break;
	case PS_UNCALIBRATED:
		if (is_peer_port(p->clock, p))
			set_slave_port(p->clock, p);
		flush_last_sync(p);
		flush_delay_req(p);
		/* fall through */
	case PS_SLAVE:

		/* Host port does not handle Announce. */
		if (is_peer_port(p->clock, p) &&
		    !clock_slave_only(p->clock))
		port_set_announce_tmo(p);

		/* Host port does not handle delay. */
		if (!is_peer_port(p->clock, p))
			break;
		port_set_delay_tmo(p);
		break;
	};
}

static void port_p2p_transition(struct port *p, enum port_state next)
{
	port_clr_tmo(p->fda.fd[FD_ANNOUNCE_TIMER]);
	port_clr_tmo(p->fda.fd[FD_SYNC_RX_TIMER]);
	/* Leave FD_DELAY_TIMER running. */
	port_clr_tmo(p->fda.fd[FD_QUALIFICATION_TIMER]);
	port_clr_tmo(p->fda.fd[FD_MANNO_TIMER]);
	port_clr_tmo(p->fda.fd[FD_SYNC_TX_TIMER]);
#ifdef KSZ_1588_PTP
	port_clr_tmo(p->fda.fd[FD_FUP_RX_TIMER]);
	port_clr_tmo(p->fda.fd[FD_SYNC_CONT_TIMER]);
	p->ann_rx_timeout = 0;
	p->ann_tx_timeout = 0;
	p->sync_rx_timeout = 0;
	p->sync_tx_timeout = 0;
	p->fup_rx_timeout = 0;
	p->fup_tx_timeout = 0;
#endif

	switch (next) {
	case PS_INITIALIZING:
		break;
	case PS_FAULTY:
	case PS_DISABLED:
		port_disable(p);
		break;
	case PS_LISTENING:
		if (p == get_slave_port(p->clock))
			set_slave_port(p->clock, NULL);

		/* Host port does not handle Announce. */
		if (is_peer_port(p->clock, p) &&
		    !clock_slave_only(p->clock) &&
		    PS_MASTER != p->state &&
		    PS_GRAND_MASTER != p->state)
		port_set_announce_tmo(p);

		/* Host port does not handle delay. */
		if (!is_peer_port(p->clock, p))
			break;
		port_set_delay_tmo(p);
		break;
	case PS_PRE_MASTER:

		/* Only host port handles master clock operation. */
		if (!is_host_port(p->clock, p))
			break;
		port_set_qualification_tmo(p);
		break;
	case PS_MASTER:
	case PS_GRAND_MASTER:
#ifdef KSZ_1588_PTP
		if (port_is_ieee8021as(p) && p->sync_ts.tv_sec &&
		    p == get_slave_port(p->clock)) {
			clock_set_follow_up_info(p->clock);
			clock_clear_sync_fup(p->clock, p->index);
		}
		if (p == get_slave_port(p->clock))
			set_slave_port(p->clock, NULL);

		/* Only host port handles master clock operation. */
		if (!is_host_port(p->clock, p))
			break;
#endif
		set_tmo_log(p->fda.fd[FD_MANNO_TIMER], 1, -10); /*~1ms*/
		if (!p->ann_tx_timeout) {
#ifdef KSZ_DBG_TIMER
#if 0 
if (portnum(p) == 5)
#endif
printf(" %s ann_tx %d\n", __func__, portnum(p));
#endif
			p->ann_tx_timeout = 1;
		}
		port_set_sync_tx_tmo(p);
		break;
	case PS_PASSIVE:

		/* Host port does not handle Announce. */
		if (!is_peer_port(p->clock, p))
			break;
		port_set_announce_tmo(p);
		break;
	case PS_UNCALIBRATED:
		if (is_peer_port(p->clock, p))
			set_slave_port(p->clock, p);
		flush_last_sync(p);
		flush_peer_delay(p);
		/* fall through */
	case PS_SLAVE:

		/* Host port does not handle Announce and Sync. */
		if (!is_peer_port(p->clock, p) || clock_slave_only(p->clock))
			break;
		port_set_announce_tmo(p);
		break;
	};
}

void port_dispatch(struct port *p, enum fsm_event event, int mdiff)
{
	enum port_state next;
	enum port_state old = p->state;

#if 0
if (event)
printf("%s %d %d\n", __func__, portnum(p), event);
#endif
	if (clock_slave_only(p->clock)) {
		if (event == EV_RS_MASTER || event == EV_RS_GRAND_MASTER) {
			port_slave_priority_warning(p);
		}
	}
	next = p->state_machine(p->state, event, mdiff);
#ifdef KSZ_1588_PTP
	if (!clock_slave_only(p->clock)) {
		if (transparent_clock(p->clock) &&
		    (PS_UNCALIBRATED != p->state && PS_SLAVE != p->state) &&
		    !clock_master_lost(p->clock)) {
			switch (next) {
			case PS_MASTER:
			case PS_GRAND_MASTER:
			case PS_PASSIVE:
#if 0
printf(" change to listen %d\n", portnum(p));
#endif
				next = PS_LISTENING;

				if (next != p->state)
					break;

				/* Will not go through transition. */
				port_clr_tmo(p->fda.fd[FD_ANNOUNCE_TIMER]);
				if (p->ann_rx_timeout) {
#ifdef KSZ_DBG_TIMER
#if 0 
if (portnum(p) == 5)
#endif
printf(" %s ann_rx %d\n", __func__, portnum(p));
#endif
					p->ann_rx_timeout = 0;
				}
				break;
			default:
				break;
			}
		}
	}
#endif

	if (PS_FAULTY == next) {
		struct fault_interval i;
		fault_interval(p, last_fault_type(p), &i);
		if (clear_fault_asap(&i)) {
			pr_notice("port %hu: clearing fault immediately", portnum(p));
			next = p->state_machine(next, EV_FAULT_CLEARED, 0);
		}
	}
	if (PS_INITIALIZING == next) {
		/*
		 * This is a special case. Since we initialize the
		 * port immediately, we can skip right to listening
		 * state if all goes well.
		 */
		if (port_is_enabled(p)) {
			port_disable(p);
		}
		if (port_initialize(p)) {
			event = EV_FAULT_DETECTED;
		} else {
			event = EV_INIT_COMPLETE;
		}
		next = p->state_machine(next, event, 0);
	}

#ifdef KSZ_1588_PTP
	p->new_state = 0;
#endif
	if (next == p->state)
		return;

	port_show_transition(p, next, event);

	if (p->delayMechanism == DM_P2P) {
		port_p2p_transition(p, next);
	} else {
		port_e2e_transition(p, next);
	}

#ifdef KSZ_1588_PTP
	if (event == EV_SYNCHRONIZATION_FAULT && p->best) {
		if (!p->best->bad_master) {
			p->best->good_cnt = 0;
			p->best->bad_cnt++;
			if (p->best->bad_cnt > 2) {
				p->best->bad_master = 1;
				set_tmo_ms(p->fda.fd[FD_ANNOUNCE_TIMER], 10);
			}
		}
	}
	if (boundary_clock(p->clock))
		;
	else if (!is_host_port(p->clock, p))
		;
	else if (p->state == PS_MASTER || p->state == PS_GRAND_MASTER)
		set_hw_master(&ptpdev, 0);
	else if (next == PS_MASTER || next == PS_GRAND_MASTER)
		set_hw_master(&ptpdev, 1);
	else if (need_stop_forwarding(p->clock)) {
		if (p->state == PS_SLAVE)
			set_hw_as(&ptpdev, 1);
		else if (next == PS_SLAVE)
			set_hw_as(&ptpdev, 0);
	}
	p->new_state = 1;
#endif
	p->state = next;

#ifdef KSZ_1588_PTP
	/* Pass event to host port. */
	if (transparent_clock(p->clock) && !port_dispatched(p->clock) &&
	    p != p->host_port) {
		clock_port_dispatch(p->clock, p);
		switch (next) {
		case PS_FAULTY:
			update_dev_cnt(p->clock, -1);
			if (0 == get_dev_cnt(p->clock))
				port_dispatch(p->host_port,
					      EV_FAULT_DETECTED, 0);
			break;
		case PS_LISTENING:

			/* Not being slave anymore. */
			if (PS_UNCALIBRATED == old || PS_SLAVE == old) {
				port_dispatch(p->host_port, event, 0);
			} else if (PS_FAULTY == old || PS_INITIALIZING == old) {
				update_dev_cnt(p->clock, 1);
				if (0 != get_dev_cnt(p->clock))
					port_dispatch(p->host_port,
						      EV_FAULT_CLEARED, 0);
			}
			break;
		case PS_MASTER:
		case PS_GRAND_MASTER:
			port_dispatch(p->host_port, event, 0);
			clock_set_port_state(p->clock, event);
			break;
		case PS_UNCALIBRATED:
		case PS_SLAVE:
			port_dispatch(p->host_port, event, 0);
			clock_set_port_state(p->clock,
					     EV_INIT_COMPLETE);
			break;
		default:
			break;
		}
		clock_port_dispatch(p->clock, NULL);
	}
#endif
	port_notify_event(p, NOTIFY_PORT_STATE);

	if (p->jbod && next == PS_UNCALIBRATED) {
		if (clock_switch_phc(p->clock, p->phc_index)) {
			p->last_fault_type = FT_SWITCH_PHC;
			port_dispatch(p, EV_FAULT_DETECTED, 0);
			return;
		}
		clock_sync_interval(p->clock, p->log_sync_interval);
	}
}

enum fsm_event port_event(struct port *p, int fd_index)
{
	enum fsm_event event = EV_NONE;
	struct ptp_message *msg;
	int cnt, fd = p->fda.fd[fd_index], err;

	switch (fd_index) {
	case FD_SYNC_RX_TIMER:
		if (port_is_ieee8021as(p)) {
#ifdef KSZ_DBG_TIMER
printf("syn: %d=%d %d\n", fd_index, p->announceReceiptTimeout, p->syncReceiptTimeout);
#endif
			port_clr_tmo(p->fda.fd[FD_SYNC_RX_TIMER]);
			if (p->sync_rx_timeout) {
#ifdef KSZ_DBG_TIMER
#if 0 
if (portnum(p) == 5)
#endif
printf(" %s sync_rx %d\n", __func__, portnum(p));
#endif
				p->sync_rx_timeout = 0;
			}
			clock_clear_sync_tx(p->clock, p->index);
		}
	case FD_ANNOUNCE_TIMER:
		pr_debug("port %hu: %s timeout", portnum(p),
			 fd_index == FD_SYNC_RX_TIMER ? "rx sync" : "announce");
		if (p->best)
			fc_clear(p->best);
#ifdef KSZ_DBG_HOST
if (!is_peer_port(p->clock, p))
printf("  !! %s 1\n", __func__);
#endif
		if (clock_slave_only(p->clock) ||
		    PS_MASTER == p->state || PS_GRAND_MASTER == p->state) {
			port_clr_tmo(p->fda.fd[FD_ANNOUNCE_TIMER]);
			port_clr_tmo(p->fda.fd[FD_SYNC_RX_TIMER]);
			if (p->ann_rx_timeout) {
#ifdef KSZ_DBG_TIMER
#if 0 
if (portnum(p) == 5)
#endif
printf(" %s ann_rx %d\n", __func__, portnum(p));
#endif
				p->ann_rx_timeout = 0;
			}
			if (p->sync_rx_timeout) {
#ifdef KSZ_DBG_TIMER
#if 0 
if (portnum(p) == 5)
#endif
printf(" %s sync_rx %d\n", __func__, portnum(p));
#endif
				p->sync_rx_timeout = 0;
			}
		}
		else
		port_set_announce_tmo(p);
#ifdef KSZ_1588_PTP
		if (clock_slave_only(p->clock) && clock_master_lost(p->clock)
				&& port_renew_transport(p)) {
#else
		if (clock_slave_only(p->clock) && p->delayMechanism != DM_P2P &&
		    port_renew_transport(p)) {
#endif
			return EV_FAULT_DETECTED;
		}
		if (p->syncTxCont) {
			p->syncTxCont = 0;
		}
#ifdef KSZ_DBG_TIMER
printf("ann: %d=%d %d\n", fd_index, portnum(p), p->syncTxCont);
#endif
		return EV_ANNOUNCE_RECEIPT_TIMEOUT_EXPIRES;

	case FD_DELAY_TIMER:
		pr_debug("port %hu: delay timeout", portnum(p));
#ifdef KSZ_DBG_HOST
if (!is_peer_port(p->clock, p))
printf("  !! %s %d 2a %p\n", __func__, portnum(p), p);
#endif
		port_set_delay_tmo(p);
		return port_delay_request(p) ? EV_FAULT_DETECTED : EV_NONE;

	case FD_QUALIFICATION_TIMER:
		pr_debug("port %hu: qualification timeout", portnum(p));
		return EV_QUALIFICATION_TIMEOUT_EXPIRES;

	case FD_MANNO_TIMER:
#ifdef KSZ_DBG_HOST
if (!is_host_port(p->clock, p))
printf("  !! %s %d 3\n", __func__, portnum(p));
#endif
		pr_debug("port %hu: master tx announce timeout", portnum(p));
		port_set_manno_tmo(p);
		return port_tx_announce(p) ? EV_FAULT_DETECTED : EV_NONE;

	case FD_SYNC_TX_TIMER:
		pr_debug("port %hu: master sync timeout", portnum(p));
#ifdef KSZ_DBG_HOST
if (!is_host_port(p->clock, p))
printf("  !! %s %d 4\n", __func__, portnum(p));
#endif
#ifdef KSZ_1588_PTP
		/* Clearing the timeout may not take effect yet. */
		if (p->state != PS_MASTER && p->state != PS_GRAND_MASTER)
			return event;
#endif

#ifdef KSZ_1588_PTP
		/* Need to wait for Sync from grandmaster. */
		if (port_is_ieee8021as(p) && p->sync_ts.tv_sec &&
		    !p->syncTxCont) {
#if 0
printf(" wait tx: %d %ld; ", portnum(p), p->sync_ts.tv_sec);
#endif
			port_clr_tmo(p->fda.fd[FD_SYNC_TX_TIMER]);
			if (p->sync_tx_timeout) {
#ifdef KSZ_DBG_TIMER
#if 0
if (portnum(p) == 5)
#endif
printf(" %s sync_tx %d\n", __func__, portnum(p));
#endif
				p->sync_tx_timeout = 0;
			}
		} else
#endif
		port_set_sync_tx_tmo(p);
		return port_tx_sync(p) ? EV_FAULT_DETECTED : EV_NONE;

#ifdef KSZ_1588_PTP
	case FD_FUP_RX_TIMER:
		port_clr_tmo(p->fda.fd[FD_FUP_RX_TIMER]);
		if (p->fup_rx_timeout) {
#ifdef KSZ_DBG_TIMER
#if 1
if (portnum(p) == 5)
#endif
printf(" %s fup_rx %d\n", __func__, portnum(p));
#endif
			p->fup_rx_timeout = 0;
		}
		if (p->syfu == SF_HAVE_SYNC) {
			msg_put(p->last_syncfup);
			p->syfu = SF_EMPTY;
		}
		return port_event(p, FD_ANNOUNCE_TIMER);
	case FD_SYNC_CONT_TIMER:
		port_clr_tmo(p->fda.fd[FD_SYNC_CONT_TIMER]);
		if (p->fup_tx_timeout) {
#ifdef KSZ_DBG_TIMER
if (portnum(p) == 5)
printf(" %s fup_tx %d\n", __func__, portnum(p));
#endif
			p->fup_tx_timeout = 0;
		}
		p->sync_tx_timeout = 0;
		p->syncTxCont = 1;
		clock_update_sync_tx(p->clock, p->index);
#ifdef KSZ_DBG_TIMER
printf(" sync cont\n");
#endif
		return EV_NONE;
#endif
	}

	msg = msg_allocate();
	if (!msg)
		return EV_FAULT_DETECTED;

	msg->hwts.type = p->timestamping;

	cnt = transport_recv(p->trp, fd, msg);
	if (cnt <= 0) {
		pr_err("port %hu: recv message failed", portnum(p));
		msg_put(msg);
#ifdef KSZ_1588_PTP
		return EV_POWERUP;
#else
		return EV_FAULT_DETECTED;
#endif
	}
#ifdef KSZ_1588_PTP
	/* A hack to drop looped transmitted raw frame. */
	if (cnt <= 1) {
		msg_put(msg);
		return EV_NONE;
	}
	p->receive_port = portnum(p);
	if (need_dest_port(p->clock) ||
	    !skip_host_port(p->clock, p)) {
		if (get_hw_version(p->clock) >= 2) {
			u32 port;
			u32 sec;
			u32 nsec;
			int rc;
			int tx = 0;

			rc = port_get_msg_info(p, &msg->header, &tx,
				&port, &sec, &nsec);
			if (!rc) {
#if 0
printf("rc: %d\n", port);
#endif
				p->receive_port = port;
			}
		} else {
			p->receive_port = msg->header.reserved1;
		}
	}
#endif
	err = msg_post_recv(msg, cnt);
	if (err) {
		switch (err) {
		case -EBADMSG:
			pr_err("port %hu: bad message", portnum(p));
			break;
		case -ETIME:
			pr_err("port %hu: received %s without timestamp",
				portnum(p), msg_type_string(msg_type(msg)));
			break;
		case -EPROTO:
			pr_debug("port %hu: ignoring message", portnum(p));
			break;
		}
		msg_put(msg);
		return EV_NONE;
	}
	if (msg_sots_valid(msg)) {
#ifndef KSZ_1588_PTP
		ts_add(&msg->hwts.ts, -p->rx_timestamp_offset);
#endif
		clock_check_ts(p->clock, msg->hwts.ts);
	}
	if (port_ignore(p, msg)) {
		msg_put(msg);
		return EV_NONE;
	}

	switch (msg_type(msg)) {
	case SYNC:
#ifdef KSZ_DBG_HOST
if (!is_peer_port(p->clock, p))
printf("  !! %s %d a\n", __func__, portnum(p));
#endif
		process_sync(p, msg);
		break;
	case DELAY_REQ:
#ifdef KSZ_DBG_HOST
if (!is_peer_port(p->clock, p))
printf("  !! %s %d b\n", __func__, portnum(p));
#endif
		if (process_delay_req(p, msg))
			event = EV_FAULT_DETECTED;
		break;
	case PDELAY_REQ:
#ifdef KSZ_DBG_HOST
if (!is_peer_port(p->clock, p))
printf("  !! %s %d c\n", __func__, portnum(p));
else
#endif
		if (process_pdelay_req(p, msg))
			event = EV_FAULT_DETECTED;
		break;
	case PDELAY_RESP:
#ifdef KSZ_DBG_HOST
if (!is_peer_port(p->clock, p))
printf("  !! %s %d d\n", __func__, portnum(p));
#endif
#ifdef KSZ_1588_PTP
		p->pdelay_resp_port = p->receive_port;
#endif
		if (process_pdelay_resp(p, msg))
			event = EV_FAULT_DETECTED;
		break;
	case FOLLOW_UP:
#ifdef KSZ_DBG_HOST
if (!is_peer_port(p->clock, p))
printf("  !! %s %d e\n", __func__, portnum(p));
#endif
		process_follow_up(p, msg);
		break;
	case DELAY_RESP:
#ifdef KSZ_DBG_HOST
if (!is_peer_port(p->clock, p))
printf("  !! %s %d f\n", __func__, portnum(p));
#endif
		process_delay_resp(p, msg);
#ifdef KSZ_1588_PTP
		if (p->delay_resp)
			msg_get(msg);
#endif
		break;
	case PDELAY_RESP_FOLLOW_UP:
#ifdef KSZ_DBG_HOST
if (!is_peer_port(p->clock, p))
printf("  !! %s %d g\n", __func__, portnum(p));
#endif
#ifdef KSZ_1588_PTP
		p->pdelay_resp_fup_port = p->receive_port;
#endif
		process_pdelay_resp_fup(p, msg);
		break;
	case ANNOUNCE:
#ifdef KSZ_DBG_HOST
if (!is_peer_port(p->clock, p))
printf("  !! %s %d h\n", __func__, portnum(p));
#endif
		if (process_announce(p, msg))
			event = EV_STATE_DECISION_EVENT;
		break;
	case SIGNALING:
		break;
	case MANAGEMENT:
		if (clock_manage(p->clock, p, msg))
			event = EV_STATE_DECISION_EVENT;
		break;
	}

	msg_put(msg);
	return event;
}

#ifdef KSZ_1588_PTP
static int matched_ptp_header(struct ptp_header *src, struct ptp_header *dst)
{
	if (!pid_eq(&src->sourcePortIdentity, &dst->sourcePortIdentity))
		return 0;
	if (src->sequenceId != dst->sequenceId)
		return 0;
	return 1;
}

static int process_delayed_sync(struct port *p, struct ptp_message *msg)
{
	struct ptp_message *sync = p->sync;
	struct ptp_message *fup = p->follow_up;
	struct ptp_header *hdr;
	int pdulen;
	int err = 0;
	struct port *q = p;

	if (!fup)
		return 0;

	hdr = &sync->header;
	if (!matched_ptp_header(&msg->header, hdr))
		return 0;

	/*
	 * Send the follow up message right away.
	 */
	pdulen = sizeof(struct follow_up_msg);
	fup->hwts.type = p->timestamping;

	if (p->follow_up_info)
		pdulen += follow_up_info_append(p, fup);

	fup->header.tsmt               = FOLLOW_UP | p->transportSpecific;
	fup->header.ver                = PTP_VERSION;
	fup->header.messageLength      = pdulen;
	fup->header.domainNumber       = clock_domain_number(p->clock);
	fup->header.sourcePortIdentity = p->portIdentity;
	fup->header.sequenceId         = p->seqnum.sync - 1;
	fup->header.control            = CTL_FOLLOW_UP;
	fup->header.logMessageInterval = p->logSyncInterval;
#if 0
if (p->syncTxCont)
fup->header.logMessageInterval = -1;
#endif

#if 0
	p->tx_sec = msg->hwts.ts.tv_sec;
	if (p->last_tx_sec != p->tx_sec) {
		p->last_tx_sec = p->tx_sec;
printf(" s: %u\n", p->tx_sec);
	}
#endif
	ts_to_timestamp(&msg->hwts.ts, &fup->follow_up.preciseOriginTimestamp);
	if (port_is_ieee8021as(p) && p->sync_ts.tv_sec) {
		Integer64 r1;
		Integer64 t1;
		Integer64 correction;

		r1 = p->sync_ts.tv_sec;
		r1 *= NS_PER_SEC;
		r1 += p->sync_ts.tv_nsec;
		t1 = msg->hwts.ts.tv_sec;
		t1 *= NS_PER_SEC;
		t1 += msg->hwts.ts.tv_nsec;
		correction = t1 - r1;
		if (correction < 0)
			correction = 0;
		correction <<= 16;
		correction += p->fup_correction;
		fup->header.correction = correction;
		tspdu_to_timestamp(&p->fup_timestamp,
			&fup->follow_up.preciseOriginTimestamp);
#if 0
if (p->syncTxCont)
printf(" tx fup:%d\n", portnum(p));
#endif
	}

	fup->header.flagField[1] |= PTP_TIMESCALE;
	p->dest_port = msg->header.reserved1;

	/* Follow_Up needs to be sent by individual port. */
	if (!is_peer_port(p->clock, p)) {
		q = get_port(p->clock, p->dest_port);
		if (!q)
			return err;
	}
	err = port_prepare_and_send(q, fup, 0);
	if (err)
		pr_err("port %hu: send follow up failed", portnum(p));
	if (!need_dest_port(p->clock) && is_peer_port(p->clock, p)) {
		msg = p->sync;
		p->sync = NULL;
		msg_put(msg);
		msg = p->follow_up;
		p->follow_up = NULL;
		msg_put(msg);
	}
	return err;
}

static void process_delayed_delay_req(struct port *p, struct ptp_message *msg)
{
	struct ptp_header *hdr;

	hdr = &p->delay_req->header;
	if (!matched_ptp_header(&msg->header, hdr))
		return;

	if (need_dest_port(p->clock) &&
			msg->header.reserved1 != get_master_port(p->clock))
		return;

	memcpy(&p->delay_req->hwts.ts, &msg->hwts.ts, sizeof(msg->hwts.ts));
	if (p->delay_resp) {
		msg = p->delay_resp;
		process_delay_resp(p, msg);
		if (!p->delay_resp)
			msg_put(msg);
	}
}

static int process_delayed_pdelay_req(struct port *p, struct ptp_message *msg)
{
	struct ptp_header *hdr;
	int err = 0;

	/* Response to Pdelay_Req message may already be processed. */
	if (!p->peer_delay_req)
		return 0;

	hdr = &p->peer_delay_req->header;
	if (!matched_ptp_header(&msg->header, hdr))
		return 0;

	/* Pdelay_Req in 1-step mode has real port number in port identity. */
	if (need_dest_port(p->clock) &&
	    (p->state == PS_UNCALIBRATED || p->state == PS_SLAVE) &&
	    msg->header.reserved1 != get_master_port(p->clock))
		return 0;

	memcpy(&p->peer_delay_req->hwts.ts, &msg->hwts.ts,
		sizeof(msg->hwts.ts));
	if (p->peer_delay_resp) {
		if (need_dest_port(p->clock) &&
		    msg->header.reserved1 != p->pdelay_resp_port)
			return err;
		msg = p->peer_delay_resp;
		p->peer_delay_resp = NULL;
		err = process_pdelay_resp(p, msg);
		msg_put(msg);
	}
	return err;
}

static int process_delayed_pdelay_resp(struct port *p, struct ptp_message *msg)
{
	struct ptp_message *rsp = p->pdelay_resp;
	struct ptp_message *fup = p->pdelay_resp_fup;
	struct ptp_header *hdr;
	int err = -1;

	if (!fup)
		return 0;

	hdr = &rsp->header;
	if (!matched_ptp_header(&msg->header, hdr))
		return 0;

	ts_to_timestamp(&msg->hwts.ts,
			&fup->pdelay_resp_fup.responseOriginTimestamp);

	p->receive_port = msg->header.reserved1;
	err = peer_prepare_and_send(p, fup, 0);
	if (err)
		pr_err("port %hu: send pdelay_resp_fup failed", portnum(p));
	msg = p->pdelay_resp;
	p->pdelay_resp = NULL;
	msg_put(msg);
	msg = p->pdelay_resp_fup;
	p->pdelay_resp_fup = NULL;
	msg_put(msg);
	return err;
}

enum fsm_event port_tx_event(struct port *p, int fd_index)
{
	enum fsm_event event = EV_NONE;
	struct ptp_message *msg;
	int cnt, fd = p->fda.fd[fd_index];

	msg = msg_allocate();
	if (!msg)
		return EV_FAULT_DETECTED;

	msg->hwts.type = p->timestamping;

	cnt = transport_recv_err(p->trp, fd, msg);
	if (cnt <= 0) {
		pr_err("port %hu: recv message failed", portnum(p));
		msg_put(msg);
		return EV_POWERUP;
	}

	switch (msg_type(msg)) {
	case SYNC:
#ifdef KSZ_DBG_HOST
if (!is_host_port(p->clock, p))
printf("  !! %s %d a\n", __func__, portnum(p));
#endif
		if (process_delayed_sync(p, msg))
			event = EV_FAULT_DETECTED;
		break;
	case DELAY_REQ:
#ifdef KSZ_DBG_HOST
if (!is_peer_port(p->clock, p))
printf("  !! %s %d b\n", __func__, portnum(p));
#endif
		process_delayed_delay_req(p, msg);
		break;
	case PDELAY_REQ:
#ifdef KSZ_DBG_HOST
if (!is_peer_port(p->clock, p))
printf("  !! %s %d c\n", __func__, portnum(p));
#endif
		if (process_delayed_pdelay_req(p, msg))
			event = EV_FAULT_DETECTED;
		break;
	case PDELAY_RESP:
#ifdef KSZ_DBG_HOST
if (!is_peer_port(p->clock, p))
printf("  !! %s %d d\n", __func__, portnum(p));
#endif
		if (process_delayed_pdelay_resp(p, msg))
			event = EV_FAULT_DETECTED;
		break;
	}

	msg_put(msg);
	return event;
}
#endif

int port_forward(struct port *p, struct ptp_message *msg)
{
	int cnt;
#ifdef KSZ_1588_PTP
	if (p->forward_port)
		port_set_msg_info(p, &msg->header,
			portdst(p->forward_port), 0, 0);
#endif
	cnt = transport_send(p->trp, &p->fda, 0, msg);
	return cnt <= 0 ? -1 : 0;
}

int port_forward_to(struct port *p, struct ptp_message *msg)
{
	int cnt;
#ifdef KSZ_1588_PTP
	if (p->forward_port)
		port_set_msg_info(p, &msg->header,
			portdst(p->forward_port), 0, 0);
#endif
	cnt = transport_sendto(p->trp, &p->fda, 0, msg);
	return cnt <= 0 ? -1 : 0;
}

int port_prepare_and_send(struct port *p, struct ptp_message *msg, int event)
{
	int cnt;

	if (msg_pre_send(msg))
		return -1;
#ifdef KSZ_1588_PTP
	if (need_dest_port(p->clock) && p->dest_port) {
		if (get_hw_version(p->clock) < 2)
			msg->header.reserved1 = p->dest_port;
		if (get_hw_version(p->clock) >= 2)
			port_set_msg_info(p, &msg->header,
				portdst(p->dest_port), 0, 0);
	}
#endif

	if (msg->header.flagField[0] & UNICAST) {
		cnt = transport_sendto(p->trp, &p->fda, event, msg);
	} else {
		cnt = transport_send(p->trp, &p->fda, event, msg);
	}
	if (cnt <= 0) {
		return -1;
	}
#ifndef KSZ_1588_PTP
	if (msg_sots_valid(msg)) {
		ts_add(&msg->hwts.ts, p->tx_timestamp_offset);
	}
#endif
	return 0;
}

struct PortIdentity port_identity(struct port *p)
{
	return p->portIdentity;
}

int port_number(struct port *p)
{
	return portnum(p);
}

int port_link_status_get(struct port *p)
{
	return p->link_status;
}

void port_link_status_set(struct port *p, int up)
{
	p->link_status = up ? 1 : 0;
	pr_notice("port %hu: link %s", portnum(p), up ? "up" : "down");
}

int port_manage(struct port *p, struct port *ingress, struct ptp_message *msg)
{
	struct management_tlv *mgt;
	UInteger16 target = msg->management.targetPortIdentity.portNumber;

	if (target != portnum(p) && target != 0xffff) {
		return 0;
	}
	mgt = (struct management_tlv *) msg->management.suffix;

	switch (management_action(msg)) {
	case GET:
		if (port_management_get_response(p, ingress, mgt->id, msg))
			return 1;
		break;
	case SET:
		if (port_management_set(p, ingress, mgt->id, msg))
			return 1;
		break;
	case COMMAND:
		break;
	default:
		return -1;
	}

	switch (mgt->id) {
	case TLV_NULL_MANAGEMENT:
	case TLV_CLOCK_DESCRIPTION:
	case TLV_PORT_DATA_SET:
	case TLV_LOG_ANNOUNCE_INTERVAL:
	case TLV_ANNOUNCE_RECEIPT_TIMEOUT:
	case TLV_LOG_SYNC_INTERVAL:
	case TLV_VERSION_NUMBER:
#ifndef KSZ_1588_PTP
	case TLV_ENABLE_PORT:
	case TLV_DISABLE_PORT:
#endif
	case TLV_UNICAST_NEGOTIATION_ENABLE:
	case TLV_UNICAST_MASTER_TABLE:
	case TLV_UNICAST_MASTER_MAX_TABLE_SIZE:
	case TLV_ACCEPTABLE_MASTER_TABLE_ENABLED:
	case TLV_ALTERNATE_MASTER:
	case TLV_TRANSPARENT_CLOCK_PORT_DATA_SET:
	case TLV_DELAY_MECHANISM:
	case TLV_LOG_MIN_PDELAY_REQ_INTERVAL:
		port_management_send_error(p, ingress, msg, TLV_NOT_SUPPORTED);
		break;
	default:
		port_management_send_error(p, ingress, msg, TLV_NO_SUCH_ID);
		return -1;
	}
	return 1;
}

int port_management_error(struct PortIdentity pid, struct port *ingress,
			  struct ptp_message *req, Enumeration16 error_id)
{
	struct ptp_message *msg;
	struct management_tlv *mgt;
	struct management_error_status *mes;
	int err = 0, pdulen;

	msg = port_management_reply(pid, ingress, req);
	if (!msg) {
		return -1;
	}
	mgt = (struct management_tlv *) req->management.suffix;
	mes = (struct management_error_status *) msg->management.suffix;
	mes->type = TLV_MANAGEMENT_ERROR_STATUS;
	mes->length = 8;
	mes->error = error_id;
	mes->id = mgt->id;
	pdulen = msg->header.messageLength + sizeof(*mes);
	msg->header.messageLength = pdulen;
	msg->tlv_count = 1;

#ifdef KSZ_1588_PTP
	ingress->dest_port = ingress->receive_port;
#endif
	err = port_prepare_and_send(ingress, msg, 0);
	msg_put(msg);
	return err;
}

static struct ptp_message *
port_management_construct(struct PortIdentity pid, struct port *ingress,
			  UInteger16 sequenceId,
			  struct PortIdentity *targetPortIdentity,
			  UInteger8 boundaryHops, uint8_t action)
{
	struct ptp_message *msg;
	int pdulen;

	msg = msg_allocate();
	if (!msg)
		return NULL;

	pdulen = sizeof(struct management_msg);
	msg->hwts.type = ingress->timestamping;

	msg->header.tsmt               = MANAGEMENT | ingress->transportSpecific;
	msg->header.ver                = PTP_VERSION;
	msg->header.messageLength      = pdulen;
	msg->header.domainNumber       = clock_domain_number(ingress->clock);
	msg->header.sourcePortIdentity = pid;
	msg->header.sequenceId         = sequenceId;
	msg->header.control            = CTL_MANAGEMENT;
	msg->header.logMessageInterval = 0x7f;

	if (targetPortIdentity)
		msg->management.targetPortIdentity = *targetPortIdentity;
	msg->management.startingBoundaryHops = boundaryHops;
	msg->management.boundaryHops = boundaryHops;

	switch (action) {
	case GET: case SET:
		msg->management.flags = RESPONSE;
		break;
	case COMMAND:
		msg->management.flags = ACKNOWLEDGE;
		break;
	}
	return msg;
}

struct ptp_message *port_management_reply(struct PortIdentity pid,
					  struct port *ingress,
					  struct ptp_message *req)
{
	UInteger8 boundaryHops;

	boundaryHops = req->management.startingBoundaryHops -
		       req->management.boundaryHops;
	return port_management_construct(pid, ingress,
					 req->header.sequenceId,
					 &req->header.sourcePortIdentity,
					 boundaryHops,
					 management_action(req));
}

struct ptp_message *port_management_notify(struct PortIdentity pid,
					   struct port *port)
{
	return port_management_construct(pid, port, 0, NULL, 1, GET);
}

void port_notify_event(struct port *p, enum notification event)
{
	struct PortIdentity pid = port_identity(p);
	struct ptp_message *msg;
	UInteger16 msg_len;
	int id;

	switch (event) {
	case NOTIFY_PORT_STATE:
		id = TLV_PORT_DATA_SET;
		break;
	default:
		return;
	}
	/* targetPortIdentity and sequenceId will be filled by
	 * clock_send_notification */
	msg = port_management_notify(pid, p);
	if (!msg)
		return;
	if (!port_management_fill_response(p, msg, id))
		goto err;
	msg_len = msg->header.messageLength;
	if (msg_pre_send(msg))
		goto err;
	clock_send_notification(p->clock, msg, msg_len, event);
err:
	msg_put(msg);
}

struct port *port_open(int phc_index,
		       enum timestamp_type timestamping,
		       int number,
		       struct interface *interface,
		       struct clock *clock)
{
	struct config *cfg = clock_config(clock);
	struct port *p = malloc(sizeof(*p));
	enum transport_type transport;
	int i;

	if (!p)
		return NULL;

	memset(p, 0, sizeof(*p));

	p->state_machine = clock_slave_only(clock) ? ptp_slave_fsm : ptp_fsm;
	p->phc_index = phc_index;
	p->jbod = config_get_int(cfg, interface->name, "boundary_clock_jbod");
	transport = config_get_int(cfg, interface->name, "network_transport");

	if (transport == TRANS_UDS)
		; /* UDS cannot have a PHC. */
	else if (!interface->ts_info.valid)
		pr_warning("port %d: get_ts_info not supported", number);
	else if (phc_index >= 0 && phc_index != interface->ts_info.phc_index) {
		if (p->jbod) {
			pr_warning("port %d: just a bunch of devices", number);
			p->phc_index = interface->ts_info.phc_index;
		} else {
			pr_err("port %d: PHC device mismatch", number);
			pr_err("port %d: /dev/ptp%d requested, ptp%d attached",
			       number, phc_index, interface->ts_info.phc_index);
			goto err_port;
		}
	}

#ifdef KSZ_1588_PTP
	p->basename = interface->basename;
	p->devname = interface->devname;
	p->host_port = p;

	/* Port number may not be the same as index. */
	p->index = number;
#endif
	p->name = interface->name;
	p->asymmetry = config_get_int(cfg, p->name, "delayAsymmetry");
	p->asymmetry <<= 16;
	p->announce_span = transport == TRANS_UDS ? 0 : ANNOUNCE_SPAN;
	p->follow_up_info = config_get_int(cfg, p->name, "follow_up_info");
	p->freq_est_interval = config_get_int(cfg, p->name, "freq_est_interval");
	p->hybrid_e2e = config_get_int(cfg, p->name, "hybrid_e2e");
	p->path_trace_enabled = config_get_int(cfg, p->name, "path_trace_enabled");
	p->rx_timestamp_offset = config_get_int(cfg, p->name, "ingressLatency");
	p->tx_timestamp_offset = config_get_int(cfg, p->name, "egressLatency");
	p->link_status = 1;
	p->clock = clock;
	p->trp = transport_create(cfg, transport);
	if (!p->trp)
		goto err_port;
	p->timestamping = timestamping;
	p->portIdentity.clockIdentity = clock_identity(clock);
	p->portIdentity.portNumber = number;
	p->state = PS_INITIALIZING;
	p->delayMechanism = config_get_int(cfg, p->name, "delay_mechanism");
	p->versionNumber = PTP_VERSION;

	if (p->hybrid_e2e && p->delayMechanism != DM_E2E) {
		pr_warning("port %d: hybrid_e2e only works with E2E", number);
	}

	/* Set fault timeouts to a default value */
	for (i = 0; i < FT_CNT; i++) {
		p->flt_interval_pertype[i].type = FTMO_LOG2_SECONDS;
		p->flt_interval_pertype[i].val = 4;
	}
	p->flt_interval_pertype[FT_BAD_PEER_NETWORK].type = FTMO_LINEAR_SECONDS;
	p->flt_interval_pertype[FT_BAD_PEER_NETWORK].val =
		config_get_int(cfg, p->name, "fault_badpeernet_interval");

	p->flt_interval_pertype[FT_UNSPECIFIED].val =
		config_get_int(cfg, p->name, "fault_reset_interval");

	p->tsproc = tsproc_create(config_get_int(cfg, p->name, "tsproc_mode"),
				  config_get_int(cfg, p->name, "delay_filter"),
				  config_get_int(cfg, p->name, "delay_filter_length"));
	if (!p->tsproc) {
		pr_err("Failed to create time stamp processor");
		goto err_transport;
	}
	p->nrate.ratio = 1.0;

	port_clear_fda(p, N_POLLFD);
	p->fault_fd = -1;
	if (number) {
		p->fault_fd = timerfd_create(CLOCK_MONOTONIC, 0);
		if (p->fault_fd < 0) {
			pr_err("timerfd_create failed: %m");
			goto err_tsproc;
		}
	}
	return p;

err_tsproc:
	tsproc_destroy(p->tsproc);
err_transport:
	transport_destroy(p->trp);
err_port:
	free(p);
	return NULL;
}

enum port_state port_state(struct port *port)
{
	return port->state;
}

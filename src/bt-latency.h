/* Spa Bluez5 ISO & A2DP I/O Latency Tracking */
/* SPDX-FileCopyrightText: Copyright © 2024 Pauli Virtanen */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_BLUEZ5_BT_LATENCY_H
#define SPA_BLUEZ5_BT_LATENCY_H

#include <time.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/net_tstamp.h>
#include <linux/errqueue.h>
#include <linux/sockios.h>

#include <spa/utils/defs.h>
#include <spa/utils/ratelimit.h>
#include <spa/support/log.h>

#include "defs.h"
#include "rate-control.h"

/* New kernel API */
#ifndef BT_SCM_ERROR
#define BT_SCM_ERROR 0x04
#endif

#define NEW_SOF_TIMESTAMPING_TX_COMPLETION	(1 << 18)
#define NEW_SCM_TSTAMP_COMPLETION		(SCM_TSTAMP_ACK + 1)

#define SPA_BT_LATENCY_PENDING_MAX		256
#define SPA_BT_LATENCY_STALL_TIMEOUT		(350 * SPA_NSEC_PER_MSEC)

/**
 * Bluetooth latency tracking.
 */
struct spa_bt_latency
{
	uint64_t value;
	struct spa_bt_ptp ptp;
	bool valid;
	bool enabled;
	bool have_completion;
	bool stalled;
	bool timed_out;
	uint32_t queue;
	uint32_t kernel_queue;
	size_t unsent;
	uint64_t oldest_age_ns;
	uint64_t max_completion_ns;
	uint64_t baseline_ns;

	struct {
		struct {
			int64_t send;
			uint32_t pos;
			size_t size;
			bool snd;
			bool completion;
		} pending[SPA_BT_LATENCY_PENDING_MAX];
		uint32_t pos;
		int64_t prev_tx;
		struct spa_ratelimit rate_limit;
	} impl;
};

static inline uint32_t spa_bt_latency_inflight(const struct spa_bt_latency *lat)
{
	if (!lat || !lat->enabled)
		return 0;
	return lat->have_completion ? lat->queue : lat->kernel_queue;
}

static inline void spa_bt_latency_init(struct spa_bt_latency *lat, struct spa_bt_transport *transport,
		uint32_t period, struct spa_log *log)
{
	int so_timestamping = (NEW_SOF_TIMESTAMPING_TX_COMPLETION | SOF_TIMESTAMPING_TX_SOFTWARE |
			SOF_TIMESTAMPING_SOFTWARE | SOF_TIMESTAMPING_OPT_ID | SOF_TIMESTAMPING_OPT_TSONLY);
	int res;

	spa_zero(*lat);

	lat->impl.rate_limit.interval = 5 * 60 * SPA_NSEC_PER_SEC;
	lat->impl.rate_limit.burst = 8;

	if (!transport->device->adapter->tx_timestamping_supported)
		return;

	res = setsockopt(transport->fd, SOL_SOCKET, SO_TIMESTAMPING, &so_timestamping, sizeof(so_timestamping));
	if (res < 0) {
		spa_log_info(log, "setsockopt(SO_TIMESTAMPING) failed (kernel feature not enabled?): %d (%m)", errno);
		return;
	}

	/* Flush errqueue on start */
	do {
		res = recv(transport->fd, NULL, 0, MSG_ERRQUEUE | MSG_DONTWAIT | MSG_TRUNC);
	} while (res == 0);

	spa_bt_ptp_init(&lat->ptp, period, period / 2);

	lat->enabled = true;
}

static inline void spa_bt_latency_reset(struct spa_bt_latency *lat)
{
	lat->value = 0;
	lat->valid = false;
	lat->stalled = false;
	lat->oldest_age_ns = 0;
	lat->max_completion_ns = 0;
	spa_bt_ptp_init(&lat->ptp, lat->ptp.period, lat->ptp.period / 2);
}

static inline void spa_bt_latency_clear_pending(struct spa_bt_latency *lat, unsigned int i,
		bool snd, bool completion)
{
	i = i % SPA_N_ELEMENTS(lat->impl.pending);

	if (snd && lat->impl.pending[i].snd) {
		if (lat->kernel_queue)
			lat->kernel_queue--;

		lat->impl.pending[i].snd = false;
	}

	if (completion && lat->impl.pending[i].completion) {
		if (lat->unsent > lat->impl.pending[i].size)
			lat->unsent -= lat->impl.pending[i].size;
		else
			lat->unsent = 0;

		if (lat->queue > 0)
			lat->queue--;
		if (!lat->queue)
			lat->unsent = 0;

		lat->impl.pending[i].completion = false;
	}
}

static inline void spa_bt_latency_clear_all_pending(struct spa_bt_latency *lat)
{
	unsigned int i;

	for (i = 0; i < SPA_N_ELEMENTS(lat->impl.pending); ++i)
		spa_bt_latency_clear_pending(lat, i, true, true);

	lat->queue = 0;
	lat->kernel_queue = 0;
	lat->unsent = 0;
	lat->oldest_age_ns = 0;
}

static inline void spa_bt_latency_inspect(struct spa_bt_latency *lat, int64_t now,
		struct spa_log *log, int fd)
{
	unsigned int i;
	uint64_t max_age = 0;
	int outq = 0;

	if (!lat->enabled || now <= 0) {
		lat->oldest_age_ns = 0;
		return;
	}

	if (fd >= 0 && ioctl(fd, TIOCOUTQ, &outq) < 0)
		outq = 0;

	for (i = 0; i < SPA_N_ELEMENTS(lat->impl.pending); ++i) {
		bool active_slot;
		uint64_t age;

		if (!lat->impl.pending[i].snd && !lat->impl.pending[i].completion)
			continue;

		age = (now > lat->impl.pending[i].send)
			? (uint64_t)(now - lat->impl.pending[i].send)
			: 0;

		active_slot = lat->have_completion
			? lat->impl.pending[i].completion
			: lat->impl.pending[i].snd;
		if (!active_slot) {
			if (!lat->have_completion && lat->impl.pending[i].completion)
				spa_bt_latency_clear_pending(lat, i, false, true);
			continue;
		}

		if (age > SPA_BT_LATENCY_STALL_TIMEOUT)
			lat->stalled = true;

		if ((outq <= 0 && !lat->impl.pending[i].snd &&
		     age > 2 * SPA_NSEC_PER_SEC) ||
		    age > 15 * SPA_NSEC_PER_SEC) {
			int suppressed;

			lat->stalled = true;
			lat->timed_out = true;

			if (log && (suppressed = spa_ratelimit_test(&lat->impl.rate_limit, now)) >= 0)
				spa_log_debug(log, "RF stall or missing completion report for packet (%d suppressed)",
						suppressed);

			if (log)
				spa_log_trace(log, "fd:%d latency[%u] too late", fd, i);
			spa_bt_latency_clear_pending(lat, i, true, true);
			continue;
		}

		if (age > max_age)
			max_age = age;
	}

	lat->oldest_age_ns = spa_bt_latency_inflight(lat) ? max_age : 0;
}

static inline ssize_t spa_bt_send(int fd, const void *buf, size_t size,
		struct spa_bt_latency *lat, uint64_t now)
{
	ssize_t res = send(fd, buf, size, MSG_DONTWAIT | MSG_NOSIGNAL);

	if (!lat || !lat->enabled)
		return res;

	if (res >= 0) {
		uint32_t i = lat->impl.pos % SPA_N_ELEMENTS(lat->impl.pending);

		if (lat->have_completion
				? (lat->impl.pending[i].snd || lat->impl.pending[i].completion)
				: lat->impl.pending[i].snd)
			lat->stalled = true;

		spa_bt_latency_clear_pending(lat, i, true, true);
		lat->impl.pending[i].send = now;
		lat->impl.pending[i].size = size;
		lat->impl.pending[i].pos = lat->impl.pos % UINT16_MAX;
		lat->impl.pending[i].snd = true;
		lat->impl.pending[i].completion = true;

		lat->impl.pos++;
		lat->queue++;
		lat->kernel_queue++;
		lat->unsent += size;
	}

	return res;
}

static inline int spa_bt_latency_recv_errqueue(struct spa_bt_latency *lat, int fd, int64_t now,
		struct spa_log *log)
{
	union {
		char buf[CMSG_SPACE(32 * sizeof(struct scm_timestamping))];
		struct cmsghdr align;
	} control;

	if (!lat->enabled)
		return -EOPNOTSUPP;

	do {
		struct iovec data = {
			.iov_base = NULL,
			.iov_len = 0
		};
		struct msghdr msg = {
			.msg_iov = &data,
			.msg_iovlen = 1,
			.msg_control = control.buf,
			.msg_controllen = sizeof(control.buf),
		};
		struct cmsghdr *cmsg;
		bool have_tss = false, have_serr = false;
		struct scm_timestamping tss;
		struct sock_extended_err serr;
		int res;

		res = recvmsg(fd, &msg, MSG_ERRQUEUE | MSG_DONTWAIT);
		if (res < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			return -errno;
		}

		for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
			if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_TIMESTAMPING) {
				memcpy(&tss, CMSG_DATA(cmsg), sizeof(tss));
				have_tss = true;
			} else if (cmsg->cmsg_level == SOL_BLUETOOTH && cmsg->cmsg_type == BT_SCM_ERROR) {
				memcpy(&serr, CMSG_DATA(cmsg), sizeof(serr));
				have_serr = true;
			} else {
				continue;
			}
		}

		if (!have_tss || !have_serr || serr.ee_errno != ENOMSG || serr.ee_origin != SO_EE_ORIGIN_TIMESTAMPING)
			continue;

		uint32_t tx_pos = serr.ee_data % SPA_N_ELEMENTS(lat->impl.pending);

		if (serr.ee_data % UINT16_MAX != lat->impl.pending[tx_pos].pos) {
			spa_log_debug(log, "fd:%d latency[%u] bad value %u", fd, tx_pos, serr.ee_data);
			continue;
		}

		struct timespec *ts = &tss.ts[0];
		int64_t tx_time = SPA_TIMESPEC_TO_NSEC(ts);
		int64_t completed_send = lat->impl.pending[tx_pos].send;

		switch (serr.ee_info) {
		case SCM_TSTAMP_SND:
			if (lat->have_completion) {
				spa_bt_latency_clear_pending(lat, tx_pos, true, false);
			} else {
				spa_bt_latency_clear_pending(lat, tx_pos, true, true);
				for (unsigned int j = 0; j < SPA_N_ELEMENTS(lat->impl.pending); ++j) {
					if ((lat->impl.pending[j].snd || lat->impl.pending[j].completion) &&
					    lat->impl.pending[j].send < completed_send)
						spa_bt_latency_clear_pending(lat, j, true, true);
				}
			}
			continue;
		case NEW_SCM_TSTAMP_COMPLETION:
			lat->have_completion = true;
			if (!lat->impl.pending[tx_pos].completion)
				continue;
			lat->timed_out = false;
			break;
		default:
			continue;
		}

		if (tx_time >= completed_send) {
			lat->value = (uint64_t)(tx_time - completed_send);
			if (lat->value > lat->max_completion_ns)
				lat->max_completion_ns = lat->value;
			if (lat->value > 0 && lat->value < 60 * SPA_NSEC_PER_MSEC) {
				if (lat->baseline_ns == 0 || lat->value < lat->baseline_ns)
					lat->baseline_ns = lat->value;
				else
					lat->baseline_ns = (lat->baseline_ns * 7u + lat->value) / 8u;
			}
		} else {
			lat->value = 0;
		}

		if (lat->impl.prev_tx && tx_time > lat->impl.prev_tx)
			spa_bt_ptp_update(&lat->ptp, lat->value, tx_time - lat->impl.prev_tx);

		lat->impl.prev_tx = tx_time;

		spa_bt_latency_clear_pending(lat, tx_pos, true, true);

		/* Any older slot sent before completed_send whose completion report was
		 * lost by adapter firmware can now be retired cleanly. */
		for (unsigned int j = 0; j < SPA_N_ELEMENTS(lat->impl.pending); ++j) {
			if ((lat->impl.pending[j].snd || lat->impl.pending[j].completion) &&
			    lat->impl.pending[j].send < completed_send) {
				spa_bt_latency_clear_pending(lat, j, true, true);
			}
		}

		spa_log_trace(log, "fd:%d latency[%d] nsec:%"PRIu64" range:%d..%d ms",
				fd, tx_pos, lat->value,
				(int)(spa_bt_ptp_valid(&lat->ptp) ? lat->ptp.min / SPA_NSEC_PER_MSEC : -1),
				(int)(spa_bt_ptp_valid(&lat->ptp) ? lat->ptp.max / SPA_NSEC_PER_MSEC : -1));
	} while (true);

	spa_bt_latency_inspect(lat, now, log, fd);

	lat->valid = spa_bt_ptp_valid(&lat->ptp);

	return 0;
}

static inline void spa_bt_latency_flush(struct spa_bt_latency *lat, int fd, struct spa_log *log)
{
	int so_timestamping = 0;

	if (!lat->enabled)
		return;

	/* Disable timestamping and flush errqueue */
	setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &so_timestamping, sizeof(so_timestamping));
	spa_bt_latency_recv_errqueue(lat, fd, 0, log);
	spa_bt_latency_clear_all_pending(lat);

	lat->enabled = false;
}

#endif

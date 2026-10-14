//
//  MtkMwx.cpp
//  SimpleMtkWlan
//
//  Created by laobamac (王孝慈) on 2026/5/5.
//  Copyright © 2026 laobamac (王孝慈). All rights reserved.
//

/*	$OpenBSD: if_mwx.c,v 1.9 2026/05/09 12:13:15 claudio Exp $ */
/*
 * Copyright (c) 2022 Claudio Jeker <claudio@openbsd.org>
 * Copyright (c) 2021 MediaTek Inc.
 * Copyright (c) 2021 Lorenzo Bianconi <lorenzo.bianconi83@gmail.com>
 * Copyright (c) 2017 Stefan Sperling <stsp@openbsd.org>
 * Copyright (c) 2016 Felix Fietkau <nbd@nbd.name>
 * Copyright (c) 2007-2010 Damien Bergamini <damien.bergamini@free.fr>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "MtkMwx.hpp"
#ifdef AIRPORT_WCL
#include "WCL/WCL.hpp"
#endif

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/bitfield.h>
#include <sys/kpi_mbuf.h>
#include <sys/_if_ether.h>
#include <sys/_if_media.h>
#include <sys/_malloc.h>
#include <sys/_mbuf.h>
#include <sys/_task.h>
#include <net/ethernet.h>
#include <net80211/ieee80211_proto.h>
#include <net80211/ieee80211_radiotap.h>
#include <FwData.h>

#include "if_mwxreg.h"

OSDefineMetaClassAndStructors(MtkMwx, MtkHalService)

#ifndef NBPFILTER
#define NBPFILTER 0
#endif

#define PCI_VENDOR_MEDIATEK            0x14c3
#define PCI_PRODUCT_MEDIATEK_MT7921    0x7961
#define PCI_PRODUCT_MEDIATEK_MT7921K   0x0608
#define PCI_PRODUCT_MEDIATEK_MT7922    0x7922
#define PCI_PRODUCT_MEDIATEK_MT7922_RZ616 0x0616 /* AMD RZ616, MT7922 silicon; Linux mt7921e uses the MT7922 firmware */

/* Everything this file prints also lands in the driver log published to the I/O Registry. */
#include "../MtkLog.hpp"
#undef printf
#define printf mtk_log
#define MWX_LOG(x...) mtk_log("SimpleMtkWlan-mwx: " x)
#define MWX_DEV_LOG(sc, fmt, args...) \
	mtk_log("SimpleMtkWlan-mwx: %s: " fmt, DEVNAME(sc), ##args)

#define ifnet _ifnet
#define mbuf __mbuf

#undef KASSERT
#define KASSERT(exp, ...) do { if (!(exp)) panic("mwx assertion failed: %s", #exp); } while (0)
#ifndef __unused
#define __unused __attribute__((unused))
#endif
#ifndef ISSET
#define ISSET(t, f) (((t) & (f)) != 0)
#endif
#ifndef IEEE80211_CHAN_40MHZ
#define IEEE80211_CHAN_40MHZ IEEE80211_CHAN_HT40
#endif
#ifndef IEEE80211_CHAN_6GHZ
#define IEEE80211_CHAN_6GHZ 0
#endif
#ifndef IEEE80211_IS_CHAN_6GHZ
#define IEEE80211_IS_CHAN_6GHZ(_c) false
#endif

#ifndef M_DEVBUF
#define M_DEVBUF 0
#endif
#ifndef M_NOWAIT
#define M_NOWAIT 0
#endif
#ifndef M_WAITOK
#define M_WAITOK 0
#endif
#ifndef M_ZERO
#define M_ZERO 0
#endif

static MtkMwx *gMwxHal;

static inline void *
mallocarray(size_t n, size_t size, int type, int flags)
{
	if (size != 0 && n > SIZE_MAX / size)
		return NULL;
	return malloc(n * size, type, flags);
}

static inline void
mwx_free_mem(void *addr)
{
	free(addr);
}

#define free(addr, type, size) mwx_free_mem(addr)

static inline int
loadfirmware(const char *name, u_char **buf, size_t *len)
{
	MWX_LOG("firmware lookup start name=%s\n", name);
	OSData *data = getFWDescByName(name);
	if (data == NULL) {
		MWX_LOG("firmware lookup failed name=%s error=%d\n", name, ENOENT);
		return ENOENT;
	}
	*len = data->getLength();
	*buf = (u_char *)malloc(*len, M_DEVBUF, M_WAITOK);
	if (*buf == NULL) {
		data->release();
		MWX_LOG("firmware copy allocation failed name=%s size=%zu error=%d\n",
		    name, *len, ENOMEM);
		return ENOMEM;
	}
	memcpy(*buf, data->getBytesNoCopy(), *len);
	data->release();
	MWX_LOG("firmware lookup ok name=%s size=%zu\n", name, *len);
	return 0;
}

static inline int
tsleep_nsec(void *ident, int priority, const char *wmesg, uint64_t timo)
{
	if (gMwxHal == NULL)
		return EWOULDBLOCK;
	return gMwxHal->mwxSleepNsec(ident, priority, wmesg, timo);
}

static inline void
config_mountroot(struct device *self, void (*fn)(struct device *))
{
	fn(self);
}

static inline void delay(int usec) { IOSleep((usec + 999) / 1000); }

static inline struct mbuf *
m_pullup(struct mbuf *m, size_t len)
{
	struct mbuf *n = NULL;
	size_t pktlen;
	u_int32_t chunks = 1;
	static uint32_t pullup_rebuild_log_count;

	if (m == NULL)
		return NULL;
	if (mbuf_len(m) >= len)
		return m;

	pktlen = mbuf_pkthdr_len(m);
	if (len > pktlen) {
		m_freem(m);
		return NULL;
	}

	if (pullup_rebuild_log_count < 32) {
		MWX_LOG("m_pullup rebuild len=%zu first_len=%zu pktlen=%zu\n",
		    len, mbuf_len(m), pktlen);
		pullup_rebuild_log_count++;
	}
	if (mbuf_allocpacket(M_DONTWAIT, pktlen, &chunks, &n) != 0) {
		m_freem(m);
		return NULL;
	}
	mbuf_setlen(n, pktlen);
	mbuf_pkthdr_setlen(n, pktlen);
	if (mbuf_copydata(m, 0, pktlen, mtod(n, void *)) != 0) {
		m_freem(n);
		m_freem(m);
		return NULL;
	}
	m_freem(m);
	return n;
}

static inline void
m_adj(struct mbuf *m, int len)
{
	mbuf_adj(m, len);
}

static inline struct mbuf *
m_prepend(struct mbuf *m, size_t len, int how)
{
	return mbuf_prepend(&m, len, how) == 0 ? m : NULL;
}

static inline size_t
m_leadingspace(struct mbuf *m)
{
	return mbuf_leadingspace(m);
}

static inline size_t
m_trailingspace(struct mbuf *m)
{
	return mbuf_trailingspace(m);
}

static inline struct mbuf *
m_gethdr(int how, int type)
{
	struct mbuf *m = NULL;
	mbuf_gethdr(how, type, &m);
	return m;
}

static inline struct mbuf *
MCLGETL(struct mbuf *m, int how, size_t len)
{
	static uint32_t fail_log_count;
	u_int32_t chunks = 1;
	errno_t err;

	/*
	 * A single contiguous buffer above 4096 bytes needs a 16 KB jumbo cluster,
	 * which macOS may not provide (it does not in the installer), so callers
	 * keep their messages within 4096 bytes.
	 */
	if (m == NULL)
		err = mbuf_allocpacket(how, len, &chunks, &m);
	else
		err = mbuf_getcluster(how, mbuf_type(m), len, &m);
	if (err != 0) {
		if (fail_log_count++ < 16)
			mtk_log("SimpleMtkWlan-mwx: mbuf allocation failed len=%zu error=%d\n",
			    len, err);
		return NULL;
	}
	return m;
}

static const struct pci_matchid mwx_devices[] = {
	{ PCI_VENDOR_MEDIATEK, PCI_PRODUCT_MEDIATEK_MT7921 },
	{ PCI_VENDOR_MEDIATEK, PCI_PRODUCT_MEDIATEK_MT7921K },
	{ PCI_VENDOR_MEDIATEK, PCI_PRODUCT_MEDIATEK_MT7922 },
	{ PCI_VENDOR_MEDIATEK, PCI_PRODUCT_MEDIATEK_MT7922_RZ616 },
};

/* Keep the OpenBSD DPRINTF path available, but do not enable it by default.
 * The PCIe interrupt and MCU paths can otherwise flood the kernel log. */
/* #define MWX_DEBUG	1 */

#define	MT7921_ROM_PATCH	"mwx-mt7961_patch_mcu_1_2_hdr"
#define	MT7921_FIRMWARE_WM	"mwx-mt7961_ram_code_1"
#define	MT7922_ROM_PATCH	"mwx-mt7922_patch_mcu_1_1_hdr"
#define	MT7922_FIRMWARE_WM	"mwx-mt7922_ram_code_1"

/*
 * Firmware is sent in FW_SCATTER chunks. Linux uses 4096 data bytes per chunk,
 * but with the 64-byte MCU header that needs a 16 KB mbuf cluster on macOS;
 * keep each message within one 4 KB cluster. The MCU accepts any chunk length
 * (Linux uses 2048 on SDIO).
 */
#define	MWX_FW_CHUNK_LEN	(4096 - sizeof(struct mwx_mcu_txd))

#if NBPFILTER > 0
struct mwx_rx_radiotap_header {
	struct ieee80211_radiotap_header	wr_ihdr;
	uint64_t				wr_tsft;
	uint8_t					wr_flags;
	uint8_t					wr_rate;
	uint16_t				wr_chan_freq;
	uint16_t				wr_chan_flags;
	int8_t					wr_dbm_antsignal;
	int8_t					wr_dbm_antnoise;
} __packed;

#define	MWX_RX_RADIOTAP_PRESENT				\
	((1 << IEEE80211_RADIOTAP_TSFT) |		\
	(1 << IEEE80211_RADIOTAP_FLAGS) |		\
	(1 << IEEE80211_RADIOTAP_RATE) |		\
	(1 << IEEE80211_RADIOTAP_CHANNEL) |		\
	(1 << IEEE80211_RADIOTAP_DBM_ANTSIGNAL) |	\
	(1 << IEEE80211_RADIOTAP_DBM_ANTNOISE))

struct mwx_tx_radiotap_header {
	struct ieee80211_radiotap_header	wt_ihdr;
	uint8_t					wt_flags;
	uint8_t					wt_rate;
	uint16_t				wt_chan_freq;
	uint16_t				wt_chan_flags;
} __packed;

#define	MWX_TX_RADIOTAP_PRESENT			\
	((1 << IEEE80211_RADIOTAP_FLAGS) |	\
	(1 << IEEE80211_RADIOTAP_RATE) |	\
	(1 << IEEE80211_RADIOTAP_CHANNEL))

#endif

struct mwx_txwi {
	struct mt76_txwi		*mt_desc;
	struct mbuf			*mt_mbuf;
	struct ieee80211_node		*mt_ni;	/* reference, dropped in mwx_txwi_put() */
	bus_dmamap_t			mt_map;
	LIST_ENTRY(mwx_txwi)		mt_entry;
	u_int32_t			mt_addr;
	u_int				mt_idx;
	int				mt_busy;
};

struct mwx_txwi_desc {
	struct mt76_txwi		*mt_desc;
	struct mwx_txwi			*mt_data;

	u_int				mt_count;
	bus_dmamap_t			mt_map;
	bus_dma_segment_t		mt_seg;
	LIST_HEAD(, mwx_txwi)		mt_freelist;
};

struct mwx_queue_data {
	struct mbuf			*md_mbuf;
	struct mwx_txwi			*md_txwi;
	bus_dmamap_t			md_map;
};

struct mwx_queue {
	uint32_t			mq_regbase;
	u_int				mq_count;
	u_int				mq_prod;
	u_int				mq_cons;

	struct mwx_desc		*mq_desc;
	struct mwx_queue_data		*mq_data;

	bus_dmamap_t			mq_map;
	bus_dma_segment_t		mq_seg;
	int				mq_wakeme;
};

struct mwx_hw_capa {
	int8_t		has_2ghz;
	int8_t		has_5ghz;
	int8_t		has_6ghz;
	uint8_t		antenna_mask;
	uint8_t		num_streams;
};

struct mwx_node {
	struct ieee80211_node	ni;
	uint16_t		wcid;
	uint16_t		flags;
};
#define	MWX_NODE_FLAG_HAVE_PAIRWISE_KEY		0x1
#define	MWX_NODE_FLAG_HAVE_GROUP_KEY		0x2

struct mwx_vif {
	struct mwx_node		vif_mn;
	uint8_t			idx;
	uint8_t			omac_idx;
	uint8_t			band_idx;
	uint8_t			wmm_idx;
	uint8_t			scan_seq_num;
};

enum mwx_hw_type {
	MWX_HW_MT7921,
	MWX_HW_MT7922,
};

struct mwx_setkey_task_arg {
	struct ieee80211_node	*ni;
	struct ieee80211_key	*k;
};

/* What the firmware was told about the joined BSS (Linux: vif->bss_conf). */
struct mwx_bss_conf {
	uint8_t			bssid[IEEE80211_ADDR_LEN];
	struct ieee80211_channel *chan;
	uint16_t		intval;
	uint8_t			dtim;
	uint8_t			qos;
	uint8_t			phymode;
	uint16_t		nonht_phy;
	uint8_t			he;		/* HE negotiated */
	uint8_t			he_pe;		/* default PE duration */
	uint16_t		he_rts;		/* TXOP duration RTS threshold */
	uint8_t			bss_color;
	uint8_t			bss_color_en;
	int			valid;
};

struct mwx_softc {
	struct device		sc_dev;
	struct ieee80211com	sc_ic;

	enum mwx_hw_type	sc_hwtype;

	struct mwx_queue	sc_txq;
	struct mwx_queue	sc_txmcuq;
	struct mwx_queue	sc_txfwdlq;

	struct mwx_queue	sc_rxq;
	struct mwx_queue	sc_rxmcuq;
	struct mwx_queue	sc_rxfwdlq;

	struct mwx_txwi_desc	sc_txwi;

	bus_space_tag_t		sc_st;
	bus_space_handle_t	sc_memh;
	bus_size_t		sc_mems;
	bus_dma_tag_t		sc_dmat;
	pcitag_t		sc_tag;
	pci_chipset_tag_t	sc_pc;
	void			*sc_ih;

	int			(*sc_newstate)(struct ieee80211com *,
				    enum ieee80211_state, int);

	struct taskq		*sc_nswq;
	struct task		sc_newstate_task;
	struct task		sc_bgscan_done_task;
	struct task		sc_setkey_task;
	struct task		sc_scan_task;
	struct task		sc_reset_task;
	CTimeout		*sc_reset_to;
	u_int			sc_flags;
#define MWX_FLAG_SCANNING		0x01
#define MWX_FLAG_BGSCAN			0x02
	int			sc_coredump_cnt;
	int8_t			sc_resetting;
	int8_t			sc_fw_loaded;

	enum ieee80211_state	sc_ns_state;
	int			sc_ns_arg;

	struct ieee80211_node_switch_bss_arg	*sc_bgscan_arg;
	size_t			sc_bgscan_arg_size;

	struct mwx_setkey_task_arg	sc_setkey_arg[8];
	int			sc_setkey_cur;
	int			sc_setkey_tail;

#if NBPFILTER > 0
	caddr_t			sc_drvbpf;
	union {
		struct mwx_rx_radiotap_header th;
		uint8_t pad[IEEE80211_RADIOTAP_HDRLEN];
	}			sc_rxtapu;
	int			sc_rxtap_len;
	union {
		struct mwx_tx_radiotap_header th;
		uint8_t pad[IEEE80211_RADIOTAP_HDRLEN];
	}			sc_txtapu;
	int			sc_txtap_len;
#define	sc_rxtap	sc_rxtapu.th
#define	sc_txtap	sc_txtapu.th
#endif

	struct mwx_vif		sc_vif;
	uint32_t		sc_wcid_mask;
	uint32_t		sc_intr_mask;

	/* mcu */
	uint32_t		sc_mcu_seq;
	struct {
		struct mbuf	*mcu_m;
		uint32_t	 mcu_cmd;
		uint32_t	 mcu_int;
		volatile int	 mcu_done;	/* set by the interrupt path before wakeup */
	}			sc_mcu_wait[16];
	/*
	 * splnet() is a no-op in this port, so the interrupt work loop and the
	 * thread sending commands both reclaim TX rings. This lock serializes
	 * them, like Linux's per-queue cleanup_lock.
	 */
	lck_grp_t		*sc_txlock_grp;
	lck_mtx_t		*sc_txlock;
	uint8_t			sc_scan_seq_num;

	/* phy / hw */
	struct mwx_hw_capa	sc_capa;
	uint8_t			sc_lladdr[ETHER_ADDR_LEN];
	char			sc_alpha2[4]; /* regulatory-domain */

	int16_t			sc_coverage_class;
	uint8_t			sc_slottime;

	/* mac specific */
	uint32_t		sc_rxfilter;

	/* diagnostic counters, all rate-limited at print sites */
	uint32_t		sc_rx_process_events;
	uint32_t		sc_rx_pkt_event;
	uint32_t		sc_rx_pkt_txrx_notify;
	uint32_t		sc_rx_pkt_normal_mcu;
	uint32_t		sc_rx_pkt_normal;
	uint32_t		sc_rx_pkt_unknown;
	uint32_t		sc_rx_fill_fail;
	uint32_t		sc_rx_delivered;
	uint32_t		sc_scan_done_count;
	int			sc_scan_wd;	/* seconds the current HW scan has run */
	int			sc_in_intr;	/* mwx_intr() is running */
	volatile uint32_t	sc_intr_count;	/* serviced interrupt passes */
	int			sc_mcu_timeouts;	/* consecutive unanswered commands */
	volatile int		sc_isr_phase;	/* where mwx_intr_locked() is */
	uint32_t		sc_wd_last_intr;
	int			sc_wd_status_log;
	struct mwx_bss_conf	sc_bss_conf;	/* last BSS given to the firmware */
	uint32_t		sc_last_rxd1;	/* RX descriptor of the frame in mwx_rx() */
	uint32_t		sc_last_rxd3;
	uint32_t		sc_last_rxd4;
	uint32_t		sc_scan_last_ieee80211_nodes;

	/* Channel last given to the firmware (CHANNEL_SWITCH and BSS RLM). */
	uint8_t			sc_chan_ctrl;
	uint8_t			sc_chan_center;
	uint8_t			sc_chan_bw;	/* CMD_CBW_* */
	/* Highest PHY mode offered to APs: MWX_PHY_LIMIT_*. */
	int			sc_phy_limit;

	/*
	 * Block Ack sessions net80211 accepted and the firmware still has to
	 * be told about (bit per TID). Set from the RX path, consumed by
	 * sc_ba_task because firmware commands cannot wait for replies there.
	 */
	struct task		sc_ba_task;
	volatile uint32_t	sc_ba_rx_start;
	volatile uint32_t	sc_ba_rx_stop;
	volatile uint32_t	sc_ba_tx_start;
	volatile uint32_t	sc_ba_tx_stop;
	int			sc_ba_log;
	uint32_t		sc_tx_ba_mask;
	uint32_t		sc_tx_amsdu_mask;
	uint32_t		sc_link_generation;

	/* A-MSDU subframes split by the hardware, regrouped per MPDU. */
	struct mbuf		*sc_amsdu_head;
	struct ieee80211_rxinfo	sc_amsdu_rxi;
	uint32_t		sc_amsdu_frames;
	uint32_t		sc_amsdu_partial;
	uint16_t		sc_amsdu_seq;
	uint8_t			sc_amsdu_tid;

	/* Rate of the last data frame from the AP (P-RXV), for status/logs. */
	uint32_t		sc_rx_rate_kbps;
	uint8_t			sc_rx_rate_mode;
	uint8_t			sc_rx_rate_mcs;
	uint8_t			sc_rx_rate_nss;
	uint8_t			sc_rx_rate_bw;
	uint8_t			sc_rx_rate_gi;
	uint32_t		sc_rx_data_frames;
	uint32_t		sc_tx_data_frames;
	uint64_t		sc_link_up_ns;	/* when the port became valid */
};

#define	MWX_PHY_LIMIT_LEGACY	0
#define	MWX_PHY_LIMIT_HT	1
#define	MWX_PHY_LIMIT_VHT	2
#define	MWX_PHY_LIMIT_HE	3

const uint8_t mwx_channels_2ghz[] = {
	1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14
};

#define MWX_NUM_2GHZ_CHANNELS   nitems(mwx_channels_2ghz)

const uint8_t mwx_channels_5ghz[] = {
	36, 40, 44, 48, 52, 56, 60, 64,
	100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
	149, 153, 157, 161, 165, 169, 173
};

#define MWX_NUM_5GHZ_CHANNELS   nitems(mwx_channels_5ghz)

const uint8_t mwx_channels_6ghz[] = {
	/* UNII-5 */
	1, 5, 9, 13, 17, 21, 25, 29, 33, 37, 41, 45, 49, 53, 57,
	61, 65, 69, 73, 77, 81, 85, 89, 93,
	/* UNII-6 */
	97, 101, 105, 109, 113, 117,
	/* UNII-7 */
	121, 125, 129, 133, 137, 141, 145, 149, 153, 157, 161, 165, 169,
	173, 177, 181, 185,
	/* UNII-8 */
	189, 193, 197, 201, 205, 209, 213, 217, 221, 225, 229, 233
};

const struct mwx_rate {
	uint16_t	rate;
	uint16_t	hw_value;
} mt76_rates[] = {
	{	2,	(MT_PHY_TYPE_CCK << 8) | 0 },
	{	4,	(MT_PHY_TYPE_CCK << 8) | 1 },
	{	11,	(MT_PHY_TYPE_CCK << 8) | 2 },
	{	22,	(MT_PHY_TYPE_CCK << 8) | 3 },
	{	12,	(MT_PHY_TYPE_OFDM << 8) | 11 },
	{	18,	(MT_PHY_TYPE_OFDM << 8) | 15 },
	{	24,	(MT_PHY_TYPE_OFDM << 8) | 10 },
	{	36,	(MT_PHY_TYPE_OFDM << 8) | 14 },
	{	48,	(MT_PHY_TYPE_OFDM << 8) | 9 },
	{	72,	(MT_PHY_TYPE_OFDM << 8) | 13 },
	{	96,	(MT_PHY_TYPE_OFDM << 8) | 8 },
	{	108,	(MT_PHY_TYPE_OFDM << 8) | 12 },
};


#define MWX_NUM_6GHZ_CHANNELS   nitems(mwx_channels_6ghz)

#define MWX_RATE_HT_SISO_MCS_INV_PLCP 0x20

#define	DEVNAME(s)	((s)->sc_dev.dv_xname)
#define	DEVDEBUG(x)	((x)->sc_ic.ic_if.if_flags & IFF_DEBUG)

#define MWX_RX_DROP_LOG(sc, reason, rv, rxd0, rxd1, rxd2, rxd3, rxd4) do { \
	if ((sc)->sc_rx_fill_fail < 64) { \
		MWX_DEV_LOG((sc), \
		    "rx drop reason=%s rv=%d rxd=%08x/%08x/%08x/%08x/%08x len=%zu\n", \
		    (reason), (rv), (rxd0), (rxd1), (rxd2), (rxd3), \
		    (rxd4), mbuf_len(m)); \
	} \
} while (0)

#ifdef MWX_DEBUG
#define	DPRINTF(x...)		MWX_LOG(x)
#else
#define	DPRINTF(x...)
#endif

#ifdef MWX_PACKET_HEXDUMP
static void
pkt_hex_dump(struct mbuf *m)
{
	int len, rowsize = 16;
	int i, l, linelen;
	uint8_t *data;

	printf("Packet hex dump:\n");
	data = mtod(m, uint8_t *);
	len = mbuf_len(m);

	for (i = 0; i < len; i += linelen) {
		printf("%04x\t", i);
		linelen = len - i;
		if (len - i > rowsize)
			linelen = rowsize;
		for (l = 0; l < linelen; l++)
			printf("%02X ", (uint32_t)data[l]);
		data += linelen;
		printf("\n");
	}
}
#endif

int	mwx_init(struct ifnet *);
int	mwx_init_hw(struct ifnet *);
void	mwx_stop(struct ifnet *);
void	mwx_watchdog(struct ifnet *);
void	mwx_start(struct ifnet *);
int	mwx_ioctl(struct ifnet *, u_long, caddr_t);

struct ieee80211_node *mwx_node_alloc(struct ieee80211com *);
int	mwx_media_change(struct ifnet *);
#if NBPFILTER > 0
void	mwx_radiotap_attach(struct mwx_softc *);
#endif

int	mwx_newstate(struct ieee80211com *, enum ieee80211_state, int);
void	mwx_newstate_task(void *);
int	mwx_scan(struct mwx_softc *);
int	mwx_bgscan(struct ieee80211com *);
void	mwx_bgscan_done(struct ieee80211com *,
	    struct ieee80211_node_switch_bss_arg *, size_t);
void	mwx_bgscan_done_task(void *);
int	mwx_set_key(struct ieee80211com *, struct ieee80211_node *,
	    struct ieee80211_key *);
void	mwx_delete_key(struct ieee80211com *, struct ieee80211_node *,
	    struct ieee80211_key *);
void	mwx_setkey_task(void *);

int	mwx_tx(struct mwx_softc *, struct mbuf *, struct ieee80211_node *);
void	mwx_rx(struct mwx_softc *, struct mbuf *, struct mbuf_list *);
int	mwx_count_nodes(struct ieee80211com *);
int	mwx_intr(void *);
int	mwx_intr_filter(void *);
int	mwx_intr_locked(struct mwx_softc *, uint32_t);
int	mwx_preinit(struct mwx_softc *);
/* How long a full TX ring may stay full before a send fails instead of blocking start(). */
#define MWX_TX_RING_WAIT_MS	3000
/* Linux uses 10 (MT792x_MCU_INIT_RETRY_COUNT); fewer keeps a dead card from stalling boot. */
#define MWX_INIT_RETRY_COUNT	3
void	mwx_attach_hook(struct device *);
bool	mwx_attach(struct device *, struct device *, void *);
int	mwx_activate(struct device *, int);

void	mwx_reset(struct mwx_softc *);
void	mwx_reset_timeo(void *);
void	mwx_reset_task(void *);
int	mwx_txwi_alloc(struct mwx_softc *, int);
void	mwx_txwi_free(struct mwx_softc *);
struct mwx_txwi	*mwx_txwi_get(struct mwx_softc *);
void	mwx_txwi_put(struct mwx_softc *, struct mwx_txwi *);
int	mwx_txwi_enqueue(struct mwx_softc *, struct mwx_txwi *, struct mbuf *,
	    struct ieee80211_node *);
int	mwx_queue_alloc(struct mwx_softc *, struct mwx_queue *, int, uint32_t);
void	mwx_queue_free(struct mwx_softc *, struct mwx_queue *);
void	mwx_queue_reset(struct mwx_softc *, struct mwx_queue *);
int	mwx_buf_fill(struct mwx_softc *, struct mwx_queue_data *,
	    struct mwx_desc *);
int	mwx_queue_fill(struct mwx_softc *, struct mwx_queue *);
int	mwx_dma_alloc(struct mwx_softc *);
int	mwx_dma_reset(struct mwx_softc *, int);
void	mwx_dma_free(struct mwx_softc *);
int	mwx_dma_tx_enqueue(struct mwx_softc *, struct mwx_queue *,
	    struct mbuf *);
static inline int mwx_dma_free_slots(struct mwx_queue *);
void	mwx_dma_txwi_enqueue(struct mwx_softc *, struct mwx_queue *,
	    struct mwx_txwi *);
void	mwx_dma_tx_cleanup(struct mwx_softc *, struct mwx_queue *);
void	mwx_txlock_free(struct mwx_softc *);
void	mwx_dma_tx_done(struct mwx_softc *);
void	mwx_dma_rx_process(struct mwx_softc *, struct mbuf_list *);
void	mwx_dma_rx_dequeue(struct mwx_softc *, struct mwx_queue *,
	    struct mbuf_list *);
void	mwx_dma_rx_done(struct mwx_softc *, struct mwx_queue *);

struct mbuf	*mwx_mcu_alloc_msg(size_t);
void	mwx_mcu_set_len(struct mbuf *, void *);
int	mwx_mcu_send_mbuf(struct mwx_softc *, uint32_t, struct mbuf *, int *);
int	mwx_mcu_send_msg(struct mwx_softc *, uint32_t, void *, size_t, int *);
int	mwx_mcu_send_wait(struct mwx_softc *, uint32_t, void *, size_t);
int	mwx_mcu_send_mbuf_wait(struct mwx_softc *, uint32_t, struct mbuf *);
void	mwx_mcu_rx_event(struct mwx_softc *, struct mbuf *);
int	mwx_mcu_wait_resp_int(struct mwx_softc *, uint32_t, int, uint32_t *);
int	mwx_mcu_wait_resp_msg(struct mwx_softc *, uint32_t, int,
	    struct mbuf **);

int		mwx_dma_disable(struct mwx_softc *sc, int force);
void		mwx_dma_enable(struct mwx_softc *sc);
int		mwx_mcu_fw_pmctrl(struct mwx_softc *);
int		mwx_mcu_drv_pmctrl(struct mwx_softc *);
int		mwx_wfsys_reset(struct mwx_softc *sc);
uint32_t	mwx_reg_addr(struct mwx_softc *, uint32_t);
int		mwx_init_hardware(struct mwx_softc *);
int		mt7921_mcu_init(struct mwx_softc *);
int		mwx_load_firmware(struct mwx_softc *);
int		mwx_mac_wtbl_update(struct mwx_softc *, int);
void		mwx_mac_init_band(struct mwx_softc *sc, uint32_t);
void		mwx_mac_init(struct mwx_softc *);
int		mwx_mcu_patch_sem_ctrl(struct mwx_softc *, int);
int		mwx_mcu_init_download(struct mwx_softc *, uint32_t,
		    uint32_t, uint32_t);
int		mwx_mcu_send_firmware(struct mwx_softc *, int,
		    u_char *, size_t, size_t);
int		mwx_mcu_start_patch(struct mwx_softc *);
int		mwx_mcu_start_firmware(struct mwx_softc *, uint32_t,
		    uint32_t);
int		mt7921_mcu_get_nic_capability(struct mwx_softc *);
int		mt7921_mcu_fw_log_2_host(struct mwx_softc *, uint8_t);
int		mt7921_mcu_set_eeprom(struct mwx_softc *);
int		mt7921_mcu_set_rts_thresh(struct mwx_softc *, uint32_t,
		    uint8_t);
int		mwx_mcu_set_deep_sleep(struct mwx_softc *, int);
void		mt7921_mcu_low_power_event(struct mwx_softc *, struct mbuf *);
void		mt7921_mcu_tx_done_event(struct mwx_softc *, struct mbuf *);
void		mwx_end_scan_task(void *);
void		mt7921_mcu_scan_event(struct mwx_softc *, struct mbuf *);
int		mt7921_mcu_hw_scan(struct mwx_softc *, int);
int		mt7921_mcu_hw_scan_cancel(struct mwx_softc *);
int		mt7921_mcu_set_mac_enable(struct mwx_softc *, int, int);
int		mt7921_mcu_set_channel_domain(struct mwx_softc *);
int		mwx_5ghz_center(int, int);
uint32_t	mwx_chan_ht_flags(int, int);
void		mwx_bss_chandef(struct mwx_softc *, struct ieee80211_node *,
		    int, uint8_t *, uint8_t *, uint8_t *);
int		mwx_parse_phy_limit(const char *);
void		mwx_he_band_caps(struct mwx_softc *, int);
void		mwx_setup_phy_caps(struct mwx_softc *);
void		mwx_apply_phy_limit(struct mwx_softc *);
static const char *mwx_phy_limit_name(int);
static const char *mwx_cbw_name(uint8_t);
int		mwx_ampdu_rx_start(struct ieee80211com *, struct ieee80211_node *,
		    uint8_t);
void		mwx_ampdu_rx_stop(struct ieee80211com *, struct ieee80211_node *,
		    uint8_t);
int		mwx_ampdu_tx_start(struct ieee80211com *, struct ieee80211_node *,
		    uint8_t);
void		mwx_ampdu_tx_stop(struct ieee80211com *, struct ieee80211_node *,
		    uint8_t);
void		mwx_ba_task(void *);
void		mwx_link_reset(struct mwx_softc *);
int		mt7921_mcu_set_chan_info(struct mwx_softc *, int);
void		mt7921_mcu_build_sku(struct mwx_softc *, int, int8_t *);
int		mt7921_mcu_rate_txpower_band(struct mwx_softc *, int,
		    const uint8_t *, int, int);
int		mt7921_mcu_set_rate_txpower(struct mwx_softc *);
void		mt7921_mac_reset_counters(struct mwx_softc *);
void		mt7921_mac_set_timing(struct mwx_softc *);
int		mt7921_mcu_uni_add_dev(struct mwx_softc *, struct mwx_vif *,
		    struct mwx_node *, int);
int		mt7921_mcu_set_sniffer(struct mwx_softc *, int);
int		mt7921_mcu_set_beacon_filter(struct mwx_softc *, int);
int		mt7921_mcu_set_bss_pm(struct mwx_softc *, int);
int		mt7921_mcu_set_tx(struct mwx_softc *, struct mwx_vif *);
int		mt7921_mac_fill_rx(struct mwx_softc *, struct mbuf *,
		    struct ieee80211_rxinfo *);
struct mbuf	*mwx_rx_amsdu(struct mwx_softc *, struct mbuf *,
		    struct ieee80211_rxinfo *);
uint32_t	mt7921_mac_tx_rate_val(struct mwx_softc *);
void		mt7921_mac_write_txwi_80211(struct mwx_softc *, struct mbuf *,
		    struct ieee80211_node *, struct mt76_txwi *);
void		mt7921_mac_write_txwi(struct mwx_softc *, struct mbuf *,
		    struct ieee80211_node *, struct mt76_txwi *);
void		mwx_mac_tx_free(struct mwx_softc *, struct mbuf *);
int		mt7921_set_channel(struct mwx_softc *);

uint8_t		 mt7921_get_phy_mode_v2(struct mwx_softc *,
		    struct ieee80211_node *);
struct mbuf	*mwx_alloc_sta_req_tlv(int);
void		*mwx_append_len(struct mbuf *, int);
void		*mwx_append_tlv(struct mbuf *, uint16_t *, int, int);
struct mbuf	*mwx_fill_sta_req_hdr(struct mbuf *, struct mwx_vif *,
		    uint8_t, uint16_t, uint16_t);
void		 mt7921_mcu_add_basic_tlv(struct mbuf *, uint16_t *,
		    struct mwx_softc *, struct ieee80211_node *, int, int);
void		 mt7921_mcu_add_sta_tlv(struct mbuf *, uint16_t *,
		    struct mwx_softc *, struct ieee80211_node *, int, int, int);
int		 mt7921_mcu_wtbl_generic_tlv(struct mbuf *, uint16_t *,
		    struct mwx_softc *, struct ieee80211_node *);
int		 mt7921_mcu_wtbl_hdr_trans_tlv(struct mbuf *, uint16_t *,
		    struct mwx_softc *, struct ieee80211_node *);
int		 mt7921_mcu_wtbl_ht_tlv(struct mbuf *, uint16_t *,
		    struct mwx_softc *, struct ieee80211_node *);
int		 mt7921_mac_sta_update(struct mwx_softc *,
		    struct ieee80211_node *, int, int, int);
int		 mt7921_mcu_uni_add_bss(struct mwx_softc *, int);
void		 mt7921_bss_conf_from_node(struct mwx_softc *,
		    struct ieee80211_node *);
int		 mt7921_mcu_uni_bss_ps(struct mwx_softc *, int);
int		 mt7921_mcu_uni_bss_bcnft(struct mwx_softc *);
void		 mt7921_mcu_add_key_tlv(struct mbuf *, uint16_t *,
		    struct ieee80211_key *, int);
int		 mt7921_mcu_sta_key_update(struct mwx_softc *,
		    struct ieee80211_node *, struct ieee80211_key *);
void		 mt7921_mcu_sta_key_delete(struct mwx_softc *,
			    struct ieee80211_node *, struct ieee80211_key *);

static inline int
mwx_dbm_to_rssi(int dbm)
{
	if (dbm <= -100)
		return 0;
	if (dbm >= 0)
		return 100;
	return dbm + 100;
}

uint64_t
mwx_uptime_ns()
{
	uint64_t time;

	clock_get_uptime(&time);
	absolutetime_to_nanoseconds(time, &time);
	return time;
}

static inline uint32_t
mwx_read(struct mwx_softc *sc, uint32_t reg)
{
	reg = mwx_reg_addr(sc, reg);
	return bus_space_read_4(sc->sc_st, sc->sc_memh, reg);
}

static inline void
mwx_write(struct mwx_softc *sc, uint32_t reg, uint32_t val)
{
	reg = mwx_reg_addr(sc, reg);
	bus_space_write_4(sc->sc_st, sc->sc_memh, reg, val);
}

static inline void
mwx_barrier(struct mwx_softc *sc)
{
	bus_space_barrier(sc->sc_st, sc->sc_memh, 0, sc->sc_mems,
	    BUS_SPACE_BARRIER_READ | BUS_SPACE_BARRIER_WRITE);
}

static inline uint32_t
mwx_rmw(struct mwx_softc *sc, uint32_t reg, uint32_t val, uint32_t mask)
{
	reg = mwx_reg_addr(sc, reg);
	val |= bus_space_read_4(sc->sc_st, sc->sc_memh, reg) & ~mask;
	bus_space_write_4(sc->sc_st, sc->sc_memh, reg, val);
	return val;
}

static inline uint32_t
mwx_set(struct mwx_softc *sc, uint32_t reg, uint32_t bits)
{
	return mwx_rmw(sc, reg, bits, 0);
}

static inline uint32_t
mwx_clear(struct mwx_softc *sc, uint32_t reg, uint32_t bits)
{
	return mwx_rmw(sc, reg, 0, bits);
}

static inline uint32_t
mt7921_map_reg_l1(struct mwx_softc *sc, uint32_t reg)
{
	uint32_t offset = MWX_HIF_REG_OFFSET(reg);
	uint32_t base = MWX_HIF_REG_BASE(reg);

	mwx_rmw(sc, MT7921_HIF_REMAP_L1,
	    base << MT7921_HIF_REMAP_L1_SHIFT, MT7921_HIF_REMAP_L1_MASK);
	mwx_barrier(sc);

	return MT7921_HIF_REMAP_BASE_L1 + offset;
}

/*
 * Poll for timeout milliseconds or until register reg read the value val
 * after applying the mask to the value read. Returns 0 on success ETIMEDOUT
 * on failure.
 */
int
mwx_poll(struct mwx_softc *sc, uint32_t reg, uint32_t val, uint32_t mask,
    int timeout)
{
	uint32_t cur;

	reg = mwx_reg_addr(sc, reg);
	timeout *= 100;
	do {
		cur = bus_space_read_4(sc->sc_st, sc->sc_memh, reg) & mask;
		if (cur == val)
			return 0;
		delay(10);
	} while (timeout-- > 0);

	DPRINTF("%s: poll timeout reg %x val %x mask %x cur %x\n",
	    DEVNAME(sc), reg, val, mask, cur);
	return ETIMEDOUT;
}

/*
 * ifp specific functions
 */
int
mwx_init(struct ifnet *ifp)
{
#ifdef AIRPORT_WCL
	/*
	 * The native Wi-Fi interface (AirportMtkWlan) reports the adapter to
	 * macOS as resetting, then available or gone, from these events.
	 */
	struct mwx_softc *sc = (struct mwx_softc *)ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;
	int rv;

	if (ic->ic_event_handler)
		ic->ic_event_handler(ic, IEEE80211_EVT_DRIVER_RESET_BEGIN, NULL);
	rv = mwx_init_hw(ifp);
	if (ic->ic_event_handler)
		ic->ic_event_handler(ic, rv ? IEEE80211_EVT_DRIVER_STOPPED :
		    IEEE80211_EVT_DRIVER_READY, NULL);
	return rv;
#else
	return mwx_init_hw(ifp);
#endif
}

int
mwx_init_hw(struct ifnet *ifp)
{
	struct mwx_softc *sc = (struct mwx_softc *)ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;
	struct mwx_node *mn;
	const uint8_t *lladdr = ((struct arpcom *)ifp)->ac_enaddr;
	int rv;

	if (!sc->sc_fw_loaded) {
		rv = mwx_preinit(sc);
		if (rv) {
			MWX_DEV_LOG(sc, "init preinit failed error=%d\n", rv);
			return rv;
		}
	}

	/*
	 * Use the address the upper layers set (macOS's private Wi-Fi address
	 * per network), like iwx_preinit(): net80211 sends with ic_myaddr and
	 * the firmware's vif (registered below) filters on it. Otherwise DHCP
	 * runs on the private address while the AP knows us by the factory one.
	 */
	if (!IEEE80211_ADDR_EQ(lladdr, etheranyaddr) &&
	    !IEEE80211_ADDR_EQ(lladdr, ic->ic_myaddr)) {
		IEEE80211_ADDR_COPY(ic->ic_myaddr, lladdr);
		MWX_DEV_LOG(sc, "init mac address now %s\n",
		    ether_sprintf(ic->ic_myaddr));
	}

	sc->sc_mcu_timeouts = 0;
	MWX_DEV_LOG(sc, "init start opmode=%d flags=0x%x ic_state=%d\n",
	    ic->ic_opmode, ifp->if_flags, ic->ic_state);
	rv = mwx_mcu_set_deep_sleep(sc, 0);
	if (rv)
		MWX_DEV_LOG(sc, "init disable deep sleep returned %d\n", rv);
	else
		MWX_DEV_LOG(sc, "init deep sleep disabled\n");

	rv = mt7921_mcu_set_mac_enable(sc, 0, 1);
	if (rv) {
		MWX_DEV_LOG(sc, "init mac enable failed error=%d\n", rv);
		return rv;
	}
	MWX_DEV_LOG(sc, "init mac enable ok\n");

	rv = mt7921_mcu_set_channel_domain(sc);
	if (rv) {
		MWX_DEV_LOG(sc, "init channel domain failed error=%d\n", rv);
		return rv;
	}
	MWX_DEV_LOG(sc, "init channel domain ok\n");

#if 0
	/* XXX no channel available yet */
	rv = mt7921_mcu_set_chan_info(sc, MCU_EXT_CMD_SET_RX_PATH);
	if (rv)
		return rv;
#endif

	rv = mt7921_mcu_set_rate_txpower(sc);
	if (rv) {
		MWX_DEV_LOG(sc, "init rate txpower failed error=%d\n", rv);
		return rv;
	}
	MWX_DEV_LOG(sc, "init rate txpower ok\n");

	mt7921_mac_reset_counters(sc);
	MWX_DEV_LOG(sc, "init counters reset\n");

	mn = &sc->sc_vif.vif_mn;
	MWX_DEV_LOG(sc, "init vif node=%p wcid=%u\n", mn,
	    mn != NULL ? mn->wcid : 0);

	mwx_mac_wtbl_update(sc, mn->wcid);
	MWX_DEV_LOG(sc, "init wtbl updated wcid=%u\n", mn->wcid);

	rv = mt7921_mcu_uni_add_dev(sc, &sc->sc_vif, mn, 1);
	if (rv) {
		MWX_DEV_LOG(sc, "init add dev failed error=%d\n", rv);
		return rv;
	}
	MWX_DEV_LOG(sc, "init add dev ok\n");

	rv = mt7921_mcu_set_tx(sc, &sc->sc_vif);
	if (rv) {
		MWX_DEV_LOG(sc, "init set tx failed error=%d\n", rv);
		return rv;
	}
	MWX_DEV_LOG(sc, "init set tx ok\n");

	if (ic->ic_opmode == IEEE80211_M_MONITOR) {
		rv = mt7921_mcu_set_chan_info(sc, MCU_EXT_CMD_SET_RX_PATH);
		if (rv)
			return rv;
		rv = mt7921_set_channel(sc);
		if (rv)
			return rv;

		rv = mt7921_mcu_set_sniffer(sc, 1);
		MWX_DEV_LOG(sc, "init monitor sniffer enable returned %d\n", rv);
		rv = mt7921_mcu_set_beacon_filter(sc, 0);
		MWX_DEV_LOG(sc, "init monitor beacon filter disable returned %d\n", rv);
		sc->sc_rxfilter = 0;
		mwx_set(sc, MT_DMA_DCR0(0), MT_DMA_DCR0_RXD_G5_EN);
	} else {
		rv = mt7921_mcu_set_sniffer(sc, 0);
		MWX_DEV_LOG(sc, "init sniffer disable returned %d\n", rv);
		sc->sc_rxfilter |= MT_WF_RFCR_DROP_OTHER_UC;
		mwx_clear(sc, MT_DMA_DCR0(0), MT_DMA_DCR0_RXD_G5_EN);
	}
	mwx_write(sc, MT_WF_RFCR(0), sc->sc_rxfilter);
	MWX_DEV_LOG(sc, "init rxfilter set 0x%08x\n", sc->sc_rxfilter);

	ifp->if_flags |= IFF_RUNNING;
	ifq_clr_oactive(&ifp->if_snd);
	MWX_DEV_LOG(sc, "init interface running flags=0x%x\n", ifp->if_flags);

	if (ic->ic_opmode == IEEE80211_M_MONITOR) {
		ic->ic_bss->ni_chan = ic->ic_ibss_chan;
		ieee80211_new_state(ic, IEEE80211_S_RUN, -1);
		MWX_DEV_LOG(sc, "init monitor state run\n");
		return 0;
	}

	MWX_DEV_LOG(sc, "init begin scan\n");
	ieee80211_begin_scan(ifp);
	MWX_DEV_LOG(sc, "init begin scan returned state=%d flags=0x%x sc_flags=0x%x\n",
	    ic->ic_state, ifp->if_flags, sc->sc_flags);

	/*
	 * ieee80211_begin_scan() ends up scheduling mwx_newstate_task().
	 * Wait until the transition to SCAN state has completed.
	 */

	return 0;
}

void
mwx_stop(struct ifnet *ifp)
{
	struct mwx_softc *sc = (struct mwx_softc *)ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;
	int s = splnet();
	int was_running = (ifp->if_flags & IFF_RUNNING) != 0;

	DPRINTF("%s: stop\n", DEVNAME(sc));

	/*
	 * Tell the AP we are leaving before the radio goes off, like Linux does
	 * on disconnect or interface down. Without it the AP (a mesh here) kept
	 * a stale session for our MAC, and the next OS to join with the same MAC
	 * had no traffic until reconnecting.
	 */
	if (was_running && ic->ic_opmode == IEEE80211_M_STA &&
	    ic->ic_state == IEEE80211_S_RUN && ic->ic_bss != NULL) {
		MWX_DEV_LOG(sc, "stop: deauthenticating from %s\n",
		    ether_sprintf(ic->ic_bss->ni_bssid));
		IEEE80211_SEND_MGMT(ic, ic->ic_bss, IEEE80211_FC0_SUBTYPE_DEAUTH,
		    IEEE80211_REASON_AUTH_LEAVE);
		delay(100 * 1000);	/* let it leave the TX ring */
	}

	/* Cancel scheduled tasks and let any stale tasks finish up. */
	if (sc->sc_nswq != NULL) {
		task_del(sc->sc_nswq, &sc->sc_newstate_task);
		task_del(sc->sc_nswq, &sc->sc_bgscan_done_task);
		task_del(sc->sc_nswq, &sc->sc_setkey_task);
		task_del(sc->sc_nswq, &sc->sc_scan_task);
		task_del(sc->sc_nswq, &sc->sc_reset_task);
		task_del(sc->sc_nswq, &sc->sc_ba_task);
	}
	timeout_del(&sc->sc_reset_to);

	ifp->if_timer = 0;
	ifp->if_flags &= ~IFF_RUNNING;
	ifq_clr_oactive(&ifp->if_snd);

	if (was_running) {
		struct mwx_node *mn;

		free(sc->sc_bgscan_arg, M_DEVBUF, sc->sc_bgscan_arg_size);
		sc->sc_bgscan_arg = NULL;
		sc->sc_bgscan_arg_size = 0;
		if (sc->sc_flags & MWX_FLAG_SCANNING) {
			mt7921_mcu_hw_scan_cancel(sc);
			sc->sc_flags &= ~MWX_FLAG_SCANNING;
		}

		mn = &sc->sc_vif.vif_mn;
		mt7921_mcu_uni_add_dev(sc, &sc->sc_vif, mn, 0);
		mwx_mcu_set_deep_sleep(sc, 1);
		mt7921_mcu_set_mac_enable(sc, 0, 0);
	}

	sc->sc_newstate(ic, IEEE80211_S_INIT, -1);
	sc->sc_ns_state = IEEE80211_S_INIT;
	ifp->if_timer = 0;

#ifdef AIRPORT_WCL
	/* A stopped firmware scan cannot deliver its completion event. */
	ic->ic_flags &= ~(IEEE80211_F_BGSCAN | IEEE80211_F_ASCAN);
	ic->ic_wcl_scan_requested = false;
	ic->ic_wcl_scan_active = false;
	ic->ic_wcl_scan_ssid_length = 0;
	ic->ic_wcl_scan_idle = false;
	if (ic->ic_event_handler)
		ic->ic_event_handler(ic, IEEE80211_EVT_DRIVER_STOPPED, NULL);
#endif

	splx(s);
}

void
mwx_watchdog(struct ifnet *ifp)
{
	struct mwx_softc *sc = (struct mwx_softc *)ifp->if_softc;

	/*
	 * A HW scan reports done within its ~5 s timeout. If the event never
	 * comes, net80211 waits in SCAN forever (no auth, no further scans), so
	 * reset the driver instead. Called once a second by the controller.
	 */
	if ((sc->sc_flags & MWX_FLAG_SCANNING) && !sc->sc_resetting &&
	    ++sc->sc_scan_wd > 15) {
		MWX_DEV_LOG(sc, "scan did not finish within 15 s, resetting\n");
		sc->sc_scan_wd = 0;
		mwx_reset(sc);
	}

	/*
	 * Hardware status: shows whether the interrupt handler is stuck (phase
	 * != 0, no new passes), interrupts are masked (ena 0), or the card stopped
	 * filling the RX rings (dma index not moving). Every 30 s while associated,
	 * immediately when no interrupt pass happened for 3 s, within a budget.
	 */
	{
		static int ticks_since;
		uint32_t n = sc->sc_intr_count;
		int idle = (n == sc->sc_wd_last_intr);

		ticks_since++;
		if (sc->sc_wd_status_log < 120 &&
		    ((sc->sc_ic.ic_state == IEEE80211_S_RUN && ticks_since >= 30) ||
		    (idle && ticks_since >= 3 &&
		    sc->sc_ic.ic_state >= IEEE80211_S_AUTH))) {
			struct mwx_queue *rd = &sc->sc_rxq, *r0 = &sc->sc_rxfwdlq;
			struct mwx_queue *t = &sc->sc_txq;

			sc->sc_wd_status_log++;
			ticks_since = 0;
			MWX_DEV_LOG(sc, "hw status state=%d ints=%u phase=%d in_intr=%d sta=%08x ena=%08x rxdata cpu/dma/cons=%u/%u/%u rxmcu=%u/%u/%u tx=%u/%u/%u\n",
			    sc->sc_ic.ic_state, n, sc->sc_isr_phase, sc->sc_in_intr,
			    mwx_read(sc, MT_WFDMA0_HOST_INT_STA),
			    mwx_read(sc, MT_WFDMA0_HOST_INT_ENA),
			    mwx_read(sc, rd->mq_regbase + MT_DMA_CPU_IDX),
			    mwx_read(sc, rd->mq_regbase + MT_DMA_DMA_IDX), rd->mq_cons,
			    mwx_read(sc, r0->mq_regbase + MT_DMA_CPU_IDX),
			    mwx_read(sc, r0->mq_regbase + MT_DMA_DMA_IDX), r0->mq_cons,
			    mwx_read(sc, t->mq_regbase + MT_DMA_CPU_IDX),
			    mwx_read(sc, t->mq_regbase + MT_DMA_DMA_IDX), t->mq_cons);
			MWX_DEV_LOG(sc, "link rx_rate=%u kbps rx_data=%u tx_data=%u amsdu=%u partial=%u tx_ba=0x%x\n",
			    sc->sc_rx_rate_kbps, sc->sc_rx_data_frames,
			    sc->sc_tx_data_frames, sc->sc_amsdu_frames,
			    sc->sc_amsdu_partial, sc->sc_tx_ba_mask);
		}
		sc->sc_wd_last_intr = n;
	}

	ifp->if_timer = 0;
	ieee80211_watchdog(ifp);
}

void
mwx_start(struct ifnet *ifp)
{
	struct mwx_softc *sc = (struct mwx_softc *)ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_node *ni;
	struct ether_header *eh;
	struct mbuf *m;

	if (!(ifp->if_flags & IFF_RUNNING) || ifq_is_oactive(&ifp->if_snd))
		return;

	for (;;) {
		/* XXX TODO handle oactive
			ifq_set_oactive(&ifp->if_snd);
		*/

		/* need to send management frames even if we're not RUNning */
		m = mq_dequeue(&ic->ic_mgtq);
		if (m) {
			ni = (struct ieee80211_node *)mbuf_pkthdr_rcvif(m);
			goto sendit;
		}

		if (ic->ic_state != IEEE80211_S_RUN ||
		    (ic->ic_xflags & IEEE80211_F_TX_MGMT_ONLY))
			break;

		m = ifq_dequeue(&ifp->if_snd);
		if (!m)
			break;
		if (mbuf_len(m) < sizeof (*eh) &&
		    (m = m_pullup(m, sizeof (*eh))) == NULL) {
			ifp->if_oerrors++;
			continue;
		}
#if NBPFILTER > 0
		if (ifp->if_bpf != NULL)
			bpf_mtap(ifp->if_bpf, m, BPF_DIRECTION_OUT);
#endif
		if ((m = ieee80211_encap(ifp, m, &ni)) == NULL) {
			ifp->if_oerrors++;
			continue;
		}

 sendit:
#if NBPFILTER > 0
		if (ic->ic_rawbpf != NULL)
			bpf_mtap(ic->ic_rawbpf, m, BPF_DIRECTION_OUT);
#endif
		if (mwx_tx(sc, m, ni) != 0) {
			ieee80211_release_node(ic, ni);
			ifp->if_oerrors++;
			continue;
		}

		if (ifp->if_flags & IFF_UP)
			ifp->if_timer = 1;
	}
}

int
mwx_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
	int s, err = 0;

	s = splnet();
	switch (cmd) {
	case SIOCSIFADDR:
		ifp->if_flags |= IFF_UP;
		/* FALLTHROUGH */
	case SIOCSIFFLAGS:
		if (ifp->if_flags & IFF_UP) {
			if (!(ifp->if_flags & IFF_RUNNING)) {
				mwx_stop(ifp);
				err = mwx_init(ifp);
			}
		} else {
			if (ifp->if_flags & IFF_RUNNING)
				mwx_stop(ifp);
		}
		break;
	default:
		err = ieee80211_ioctl(ifp, cmd, data);
		break;
	}

	if (err == ENETRESET) {
		err = 0;
		if ((ifp->if_flags & (IFF_UP | IFF_RUNNING)) ==
		    (IFF_UP | IFF_RUNNING)) {
			mwx_stop(ifp);
			err = mwx_init(ifp);
		}
	}
	splx(s);
	return err;
}

int
mwx_media_change(struct ifnet *ifp)
{
	struct mwx_softc *sc = (struct mwx_softc *)ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;
	int err;

	err = ieee80211_media_change(ifp);
	if (err != ENETRESET)
		return err;

	/* TODO lot more handling here */
	if (ic->ic_fixed_mcs != -1) {
		;
	} else if (ic->ic_fixed_rate != -1) {
		;
	}
	if ((ifp->if_flags & (IFF_UP | IFF_RUNNING)) ==
	    (IFF_UP | IFF_RUNNING)) {
		/* XXX could be a bit harsh */
		mwx_stop(ifp);
		err = mwx_init(ifp);
	}
	return err;
}

/*
 * net80211 specific functions.
 */

struct ieee80211_node *
mwx_node_alloc(struct ieee80211com *ic)
{
	/* XXX this is just wrong */
	static uint32_t wcid = 0;
	struct mwx_softc *sc = (struct mwx_softc *)ic->ic_softc;
	struct mwx_node *mn;

	mn = (struct mwx_node *)malloc(sizeof(struct mwx_node), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (mn == NULL)
		return NULL;
	mn->wcid = 1 + wcid++ % (MWX_WTBL_STA - 1);

	/* init WCID table entry */
	mwx_mac_wtbl_update(sc, mn->wcid);

	return &mn->ni;
}

void
mwx_newassoc(struct ieee80211com *ic, struct ieee80211_node *ni, int isnew)
{
	struct mwx_softc *sc = (struct mwx_softc *)ic->ic_softc;
	struct mwx_node *mn = (struct mwx_node *)ni;
	uint16_t wcid = 0;

	if (isnew && ni->ni_associd != 0) {
		/* only interested in true associations */
		wcid = IEEE80211_AID(ni->ni_associd);

	}
	printf("%s: new assoc isnew=%d addr=%s WCID=%d\n", DEVNAME(sc),
	    isnew, ether_sprintf(ni->ni_macaddr), mn->wcid);

	/* XXX TODO rate handling here */
}

#ifndef IEEE80211_STA_ONLY
void
mwx_node_leave(struct ieee80211com *ic, struct ieee80211_node *ni)
{
#if 0
	struct mwx_softc *sc = (struct mwx_softc *)ic->ic_softc;
	struct mwx_node *mn = (struct mwx_node *)ni;
	uint16_t wcid = mn->wcid;

	/* TODO clear WCID */
#endif
}
#endif

int
mwx_scan(struct mwx_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &ic->ic_if;
	int rv;

	if (sc->sc_flags & MWX_FLAG_BGSCAN) {
		rv = mt7921_mcu_hw_scan_cancel(sc);
		if (rv) {
			printf("%s: could not abort background scan\n",
			    DEVNAME(sc));
			return rv;
		}
	}

	rv = mt7921_mcu_hw_scan(sc, 0);
	if (rv) {
		printf("%s: could not initiate scan\n", DEVNAME(sc));
		return rv;
	}

	/*
	 * The current mode might have been fixed during association.
	 * Ensure all channels get scanned.
	 */
	if (IFM_MODE(ic->ic_media.ifm_cur->ifm_media) != IFM_AUTO)
		ieee80211_setmode(ic, IEEE80211_MODE_AUTO);

	sc->sc_flags |= MWX_FLAG_SCANNING;
	if (ifp->if_flags & IFF_DEBUG)
		printf("%s: %s -> %s\n", ifp->if_xname,
		    ieee80211_state_name[ic->ic_state],
		    ieee80211_state_name[IEEE80211_S_SCAN]);
	if ((sc->sc_flags & MWX_FLAG_BGSCAN) == 0) {
		ieee80211_set_link_state(ic, LINK_STATE_DOWN);
		ieee80211_node_cleanup(ic, ic->ic_bss);
	}
	ic->ic_state = IEEE80211_S_SCAN;

	return 0;
}

int
mwx_newstate(struct ieee80211com *ic, enum ieee80211_state nstate, int arg)
{
	struct mwx_softc *sc = (struct mwx_softc *)ic->ic_softc;

	/*
	 * Prevent attempts to transition towards the same state, unless
	 * we are scanning in which case a SCAN -> SCAN transition
	 * triggers another scan iteration. And AUTH -> AUTH is needed
	 * to support band-steering.
	 */
	if (sc->sc_ns_state == nstate && nstate != IEEE80211_S_SCAN &&
	    nstate != IEEE80211_S_AUTH)
		return 0;

	if (ic->ic_state == IEEE80211_S_RUN) {
		/* cancel other tasks here */
	}

	sc->sc_ns_state = nstate;
	sc->sc_ns_arg = arg;

	task_add(sc->sc_nswq, &sc->sc_newstate_task);
	return 0;
}

void
mwx_newstate_task(void *ptr)
{
	struct mwx_softc *sc = (struct mwx_softc *)ptr;
	struct ieee80211com *ic = &sc->sc_ic;
	enum ieee80211_state ostate = ic->ic_state;
	enum ieee80211_state nstate = sc->sc_ns_state;
	int arg = sc->sc_ns_arg;
	int s = splnet();
	int rv = 0;

	switch (ostate) {
	case IEEE80211_S_RUN:
		if (nstate != ostate) {
			/*
			 * Linux mt7921_mac_sta_remove: drop the BSS connection.
			 * Use the saved BSS: on a re-join net80211 has already
			 * pointed ic_bss at the new AP.
			 */
			if (sc->sc_bss_conf.valid) {
				int drv = mt7921_mcu_uni_add_bss(sc, 0);
				MWX_DEV_LOG(sc, "leave bss %02x:%02x:%02x:%02x:%02x:%02x returned %d\n",
				    sc->sc_bss_conf.bssid[0], sc->sc_bss_conf.bssid[1],
				    sc->sc_bss_conf.bssid[2], sc->sc_bss_conf.bssid[3],
				    sc->sc_bss_conf.bssid[4], sc->sc_bss_conf.bssid[5], drv);
				sc->sc_bss_conf.valid = 0;
			}
			mwx_link_reset(sc);
			mwx_mcu_set_deep_sleep(sc, 1);
		}
		break;
	case IEEE80211_S_SCAN:
		if (nstate == ostate) {
			if (sc->sc_flags & MWX_FLAG_SCANNING) {
				splx(s);
				return;
			}
		}
		break;
	default:
		break;
	}

	DPRINTF("%s: %s %d -> %d\n", DEVNAME(sc), __func__, ostate, nstate);
	if (ostate != nstate || mtk_verbose_scans > 0)
		MWX_DEV_LOG(sc, "newstate task %d -> %d arg=%d\n", ostate, nstate, arg);

	switch (nstate) {
	case IEEE80211_S_INIT:
		break;
	case IEEE80211_S_SCAN:
		/*
		 * The firmware runs the whole scan (hw scan); like OpenBSD don't
		 * hand SCAN to net80211, which would send its own probe request
		 * and redo the state entry work.
		 */
		rv = mwx_scan(sc);
		if (rv)
			break;
		splx(s);
		return;
	case IEEE80211_S_AUTH:
		/* AUTH -> AUTH is a retry or band steering: keep the setup. */
		if (ostate != IEEE80211_S_AUTH) {
			struct ieee80211_node *ni = ic->ic_bss;

			/*
			 * Tune to the AP's operating width before authenticating,
			 * like mac80211 (management frames still go out on the
			 * primary 20 MHz). The HE capability element sent in the
			 * association request depends on the band.
			 */
			mwx_he_band_caps(sc, IEEE80211_IS_CHAN_5GHZ(ni->ni_chan));
			mwx_bss_chandef(sc, ni, 0, &sc->sc_chan_ctrl,
			    &sc->sc_chan_center, &sc->sc_chan_bw);
			MWX_DEV_LOG(sc, "auth setup bss=%s chan=%u width=%s center=%u rssi=%d phy=%s ap=%s%s%s\n",
			    ether_sprintf(ni->ni_bssid),
			    ieee80211_chan2ieee(ic, ni->ni_chan),
			    mwx_cbw_name(sc->sc_chan_bw), sc->sc_chan_center,
			    ni->ni_rssi, mwx_phy_limit_name(sc->sc_phy_limit),
			    ieee80211_node_supports_ht(ni) ? "n" : "legacy",
			    ieee80211_node_supports_vht(ni) ? "+ac" : "",
			    (ni->ni_he_cap_elem.phy_cap_info[0] |
			    ni->ni_he_cap_elem.mac_cap_info[0]) ? "+ax" : "");
			rv = mt7921_set_channel(sc);
			if (rv) {
				MWX_DEV_LOG(sc, "auth set channel failed error=%d\n", rv);
				break;
			}
			mwx_mcu_set_deep_sleep(sc, 0);
			int srv = mt7921_mac_sta_update(sc, ni, 1, 1,
			    MWX_STA_STATE_NONE);
			MWX_DEV_LOG(sc, "auth sta record update returned %d wcid=%u\n",
			    srv, ((struct mwx_node *)ni)->wcid);
		}
		break;
	case IEEE80211_S_ASSOC:
		/*
		 * OpenBSD if_mwx.c 1.43: enabling deep sleep here breaks
		 * establishing the connection.
		 */
		break;
	case IEEE80211_S_RUN:
		if (ic->ic_opmode == IEEE80211_M_MONITOR)
			break;

		mt7921_mcu_hw_scan_cancel(sc);
		mwx_mcu_set_deep_sleep(sc, 0);
		if (ostate != IEEE80211_S_RUN) {
			/*
			 * Linux (mt7921_mac_sta_event ASSOC) gives the firmware the
			 * BSS (BSSID, beacon interval, DTIM, channel) and the AP's
			 * station record with our AID, and keeps the BSS awake.
			 * Without this only EAPOL and DTIM broadcasts got through:
			 * no beacons, no unicast data, so DHCP never completed.
			 */
			struct ieee80211_node *ni = ic->ic_bss;
			int brv, srv, prv, crv = 0;
			uint8_t ctrl, center, bw;

			/*
			 * net80211 negotiated HT/VHT/HE and the width on the
			 * association response; retune if that differs from what
			 * the beacon suggested at AUTH.
			 */
			mwx_bss_chandef(sc, ni, 1, &ctrl, &center, &bw);
			if (ctrl != sc->sc_chan_ctrl ||
			    center != sc->sc_chan_center ||
			    bw != sc->sc_chan_bw) {
				sc->sc_chan_ctrl = ctrl;
				sc->sc_chan_center = center;
				sc->sc_chan_bw = bw;
				crv = mt7921_set_channel(sc);
			}
			MWX_DEV_LOG(sc, "assoc negotiated mode=%s%s%s width=%s center=%u chw=%u nss=%u htcaps=0x%04x ampdu=0x%02x vhtcaps=0x%08x vhtmcs=0x%04x/0x%04x hemcs=0x%04x heop=0x%08x retune=%d\n",
			    (ni->ni_flags & IEEE80211_NODE_HT) ? "n" : "legacy",
			    (ni->ni_flags & IEEE80211_NODE_VHT) ? "+ac" : "",
			    (ni->ni_flags & IEEE80211_NODE_HE) ? "+ax" : "",
			    mwx_cbw_name(bw), center, ni->ni_chw, ni->ni_rx_nss,
			    ni->ni_htcaps, ni->ni_ampdu_param, ni->ni_vhtcaps,
			    ni->ni_vht_mcsinfo.rx_mcs_map,
			    ni->ni_vht_mcsinfo.tx_mcs_map,
			    le16toh(ni->ni_he_mcs_nss_supp.rx_mcs_80),
			    ni->ni_he_oper_params, crv);

			mt7921_bss_conf_from_node(sc, ni);
			brv = mt7921_mcu_uni_add_bss(sc, 1);
			mwx_mac_wtbl_update(sc, ((struct mwx_node *)ni)->wcid);
			srv = mt7921_mac_sta_update(sc, ni, 1, 0,
			    MWX_STA_STATE_ASSOC);
			if (srv == 0)
				srv = mt7921_mac_sta_update(sc, NULL, 1, 0,
				    MWX_STA_STATE_ASSOC);
			prv = mt7921_mcu_uni_bss_ps(sc, 0);
			if (prv == 0)
				prv = mt7921_mcu_uni_bss_bcnft(sc);
			if (prv == 0)
				prv = mt7921_mcu_set_tx(sc, &sc->sc_vif);
			MWX_DEV_LOG(sc, "assoc setup bss=%s aid=%u intval=%u dtim=%u: bss=%d sta=%d ps=%d\n",
			    ether_sprintf(ni->ni_bssid),
			    IEEE80211_AID(ni->ni_associd), ni->ni_intval,
			    ni->ni_dtimperiod, brv, srv, prv);
		}
		mt7921_mcu_set_rts_thresh(sc, 0x92b, 0);
		break;
	}

	if (rv)
		mwx_reset(sc);
	else
		sc->sc_newstate(ic, nstate, arg);
	splx(s);
}

int
mwx_bgscan(struct ieee80211com *ic)
{
	struct mwx_softc *sc = (struct mwx_softc *)ic->ic_softc;
	int err;

	if (sc->sc_flags & MWX_FLAG_SCANNING)
		return 0;

	err = mt7921_mcu_hw_scan(sc, 1);
	if (err) {
		printf("%s: could not initiate scan\n", DEVNAME(sc));
		return err;
	}

	sc->sc_flags |= MWX_FLAG_BGSCAN;
	return 0;
}

void
mwx_bgscan_done(struct ieee80211com *ic,
    struct ieee80211_node_switch_bss_arg *arg, size_t arg_size)
{
	struct mwx_softc *sc = (struct mwx_softc *)ic->ic_softc;

	free(sc->sc_bgscan_arg, M_DEVBUF, sc->sc_bgscan_arg_size);
	sc->sc_bgscan_arg = arg;
	sc->sc_bgscan_arg_size = arg_size;
	task_add(sc->sc_nswq, &sc->sc_bgscan_done_task);
}

void
mwx_bgscan_done_task(void *arg)
{
	struct mwx_softc *sc = (struct mwx_softc *)arg;
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_node *ni = ic->ic_bss;
	int err = 0;
	int s = splnet();

	if (ic->ic_state != IEEE80211_S_RUN ||
	    (ic->ic_flags & IEEE80211_F_BGSCAN) == 0) {
		err = ENXIO;
		goto done;
	}

	/*
	 * Remove installed crypto keys while we still have access to them.
	 * Once mwx_newstate() is entered ic_bss will already contain
	 * information about our next AP.
	 */
	if (ic->ic_flags & IEEE80211_F_RSNON) {
		struct ieee80211_key *k;

		if (ni->ni_pairwise_key.k_cipher != IEEE80211_CIPHER_NONE)
			(*ic->ic_delete_key)(ic, ni, &ni->ni_pairwise_key);

		if (ic->ic_def_txkey == 1 || ic->ic_def_txkey == 2) {
			k = &ic->ic_nw_keys[ic->ic_def_txkey];
			if ((k->k_flags & IEEE80211_KEY_GROUP) &&
			    k->k_cipher == IEEE80211_CIPHER_CCMP)
				(*ic->ic_delete_key)(ic, ni, k);
		}

		if (ic->ic_igtk_kid == 4 || ic->ic_igtk_kid == 5) {
			k = &ic->ic_nw_keys[ic->ic_igtk_kid];
			if (k->k_flags & IEEE80211_KEY_IGTK)
				(*ic->ic_delete_key)(ic, ni, k);
		}

		ni->ni_port_valid = 0;
		ni->ni_flags &= ~IEEE80211_NODE_TXRXPROT;
		ni->ni_flags &= ~IEEE80211_NODE_TXMGMTPROT;
		ni->ni_flags &= ~IEEE80211_NODE_RXMGMTPROT;
		ni->ni_rsn_supp_state = RSNA_SUPP_INITIALIZE;
	}

	/*
	 * The local net80211 port switches BSS from ni_unref_cb when the
	 * current node's final reference is released. Install the same
	 * callback after the driver has removed hardware keys.
	 */
	ni->ni_unref_arg = sc->sc_bgscan_arg;
	ni->ni_unref_arg_size = sc->sc_bgscan_arg_size;
	ni->ni_unref_cb = ieee80211_node_switch_bss;
	sc->sc_bgscan_arg = NULL;
	sc->sc_bgscan_arg_size = 0;
	if (ni->ni_flags & IEEE80211_NODE_MFP)
		ieee80211_node_switch_bss(ic, ni);

 done:
	if (err) {
		free(sc->sc_bgscan_arg, M_DEVBUF, sc->sc_bgscan_arg_size);
		sc->sc_bgscan_arg = NULL;
		sc->sc_bgscan_arg_size = 0;
	}
	splx(s);
}

int
mwx_set_key(struct ieee80211com *ic, struct ieee80211_node *ni,
    struct ieee80211_key *k)
{
	struct mwx_softc *sc = (struct mwx_softc *)ic->ic_softc;
	struct mwx_setkey_task_arg *a;

	DPRINTF("%s: set_key: ni %p, k_id %d, k_flags %x k_cipher %d\n",
	    DEVNAME(sc), ni, k->k_id, k->k_flags, k->k_cipher);

	if ((sc->sc_setkey_cur + 1) % nitems(sc->sc_setkey_arg) ==
	    sc->sc_setkey_tail)
		return ENOSPC;

	a = &sc->sc_setkey_arg[sc->sc_setkey_cur];
	a->ni = ni;
	a->k = k;
	sc->sc_setkey_cur = (sc->sc_setkey_cur + 1) % nitems(sc->sc_setkey_arg);
	task_add(sc->sc_nswq, &sc->sc_setkey_task);
	return EBUSY;
}

void
mwx_delete_key(struct ieee80211com *ic, struct ieee80211_node *ni,
    struct ieee80211_key *k)
{
	struct mwx_softc *sc = (struct mwx_softc *)ic->ic_softc;

	DPRINTF("%s: delete_key: ni %p, k_id %d, k_flags %x k_cipher %d\n",
	    DEVNAME(sc), ni, k->k_id, k->k_flags, k->k_cipher);

	mt7921_mcu_sta_key_delete(sc, ni, k);
}

void
mwx_setkey_task(void *arg)
{
	struct mwx_softc *sc = (struct mwx_softc *)arg;
	struct mwx_setkey_task_arg *a;
	int s = splnet();

	while (sc->sc_setkey_tail != sc->sc_setkey_cur) {
		a = &sc->sc_setkey_arg[sc->sc_setkey_tail];
		mt7921_mcu_sta_key_update(sc, a->ni, a->k);
		a->ni = NULL;
		a->k = NULL;

		sc->sc_setkey_tail = (sc->sc_setkey_tail + 1) %
		    nitems(sc->sc_setkey_arg);
	}

	splx(s);
}

#if NBPFILTER > 0
void
mwx_radiotap_attach(struct mwx_softc *sc)
{
	bpfattach(&sc->sc_drvbpf, &sc->sc_ic.ic_if, DLT_IEEE802_11_RADIO,
	    sizeof(struct ieee80211_frame) + IEEE80211_RADIOTAP_HDRLEN);

	sc->sc_rxtap_len = sizeof sc->sc_rxtapu;
	sc->sc_rxtap.wr_ihdr.it_len = htole16(sc->sc_rxtap_len);
	sc->sc_rxtap.wr_ihdr.it_present = htole32(MWX_RX_RADIOTAP_PRESENT);

	sc->sc_txtap_len = sizeof sc->sc_txtapu;
	sc->sc_txtap.wt_ihdr.it_len = htole16(sc->sc_txtap_len);
	sc->sc_txtap.wt_ihdr.it_present = htole32(MWX_TX_RADIOTAP_PRESENT);
}
#endif

int
mwx_tx(struct mwx_softc *sc, struct mbuf *m, struct ieee80211_node *ni)
{
	struct mwx_node *mn = (struct mwx_node *)ni;
	struct mwx_txwi *mt;
	struct mt76_txwi *txp;
	int rv;
#ifdef AIRPORT_WCL
	struct ieee80211_frame *wcl_wh;
	bool wcl_data;
	unsigned wcl_ac;
#endif

	/*
	 * On success the txwi owns m and the node reference until the TX free
	 * event puts it back. On failure m is freed here and the caller
	 * releases the node. The txwi free list and the TX ring are shared
	 * with the interrupt path, so hold sc_txlock.
	 */
	lck_mtx_lock(sc->sc_txlock);
	if ((mt = mwx_txwi_get(sc)) == NULL) {
		rv = ENOBUFS;
		goto fail;
	}
	/* check if there is enough space (OpenBSD if_mwx.c 1.41) */
	if (mwx_dma_free_slots(&sc->sc_txq) < 1) {
		rv = EBUSY;
		goto fail_put;
	}
	/* XXX DMA memory access without BUS_DMASYNC_PREWRITE */
	txp = mt->mt_desc;
	memset(txp, 0, sizeof(*txp));
	mt7921_mac_write_txwi(sc, m, ni, txp);

#ifdef AIRPORT_WCL
	/* Per access category data counters for macOS's link quality. */
	wcl_wh = mtod(m, struct ieee80211_frame *);
	wcl_data = (wcl_wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK) == IEEE80211_FC0_TYPE_DATA &&
	    !(wcl_wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_NODATA);
	wcl_ac = ieee80211_has_qos(wcl_wh) ?
	    ieee80211_wcl_ac(ieee80211_get_qos(wcl_wh) & IEEE80211_QOS_TID) : EDCA_AC_BE;
#endif
	rv = mwx_txwi_enqueue(sc, mt, m, ni);
	if (rv != 0)
		goto fail_put;
#ifdef AIRPORT_WCL
	if (wcl_data)
		++sc->sc_ic.ic_wcl_tx_packets[wcl_ac];
#endif

	static uint32_t tx_log_count;
	if (tx_log_count < 32) {
		MWX_DEV_LOG(sc, "tx frame wcid=%u pid=%d len=%zu\n",
		    mn->wcid, mt->mt_idx, mbuf_pkthdr_len(m));
		MWX_DEV_LOG(sc,
		    "txwi %08x %08x %08x %08x %08x %08x %08x %08x\n",
		    txp->txwi[0], txp->txwi[1], txp->txwi[2], txp->txwi[3],
		    txp->txwi[4], txp->txwi[5], txp->txwi[6], txp->txwi[7]);
		MWX_DEV_LOG(sc,
		    "tx desc msdu=%u/%u/%u/%u len=%u/%u/%u/%u\n",
		    txp->msdu_id[0], txp->msdu_id[1], txp->msdu_id[2],
		    txp->msdu_id[3], txp->ptr[0].len0, txp->ptr[0].len1,
		    txp->ptr[1].len0, txp->ptr[1].len1);
		tx_log_count++;
	}

	mwx_dma_txwi_enqueue(sc, &sc->sc_txq, mt);
	lck_mtx_unlock(sc->sc_txlock);
	return 0;

fail_put:
	mwx_txwi_put(sc, mt);
fail:
	lck_mtx_unlock(sc->sc_txlock);
	m_freem(m);
	return rv;
}

/* Reassemble hardware-split A-MSDUs before net80211's sequence reordering. */
struct mbuf *
mwx_rx_amsdu(struct mwx_softc *sc, struct mbuf *m, struct ieee80211_rxinfo *rxi)
{
	struct ieee80211_frame *wh = mtod(m, struct ieee80211_frame *);
	uint8_t format = sc->sc_last_rxd4 & MT_RXD4_NORMAL_PAYLOAD_FORMAT;
	struct mbuf *head = sc->sc_amsdu_head;
	struct ether_header *eh;
	size_t hdrlen, sublen, len, padding;
	uint16_t seq;
	uint8_t tid;
	static const uint8_t zeros[3] = { 0, 0, 0 };

	if (format == 0) {
		if (head != NULL) {
			m_freem(head);
			sc->sc_amsdu_head = NULL;
			sc->sc_amsdu_partial++;
		}
		return m;
	}
	hdrlen = ieee80211_get_hdrlen(wh);
	if (!ieee80211_has_qos(wh) ||
	    mbuf_len(m) < hdrlen + ETHER_HDR_LEN)
		goto drop;
	seq = le16dec(wh->i_seq);
	tid = ieee80211_get_qos(wh) & IEEE80211_QOS_TID;
	eh = (struct ether_header *)(mtod(m, uint8_t *) + hdrlen);
	sublen = ETHER_HDR_LEN + ntohs(eh->ether_type);
	if (sublen < ETHER_HDR_LEN + 8 /* LLC + SNAP */ ||
	    sublen > mbuf_pkthdr_len(m) - hdrlen || sublen > 3839)
		goto drop;
	/* The descriptor may include trailing hardware padding. */
	m_adj(m, -(int)(mbuf_pkthdr_len(m) - hdrlen - sublen));
	if (format == MT_RXD4_FIRST_AMSDU_FRAME) {
		uint8_t *qos = ieee80211_has_addr4(wh) ?
		    ((struct ieee80211_qosframe_addr4 *)wh)->i_qos :
		    ((struct ieee80211_qosframe *)wh)->i_qos;

		qos[0] |= IEEE80211_QOS_AMSDU;
		if (head != NULL) {
			m_freem(head);
			sc->sc_amsdu_partial++;
		}
		sc->sc_amsdu_head = m;
		sc->sc_amsdu_rxi = *rxi;
		sc->sc_amsdu_seq = seq;
		sc->sc_amsdu_tid = tid;
		return NULL;
	}
	if (head == NULL || seq != sc->sc_amsdu_seq || tid != sc->sc_amsdu_tid ||
	    !IEEE80211_ADDR_EQ(wh->i_addr2, mtod(head, struct ieee80211_frame *)->i_addr2) ||
	    hdrlen != (size_t)ieee80211_get_hdrlen(mtod(head, struct ieee80211_frame *)) ||
	    rxi->rxi_flags != sc->sc_amsdu_rxi.rxi_flags)
		goto drop;
	len = mbuf_pkthdr_len(head);
	padding = (4 - ((len - hdrlen) & 3)) & 3;
	if (len + padding + sublen > hdrlen + 3839 ||
	    (padding != 0 && mbuf_copyback(head, len, padding, zeros, MBUF_DONTWAIT) != 0))
		goto drop;
	m_adj(m, hdrlen);
	m_cat(head, m);
	mbuf_pkthdr_setlen(head, len + padding + sublen);
	if (format != MT_RXD4_LAST_AMSDU_FRAME)
		return NULL;
	sc->sc_amsdu_head = NULL;
	sc->sc_amsdu_frames++;
	*rxi = sc->sc_amsdu_rxi;
	return head;

drop:
	if (sc->sc_amsdu_partial++ < 8)
		MWX_DEV_LOG(sc, "rx incomplete A-MSDU format=%u len=%zu\n",
		    format, mbuf_pkthdr_len(m));
	m_freem(head);
	m_freem(m);
	sc->sc_amsdu_head = NULL;
	return NULL;
}

void
mwx_rx(struct mwx_softc *sc, struct mbuf *m, struct mbuf_list *ml)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &ic->ic_if;
	struct ieee80211_node *ni;
	struct ieee80211_frame *wh;
	struct ieee80211_rxinfo rxi = { 0 };
	int rv;

	rv = mt7921_mac_fill_rx(sc, m, &rxi);
	if (rv != 0) {
		if (sc->sc_rx_fill_fail < 32) {
			MWX_DEV_LOG(sc,
			    "rx fill failed rv=%d len=%zu fail_count=%u\n",
			    rv, mbuf_len(m), sc->sc_rx_fill_fail + 1);
		}
		sc->sc_rx_fill_fail++;
		ifp->if_ierrors++;
		m_freem(m);
		return;
	}

	m = mwx_rx_amsdu(sc, m, &rxi);
	if (m == NULL)
		return;
	wh = mtod(m, struct ieee80211_frame *);
	if ((wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK) == IEEE80211_FC0_TYPE_DATA &&
	    ic->ic_bss != NULL && ic->ic_bss->ni_port_valid) {
		if (sc->sc_rx_data_frames++ == 0 && sc->sc_link_up_ns != 0)
			MWX_DEV_LOG(sc, "link first RX data after %llu ms\n",
			    (unsigned long long)((mwx_uptime_ns() - sc->sc_link_up_ns) / 1000000));
	}
	if (sc->sc_rx_delivered < 32) {
		uint8_t type = wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK;
		uint8_t subtype = wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK;

		MWX_DEV_LOG(sc,
		    "rx frame delivered type=0x%02x subtype=0x%02x chan=%u rssi=%d len=%zu\n",
		    type, subtype, rxi.rxi_chan, rxi.rxi_rssi, mbuf_len(m));
	}
	{
		static uint32_t rx_run_log_count;
		uint8_t type = wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK;
		uint8_t subtype = wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK;

		if (rx_run_log_count < 96 &&
		    (type == IEEE80211_FC0_TYPE_DATA ||
		    (ic->ic_state == IEEE80211_S_RUN &&
		    (rxi.rxi_flags & IEEE80211_RXI_HWDEC)))) {
			uint32_t d1 = sc->sc_last_rxd1, d3 = sc->sc_last_rxd3;
			int hl = ieee80211_get_hdrlen(wh), kid = -1;

			if ((wh->i_fc[1] & IEEE80211_FC1_PROTECTED) &&
			    mbuf_len(m) >= (size_t)hl + 4)
				kid = (mtod(m, uint8_t *)[hl + 3] >> 6) & 3;
			/*
			 * widx: WTBL entry the hardware matched; sec/cm/clm: cipher
			 * mode and cipher(-length) mismatch; u2m: addressed to us.
			 */
			MWX_DEV_LOG(sc,
			    "rx run frame state=%d fc=%02x/%02x rxi_flags=0x%x rssi=%d len=%zu widx=%u sec=%u cm=%d clm=%d u2m=%d keyid=%d a1=%02x:%02x:%02x:%02x:%02x:%02x a2=%02x:%02x:%02x:%02x:%02x:%02x\n",
			    ic->ic_state, wh->i_fc[0], wh->i_fc[1],
			    rxi.rxi_flags, rxi.rxi_rssi, mbuf_len(m),
			    d1 & MT_RXD1_NORMAL_WLAN_IDX_MASK,
			    (d1 & MT_RXD1_NORMAL_SEC_MODE_MASK) >>
			    MT_RXD1_NORMAL_SEC_MODE_SHIFT,
			    (d1 & MT_RXD1_NORMAL_CM) != 0,
			    (d1 & MT_RXD1_NORMAL_CLM) != 0,
			    (d3 & MT_RXD3_NORMAL_ADDR_TYPE_MASK) ==
			    MT_RXD3_NORMAL_U2M, kid,
			    wh->i_addr1[0], wh->i_addr1[1], wh->i_addr1[2],
			    wh->i_addr1[3], wh->i_addr1[4], wh->i_addr1[5],
			    wh->i_addr2[0], wh->i_addr2[1], wh->i_addr2[2],
			    wh->i_addr2[3], wh->i_addr2[4], wh->i_addr2[5]);
			rx_run_log_count++;
		}
	}
	sc->sc_rx_delivered++;

#if NBPFILTER > 0
	if (__predict_false(sc->sc_drvbpf != NULL)) {
		struct mwx_rx_radiotap_header *tap = &sc->sc_rxtap;
		uint32_t tsf_lo, tsf_hi;
		/* get timestamp (low and high 32 bits) */
		tsf_hi = 0;
		tsf_lo = 0;
		tap->wr_tsft = htole64(((uint64_t)tsf_hi << 32) | tsf_lo);
		tap->wr_flags = 0;
		tap->wr_rate = 2;	/* XXX */
		tap->wr_chan_freq =
		    htole16(ic->ic_channels[rxi.rxi_chan].ic_freq);
		tap->wr_chan_flags =
		    ic->ic_channels[rxi.rxi_chan].ic_flags;
		tap->wr_dbm_antsignal = rxi.rxi_rssi;
		bpf_mtap_hdr(sc->sc_drvbpf, tap, sc->sc_rxtap_len, m,
		    BPF_DIRECTION_IN);
	}
#endif

	/* grab a reference to the source node */
	ni = ieee80211_find_rxnode(ic, wh);

	if (ic->ic_max_rssi && rxi.rxi_rssi < 0)
		rxi.rxi_rssi = mwx_dbm_to_rssi(rxi.rxi_rssi);

	/* send the frame to the 802.11 layer */
	ieee80211_inputm(ifp, m, ni, &rxi, ml);

	/* node is no longer needed */
	ieee80211_release_node(ic, ni);
}

int
mwx_count_nodes(struct ieee80211com *ic)
{
	struct ieee80211_node *ni;
	int count = 0;

	RB_FOREACH(ni, ieee80211_tree, &ic->ic_tree)
		count++;
	return count;
}

/*
 * Driver specific functions.
 */
int
mwx_intr_filter(void *arg)
{
	struct mwx_softc *sc = (struct mwx_softc *)arg;

	mwx_write(sc, MT_WFDMA0_HOST_INT_ENA, 0);
	return 1;
}

int
mwx_intr(void *arg)
{
	struct mwx_softc *sc = (struct mwx_softc *)arg;
	uint32_t mask = sc->sc_intr_mask;
	int rv;

	/* also called from mwx_mcu_wait_done() on the work loop thread */
	if (sc->sc_in_intr)
		return 0;
	sc->sc_in_intr = 1;
	rv = mwx_intr_locked(sc, mask);
	sc->sc_in_intr = 0;
	return rv;
}

int
mwx_intr_locked(struct mwx_softc *sc, uint32_t mask)
{
	uint32_t intr, intr_sw;
	static uint32_t intr_log_count;
	static uint32_t intr_unhandled_log_count;

	sc->sc_isr_phase = 1;
	mwx_write(sc, MT_WFDMA0_HOST_INT_ENA, 0);
	intr = mwx_read(sc, MT_WFDMA0_HOST_INT_STA);
	if (intr == 0) {
		mwx_write(sc, MT_WFDMA0_HOST_INT_ENA, mask);
		sc->sc_isr_phase = 0;
		return 0;
	}
	sc->sc_intr_count++;

	/* TODO power management */
//	mt76_connac_pm_ref(&dev->mphy, &dev->pm);

	if (intr_log_count < 16) {
		MWX_DEV_LOG(sc, "interrupt status=0x%08x mask=0x%08x\n",
		    intr, mask);
		intr_log_count++;
	}

	if (intr & ~mask && intr_unhandled_log_count < 16) {
		printf("%s: unhandled interrupt %08x\n", DEVNAME(sc),
		    intr & ~mask);
		intr_unhandled_log_count++;
	}
	/* ack interrupts */
	intr &= mask;
	mwx_write(sc, MT_WFDMA0_HOST_INT_STA, intr);

	if (intr & MT_INT_MCU_CMD) {
		intr_sw = mwx_read(sc, MT_MCU_CMD);
		if (intr_log_count < 16)
			MWX_DEV_LOG(sc, "mcu interrupt status=0x%08x\n", intr_sw);
		/* ack MCU2HOST_SW_INT_STA */
		mwx_write(sc, MT_MCU_CMD, intr_sw);
		if (intr_sw & MT_MCU_CMD_WAKE_RX_PCIE)
			intr |= MT_INT_RX_DONE_DATA;
	}

	sc->sc_isr_phase = 2;
	if (intr & MT_INT_TX_DONE_ALL)
		mwx_dma_tx_done(sc);

	sc->sc_isr_phase = 3;
	if (intr & MT_INT_RX_DONE_WM)
		mwx_dma_rx_done(sc, &sc->sc_rxfwdlq);
	sc->sc_isr_phase = 4;
	if (intr & MT_INT_RX_DONE_WM2)
		mwx_dma_rx_done(sc, &sc->sc_rxmcuq);
	sc->sc_isr_phase = 5;
	if (intr & MT_INT_RX_DONE_DATA)
		mwx_dma_rx_done(sc, &sc->sc_rxq);
	sc->sc_isr_phase = 6;

	/* TODO power management */
//	mt76_connac_pm_unref(&dev->mphy, &dev->pm);

	mwx_write(sc, MT_WFDMA0_HOST_INT_ENA, mask);
	sc->sc_isr_phase = 0;

	return 1;
}

int
mwx_preinit(struct mwx_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &ic->ic_if;
	int rv, i;
	uint8_t chan;

	MWX_DEV_LOG(sc, "preinit start fw_loaded=%d\n", sc->sc_fw_loaded);

	for (i = 1; ; i++) {
		if ((rv = mwx_init_hardware(sc)) == 0)
			break;
		MWX_DEV_LOG(sc, "preinit hardware init attempt %d/%d failed error=%d\n",
		    i, MWX_INIT_RETRY_COUNT, rv);
		if (rv != EAGAIN || i == MWX_INIT_RETRY_COUNT)
			return rv;
	}
	MWX_DEV_LOG(sc, "preinit hardware init ok\n");

	if ((rv = mwx_mcu_set_deep_sleep(sc, 1)) != 0) {
		MWX_DEV_LOG(sc, "preinit deep sleep enable failed error=%d\n", rv);
		return rv;
	}
	MWX_DEV_LOG(sc, "preinit deep sleep enabled\n");

	ic->ic_sup_rates[IEEE80211_MODE_11B] = ieee80211_std_rateset_11b;
	ic->ic_sup_rates[IEEE80211_MODE_11G] = ieee80211_std_rateset_11g;
	MWX_DEV_LOG(sc, "preinit capability 2ghz=%d 5ghz=%d 6ghz=%d streams=%u antenna_mask=0x%x\n",
	    sc->sc_capa.has_2ghz, sc->sc_capa.has_5ghz,
	    sc->sc_capa.has_6ghz, sc->sc_capa.num_streams,
	    sc->sc_capa.antenna_mask);

	if (sc->sc_capa.has_2ghz) {
		for (i = 0; i < MWX_NUM_2GHZ_CHANNELS; i++) {
			chan = mwx_channels_2ghz[i];
			ic->ic_channels[chan].ic_freq =
			    ieee80211_ieee2mhz(chan, IEEE80211_CHAN_2GHZ);
			ic->ic_channels[chan].ic_flags =
			    IEEE80211_CHAN_CCK | IEEE80211_CHAN_OFDM |
			    IEEE80211_CHAN_DYN | IEEE80211_CHAN_2GHZ;
			ic->ic_channels[chan].ic_flags |=
			    mwx_chan_ht_flags(chan, 0);
		}

	}
	if (sc->sc_capa.has_5ghz) {
		ic->ic_sup_rates[IEEE80211_MODE_11A] =
		    ieee80211_std_rateset_11a;
		/* set supported .11a channels */
		for (i = 0; i < MWX_NUM_5GHZ_CHANNELS; i++) {
			chan = mwx_channels_5ghz[i];
			ic->ic_channels[chan].ic_freq =
			    ieee80211_ieee2mhz(chan, IEEE80211_CHAN_5GHZ);
			ic->ic_channels[chan].ic_flags = IEEE80211_CHAN_A |
			    mwx_chan_ht_flags(chan, 1);
		}
	}
#ifdef NOTYET
	/* TODO support for 6GHz */
	if (sc->sc_capa.has_6ghz) {
		for (i = 0; i < MWX_NUM_6GHZ_CHANNELS; i++) {
		}
	}
#endif

	/* Configure channel information obtained from firmware. */
	ieee80211_channel_init(ifp);
	MWX_DEV_LOG(sc, "preinit channel table ready\n");

	if (IEEE80211_ADDR_EQ(etheranyaddr, sc->sc_ic.ic_myaddr))
		IEEE80211_ADDR_COPY(ic->ic_myaddr, sc->sc_lladdr);

	/* Configure MAC address. */
	rv = if_setlladdr(ifp, ic->ic_myaddr);
	if (rv)
		printf("%s: could not set MAC address (error %d)\n",
		    DEVNAME(sc), rv);
	else
		MWX_DEV_LOG(sc, "preinit mac address=%s\n",
		    ether_sprintf(ic->ic_myaddr));

	ieee80211_media_init(ifp);
	/* stream count is known now; media_init also reset the HT switch */
	mwx_setup_phy_caps(sc);
	MWX_DEV_LOG(sc, "preinit phy %s: streams=%u htcaps=0x%04x vhtcaps=0x%08x modecaps=0x%x flags=0x%x\n",
	    mwx_phy_limit_name(sc->sc_phy_limit), sc->sc_capa.num_streams,
	    ic->ic_htcaps, ic->ic_vhtcaps, ic->ic_modecaps, ic->ic_flags);

	sc->sc_fw_loaded = 1;
	MWX_DEV_LOG(sc, "preinit complete\n");
	return 0;
}

void
mwx_attach_hook(struct device *self)
{
	struct mwx_softc *sc = (struct mwx_softc *)self;

	MWX_DEV_LOG(sc, "attach hook start\n");
	mwx_preinit(sc);
	MWX_DEV_LOG(sc, "attach hook done fw_loaded=%d\n", sc->sc_fw_loaded);
}

/* Offset of PCIe extended capability id, or 0. */
static int
mwx_pcie_find_ext_cap(IOPCIDevice *dev, uint16_t id)
{
	int off = 0x100, guard = 0;

	while (off != 0 && guard++ < 48) {
		uint32_t h = dev->extendedConfigRead32(off);

		if (h == 0 || h == 0xffffffff)
			return 0;
		if ((h & 0xffff) == id)
			return off;
		off = (h >> 20) & 0xffc;
	}
	return 0;
}

#define	MWX_PCIE_LNKCTL		0x10	/* in the PCIe capability */
#define	MWX_PCIE_LNKCTL_ASPM	0x0003	/* L0s | L1 */
#define	MWX_PCIE_EXT_L1SS	0x001e
#define	MWX_PCIE_L1SS_CTL1	0x08
#define	MWX_PCIE_L1SS_ENABLES	0x0000000f	/* PCI-PM L1.2/L1.1, ASPM L1.2/L1.1 */

static void
mwx_pcie_aspm_off(struct mwx_softc *sc, IOPCIDevice *dev, const char *who,
    int step)
{
	uint8_t pcie = 0;
	int l1ss;

	if (dev == NULL)
		return;
	if (step == 0) {
		/* L1 substates first, like Linux pci_disable_link_state() */
		l1ss = mwx_pcie_find_ext_cap(dev, MWX_PCIE_EXT_L1SS);
		if (l1ss != 0) {
			uint32_t c = dev->extendedConfigRead32(l1ss +
			    MWX_PCIE_L1SS_CTL1);
			dev->extendedConfigWrite32(l1ss + MWX_PCIE_L1SS_CTL1,
			    c & ~MWX_PCIE_L1SS_ENABLES);
			MWX_DEV_LOG(sc, "aspm %s: L1SS ctl1 0x%08x -> 0x%08x\n", who,
			    c, dev->extendedConfigRead32(l1ss + MWX_PCIE_L1SS_CTL1));
		}
		return;
	}
	if (dev->findPCICapability(0x10 /* PCI Express */, &pcie) == 0 ||
	    pcie == 0) {
		MWX_DEV_LOG(sc, "aspm %s: no PCIe capability\n", who);
		return;
	}
	uint16_t lc = dev->extendedConfigRead16(pcie + MWX_PCIE_LNKCTL);
	dev->extendedConfigWrite16(pcie + MWX_PCIE_LNKCTL,
	    lc & ~MWX_PCIE_LNKCTL_ASPM);
	MWX_DEV_LOG(sc, "aspm %s: lnkctl 0x%04x -> 0x%04x\n", who, lc,
	    dev->extendedConfigRead16(pcie + MWX_PCIE_LNKCTL));
}

/*
 * Linux mt76_pci_disable_aspm() (mt7921e disable_aspm=1): turn off ASPM on the
 * card and its upstream bridge, device first. With L1/L1.2 active the card's
 * DMA stalled for good ~1 s after association (all rings frozen, no
 * interrupts, firmware silent), right when traffic first went idle.
 */
static void
mwx_pcie_disable_aspm(struct mwx_softc *sc, IOPCIDevice *dev)
{
	IOService *bridge = dev->getProvider();
	IOPCIDevice *up = OSDynamicCast(IOPCIDevice,
	    bridge != NULL ? bridge->getProvider() : NULL);

	dev->setProperty("pci-aspm-default", 0ULL, 32);
	mwx_pcie_aspm_off(sc, dev, "card", 0);
	mwx_pcie_aspm_off(sc, up, "bridge", 0);
	mwx_pcie_aspm_off(sc, dev, "card", 1);
	mwx_pcie_aspm_off(sc, up, "bridge", 1);
}

static const char *
mwx_phy_limit_name(int limit)
{
	switch (limit) {
	case MWX_PHY_LIMIT_LEGACY:
		return "legacy";
	case MWX_PHY_LIMIT_HT:
		return "n";
	case MWX_PHY_LIMIT_VHT:
		return "ac";
	default:
		return "ax";
	}
}

/* mtkphy=a|n|ac|ax caps the PHY mode offered to access points. */
int
mwx_parse_phy_limit(const char *s)
{
	if (!strcmp(s, "a") || !strcmp(s, "g") || !strcmp(s, "legacy"))
		return MWX_PHY_LIMIT_LEGACY;
	if (!strcmp(s, "n"))
		return MWX_PHY_LIMIT_HT;
	if (!strcmp(s, "ac"))
		return MWX_PHY_LIMIT_VHT;
	if (!strcmp(s, "ax"))
		return MWX_PHY_LIMIT_HE;
	return -1;
}

/*
 * HE capability bits that depend on the band of the AP being joined
 * (Linux builds one HE capability set per band).
 */
void
mwx_he_band_caps(struct mwx_softc *sc, int is5g)
{
	uint8_t *phy0 = &sc->sc_ic.ic_he_cap_elem.phy_cap_info[0];

	if (is5g)
		*phy0 = IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_40MHZ_80MHZ_IN_5G |
		    IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_RU_MAPPING_IN_5G;
	else
		*phy0 = IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_40MHZ_IN_2G |
		    IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_RU_MAPPING_IN_2G;
}

/*
 * HT/VHT/HE capabilities of a station, from Linux mt7921
 * (mt76_init_sband/mt76_init_stream_cap, mt7921_init_he_caps). Differences:
 * maximum MPDU/A-MSDU stay at 3895/3839 bytes because RX buffers are 2 KB
 * (only A-MSDU subframes, which the hardware splits, may be longer), no
 * 160 MHz, no HE PPE thresholds (nominal 16 us padding instead) and no
 * A-MSDU in A-MPDU. Called again once the firmware reported the streams.
 */
void
mwx_setup_phy_caps(struct mwx_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_he_cap_elem *he = &ic->ic_he_cap_elem;
	int nss = sc->sc_capa.num_streams ? sc->sc_capa.num_streams : 2;
	uint16_t vht_map = 0, he_map = 0;
	int i;

	if (nss > 2)
		nss = 2;

	ic->ic_htcaps = IEEE80211_HTCAP_LDPC | IEEE80211_HTCAP_CBW20_40 |
	    IEEE80211_HTCAP_SGI20 | IEEE80211_HTCAP_SGI40 |
	    (IEEE80211_HTCAP_SMPS_DIS << IEEE80211_HTCAP_SMPS_SHIFT) |
	    (1 << IEEE80211_HTCAP_RXSTBC_SHIFT);
	if (nss > 1)
		ic->ic_htcaps |= IEEE80211_HTCAP_TXSTBC;
	ic->ic_htxcaps = 0;
	ic->ic_txbfcaps = 0;
	ic->ic_aselcaps = 0;
	ic->ic_ampdu_params = (IEEE80211_AMPDU_PARAM_SS_4 | 0x3 /* 64k */);
	memset(ic->ic_sup_mcs, 0, sizeof(ic->ic_sup_mcs));
	for (i = 0; i < nss; i++)
		ic->ic_sup_mcs[i] = 0xff;	/* MCS 0-7 per stream */
	ic->ic_tx_mcs_set = IEEE80211_TX_MCS_SET_DEFINED;
	ic->ic_max_rxrate = 0;

	ic->ic_vhtcaps = IEEE80211_VHTCAP_MAX_MPDU_LENGTH_3895 |
	    IEEE80211_VHTCAP_RXLDPC | IEEE80211_VHTCAP_SHORT_GI_80 |
	    IEEE80211_VHTCAP_RXSTBC_1 |
	    IEEE80211_VHTCAP_SU_BEAMFORMEE_CAPABLE |
	    IEEE80211_VHTCAP_MU_BEAMFORMEE_CAPABLE |
	    (3 << IEEE80211_VHTCAP_BEAMFORMEE_STS_SHIFT) |
	    (7 << IEEE80211_VHTCAP_MAX_A_MPDU_LENGTH_EXPONENT_SHIFT) |
	    IEEE80211_VHTCAP_RX_ANTENNA_PATTERN |
	    IEEE80211_VHTCAP_TX_ANTENNA_PATTERN;
	if (nss > 1)
		ic->ic_vhtcaps |= IEEE80211_VHTCAP_TXSTBC;
	for (i = 0; i < 8; i++) {
		vht_map |= (i < nss ? IEEE80211_VHT_MCS_SUPPORT_0_9 :
		    IEEE80211_VHT_MCS_NOT_SUPPORTED) << (2 * i);
		he_map |= (i < nss ? IEEE80211_HE_MCS_SUPPORT_0_11 :
		    IEEE80211_HE_MCS_NOT_SUPPORTED) << (2 * i);
	}
	ic->ic_vht_rx_mcs_map = htole16(vht_map);
	ic->ic_vht_tx_mcs_map = htole16(vht_map);
	ic->ic_vht_rx_highest = 0;
	ic->ic_vht_tx_highest = 0;
	memset(ic->ic_vht_sup_mcs, 0, sizeof(ic->ic_vht_sup_mcs));
	for (i = 0; i < nss; i++)
		ic->ic_vht_sup_mcs[i] = 0x03ff;	/* MCS 0-9 */

	memset(he, 0, sizeof(*he));
	he->mac_cap_info[0] = IEEE80211_HE_MAC_CAP0_HTC_HE;
	he->mac_cap_info[1] = IEEE80211_HE_MAC_CAP1_TF_MAC_PAD_DUR_16US;
	/* 0x18: A-MPDU length exponent extension 3 (Linux _EXT_3) */
	he->mac_cap_info[3] = IEEE80211_HE_MAC_CAP3_OMI_CONTROL | 0x18;
	mwx_he_band_caps(sc, 1);
	he->phy_cap_info[1] = IEEE80211_HE_PHY_CAP1_LDPC_CODING_IN_PAYLOAD |
	    IEEE80211_HE_PHY_CAP1_DEVICE_CLASS_A |
	    IEEE80211_HE_PHY_CAP1_HE_LTF_AND_GI_FOR_HE_PPDUS_0_8US;
	he->phy_cap_info[2] = IEEE80211_HE_PHY_CAP2_NDP_4x_LTF_AND_3_2US |
	    IEEE80211_HE_PHY_CAP2_STBC_TX_UNDER_80MHZ |
	    IEEE80211_HE_PHY_CAP2_STBC_RX_UNDER_80MHZ |
	    IEEE80211_HE_PHY_CAP2_UL_MU_FULL_MU_MIMO |
	    IEEE80211_HE_PHY_CAP2_UL_MU_PARTIAL_MU_MIMO;
	he->phy_cap_info[3] = IEEE80211_HE_PHY_CAP3_DCM_MAX_CONST_TX_QPSK |
	    IEEE80211_HE_PHY_CAP3_DCM_MAX_CONST_RX_QPSK;
	he->phy_cap_info[4] = IEEE80211_HE_PHY_CAP4_SU_BEAMFORMEE |
	    IEEE80211_HE_PHY_CAP4_BEAMFORMEE_MAX_STS_UNDER_80MHZ_4;
	he->phy_cap_info[5] = IEEE80211_HE_PHY_CAP5_NG16_SU_FEEDBACK |
	    IEEE80211_HE_PHY_CAP5_NG16_MU_FEEDBACK;
	he->phy_cap_info[6] = IEEE80211_HE_PHY_CAP6_CODEBOOK_SIZE_42_SU |
	    IEEE80211_HE_PHY_CAP6_CODEBOOK_SIZE_75_MU |
	    IEEE80211_HE_PHY_CAP6_TRIG_CQI_FB |
	    IEEE80211_HE_PHY_CAP6_PARTIAL_BW_EXT_RANGE;
	he->phy_cap_info[7] = IEEE80211_HE_PHY_CAP7_POWER_BOOST_FACTOR_AR |
	    IEEE80211_HE_PHY_CAP7_HE_SU_MU_PPDU_4XLTF_AND_08_US_GI;
	he->phy_cap_info[8] =
	    IEEE80211_HE_PHY_CAP8_20MHZ_IN_40MHZ_HE_PPDU_IN_2G |
	    IEEE80211_HE_PHY_CAP8_DCM_MAX_RU_484;
	he->phy_cap_info[9] = IEEE80211_HE_PHY_CAP9_LONGER_THAN_16_SIGB_OFDM_SYM |
	    IEEE80211_HE_PHY_CAP9_NON_TRIGGERED_CQI_FEEDBACK |
	    IEEE80211_HE_PHY_CAP9_TX_1024_QAM_LESS_THAN_242_TONE_RU |
	    IEEE80211_HE_PHY_CAP9_RX_1024_QAM_LESS_THAN_242_TONE_RU |
	    IEEE80211_HE_PHY_CAP9_RX_FULL_BW_SU_USING_MU_WITH_COMP_SIGB |
	    IEEE80211_HE_PHY_CAP9_RX_FULL_BW_SU_USING_MU_WITH_NON_COMP_SIGB |
	    IEEE80211_HE_PHY_CAP9_NOMIMAL_PKT_PADDING_16US;
	ic->ic_he_mcs_nss_supp.rx_mcs_80 = htole16(he_map);
	ic->ic_he_mcs_nss_supp.tx_mcs_80 = htole16(he_map);
	ic->ic_he_mcs_nss_supp.rx_mcs_160 = htole16(0xffff);
	ic->ic_he_mcs_nss_supp.tx_mcs_160 = htole16(0xffff);
	ic->ic_he_mcs_nss_supp.rx_mcs_80p80 = htole16(0xffff);
	ic->ic_he_mcs_nss_supp.tx_mcs_80p80 = htole16(0xffff);
	memset(ic->ic_ppe_thres, 0, sizeof(ic->ic_ppe_thres));

	mwx_apply_phy_limit(sc);
}

/*
 * Turn the net80211 HT/VHT/HE switches on up to sc_phy_limit. They take
 * effect at the next association (net80211 negotiates on the assoc
 * response). ieee80211_media_init() turns HT back on, so this runs after it.
 */
void
mwx_apply_phy_limit(struct mwx_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;

	ic->ic_flags &= ~(IEEE80211_F_HTON | IEEE80211_F_VHTON |
	    IEEE80211_F_HEON);
	if (sc->sc_phy_limit >= MWX_PHY_LIMIT_HT)
		ic->ic_flags |= IEEE80211_F_HTON;
	if (sc->sc_phy_limit >= MWX_PHY_LIMIT_VHT)
		ic->ic_flags |= IEEE80211_F_VHTON;
	if (sc->sc_phy_limit >= MWX_PHY_LIMIT_HE)
		ic->ic_flags |= IEEE80211_F_HEON;
	/* Tells the native Wi-Fi interface (AirportMtkWlan) that HE is on. */
	if (ic->ic_flags & IEEE80211_F_HEON)
		ic->ic_caps |= IEEE80211_C_HE;
	else
		ic->ic_caps &= ~IEEE80211_C_HE;
	/* QoS data (and with it A-MPDU) only with HT, like net80211 */
	ieee80211_configure_ampdu_tx(ic, sc->sc_phy_limit >= MWX_PHY_LIMIT_HT);
}

bool
mwx_attach(struct device *parent, struct device *self, void *aux)
{
	struct mwx_softc *sc = (struct mwx_softc *)self;
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &ic->ic_if;
	struct pci_attach_args *pa = (struct pci_attach_args *)aux;
	pci_intr_handle_t ih;
	pcireg_t memtype;
	uint32_t hwid, hwrev;
	int error;

	MWX_DEV_LOG(sc, "attach start pci_id=0x%08x vendor=0x%04x product=0x%04x\n",
	    pa->pa_id, PCI_VENDOR(pa->pa_id), PCI_PRODUCT(pa->pa_id));
	sc->sc_pc = pa->pa_pc;
	sc->sc_tag = pa->pa_tag;
	sc->sc_dmat = pa->pa_dmat;
	sc->sc_intr_mask = MT_INT_RX_DONE_ALL | MT_INT_TX_DONE_ALL |
	    MT_INT_MCU_CMD;
	if (PCI_PRODUCT(pa->pa_id) == PCI_PRODUCT_MEDIATEK_MT7922 ||
	    PCI_PRODUCT(pa->pa_id) == PCI_PRODUCT_MEDIATEK_MT7922_RZ616)
		sc->sc_hwtype = MWX_HW_MT7922;
	else
		sc->sc_hwtype = MWX_HW_MT7921;
	MWX_DEV_LOG(sc, "attach hardware type=%s\n",
	    sc->sc_hwtype == MWX_HW_MT7922 ? "MT7922" : "MT7921");

	memtype = pci_mapreg_type(pa->pa_pc, pa->pa_tag, PCI_MAPREG_START);
	if (pci_mapreg_map(pa, PCI_MAPREG_START, memtype, 0,
	    &sc->sc_st, &sc->sc_memh, NULL, &sc->sc_mems, 0)) {
		printf("%s: can't map mem space\n", DEVNAME(sc));
		return false;
	}
	MWX_DEV_LOG(sc, "attach BAR mapped memtype=0x%x size=%zu\n",
	    memtype, (size_t)sc->sc_mems);
	mwx_pcie_disable_aspm(sc, pa->pa_tag);

	if (pci_intr_map_msix(pa, 0, &ih) &&
	    pci_intr_map_msi(pa, &ih) &&
	    pci_intr_map(pa, &ih)) {
		printf("%s: can't map interrupt\n", DEVNAME(sc));
		bus_space_unmap(sc->sc_st, sc->sc_memh, sc->sc_mems);
		return false;
	}
	MWX_DEV_LOG(sc, "attach interrupt mapped source=%s\n",
	    pci_intr_string(pa->pa_pc, ih));

	if (mwx_mcu_fw_pmctrl(sc) != 0 ||
	    mwx_mcu_drv_pmctrl(sc) != 0) {
		MWX_DEV_LOG(sc, "attach MCU power control failed\n");
		goto fail;
	}
	MWX_DEV_LOG(sc, "attach MCU power control ok\n");

	hwid = mwx_read(sc, MT_HW_CHIPID) & 0xffff;
	hwrev = mwx_read(sc, MT_HW_REV) & 0xff;

	MWX_DEV_LOG(sc, "attach chip id=0x%04x hwrev=0x%02x interrupt=%s\n",
	    hwid, hwrev, pci_intr_string(pa->pa_pc, ih));

	sc->sc_txlock_grp = lck_grp_alloc_init("mwx_txlock", NULL);
	sc->sc_txlock = lck_mtx_alloc_init(sc->sc_txlock_grp, LCK_ATTR_NULL);

	mwx_set(sc, MT_MCU2HOST_SW_INT_ENA, MT_MCU_CMD_WAKE_RX_PCIE);
	if (mwx_wfsys_reset(sc) != 0) {
		MWX_DEV_LOG(sc, "attach WFSYS reset failed\n");
		goto fail;
	}
	MWX_DEV_LOG(sc, "attach WFSYS reset ok\n");

	mwx_write(sc, MT_WFDMA0_HOST_INT_ENA, 0);
	mwx_write(sc, MT_PCIE_MAC_INT_ENABLE, 0xff);

	/* Like Linux's hard IRQ handler: mask the card, mwx_intr unmasks after processing. */
	ih->filter = mwx_intr_filter;
	sc->sc_ih = pci_intr_establish(pa->pa_pc, ih, IPL_NET,
	    mwx_intr, sc, DEVNAME(sc));
	MWX_DEV_LOG(sc, "attach interrupt established handle=%p type=%s\n",
	    (void *)sc->sc_ih, ih->msi ? "msi" : "legacy-intx");

	if ((error = mwx_txwi_alloc(sc, MWX_TXWI_MAX)) != 0) {
		printf("%s: failed to allocate DMA resources %d\n",
		    DEVNAME(sc), error);
		goto fail;
	}

	if ((error = mwx_dma_alloc(sc)) != 0) {
		printf("%s: failed to allocate DMA resources %d\n",
		    DEVNAME(sc), error);
		goto fail;
	}
	MWX_DEV_LOG(sc, "attach DMA resources ready\n");

	/* set regulatory domain to '00' */
	sc->sc_alpha2[0] = '0';
	sc->sc_alpha2[1] = '0';

	ic->ic_phytype = IEEE80211_T_OFDM;	/* not only, but not used */
	ic->ic_opmode = IEEE80211_M_STA;	/* default to BSS mode */
	ic->ic_state = IEEE80211_S_INIT;

	/* Set device capabilities. */
	ic->ic_caps =
	    IEEE80211_C_QOS |		/* WMM, needed for HT aggregation */
	    IEEE80211_C_TX_AMPDU |	/* net80211 sets up TX Block Ack */
	    IEEE80211_C_WEP |		/* WEP */
	    IEEE80211_C_RSN |		/* WPA/RSN */
	    IEEE80211_C_SCANALL |	/* device scans all channels at once */
	    IEEE80211_C_SCANALLBAND |	/* device scans all bands at once */
	    IEEE80211_C_MONITOR |	/* monitor mode supported */
#ifndef IEEE80211_STA_ONLY
	    IEEE80211_C_IBSS |		/* IBSS mode supported */
	    IEEE80211_C_HOSTAP |	/* HostAP mode supported */
	    IEEE80211_C_APPMGT |	/* HostAP power management */
#endif
	    IEEE80211_C_SHSLOT |	/* short slot time supported */
	    IEEE80211_C_SHPREAMBLE;	/* short preamble supported */

	{
		char arg[8] = "";

		sc->sc_phy_limit = MWX_PHY_LIMIT_HE;
#ifdef AIRPORT_WCL
		/*
		 * Like laobamac's AirportItlwm, the native Wi-Fi build keeps HE
		 * opt-in (itlwm_he=1): macOS's PHY reporting for HE networks
		 * through this interface is not settled yet. mtkphy= still wins.
		 */
		int he = 0;
		if (!PE_parse_boot_argn("itlwm_he", &he, sizeof(he)) || he != 1)
			sc->sc_phy_limit = MWX_PHY_LIMIT_VHT;
#endif
		if (PE_parse_boot_argn("mtkphy", arg, sizeof(arg)) &&
		    mwx_parse_phy_limit(arg) >= 0)
			sc->sc_phy_limit = mwx_parse_phy_limit(arg);
	}
	mwx_setup_phy_caps(sc);

	/* IBSS channel undefined for now. */
	ic->ic_ibss_chan = &ic->ic_channels[1];

	/* HW supports up to 288 STAs in HostAP and IBSS modes */
	ic->ic_max_aid = min(IEEE80211_AID_MAX, MWX_WTBL_STA - 1);

	ic->ic_max_rssi = 100;	/* rxi_rssi is normalized for ni_rssi. */

	ifp->if_softc = sc;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
#ifdef DEBUG
	/* Enables net80211's join diagnostics; per-scan noise is gated by mtk_verbose_scans. */
	ifp->if_flags |= IFF_DEBUG;
#endif
	ifp->if_ioctl = mwx_ioctl;
	ifp->if_start = mwx_start;
	ifp->if_watchdog = mwx_watchdog;
	memcpy(ifp->if_xname, DEVNAME(sc), IFNAMSIZ);

	if_attach(ifp);
	ieee80211_ifattach(ifp, gMwxHal->mwxController());
	ieee80211_media_init(ifp);
	MWX_DEV_LOG(sc, "attach net80211 attached caps=0x%x max_aid=%u\n",
	    ic->ic_caps, ic->ic_max_aid);

#if NBPFILTER > 0
	mwx_radiotap_attach(sc);
#endif

	ic->ic_node_alloc = mwx_node_alloc;
	ic->ic_newassoc = mwx_newassoc;
	ic->ic_ampdu_rx_start = mwx_ampdu_rx_start;
	ic->ic_ampdu_rx_stop = mwx_ampdu_rx_stop;
	ic->ic_ampdu_tx_start = mwx_ampdu_tx_start;
	ic->ic_ampdu_tx_stop = mwx_ampdu_tx_stop;
#ifndef IEEE80211_STA_ONLY
	ic->ic_node_leave = mwx_node_leave;
#endif
	/*
	 * Scans while associated only on request (mtkwifi scan), like Linux; the
	 * RSSI-triggered roaming scan is disabled in ieee80211_input(): it took
	 * the radio off channel right after association and lost the DHCP reply.
	 */
	ic->ic_bgscan_start = mwx_bgscan;
	ic->ic_bgscan_done = mwx_bgscan_done;
	ic->ic_set_key = mwx_set_key;
	ic->ic_delete_key = mwx_delete_key;

	/* Override 802.11 state transition machine. */
	sc->sc_newstate = ic->ic_newstate;
	ic->ic_newstate = mwx_newstate;

	sc->sc_nswq = taskq_create("mwxns", 1, IPL_NET, 0);
	if (sc->sc_nswq == NULL) {
		printf("%s: can't create task queue\n", DEVNAME(sc));
		goto fail;
	}

	task_set(&sc->sc_newstate_task, mwx_newstate_task, sc, "mwx_newstate");
	task_set(&sc->sc_bgscan_done_task, mwx_bgscan_done_task, sc, "mwx_bgscan");
	task_set(&sc->sc_setkey_task, mwx_setkey_task, sc, "mwx_setkey");
	task_set(&sc->sc_reset_task, mwx_reset_task, sc, "mwx_reset");
	task_set(&sc->sc_scan_task, mwx_end_scan_task, sc, "mwx_scan");
	task_set(&sc->sc_ba_task, mwx_ba_task, sc, "mwx_ba");
	timeout_set(&sc->sc_reset_to, mwx_reset_timeo, sc);

	/*
	 * We cannot read the MAC address without loading the
	 * firmware from disk. Postpone until mountroot is done.
	 */
	config_mountroot(self, mwx_attach_hook);

	MWX_DEV_LOG(sc, "attach complete\n");
	return true;

fail:
	MWX_DEV_LOG(sc, "attach failed; cleaning resources\n");
	if (sc->sc_nswq != NULL) {
		taskq_destroy(sc->sc_nswq);
		sc->sc_nswq = NULL;
	}
	timeout_free(&sc->sc_reset_to);
	mwx_txwi_free(sc);
	mwx_dma_free(sc);
	if (sc->sc_ih != NULL)
		pci_intr_disestablish(pa->pa_pc, sc->sc_ih);
	mwx_txlock_free(sc);
	bus_space_unmap(sc->sc_st, sc->sc_memh, sc->sc_mems);
	return false;
}

void
mwx_txlock_free(struct mwx_softc *sc)
{
	if (sc->sc_txlock != NULL) {
		lck_mtx_free(sc->sc_txlock, sc->sc_txlock_grp);
		sc->sc_txlock = NULL;
	}
	if (sc->sc_txlock_grp != NULL) {
		lck_grp_free(sc->sc_txlock_grp);
		sc->sc_txlock_grp = NULL;
	}
}

int
mwx_activate(struct device *self, int act)
{
	/* XXX TODO */
	return 0;
}

#if 0
/* OpenBSD autoconf metadata is replaced by SimpleMtkWlan's IOKit matching path. */
struct cfdriver mwx_cd = {
	NULL, "mwx", DV_IFNET
};

struct cfattach mwx_ca = {
	sizeof(struct mwx_softc), mwx_match, mwx_attach,
	NULL, mwx_activate
};
#endif

void
mwx_reset(struct mwx_softc *sc)
{
	if (sc->sc_resetting)
		return;
	sc->sc_resetting = 1;
	task_add(sc->sc_nswq, &sc->sc_reset_task);
}

void
mwx_reset_timeo(void *arg)
{
	mwx_reset((struct mwx_softc *)arg);
}

void
mwx_reset_task(void *arg)
{
	struct mwx_softc *sc = (struct mwx_softc *)arg;
	struct ifnet *ifp = &sc->sc_ic.ic_if;
	int fatal = 0;

	if (ifp->if_flags & IFF_RUNNING)
		mwx_stop(ifp);

	if (!fatal && (ifp->if_flags & (IFF_UP | IFF_RUNNING)) == IFF_UP)
		mwx_init(ifp);
	sc->sc_resetting = 0;
	sc->sc_coredump_cnt = 0;
}

int
mwx_txwi_alloc(struct mwx_softc *sc, int count)
{
	int error, nsegs, i;
	struct mwx_txwi_desc *q = &sc->sc_txwi;
	bus_size_t size = count * sizeof(*q->mt_desc);
	uint32_t addr;

	MWX_DEV_LOG(sc, "txwi alloc start count=%d size=%zu\n",
	    count, (size_t)size);
	LIST_INIT(&q->mt_freelist);
	q->mt_count = count;

	error = bus_dmamap_create(sc->sc_dmat, size, 1, size, 0,
	    BUS_DMA_NOWAIT, &q->mt_map);
	if (error != 0) {
		printf("%s: could not create desc TWXI map\n", DEVNAME(sc));
		goto fail;
	}

	error = bus_dmamem_alloc(sc->sc_dmat, size, PAGE_SIZE, 0, &q->mt_seg,
	    1, &nsegs, BUS_DMA_NOWAIT | BUS_DMA_ZERO);
	if (error != 0) {
		printf("%s: could not allocate TWXI memory\n", DEVNAME(sc));
		goto fail;
	}

	error = bus_dmamem_map(sc->sc_dmat, &q->mt_seg, nsegs, size,
	    (void **)&q->mt_desc, BUS_DMA_NOWAIT);
	if (error != 0) {
		printf("%s: can't map desc DMA memory\n", DEVNAME(sc));
		goto fail;
	}

	error = bus_dmamap_load(sc->sc_dmat, q->mt_map, q->mt_desc,
	    size, NULL, BUS_DMA_NOWAIT);
	if (error != 0) {
		printf("%s: could not load desc DMA map\n", DEVNAME(sc));
		goto fail;
	}

	addr = q->mt_map->dm_segs[0].ds_addr;

	q->mt_data = (struct mwx_txwi *)mallocarray(count, sizeof(*q->mt_data),
	    M_DEVBUF, M_NOWAIT | M_ZERO);
	if (q->mt_data == NULL) {
		printf("%s: could not allocate soft data\n", DEVNAME(sc));
		error = ENOMEM;
		goto fail;
	}

	for (i = 0; i < count; i++) {
		q->mt_data[i].mt_idx = i;
		q->mt_data[i].mt_desc = &q->mt_desc[i];
		q->mt_data[i].mt_addr = addr + i * sizeof(*q->mt_desc);
		error = bus_dmamap_create(sc->sc_dmat, MT_TXD_LEN_MASK,
		    MT_MAX_SCATTER, MT_TXD_LEN_MASK, 0, BUS_DMA_NOWAIT,
		    &q->mt_data[i].mt_map);
		if (error != 0) {
			printf("%s: could not create data DMA map\n",
			    DEVNAME(sc));
			goto fail;
		}
	}

	for (i = count - 1; i >= MT_PACKET_ID_FIRST; i--)
		LIST_INSERT_HEAD(&q->mt_freelist, &q->mt_data[i], mt_entry);

	MWX_DEV_LOG(sc, "txwi alloc ok count=%d dma=0x%08x\n", count, addr);
	return 0;

fail:
	MWX_DEV_LOG(sc, "txwi alloc failed error=%d\n", error);
	mwx_txwi_free(sc);
	return error;
}

void
mwx_txwi_free(struct mwx_softc *sc)
{
	struct mwx_txwi_desc *q = &sc->sc_txwi;

	if (q->mt_data != NULL) {
		int i;
		for (i = 0; i < q->mt_count; i++) {
			struct mwx_txwi *mt = &q->mt_data[i];
			bus_dmamap_destroy(sc->sc_dmat, mt->mt_map);
			m_freem(mt->mt_mbuf);
			if (i >= MT_PACKET_ID_FIRST)
				LIST_REMOVE(mt, mt_entry);
		}
		free(q->mt_data, M_DEVBUF, q->mt_count * sizeof(*q->mt_data));
	}

	if (q->mt_desc != NULL) {
		bus_dmamap_sync(sc->sc_dmat, q->mt_map, 0,
			q->mt_map->dm_mapsize, BUS_DMASYNC_POSTWRITE);
		bus_dmamap_unload(sc->sc_dmat, q->mt_map);
	}

	/*
	 * XXX TODO this is probably not correct as a check, should use
	 * some state variable bitfield to decide which steps need to be run.
	 */
	if (q->mt_seg.ds_len != 0)
		bus_dmamem_unmap(sc->sc_dmat, (caddr_t)q->mt_desc,
			q->mt_count * sizeof(*q->mt_desc));
	if (q->mt_map != NULL)
		bus_dmamem_free(sc->sc_dmat, &q->mt_seg, 1);

	memset(q, 0, sizeof(*q));
}

struct mwx_txwi *
mwx_txwi_get(struct mwx_softc *sc)
{
	struct mwx_txwi *mt;

	mt = LIST_FIRST(&sc->sc_txwi.mt_freelist);
	if (mt == NULL)
		return NULL;
	LIST_REMOVE(mt, mt_entry);
	mt->mt_busy = 1;
	return mt;
}

void
mwx_txwi_put(struct mwx_softc *sc, struct mwx_txwi *mt)
{
	if (mt->mt_busy == 0)
		return;

	if (mt->mt_mbuf != NULL) {
		bus_dmamap_sync(sc->sc_dmat, mt->mt_map, 0,
		    mt->mt_map->dm_mapsize, BUS_DMASYNC_POSTWRITE);
		bus_dmamap_unload(sc->sc_dmat, mt->mt_map);
		m_freem(mt->mt_mbuf);
		mt->mt_mbuf = NULL;
	}
	if (mt->mt_ni != NULL) {
		ieee80211_release_node(&sc->sc_ic, mt->mt_ni);
		mt->mt_ni = NULL;
	}

	memset(mt->mt_desc, 0, sizeof(*mt->mt_desc));
	mt->mt_busy = 0;

	if (mt->mt_idx < MT_PACKET_ID_FIRST)
		return;
	LIST_INSERT_HEAD(&sc->sc_txwi.mt_freelist, mt, mt_entry);
}

int
mwx_txwi_enqueue(struct mwx_softc *sc, struct mwx_txwi *mt, struct mbuf *m,
    struct ieee80211_node *ni)
{
	struct mwx_txwi_desc *q = &sc->sc_txwi;
	struct mt76_txwi *txp = mt->mt_desc;
	struct mt76_connac_txp_ptr *ptr = &txp->ptr[0];
	uint32_t addr;
	uint16_t len;
	int i, nsegs, rv;

	rv = bus_dmamap_load_mbuf(sc->sc_dmat, mt->mt_map, m,
	    BUS_DMA_WRITE | BUS_DMA_NOWAIT);
	if (rv == EFBIG && m_defrag(m, M_DONTWAIT) == 0)
		rv = bus_dmamap_load_mbuf(sc->sc_dmat, mt->mt_map, m,
		    BUS_DMA_WRITE | BUS_DMA_NOWAIT);
	if (rv != 0)
		return rv;

	nsegs = mt->mt_map->dm_nsegs;
	/*
	 * A TXWI has room for MT_HW_TXP_MAX_BUF_NUM buffer pointers and the card
	 * reads exactly sizeof(struct mt76_txwi). More segments would overwrite
	 * the next TXWI and hide the LAST flag, which hung the card's DMA.
	 */
	if (nsegs < 1 || nsegs > MT_HW_TXP_MAX_BUF_NUM) {
		static uint32_t toomany_log;
		if (toomany_log++ < 16)
			MWX_DEV_LOG(sc, "tx drop: %d DMA segments (max %d) len=%zu\n",
			    nsegs, MT_HW_TXP_MAX_BUF_NUM, mbuf_pkthdr_len(m));
		bus_dmamap_unload(sc->sc_dmat, mt->mt_map);
		return EFBIG;
	}

	bus_dmamap_sync(sc->sc_dmat, mt->mt_map, 0, mt->mt_map->dm_mapsize,
	    BUS_DMASYNC_PREWRITE);

	txp->msdu_id[0] = htole16(mt->mt_idx | MT_MSDU_ID_VALID);
	mt->mt_mbuf = m;
	mt->mt_ni = ni;

	bus_dmamap_sync(sc->sc_dmat, q->mt_map, 0, q->mt_map->dm_mapsize,
	    BUS_DMASYNC_PREWRITE);
	for (i = 0; i < nsegs; i++) {
		KASSERT(mt->mt_map->dm_segs[i].ds_addr <= UINT32_MAX);
		KASSERT(mt->mt_map->dm_segs[i].ds_len <= MT_TXD_LEN_MASK);
		addr = mt->mt_map->dm_segs[i].ds_addr;
		len = mt->mt_map->dm_segs[i].ds_len;

		if (i == nsegs - 1)
			len |= MT_TXD_LEN_LAST;

		if ((i & 1) == 0) {
			ptr->buf0 = htole32(addr);
			ptr->len0 = htole16(len);
		} else {
			ptr->buf1 = htole32(addr);
			ptr->len1 = htole16(len);
			ptr++;
		}
	}
	bus_dmamap_sync(sc->sc_dmat, q->mt_map, 0, q->mt_map->dm_mapsize,
	    BUS_DMASYNC_POSTWRITE);

	return 0;
}

int
mwx_queue_alloc(struct mwx_softc *sc, struct mwx_queue *q, int count,
    uint32_t regbase)
{
	int error, nsegs, i;
	bus_size_t size = count * sizeof(*q->mq_desc);

	MWX_DEV_LOG(sc, "queue alloc start regbase=0x%08x count=%d size=%zu\n",
	    regbase, count, (size_t)size);
	q->mq_regbase = regbase;
	q->mq_count = count;

	error = bus_dmamap_create(sc->sc_dmat, size, 1, size, 0,
	    BUS_DMA_NOWAIT, &q->mq_map);
	if (error != 0) {
		printf("%s: could not create desc DMA map\n", DEVNAME(sc));
		goto fail;
	}

	error = bus_dmamem_alloc(sc->sc_dmat, size, PAGE_SIZE, 0, &q->mq_seg,
	    1, &nsegs, BUS_DMA_NOWAIT | BUS_DMA_ZERO);
	if (error != 0) {
		printf("%s: could not allocate DMA memory\n", DEVNAME(sc));
		goto fail;
	}

	error = bus_dmamem_map(sc->sc_dmat, &q->mq_seg, nsegs, size,
	    (void **)&q->mq_desc, BUS_DMA_NOWAIT);
	if (error != 0) {
		printf("%s: can't map desc DMA memory\n", DEVNAME(sc));
		goto fail;
	}

	error = bus_dmamap_load(sc->sc_dmat, q->mq_map, q->mq_desc,
	    size, NULL, BUS_DMA_NOWAIT);
	if (error != 0) {
		printf("%s: could not load desc DMA map\n", DEVNAME(sc));
		goto fail;
	}

	q->mq_data = (struct mwx_queue_data *)mallocarray(count, sizeof(*q->mq_data),
	    M_DEVBUF, M_NOWAIT | M_ZERO);
	if (q->mq_data == NULL) {
		printf("%s: could not allocate soft data\n", DEVNAME(sc));
		error = ENOMEM;
		goto fail;
	}

	for (i = 0; i < count; i++) {
		error = bus_dmamap_create(sc->sc_dmat, MT_MAX_SIZE,
		    MT_MAX_SCATTER, MT_MAX_SIZE, 0, BUS_DMA_NOWAIT,
		    &q->mq_data[i].md_map);
		if (error != 0) {
			printf("%s: could not create data DMA map\n",
			    DEVNAME(sc));
			goto fail;
		}
	}

	mwx_queue_reset(sc, q);
	MWX_DEV_LOG(sc, "queue alloc ok regbase=0x%08x count=%d dma=0x%llx\n",
	    regbase, count, (unsigned long long)q->mq_map->dm_segs[0].ds_addr);
	return 0;

fail:
	MWX_DEV_LOG(sc, "queue alloc failed regbase=0x%08x error=%d\n",
	    regbase, error);
	mwx_queue_free(sc, q);
	return error;
}

void
mwx_queue_free(struct mwx_softc *sc, struct mwx_queue *q)
{
	if (q->mq_data != NULL) {
		int i;
		for (i = 0; i < q->mq_count; i++) {
			struct mwx_queue_data  *md = &q->mq_data[i];
			bus_dmamap_destroy(sc->sc_dmat, md->md_map);
			m_freem(md->md_mbuf);
		}
		free(q->mq_data, M_DEVBUF, q->mq_count * sizeof(*q->mq_data));
	}

	if (q->mq_desc != NULL) {
		bus_dmamap_sync(sc->sc_dmat, q->mq_map, 0,
			q->mq_map->dm_mapsize, BUS_DMASYNC_POSTWRITE);
		bus_dmamap_unload(sc->sc_dmat, q->mq_map);
	}

	/*
	 * XXX TODO this is probably not correct as a check, should use
	 * some state variable bitfield to decide which steps need to be run.
	 */
	if (q->mq_seg.ds_len != 0)
		bus_dmamem_unmap(sc->sc_dmat, (caddr_t)q->mq_desc,
			q->mq_count * sizeof(*q->mq_desc));
	if (q->mq_map != NULL)
		bus_dmamem_free(sc->sc_dmat, &q->mq_seg, 1);

	memset(q, 0, sizeof(*q));
}

void
mwx_queue_reset(struct mwx_softc *sc, struct mwx_queue *q)
{
	int i;
	uint32_t dmaaddr;
	struct mwx_queue_data *md;

	/* clear descriptors */
	bus_dmamap_sync(sc->sc_dmat, q->mq_map, 0, q->mq_map->dm_mapsize,
	    BUS_DMASYNC_PREWRITE);

	for (i = 0; i < q->mq_count; i++) {
		q->mq_desc[i].buf0 = 0;
		q->mq_desc[i].buf1 = 0;
		q->mq_desc[i].info = 0;
		q->mq_desc[i].ctrl = htole32(MT_DMA_CTL_DMA_DONE);
	}

	bus_dmamap_sync(sc->sc_dmat, q->mq_map, 0, q->mq_map->dm_mapsize,
	    BUS_DMASYNC_POSTWRITE);

	/* reset DMA registers */
	KASSERT(q->mq_map->dm_nsegs == 1);
	KASSERT(q->mq_map->dm_segs[0].ds_addr <= UINT32_MAX);
	dmaaddr	= q->mq_map->dm_segs[0].ds_addr;
	mwx_write(sc, q->mq_regbase + MT_DMA_DESC_BASE, dmaaddr);
	mwx_write(sc, q->mq_regbase + MT_DMA_RING_SIZE, q->mq_count);
	mwx_write(sc, q->mq_regbase + MT_DMA_CPU_IDX, 0);
	mwx_write(sc, q->mq_regbase + MT_DMA_DMA_IDX, 0);
	q->mq_cons = 0;
	q->mq_prod = 0;

	/* free buffers */
	for (i = 0; i < q->mq_count; i++) {
		md = &q->mq_data[i];
		if (md->md_mbuf != NULL) {
			bus_dmamap_sync(sc->sc_dmat, md->md_map, 0,
			    md->md_map->dm_mapsize,
			    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);
			bus_dmamap_unload(sc->sc_dmat, md->md_map);
			m_freem(md->md_mbuf);
			md->md_mbuf = NULL;
		}
	}
}

int
mwx_buf_fill(struct mwx_softc *sc, struct mwx_queue_data *md,
    struct mwx_desc *desc)
{
	struct mbuf *m;
	uint32_t buf0, len0, ctrl;
	int rv;

	m = MCLGETL(NULL, M_DONTWAIT, MT_RX_BUF_SIZE);
	if (m == NULL)
		return (ENOMEM);

	mbuf_setlen(m, MT_RX_BUF_SIZE);
	mbuf_pkthdr_setlen(m, MT_RX_BUF_SIZE);

	rv = bus_dmamap_load_mbuf(sc->sc_dmat, md->md_map, m,
	     BUS_DMA_READ | BUS_DMA_NOWAIT);
	if (rv != 0) {
		printf("%s: could not load data, %d\n", DEVNAME(sc), rv);
		m_freem(m);
		return (rv);
	}

	bus_dmamap_sync(sc->sc_dmat, md->md_map, 0,
	    md->md_map->dm_mapsize, BUS_DMASYNC_PREREAD);

	md->md_mbuf = m;

	KASSERT(md->md_map->dm_nsegs == 1);
	KASSERT(md->md_map->dm_segs[0].ds_addr <= UINT32_MAX);
	buf0 = md->md_map->dm_segs[0].ds_addr;
	len0 = md->md_map->dm_segs[0].ds_len;
	ctrl = MT_DMA_CTL_SD_LEN0(len0);
	ctrl |= MT_DMA_CTL_LAST_SEC0;

	desc->buf0 = htole32(buf0);
	desc->buf1 = 0;
	desc->info = 0;
	desc->ctrl = htole32(ctrl);

	return 0;
}

int
mwx_queue_fill(struct mwx_softc *sc, struct mwx_queue *q)
{
	u_int idx, last;
	int rv;

	last = (q->mq_count + q->mq_cons - 1) % q->mq_count;
	idx = q->mq_prod;

	bus_dmamap_sync(sc->sc_dmat, q->mq_map, 0, q->mq_map->dm_mapsize,
	    BUS_DMASYNC_PREWRITE);

	while (idx != last) {
		rv = mwx_buf_fill(sc, &q->mq_data[idx], &q->mq_desc[idx]);
		if (rv != 0) {
			printf("%s: could not fill data, slot %d err %d\n",
			    DEVNAME(sc), idx, rv);
			return rv;
		}
		idx = (idx + 1) % q->mq_count;
	}

	bus_dmamap_sync(sc->sc_dmat, q->mq_map, 0, q->mq_map->dm_mapsize,
	    BUS_DMASYNC_POSTWRITE);

	q->mq_prod = idx;
	mwx_write(sc, q->mq_regbase + MT_DMA_CPU_IDX, q->mq_prod);

	return 0;
}

int
mwx_dma_alloc(struct mwx_softc *sc)
{
	int rv;

	MWX_DEV_LOG(sc, "dma alloc start\n");
	/* TX queues */
	if ((rv = mwx_queue_alloc(sc, &sc->sc_txq, 256,
	    MT_TX_DATA_RING_BASE)) != 0)
		return rv;
	if ((rv = mwx_queue_alloc(sc, &sc->sc_txmcuq, 256,
	    MT_TX_MCU_RING_BASE)) != 0)
		return rv;
	if ((rv = mwx_queue_alloc(sc, &sc->sc_txfwdlq, 16 /* XXX */,
	    MT_TX_FWDL_RING_BASE)) != 0)
		return rv;

	/* RX queues */
	if ((rv = mwx_queue_alloc(sc, &sc->sc_rxq, 256,
	    MT_RX_DATA_RING_BASE)) != 0 ||
	    (rv = mwx_queue_fill(sc, &sc->sc_rxq)) != 0)
		return rv;
	if ((rv = mwx_queue_alloc(sc, &sc->sc_rxmcuq, 256,
	    MT_RX_MCU_RING_BASE)) != 0 ||
	    (rv = mwx_queue_fill(sc, &sc->sc_rxmcuq)) != 0)
		return rv;
	if ((rv = mwx_queue_alloc(sc, &sc->sc_rxfwdlq, 16 /* XXX */,
	    MT_RX_FWDL_RING_BASE)) != 0 ||
	    (rv = mwx_queue_fill(sc, &sc->sc_rxfwdlq)) != 0)
		return rv;

	/* enable DMA engine */
	mwx_dma_enable(sc);

	MWX_DEV_LOG(sc, "dma alloc complete\n");
	return 0;
}

int
mwx_dma_reset(struct mwx_softc *sc, int fullreset)
{
	int rv;

	DPRINTF("%s: DMA reset\n", DEVNAME(sc));

	if ((rv = mwx_dma_disable(sc, fullreset)) != 0)
		return rv;
	if (fullreset)
		if ((rv = mwx_wfsys_reset(sc)) != 0)
			return rv;

	/* TX queues */
	mwx_queue_reset(sc, &sc->sc_txq);
	mwx_queue_reset(sc, &sc->sc_txmcuq);
	mwx_queue_reset(sc, &sc->sc_txfwdlq);

	/* RX queues */
	mwx_queue_reset(sc, &sc->sc_rxq);
	mwx_queue_reset(sc, &sc->sc_rxmcuq);
	mwx_queue_reset(sc, &sc->sc_rxfwdlq);

	/* TDOD mt76_tx_status_check */

	/* refill RX queues */
	if ((rv = mwx_queue_fill(sc, &sc->sc_rxq)) != 0 ||
	    (rv = mwx_queue_fill(sc, &sc->sc_rxmcuq)) != 0 ||
	    (rv = mwx_queue_fill(sc, &sc->sc_rxfwdlq)) != 0)
		return rv;

	/* enable DMA engine */
	mwx_dma_enable(sc);

	return 0;
}

void
mwx_dma_free(struct mwx_softc *sc)
{
	/* TX queues */
	mwx_queue_free(sc, &sc->sc_txq);
	mwx_queue_free(sc, &sc->sc_txmcuq);
	mwx_queue_free(sc, &sc->sc_txfwdlq);

	/* RX queues */
	mwx_queue_free(sc, &sc->sc_rxq);
	mwx_queue_free(sc, &sc->sc_rxmcuq);
	mwx_queue_free(sc, &sc->sc_rxfwdlq);
}

static inline int
mwx_dma_free_slots(struct mwx_queue *q)
{
	int free = q->mq_count - 1;
	free += q->mq_cons;
	free -= q->mq_prod;
	free %= q->mq_count;
	return free;
}

static int
mwx_dma_tx_enqueue_locked(struct mwx_softc *sc, struct mwx_queue *q,
    struct mbuf *m)
{
	struct mwx_queue_data *md;
	struct mwx_desc *desc;
	int i, nsegs, idx, rv;

	idx = q->mq_prod;
	md = &q->mq_data[idx];
	desc = &q->mq_desc[idx];

	rv = bus_dmamap_load_mbuf(sc->sc_dmat, md->md_map, m,
	    BUS_DMA_WRITE | BUS_DMA_NOWAIT);
	if (rv == EFBIG && m_defrag(m, M_DONTWAIT) == 0)
		rv = bus_dmamap_load_mbuf(sc->sc_dmat, md->md_map, m,
		    BUS_DMA_WRITE | BUS_DMA_NOWAIT);
	if (rv != 0)
		return rv;

	nsegs = md->md_map->dm_nsegs;

	/* check if there is enough space */
	if ((nsegs + 1)/2 > mwx_dma_free_slots(q)) {
		bus_dmamap_unload(sc->sc_dmat, md->md_map);
		return EBUSY;
	}

	bus_dmamap_sync(sc->sc_dmat, md->md_map, 0, md->md_map->dm_mapsize,
	    BUS_DMASYNC_PREWRITE);
	md->md_mbuf = m;
	md->md_txwi = NULL;

	bus_dmamap_sync(sc->sc_dmat, q->mq_map, 0, q->mq_map->dm_mapsize,
	    BUS_DMASYNC_PREWRITE);
	for (i = 0; i < nsegs; i += 2) {
		uint32_t	buf0, buf1 = 0;
		uint32_t	len0, len1 = 0, ctrl;

		KASSERT(md->md_map->dm_segs[i].ds_addr <= UINT32_MAX);
		buf0 = md->md_map->dm_segs[i].ds_addr;
		len0 = md->md_map->dm_segs[i].ds_len;
		ctrl = MT_DMA_CTL_SD_LEN0(len0);

		if (i < nsegs - 1) {
			KASSERT(md->md_map->dm_segs[i + 1].ds_addr <=
			    UINT32_MAX);
			buf1 = md->md_map->dm_segs[i + 1].ds_addr;
			len1 = md->md_map->dm_segs[i + 1].ds_len;
			ctrl |= MT_DMA_CTL_SD_LEN1(len1);
		}

		if (i == nsegs - 1)
			ctrl |= MT_DMA_CTL_LAST_SEC0;
		else if (i == nsegs - 2)
			ctrl |= MT_DMA_CTL_LAST_SEC1;

		desc->buf0 = htole32(buf0);
		desc->buf1 = htole32(buf1);
		desc->info = 0;
		desc->ctrl = htole32(ctrl);

		idx = (idx + 1) % q->mq_count;
		KASSERT(idx != q->mq_cons);
		md = &q->mq_data[idx];
		desc = &q->mq_desc[idx];
	}
	bus_dmamap_sync(sc->sc_dmat, q->mq_map, 0, q->mq_map->dm_mapsize,
	    BUS_DMASYNC_POSTWRITE);

	q->mq_prod = idx;

	mwx_write(sc, q->mq_regbase + MT_DMA_CPU_IDX, q->mq_prod);

	return 0;
}

void
mwx_dma_txwi_enqueue(struct mwx_softc *sc, struct mwx_queue *q,
    struct mwx_txwi *mt)
{
	struct mwx_queue_data *md;
	struct mwx_desc *desc;
	uint32_t buf0, len0, ctrl;
	int idx;

	idx = q->mq_prod;
	md = &q->mq_data[idx];
	desc = &q->mq_desc[idx];

	/* the caller (mwx_tx) checked for space under sc_txlock */
	md->md_txwi = mt;
	md->md_mbuf = NULL;

	bus_dmamap_sync(sc->sc_dmat, q->mq_map, 0, q->mq_map->dm_mapsize,
	    BUS_DMASYNC_PREWRITE);

	buf0 = mt->mt_addr;
	len0 = sizeof(*mt->mt_desc);
	ctrl = MT_DMA_CTL_SD_LEN0(len0);
	ctrl |= MT_DMA_CTL_LAST_SEC0;

	static uint32_t txwi_dma_log_count;
	if (txwi_dma_log_count < 16) {
		MWX_DEV_LOG(sc, "txwi dma idx=%d addr=0x%08x len=%u\n",
		    idx, buf0, len0);
		txwi_dma_log_count++;
	}

	desc->buf0 = htole32(buf0);
	desc->buf1 = 0;
	desc->info = 0;
	desc->ctrl = htole32(ctrl);

	idx = (idx + 1) % q->mq_count;
	KASSERT(idx != q->mq_cons);

	bus_dmamap_sync(sc->sc_dmat, q->mq_map, 0, q->mq_map->dm_mapsize,
	    BUS_DMASYNC_POSTWRITE);

	q->mq_prod = idx;

	mwx_write(sc, q->mq_regbase + MT_DMA_CPU_IDX, q->mq_prod);
}

int
mwx_dma_tx_enqueue(struct mwx_softc *sc, struct mwx_queue *q, struct mbuf *m)
{
	int rv;

	lck_mtx_lock(sc->sc_txlock);
	rv = mwx_dma_tx_enqueue_locked(sc, q, m);
	lck_mtx_unlock(sc->sc_txlock);
	return rv;
}

static void
mwx_dma_tx_cleanup_locked(struct mwx_softc *sc, struct mwx_queue *q)
{
	struct mwx_queue_data *md;
	struct mwx_desc *desc;
	int idx, last;

	idx = q->mq_cons;
	last = mwx_read(sc, q->mq_regbase + MT_DMA_DMA_IDX);

	if (idx == last)
		return;

	bus_dmamap_sync(sc->sc_dmat, q->mq_map, 0, q->mq_map->dm_mapsize,
	     BUS_DMASYNC_PREWRITE);

	while (idx != last) {
		md = &q->mq_data[idx];
		desc = &q->mq_desc[idx];

		if (md->md_mbuf != NULL) {
			bus_dmamap_sync(sc->sc_dmat, md->md_map, 0,
			    md->md_map->dm_mapsize, BUS_DMASYNC_POSTWRITE);
			bus_dmamap_unload(sc->sc_dmat, md->md_map);
			m_freem(md->md_mbuf);
			md->md_mbuf = NULL;
		}
		if (md->md_txwi != NULL) {
			static uint32_t txwi_ack_log_count;

			/* TXWI cleanup is driven by the firmware TX free event. */
			md->md_txwi = NULL;
			if (txwi_ack_log_count < 32) {
				MWX_DEV_LOG(sc, "txwi acked idx=%d\n", idx);
				txwi_ack_log_count++;
			}
		}

		/* clear DMA desc just to be sure */
		desc->buf0 = 0;
		desc->buf1 = 0;
		desc->info = 0;
		desc->ctrl = htole32(MT_DMA_CTL_DMA_DONE);

		idx = (idx + 1) % q->mq_count;

		/* check if more data made it in */
		/* XXX should we actually do that? */
		if (idx == last)
			last = mwx_read(sc, q->mq_regbase + MT_DMA_DMA_IDX);
	}

	bus_dmamap_sync(sc->sc_dmat, q->mq_map, 0, q->mq_map->dm_mapsize,
	    BUS_DMASYNC_POSTWRITE);

	q->mq_cons = idx;
	if (q->mq_wakeme) {
		q->mq_wakeme = 0;
		wakeup(q);
	}
}

void
mwx_dma_tx_cleanup(struct mwx_softc *sc, struct mwx_queue *q)
{
	lck_mtx_lock(sc->sc_txlock);
	mwx_dma_tx_cleanup_locked(sc, q);
	lck_mtx_unlock(sc->sc_txlock);
}

void
mwx_dma_tx_done(struct mwx_softc *sc)
{
	mwx_dma_tx_cleanup(sc, &sc->sc_txq);
	mwx_dma_tx_cleanup(sc, &sc->sc_txmcuq);
	mwx_dma_tx_cleanup(sc, &sc->sc_txfwdlq);

	/* XXX do we need to wakeup someone */
}

/* XXX wrong place */
void
mwx_dma_rx_process(struct mwx_softc *sc, struct mbuf_list *ml)
{
	struct mbuf_list mlout = MBUF_LIST_INITIALIZER();
	struct mbuf *m;
	uint32_t *data, rxd, type, flag;
	uint32_t event_count = 0, notify_count = 0, normal_mcu_count = 0;
	uint32_t normal_count = 0, unknown_count = 0;
	uint32_t first_unknown_type = 0, first_unknown_flag = 0, first_unknown_rxd = 0;

	while ((m = ml_dequeue(ml)) != NULL) {
		data = mtod(m, uint32_t *);
		rxd = le32toh(data[0]);

		type = MT_RXD0_PKT_TYPE_GET(rxd);
		flag = (rxd & MT_RXD0_PKT_FLAG_MASK) >> MT_RXD0_PKT_FLAG_SHIFT;

		if (type == PKT_TYPE_RX_EVENT && flag == 0x1)
			type = PKT_TYPE_NORMAL_MCU;

		switch (type) {
		case PKT_TYPE_RX_EVENT:
			event_count++;
			mwx_mcu_rx_event(sc, m);
			break;
		case PKT_TYPE_TXRX_NOTIFY:
			notify_count++;
			mwx_mac_tx_free(sc, m);
			break;
		case PKT_TYPE_TXS:
#if TODO
			for (rxd += 2; rxd + 8 <= end; rxd += 8)
				mt7921_mac_add_txs(dev, rxd);
#endif
			m_freem(m);
			break;
		case PKT_TYPE_NORMAL_MCU:
			normal_mcu_count++;
			mwx_rx(sc, m, &mlout);
			break;
		case PKT_TYPE_NORMAL:
			normal_count++;
			mwx_rx(sc, m, &mlout);
			break;
		default:
			if (unknown_count == 0) {
				first_unknown_type = type;
				first_unknown_flag = flag;
				first_unknown_rxd = rxd;
			}
			unknown_count++;
			m_freem(m);
			break;
		}
	}

	if (event_count || notify_count || normal_mcu_count ||
	    normal_count || unknown_count) {
		sc->sc_rx_pkt_event += event_count;
		sc->sc_rx_pkt_txrx_notify += notify_count;
		sc->sc_rx_pkt_normal_mcu += normal_mcu_count;
		sc->sc_rx_pkt_normal += normal_count;
		sc->sc_rx_pkt_unknown += unknown_count;
		if (sc->sc_rx_process_events < 64 ||
		    normal_count != 0 || unknown_count != 0) {
			MWX_DEV_LOG(sc,
			    "rx process event=%u notify=%u normal_mcu=%u normal=%u unknown=%u totals e=%u n=%u nmcu=%u norm=%u unk=%u\n",
			    event_count, notify_count, normal_mcu_count,
			    normal_count, unknown_count, sc->sc_rx_pkt_event,
			    sc->sc_rx_pkt_txrx_notify,
			    sc->sc_rx_pkt_normal_mcu, sc->sc_rx_pkt_normal,
			    sc->sc_rx_pkt_unknown);
			if (unknown_count != 0) {
				MWX_DEV_LOG(sc,
				    "rx unknown first type=%u flag=0x%x rxd0=0x%08x\n",
				    first_unknown_type, first_unknown_flag,
				    first_unknown_rxd);
			}
		}
		sc->sc_rx_process_events++;
	}

	if_input(&sc->sc_ic.ic_if, &mlout);
}

void
mwx_dma_rx_dequeue(struct mwx_softc *sc, struct mwx_queue *q,
    struct mbuf_list *ml)
{
	struct mwx_queue_data *md;
	struct mwx_desc *desc;
	struct mbuf *m, *m0 = NULL, *mtail = NULL;
	int idx, last;

	idx = q->mq_cons;
	last = mwx_read(sc, q->mq_regbase + MT_DMA_DMA_IDX);

	if (idx == last)
		return;

	bus_dmamap_sync(sc->sc_dmat, q->mq_map, 0, q->mq_map->dm_mapsize,
	     BUS_DMASYNC_PREREAD);

	while (idx != last) {
		uint32_t ctrl;

		md = &q->mq_data[idx];
		desc = &q->mq_desc[idx];

		bus_dmamap_sync(sc->sc_dmat, md->md_map, 0,
		    md->md_map->dm_mapsize, BUS_DMASYNC_POSTREAD);
		bus_dmamap_unload(sc->sc_dmat, md->md_map);

		/* dequeue mbuf */
		m = md->md_mbuf;
		md->md_mbuf = NULL;

		/* only buf0 is used on RX rings */
		ctrl = le32toh(desc->ctrl);
		mbuf_setlen(m, MT_DNA_CTL_SD_GET_LEN0(ctrl));

		if (m0 == NULL) {
			m0 = mtail = m;
			mbuf_pkthdr_setlen(m0, mbuf_len(m));
		} else {
			mbuf_setnext(mtail, m);
			mtail = m;
			mbuf_pkthdr_setlen(m0, mbuf_pkthdr_len(m0) + mbuf_len(m));
		}

		/* TODO handle desc->info */

		/* check if this was the last mbuf of the chain */
		if (ctrl & MT_DMA_CTL_LAST_SEC0) {
			ml_enqueue(ml, m0);
			m0 = NULL;
			mtail = NULL;
		}

		idx = (idx + 1) % q->mq_count;

		/* check if more data made it in */
		/* XXX should we actually do that? */
		if (idx == last)
			last = mwx_read(sc, q->mq_regbase + MT_DMA_DMA_IDX);
	}

	/* XXX make sure we don't have half processed data */
	KASSERT(m0 == NULL);

	bus_dmamap_sync(sc->sc_dmat, q->mq_map, 0, q->mq_map->dm_mapsize,
	    BUS_DMASYNC_POSTREAD);

	q->mq_cons = idx;
}

void
mwx_dma_rx_done(struct mwx_softc *sc, struct mwx_queue *q)
{
	struct mbuf_list ml = MBUF_LIST_INITIALIZER();

	mwx_dma_rx_dequeue(sc, q, &ml);

	if (ml_empty(&ml))
		return;

	mwx_queue_fill(sc, q);	/* TODO what if it fails, run timer? */

	mwx_dma_rx_process(sc, &ml);
}

struct mbuf *
mwx_mcu_alloc_msg(size_t len)
{
	const int headspace = sizeof(struct mwx_mcu_txd);
	struct mbuf *m;

	/* Allocate mbuf with enough space */
	m = MCLGETL(NULL, M_DONTWAIT, len + headspace);
	if (m == NULL)
		return NULL;
	mbuf_setlen(m, len + headspace);
	mbuf_pkthdr_setlen(m, len + headspace);
	/*
	 * Callers fill requests field by field (scan, channel domain, TX power),
	 * so start from zero like OpenBSD does; stale cluster memory reached the
	 * firmware as random scan flags and power limits.
	 */
	memset(mbuf_data(m), 0, len + headspace);
	m_adj(m, headspace);

	return m;
}

void
mwx_mcu_set_len(struct mbuf *m, void *end)
{
	uint8_t *start = mtod(m, uint8_t *);
	uint8_t *finish = (uint8_t *)end;
	int len = mbuf_pkthdr_len(m), adj;

	KASSERT(start <= finish && finish - start <= len);
	adj = len - (finish - start);
	m_adj(m, -adj);
}

int
mwx_mcu_send_mbuf(struct mwx_softc *sc, uint32_t cmd, struct mbuf *m, int *seqp)
{
	struct mwx_uni_txd *uni_txd;
	struct mwx_mcu_txd *mcu_txd;
	struct mwx_queue *q;
	uint32_t *txd, val;
	int s, rv, txd_len = 0, mcu_cmd = cmd & MCU_CMD_FIELD_ID_MASK;
	int waited, cons;
	int tot_len, len = mbuf_pkthdr_len(m);
	size_t lead;
	uint8_t seq = 0;

	if (cmd == MCU_CMD_FW_SCATTER) {
		q = &sc->sc_txfwdlq;
		goto enqueue;
	}

	seq = ++sc->sc_mcu_seq & 0x0f;
	if (seq == 0)
		seq = ++sc->sc_mcu_seq & 0x0f;

	KASSERT(seq < nitems(sc->sc_mcu_wait));
	txd_len = cmd & MCU_CMD_FIELD_UNI ? sizeof(*uni_txd) : sizeof(*mcu_txd);
	tot_len = txd_len + len;
	lead = m_leadingspace(m);
	if (lead < (size_t)txd_len) {
		static uint32_t rebuild_log_count;
		struct mbuf *n;

		if (rebuild_log_count < 32) {
			MWX_DEV_LOG(sc,
			    "MCU tx mbuf rebuild cmd=0x%08x len=%d lead=%zu txd_len=%d\n",
			    cmd, len, lead, txd_len);
			rebuild_log_count++;
		}
		n = MCLGETL(NULL, M_DONTWAIT, len + txd_len);
		if (n == NULL) {
			m_freem(m);
			return ENOMEM;
		}
		mbuf_setlen(n, len + txd_len);
		mbuf_pkthdr_setlen(n, len + txd_len);
		if (len != 0 &&
		    mbuf_copydata(m, 0, len, mtod(n, uint8_t *) + txd_len) != 0) {
			m_freem(n);
			m_freem(m);
			return EINVAL;
		}
		m_freem(m);
		m = n;
	} else {
		m = m_prepend(m, txd_len, M_DONTWAIT);
		if (m == NULL)
			return ENOMEM;
	}
	txd = mtod(m, uint32_t *);
	memset(txd, 0, txd_len);

	val = (tot_len & MT_TXD0_TX_BYTES_MASK) |
	    MT_TX_TYPE_CMD | MT_TXD0_Q_IDX(MT_TX_MCU_PORT_RX_Q0);
	txd[0] = htole32(val);

	val = MT_TXD1_LONG_FORMAT | MT_HDR_FORMAT_CMD;
	txd[1] = htole32(val);

	if (cmd & MCU_CMD_FIELD_UNI) {
		uni_txd = (struct mwx_uni_txd *)txd;
		uni_txd->len = htole16(tot_len - sizeof(uni_txd->txd));
		uni_txd->option = MCU_CMD_UNI_EXT_ACK;
		uni_txd->cid = htole16(mcu_cmd);
		uni_txd->s2d_index = CMD_S2D_IDX_H2N;
		uni_txd->pkt_type = MCU_PKT_ID;
		uni_txd->seq = seq;
	} else {
		mcu_txd = (struct mwx_mcu_txd *)txd;
		mcu_txd->len = htole16(tot_len - sizeof(uni_txd->txd));
		mcu_txd->pq_id = htole16(MCU_PQ_ID(MT_TX_PORT_IDX_MCU,
			MT_TX_MCU_PORT_RX_Q0));
		mcu_txd->pkt_type = MCU_PKT_ID;
		mcu_txd->seq = seq;
		mcu_txd->cid = mcu_cmd;
		mcu_txd->s2d_index = CMD_S2D_IDX_H2N;
		mcu_txd->ext_cid = MCU_GET_EXT_CMD(cmd);

		if (mcu_txd->ext_cid || (cmd & MCU_CMD_FIELD_CE)) {
			if (cmd & MCU_CMD_FIELD_QUERY)
				mcu_txd->set_query = MCU_Q_QUERY;
			else
				mcu_txd->set_query = MCU_Q_SET;
			mcu_txd->ext_cid_ack = !!mcu_txd->ext_cid;
		} else {
			mcu_txd->set_query = MCU_Q_NA;
		}
	}

	if (seqp != NULL) {
		memset(&sc->sc_mcu_wait[seq], 0, sizeof(sc->sc_mcu_wait[0]));
		sc->sc_mcu_wait[seq].mcu_cmd = cmd;
		*seqp = seq;
	}
	q = &sc->sc_txmcuq;
enqueue:

	if (cmd != MCU_CMD_FW_SCATTER) {
		static uint32_t mcu_tx_log_count;

		if (mcu_tx_log_count < 96 || (mtk_verbose_scans > 0 &&
		    cmd != MCU_CE_CMD_START_HW_SCAN &&
		    cmd != MCU_CE_CMD_CANCEL_HW_SCAN)) {
			MWX_DEV_LOG(sc,
			    "mcu tx cmd=0x%08x seq=%u len=%d txd_len=%d q_reg=0x%08x\n",
			    cmd, seq, len, txd_len, q->mq_regbase);
			mcu_tx_log_count++;
		}
#ifdef MWX_PACKET_HEXDUMP
		pkt_hex_dump(m);
#endif
		/*
		 * Payload (after the TXD) of the connect-time commands, same layout
		 * as the Linux capture (tools/capture-linux-connect.sh): UNI
		 * DEV/BSS/STA updates and the CE/EXT commands sent while joining.
		 */
		if ((cmd & 0xffff0000) == 0x00020000 ||
		    (sc->sc_ic.ic_state >= IEEE80211_S_AUTH &&
		    cmd != MCU_CE_CMD_START_HW_SCAN &&
		    cmd != MCU_CE_CMD_CANCEL_HW_SCAN && cmd != 0x000400ca)) {
			const uint8_t *pl = mtod(m, const uint8_t *) + txd_len;
			int plen = len > 192 ? 192 : len, o, i;
			char hex[3 * 48 + 1];

			for (o = 0; o < plen; o += 48) {
				int n = plen - o > 48 ? 48 : plen - o;
				for (i = 0; i < n; i++)
					snprintf(hex + 3 * i, 4, "%02x ", pl[o + i]);
				hex[3 * n] = 0;
				MWX_DEV_LOG(sc, "mcu payload cmd=0x%08x len=%d +%d: %s\n",
				    cmd, len, o, hex);
			}
		}
	}

	s = splnet();
	for (waited = 0; ; ) {
		rv = mwx_dma_tx_enqueue(sc, q, m);
		if (rv != EBUSY)
			break;
		/* Reclaim finished slots here as well; don't depend only on the interrupt. */
		cons = q->mq_cons;
		mwx_dma_tx_cleanup(sc, q);
		if (q->mq_cons != cons)
			continue;
		if (waited >= MWX_TX_RING_WAIT_MS) {
			MWX_DEV_LOG(sc, "tx ring full, giving up cmd=0x%08x q_reg=0x%08x prod=%d cons=%d dma_idx=%u cpu_idx=%u\n",
			    cmd, q->mq_regbase, q->mq_prod, q->mq_cons,
			    mwx_read(sc, q->mq_regbase + MT_DMA_DMA_IDX),
			    mwx_read(sc, q->mq_regbase + MT_DMA_CPU_IDX));
			rv = ETIMEDOUT;
			break;
		}
		q->mq_wakeme = 1;
		tsleep_nsec(q, 0, "mwxq", MSEC_TO_NSEC(10));
		waited += 10;
	}
	splx(s);
	if (rv != 0) {
		if (seqp != NULL)
			memset(&sc->sc_mcu_wait[seq], 0,
			    sizeof(sc->sc_mcu_wait[0]));
		m_freem(m);
	}
	return rv;
}

int
mwx_mcu_send_msg(struct mwx_softc *sc, uint32_t cmd, void *data, size_t len,
    int *seqp)
{
	struct mbuf *m;

	m = mwx_mcu_alloc_msg(len);
	if (m == NULL)
		return ENOMEM;

	if (len != 0)
		memcpy(mtod(m, caddr_t), data, len);

	return mwx_mcu_send_mbuf(sc, cmd, m, seqp);
}

int
mwx_mcu_send_wait(struct mwx_softc *sc, uint32_t cmd, void *data, size_t len)
{
	int rv, seq;

	rv = mwx_mcu_send_msg(sc, cmd, data, len, &seq);
	if (rv != 0)
		return rv;
	return mwx_mcu_wait_resp_int(sc, cmd, seq, NULL);
}

int
mwx_mcu_send_mbuf_wait(struct mwx_softc *sc, uint32_t cmd, struct mbuf *m)
{
	int rv, seq;

	rv = mwx_mcu_send_mbuf(sc, cmd, m, &seq);
	if (rv != 0)
		return rv;
	return mwx_mcu_wait_resp_int(sc, cmd, seq, NULL);
}

void
mwx_mcu_rx_event(struct mwx_softc *sc, struct mbuf *m)
{
	struct mwx_mcu_rxd *rxd;
	uint32_t cmd, mcu_int = 0;
	uint8_t rxd_eid, rxd_ext_eid, rxd_seq;
	int len, rxd_size;

	rxd_size = MT7921_MCU_RXD_SIZE;
	if ((m = m_pullup(m, sizeof(*rxd) + rxd_size)) == NULL)
		return;
	m_adj(m, rxd_size);
	rxd = mtod(m, struct mwx_mcu_rxd *);

	if (rxd->ext_eid == MCU_EXT_EVENT_RATE_REPORT) {
		printf("%s: MCU_EXT_EVENT_RATE_REPORT COMMAND\n", DEVNAME(sc));
		m_freem(m);
		return;
	}

	len = le16toh(rxd->len);
	/* make sure all the data is in one mbuf */
	if ((m = m_pullup(m, len)) == NULL) {
		printf("%s: mwx_mcu_rx_event m_pullup failed\n", DEVNAME(sc));
		return;
	}
	/* refetch after pullup */
	rxd = mtod(m, struct mwx_mcu_rxd *);
	rxd_eid = rxd->eid;
	rxd_ext_eid = rxd->ext_eid;
	rxd_seq = rxd->seq;
	m_adj(m, sizeof(*rxd));

	switch (rxd_eid) {
	case MCU_EVENT_SCHED_SCAN_DONE:
	case MCU_EVENT_SCAN_DONE:
		mt7921_mcu_scan_event(sc, m);
		break;
#if 0
	case MCU_EVENT_BSS_BEACON_LOSS:
		mt7921_mcu_connection_loss_event(dev, skb);
		break;
	case MCU_EVENT_BSS_ABSENCE:
		mt7921_mcu_bss_event(dev, skb);
		break;
	case MCU_EVENT_DBG_MSG:
		mt7921_mcu_debug_msg_event(dev, skb);
		break;
#endif
	case MCU_EVENT_COREDUMP:
		/* it makes little sense to write the coredump down */
		if (!sc->sc_resetting)
			printf("%s: coredump event\n", DEVNAME(sc));
		mwx_reset(sc);
		break;
	case MCU_EVENT_LP_INFO:
		mt7921_mcu_low_power_event(sc, m);
		break;
	case MCU_EVENT_TX_DONE:
		mt7921_mcu_tx_done_event(sc, m);
		break;
	case 0x6:
		printf("%s: MAGIC COMMAND\n", DEVNAME(sc));
	default:
		if (rxd_seq == 0 || rxd_seq >= nitems(sc->sc_mcu_wait)) {
			printf("%s: mcu rx bad seq %x\n", DEVNAME(sc),
			    rxd_seq);
			break;
		}

		cmd = sc->sc_mcu_wait[rxd_seq].mcu_cmd;
		if (mtk_verbose_scans > 0 || (cmd != MCU_CE_CMD_START_HW_SCAN &&
		    cmd != MCU_CE_CMD_CANCEL_HW_SCAN))
			MWX_DEV_LOG(sc, "MCU response cmd=0x%08x seq=%u eid=0x%x ext_eid=0x%x len=%d payload=%zu\n",
			    cmd, rxd_seq, rxd_eid, rxd_ext_eid, len, (size_t)mbuf_len(m));

		if (cmd == MCU_CMD_PATCH_SEM_CONTROL ||
		    cmd == MCU_CMD_PATCH_FINISH_REQ) {
			mcu_int = rxd_ext_eid;
			MWX_DEV_LOG(sc, "MCU patch response cmd=0x%08x seq=%u status=0x%x\n",
			    cmd, rxd_seq, mcu_int);
		} else if (cmd == MCU_EXT_CMD_THERMAL_CTRL) {
			if (mbuf_len(m) < sizeof(uint32_t) * 2)
				break;
			mcu_int = le32toh(mtod(m, uint32_t *)[1]);
		} else if (cmd == MCU_EXT_CMD_EFUSE_ACCESS) {
			//ret = mt7921_mcu_parse_eeprom(sc, m);
			printf("%s: mcu resp no handled yet\n", DEVNAME(sc));
		} else if (cmd == MCU_UNI_CMD_DEV_INFO_UPDATE ||
		    cmd == MCU_UNI_CMD_BSS_INFO_UPDATE ||
		    cmd == MCU_UNI_CMD_STA_REC_UPDATE ||
		    cmd == MCU_UNI_CMD_HIF_CTRL ||
		    cmd == MCU_UNI_CMD_OFFLOAD ||
		    cmd == MCU_UNI_CMD_SUSPEND) {
			struct mt7921_mcu_uni_event *event;

			if (mbuf_len(m) < sizeof(*event))
				break;
			event = mtod(m, struct mt7921_mcu_uni_event *);
			mcu_int = le32toh(event->status);
		} else if (cmd == MCU_CE_QUERY_REG_READ) {
			struct mt7921_mcu_reg_event *event;

			if (mbuf_len(m) < sizeof(*event))
				break;
			event = mtod(m, struct mt7921_mcu_reg_event *);
			mcu_int = le32toh(event->val);
		}

		sc->sc_mcu_wait[rxd_seq].mcu_int = mcu_int;
		sc->sc_mcu_wait[rxd_seq].mcu_m = m;
		__atomic_store_n(&sc->sc_mcu_wait[rxd_seq].mcu_done, 1,
		    __ATOMIC_RELEASE);
		wakeup(&sc->sc_mcu_wait[rxd_seq]);
		return;
	}

	m_freem(m);
}

/*
 * The sleep and wakeup glue does not share a lock with the interrupt path, so a
 * wakeup that arrives before the sleep starts is lost. Wait in short slices
 * and check the flag the interrupt path sets instead.
 */
static int
mwx_mcu_wait_done(struct mwx_softc *sc, int seq)
{
	int waited, limit;

	/*
	 * After two unanswered commands in a row the firmware is not responding
	 * (e.g. DMA stalled); give later commands a short chance only, so
	 * teardown and shutdown do not stall 3 s per command.
	 */
	limit = sc->sc_mcu_timeouts >= 2 ? 50 : 3000;
	for (waited = 0; waited < limit; waited += 10) {
		if (__atomic_load_n(&sc->sc_mcu_wait[seq].mcu_done,
		    __ATOMIC_ACQUIRE)) {
			sc->sc_mcu_timeouts = 0;
			return 0;
		}
		/*
		 * The interrupt handler runs on the work loop thread. When the
		 * waiter is that thread (e.g. the network stack calling disable()
		 * at shutdown), no one else can deliver the reply: poll it here.
		 */
		if (gMwxHal != NULL && gMwxHal->mwxOnWorkLoop()) {
			mwx_intr(sc);
			if (__atomic_load_n(&sc->sc_mcu_wait[seq].mcu_done,
			    __ATOMIC_ACQUIRE))
				return 0;
			IOSleep(10);
			continue;
		}
		tsleep_nsec(&sc->sc_mcu_wait[seq], 0, "mwxwait",
		    MSEC_TO_NSEC(10));
	}
	if (__atomic_load_n(&sc->sc_mcu_wait[seq].mcu_done, __ATOMIC_ACQUIRE)) {
		sc->sc_mcu_timeouts = 0;
		return 0;
	}
	sc->sc_mcu_timeouts++;
	return EWOULDBLOCK;
}

int
mwx_mcu_wait_resp_int(struct mwx_softc *sc, uint32_t cmd, int seq,
    uint32_t *val)
{
	int rv;

	KASSERT(seq < nitems(sc->sc_mcu_wait));

	rv = mwx_mcu_wait_done(sc, seq);
	if (rv != 0) {
		printf("%s: command %x timeout\n", DEVNAME(sc), cmd);
		mwx_reset(sc);
		return rv;
	}

	if (sc->sc_mcu_wait[seq].mcu_m != NULL) {
		m_freem(sc->sc_mcu_wait[seq].mcu_m);
		sc->sc_mcu_wait[seq].mcu_m = NULL;
	}
	if (val != NULL)
		*val = sc->sc_mcu_wait[seq].mcu_int;
	return 0;
}

int
mwx_mcu_wait_resp_msg(struct mwx_softc *sc, uint32_t cmd, int seq,
    struct mbuf **mp)
{
	int rv;

	KASSERT(seq < nitems(sc->sc_mcu_wait));

	rv = mwx_mcu_wait_done(sc, seq);
	if (rv != 0) {
		printf("%s: command %x timeout\n", DEVNAME(sc), cmd);
		mwx_reset(sc);
		return rv;
	}
	if (sc->sc_mcu_wait[seq].mcu_m == NULL) {
		printf("%s: command response missing\n", DEVNAME(sc));
		return ENOENT;
	}
	if (mp != NULL)
		*mp = sc->sc_mcu_wait[seq].mcu_m;
	else
		m_freem(sc->sc_mcu_wait[seq].mcu_m);
	sc->sc_mcu_wait[seq].mcu_m = NULL;
	return 0;
}

int
mwx_dma_disable(struct mwx_softc *sc, int force)
{
	int rv;

	/* disable WFDMA0 */
	mwx_clear(sc, MT_WFDMA0_GLO_CFG,
	    MT_WFDMA0_GLO_CFG_TX_DMA_EN | MT_WFDMA0_GLO_CFG_RX_DMA_EN |
	    MT_WFDMA0_GLO_CFG_CSR_DISP_BASE_PTR_CHAIN_EN |
	    MT_WFDMA0_GLO_CFG_OMIT_TX_INFO |
	    MT_WFDMA0_GLO_CFG_OMIT_RX_INFO |
	    MT_WFDMA0_GLO_CFG_OMIT_RX_INFO_PFET2);

	rv = mwx_poll(sc, MT_WFDMA0_GLO_CFG, 0,
	    MT_WFDMA0_GLO_CFG_TX_DMA_BUSY | MT_WFDMA0_GLO_CFG_RX_DMA_BUSY, 100);
	if (!force && rv != 0)
		return rv;

	/* disable DMASHDL and set bypass */
	mwx_clear(sc, MT_WFDMA0_GLO_CFG_EXT0, MT_WFDMA0_CSR_TX_DMASHDL_ENABLE);
	mwx_set(sc, MT_DMASHDL_SW_CONTROL, MT_DMASHDL_DMASHDL_BYPASS);

	if (force) {
		/* reset */
		mwx_clear(sc, MT_WFDMA0_RST, MT_WFDMA0_RST_DMASHDL_ALL_RST |
		    MT_WFDMA0_RST_LOGIC_RST);
		mwx_set(sc, MT_WFDMA0_RST, MT_WFDMA0_RST_DMASHDL_ALL_RST |
		    MT_WFDMA0_RST_LOGIC_RST);
		delay(20);
	}

	return 0;
}

void
mwx_dma_enable(struct mwx_softc *sc)
{
#define PREFETCH(base, depth)   ((base) << 16 | (depth))
	/* configure perfetch settings */
	mwx_write(sc, MT_WFDMA0_RX_RING0_EXT_CTRL, PREFETCH(0x0, 0x4));
	mwx_write(sc, MT_WFDMA0_RX_RING2_EXT_CTRL, PREFETCH(0x40, 0x4));
	mwx_write(sc, MT_WFDMA0_RX_RING3_EXT_CTRL, PREFETCH(0x80, 0x4));
	mwx_write(sc, MT_WFDMA0_RX_RING4_EXT_CTRL, PREFETCH(0xc0, 0x4));
	mwx_write(sc, MT_WFDMA0_RX_RING5_EXT_CTRL, PREFETCH(0x100, 0x4));

	mwx_write(sc, MT_WFDMA0_TX_RING0_EXT_CTRL, PREFETCH(0x140, 0x4));
	mwx_write(sc, MT_WFDMA0_TX_RING1_EXT_CTRL, PREFETCH(0x180, 0x4));
	mwx_write(sc, MT_WFDMA0_TX_RING2_EXT_CTRL, PREFETCH(0x1c0, 0x4));
	mwx_write(sc, MT_WFDMA0_TX_RING3_EXT_CTRL, PREFETCH(0x200, 0x4));
	mwx_write(sc, MT_WFDMA0_TX_RING4_EXT_CTRL, PREFETCH(0x240, 0x4));
	mwx_write(sc, MT_WFDMA0_TX_RING5_EXT_CTRL, PREFETCH(0x280, 0x4));
	mwx_write(sc, MT_WFDMA0_TX_RING6_EXT_CTRL, PREFETCH(0x2c0, 0x4));
	mwx_write(sc, MT_WFDMA0_TX_RING16_EXT_CTRL, PREFETCH(0x340, 0x4));
	mwx_write(sc, MT_WFDMA0_TX_RING17_EXT_CTRL, PREFETCH(0x380, 0x4));

	/* reset dma idx */
	mwx_write(sc, MT_WFDMA0_RST_DTX_PTR, ~0);

	/* configure delay interrupt */
	mwx_write(sc, MT_WFDMA0_PRI_DLY_INT_CFG0, 0);

	mwx_set(sc, MT_WFDMA0_GLO_CFG,
	    MT_WFDMA0_GLO_CFG_TX_WB_DDONE |
	    MT_WFDMA0_GLO_CFG_FIFO_LITTLE_ENDIAN |
	    MT_WFDMA0_GLO_CFG_CLK_GAT_DIS |
	    MT_WFDMA0_GLO_CFG_OMIT_TX_INFO |
	    MT_WFDMA0_GLO_CFG_DMA_SIZE(3) |
	    MT_WFDMA0_GLO_CFG_FIFO_DIS_CHECK |
	    MT_WFDMA0_GLO_CFG_RX_WB_DDONE |
	    MT_WFDMA0_GLO_CFG_CSR_DISP_BASE_PTR_CHAIN_EN |
	    MT_WFDMA0_GLO_CFG_OMIT_RX_INFO_PFET2);

	mwx_barrier(sc);
	mwx_set(sc, MT_WFDMA0_GLO_CFG,
	    MT_WFDMA0_GLO_CFG_TX_DMA_EN | MT_WFDMA0_GLO_CFG_RX_DMA_EN);

	mwx_set(sc, MT_WFDMA_DUMMY_CR, MT_WFDMA_NEED_REINIT);

	/* enable interrupts for TX/RX rings */
	mwx_write(sc, MT_WFDMA0_HOST_INT_ENA, sc->sc_intr_mask);
	mwx_set(sc, MT_MCU2HOST_SW_INT_ENA, MT_MCU_CMD_WAKE_RX_PCIE);
	mwx_write(sc, MT_PCIE_MAC_INT_ENABLE, 0xff);
}

int
mwx_mcu_fw_pmctrl(struct mwx_softc *sc)
{
	int i;

	for (i = 0; i < MWX_MCU_INIT_RETRY_COUNT; i++) {
		mwx_write(sc, MT_CONN_ON_LPCTL, PCIE_LPCR_HOST_SET_OWN);
		if (mwx_poll(sc, MT_CONN_ON_LPCTL, PCIE_LPCR_HOST_OWN_SYNC,
		    4, 50) == 0)
			break;
	}

	if (i == MWX_MCU_INIT_RETRY_COUNT) {
		printf("%s: firmware own failed\n", DEVNAME(sc));
		return EIO;
	}

	return 0;
}

int
mwx_mcu_drv_pmctrl(struct mwx_softc *sc)
{
	int i;

	for (i = 0; i < MWX_MCU_INIT_RETRY_COUNT; i++) {
		mwx_write(sc, MT_CONN_ON_LPCTL, PCIE_LPCR_HOST_CLR_OWN);
		delay(3000);
		if (mwx_poll(sc, MT_CONN_ON_LPCTL, 0,
		    PCIE_LPCR_HOST_OWN_SYNC, 50) == 0)
			break;
	}

	if (i == MWX_MCU_INIT_RETRY_COUNT) {
		printf("%s: driver own failed\n", DEVNAME(sc));
		return EIO;
	}

	return 0;
}

int
mwx_wfsys_reset(struct mwx_softc *sc)
{
	DPRINTF("%s: WFSYS reset\n", DEVNAME(sc));

	mwx_clear(sc, MT_WFSYS_SW_RST_B, WFSYS_SW_RST_B);
	delay(50 * 1000);
	mwx_set(sc, MT_WFSYS_SW_RST_B, WFSYS_SW_RST_B);

	return mwx_poll(sc, MT_WFSYS_SW_RST_B, WFSYS_SW_INIT_DONE,
	    WFSYS_SW_INIT_DONE, 500);
}

/*
 * To be honest this is ridiculous.
 */
uint32_t
mwx_reg_addr(struct mwx_softc *sc, uint32_t reg)
{
	static const struct {
		uint32_t phys;
		uint32_t mapped;
		uint32_t size;
	} fixed_map[] = {
	{ 0x820d0000, 0x30000, 0x10000 }, /* WF_LMAC_TOP (WF_WTBLON) */
	{ 0x820ed000, 0x24800, 0x00800 }, /* WF_LMAC_TOP BN0 (WF_MIB) */
	{ 0x820e4000, 0x21000, 0x00400 }, /* WF_LMAC_TOP BN0 (WF_TMAC) */
	{ 0x820e7000, 0x21e00, 0x00200 }, /* WF_LMAC_TOP BN0 (WF_DMA) */
	{ 0x820eb000, 0x24200, 0x00400 }, /* WF_LMAC_TOP BN0 (WF_LPON) */
	{ 0x820e2000, 0x20800, 0x00400 }, /* WF_LMAC_TOP BN0 (WF_AGG) */
	{ 0x820e3000, 0x20c00, 0x00400 }, /* WF_LMAC_TOP BN0 (WF_ARB) */
	{ 0x820e5000, 0x21400, 0x00800 }, /* WF_LMAC_TOP BN0 (WF_RMAC) */
	{ 0x00400000, 0x80000, 0x10000 }, /* WF_MCU_SYSRAM */
	{ 0x00410000, 0x90000, 0x10000 }, /* WF_MCU_SYSRAM (conf register) */
	{ 0x40000000, 0x70000, 0x10000 }, /* WF_UMAC_SYSRAM */
	{ 0x54000000, 0x02000, 0x01000 }, /* WFDMA PCIE0 MCU DMA0 */
	{ 0x55000000, 0x03000, 0x01000 }, /* WFDMA PCIE0 MCU DMA1 */
	{ 0x58000000, 0x06000, 0x01000 }, /* WFDMA PCIE1 MCU DMA0 (MEM_DMA) */
	{ 0x59000000, 0x07000, 0x01000 }, /* WFDMA PCIE1 MCU DMA1 */
	{ 0x7c000000, 0xf0000, 0x10000 }, /* CONN_INFRA */
	{ 0x7c020000, 0xd0000, 0x10000 }, /* CONN_INFRA, WFDMA */
	{ 0x7c060000, 0xe0000, 0x10000 }, /* CONN_INFRA, conn_host_csr_top */
	{ 0x80020000, 0xb0000, 0x10000 }, /* WF_TOP_MISC_OFF */
	{ 0x81020000, 0xc0000, 0x10000 }, /* WF_TOP_MISC_ON */
	{ 0x820c0000, 0x08000, 0x04000 }, /* WF_UMAC_TOP (PLE) */
	{ 0x820c8000, 0x0c000, 0x02000 }, /* WF_UMAC_TOP (PSE) */
	{ 0x820cc000, 0x0e000, 0x01000 }, /* WF_UMAC_TOP (PP) */
	{ 0x820cd000, 0x0f000, 0x01000 }, /* WF_MDP_TOP */
	{ 0x820ce000, 0x21c00, 0x00200 }, /* WF_LMAC_TOP (WF_SEC) */
	{ 0x820cf000, 0x22000, 0x01000 }, /* WF_LMAC_TOP (WF_PF) */
	{ 0x820e0000, 0x20000, 0x00400 }, /* WF_LMAC_TOP BN0 (WF_CFG) */
	{ 0x820e1000, 0x20400, 0x00200 }, /* WF_LMAC_TOP BN0 (WF_TRB) */
	{ 0x820e9000, 0x23400, 0x00200 }, /* WF_LMAC_TOP BN0 (WF_WTBLOFF) */
	{ 0x820ea000, 0x24000, 0x00200 }, /* WF_LMAC_TOP BN0 (WF_ETBF) */
	{ 0x820ec000, 0x24600, 0x00200 }, /* WF_LMAC_TOP BN0 (WF_INT) */
	{ 0x820f0000, 0xa0000, 0x00400 }, /* WF_LMAC_TOP BN1 (WF_CFG) */
	{ 0x820f1000, 0xa0600, 0x00200 }, /* WF_LMAC_TOP BN1 (WF_TRB) */
	{ 0x820f2000, 0xa0800, 0x00400 }, /* WF_LMAC_TOP BN1 (WF_AGG) */
	{ 0x820f3000, 0xa0c00, 0x00400 }, /* WF_LMAC_TOP BN1 (WF_ARB) */
	{ 0x820f4000, 0xa1000, 0x00400 }, /* WF_LMAC_TOP BN1 (WF_TMAC) */
	{ 0x820f5000, 0xa1400, 0x00800 }, /* WF_LMAC_TOP BN1 (WF_RMAC) */
	{ 0x820f7000, 0xa1e00, 0x00200 }, /* WF_LMAC_TOP BN1 (WF_DMA) */
	{ 0x820f9000, 0xa3400, 0x00200 }, /* WF_LMAC_TOP BN1 (WF_WTBLOFF) */
	{ 0x820fa000, 0xa4000, 0x00200 }, /* WF_LMAC_TOP BN1 (WF_ETBF) */
	{ 0x820fb000, 0xa4200, 0x00400 }, /* WF_LMAC_TOP BN1 (WF_LPON) */
	{ 0x820fc000, 0xa4600, 0x00200 }, /* WF_LMAC_TOP BN1 (WF_INT) */
	{ 0x820fd000, 0xa4800, 0x00800 }, /* WF_LMAC_TOP BN1 (WF_MIB) */
	};
	int i;

	if (reg < 0x100000)
		return reg;

	for (i = 0; i < nitems(fixed_map); i++) {
		uint32_t ofs;

		if (reg < fixed_map[i].phys)
			continue;

		ofs = reg - fixed_map[i].phys;
		if (ofs > fixed_map[i].size)
			continue;

		return fixed_map[i].mapped + ofs;
	}

	if ((reg >= 0x18000000 && reg < 0x18c00000) ||
	    (reg >= 0x70000000 && reg < 0x78000000) ||
	    (reg >= 0x7c000000 && reg < 0x7c400000))
		return mt7921_map_reg_l1(sc, reg);

	panic("%s: Access to currently unsupported address %08x\n",
	    DEVNAME(sc), reg);
}

int
mwx_init_hardware(struct mwx_softc *sc)
{
	int rv;

	MWX_DEV_LOG(sc, "hardware init start\n");
	sc->sc_mcu_timeouts = 0;	/* fresh firmware: full command timeouts again */
	/* reset dma */
	rv = mwx_dma_reset(sc, 1);
	if (rv != 0) {
		MWX_DEV_LOG(sc, "hardware init dma reset failed error=%d\n", rv);
		return rv;
	}
	MWX_DEV_LOG(sc, "hardware init dma reset ok\n");

	/*
	 * force firmware operation mode into normal state,
	 * which should be set before firmware download stage.
	 */
	mwx_write(sc, MT_SWDEF_MODE, MT_SWDEF_NORMAL_MODE);
	mwx_barrier(sc);
	MWX_DEV_LOG(sc, "hardware init normal firmware mode set\n");

	rv = mt7921_mcu_init(sc);
	if (rv != 0) {
		MWX_DEV_LOG(sc, "hardware init MCU init failed error=%d\n", rv);
		goto fail;
	}
	MWX_DEV_LOG(sc, "hardware init MCU init ok\n");
	/* TODO override eeprom for systems with FDT */
	rv = mt7921_mcu_set_eeprom(sc);
	if (rv != 0) {
		MWX_DEV_LOG(sc, "hardware init EEPROM setup failed error=%d\n", rv);
		goto fail;
	}
	MWX_DEV_LOG(sc, "hardware init EEPROM setup ok\n");
	mwx_mac_init(sc);

	sc->sc_vif.vif_mn.wcid = MWX_WTBL_RESERVED;

	/* MAYBE alloc beacon and mgmt frame wcid 0 here */

	MWX_DEV_LOG(sc, "hardware init complete\n");
	return 0;

 fail:
	/* reset dma */
	MWX_DEV_LOG(sc, "hardware init recovery dma reset start\n");
	rv = mwx_dma_reset(sc, 1);
	if (rv != 0) {
		MWX_DEV_LOG(sc, "hardware init recovery dma reset failed error=%d\n",
		    rv);
		return rv;
	}
	return EAGAIN;
}

int
mt7921_mcu_init(struct mwx_softc *sc)
{
	int rv;

	MWX_DEV_LOG(sc, "MCU init start\n");
	/* this read is needed to make interrupts work */
	(void) mwx_read(sc, MT_TOP_LPCR_HOST_BAND0);
	mwx_write(sc, MT_TOP_LPCR_HOST_BAND0, MT_TOP_LPCR_HOST_DRV_OWN);
	if (mwx_poll(sc, MT_TOP_LPCR_HOST_BAND0, 0, MT_TOP_LPCR_HOST_FW_OWN,
	    5000) != 0) {
	    MWX_DEV_LOG(sc, "MCU init timeout for driver own\n");
	    return EIO;
	}
	MWX_DEV_LOG(sc, "MCU init driver own ok\n");

	mwx_set(sc, MT_PCIE_MAC_PM, MT_PCIE_MAC_PM_L0S_DIS);

	if ((rv = mwx_load_firmware(sc)) != 0) {
		MWX_DEV_LOG(sc, "MCU init firmware load failed error=%d\n", rv);
		return rv;
	}
	MWX_DEV_LOG(sc, "MCU init firmware load ok\n");

	if ((rv = mt7921_mcu_get_nic_capability(sc)) != 0) {
		MWX_DEV_LOG(sc, "MCU init NIC capability failed error=%d\n", rv);
		return rv;
	}
	MWX_DEV_LOG(sc, "MCU init NIC capability ok\n");
	if ((rv = mt7921_mcu_fw_log_2_host(sc, 1)) != 0) {
		MWX_DEV_LOG(sc, "MCU init firmware log enable failed error=%d\n",
		    rv);
		return rv;
	}

	/* TODO mark MCU running */

	MWX_DEV_LOG(sc, "MCU init complete\n");
	return 0;
}

static inline uint32_t
mwx_get_data_mode(struct mwx_softc *sc, uint32_t info)
{
	uint32_t mode = DL_MODE_NEED_RSP;

	if (info == PATCH_SEC_NOT_SUPPORT)
		return mode;
	switch (info & PATCH_SEC_ENC_TYPE_MASK) {
	case PATCH_SEC_ENC_TYPE_PLAIN:
		break;
	case PATCH_SEC_ENC_TYPE_AES:
		mode |= DL_MODE_ENCRYPT;
		mode |= (info << DL_MODE_KEY_IDX_SHIFT) & DL_MODE_KEY_IDX_MASK;
		mode |= DL_MODE_RESET_SEC_IV;
		break;
	case PATCH_SEC_ENC_TYPE_SCRAMBLE:
		mode |= DL_MODE_ENCRYPT;
		mode |= DL_CONFIG_ENCRY_MODE_SEL;
		mode |= DL_MODE_RESET_SEC_IV;
		break;
	default:
		printf("%s: encryption type not supported\n", DEVNAME(sc));
	}
	return mode;
}

static inline uint32_t
mwx_mcu_gen_dl_mode(uint8_t feature_set)
{
	uint32_t ret = DL_MODE_NEED_RSP;

	if (feature_set & FW_FEATURE_SET_ENCRYPT)
		ret |= (DL_MODE_ENCRYPT | DL_MODE_RESET_SEC_IV);
	if (feature_set & FW_FEATURE_ENCRY_MODE)
		ret |= DL_CONFIG_ENCRY_MODE_SEL;

	/* FW_FEATURE_SET_KEY_IDX_MASK == DL_MODE_KEY_IDX_MASK */
	ret |= feature_set & FW_FEATURE_SET_KEY_IDX_MASK;

	return ret;
}


int
mwx_load_firmware(struct mwx_softc *sc)
{
	struct mwx_patch_hdr *hdr;
	struct mwx_fw_trailer *fwhdr;
	const char *rompatch, *fw;
	u_char *buf = NULL, *fwbuf = NULL, *dl;
	size_t buflen = 0, fwlen = 0, offset = 0;
	uint32_t reg, n_region, override = 0, option = 0;
	int i, rv, sem;

	MWX_DEV_LOG(sc, "firmware load start hwtype=%s\n",
	    sc->sc_hwtype == MWX_HW_MT7922 ? "MT7922" : "MT7921");
	/*
	 * Like Linux (mt792x_load_firmware), always download after the WFSYS
	 * reset. The N9 ready flag survives a warm reboot while the RAM
	 * firmware does not; skipping the download then leaves the ROM to
	 * answer GET_NIC_CAPAB with an empty event.
	 */
	reg = mwx_read(sc, MT_CONN_ON_MISC) & MT_TOP_MISC2_FW_N9_RDY;
	MWX_DEV_LOG(sc, "firmware ready flag before download=0x%08x\n", reg);

	switch (sc->sc_hwtype) {
	case MWX_HW_MT7921:
		rompatch = MT7921_ROM_PATCH;
		fw = MT7921_FIRMWARE_WM;
		break;
	case MWX_HW_MT7922:
		rompatch = MT7922_ROM_PATCH;
		fw = MT7922_FIRMWARE_WM;
		break;
	}
	MWX_DEV_LOG(sc, "firmware selected patch=%s wm=%s\n", rompatch, fw);

	if ((rv = loadfirmware(rompatch, &buf, &buflen)) != 0) {
		MWX_DEV_LOG(sc, "patch firmware load failed name=%s error=%d\n",
		    rompatch, rv);
		return rv;
	}
	if ((rv = loadfirmware(fw, &fwbuf, &fwlen)) != 0) {
		MWX_DEV_LOG(sc, "WM firmware load failed name=%s error=%d\n",
		    fw, rv);
		free(buf, M_DEVBUF, buflen);
		return rv;
	}
	MWX_DEV_LOG(sc, "firmware blobs loaded patch_size=%zu wm_size=%zu\n",
	    buflen, fwlen);

	rv = mwx_mcu_patch_sem_ctrl(sc, 1);
	if (rv == -1) {
		/* PATCH_IS_DL: Linux skips the patch without releasing the semaphore. */
		MWX_DEV_LOG(sc, "patch already downloaded, loading WM firmware only\n");
		goto load_ram;
	}
	if (rv != 0) {
		MWX_DEV_LOG(sc, "patch semaphore acquire failed error=%d\n", rv);
		goto fail;
	}
	MWX_DEV_LOG(sc, "patch semaphore acquired\n");

	if (buflen < sizeof(*hdr)) {
		MWX_DEV_LOG(sc, "invalid patch firmware size=%zu min=%zu\n",
		    buflen, sizeof(*hdr));
		rv = EINVAL;
		goto out;
	}
	hdr = (struct mwx_patch_hdr *)buf;
	n_region = be32toh(hdr->desc.n_region);
	if (buflen < sizeof(*hdr) + n_region * sizeof(struct mwx_patch_sec)) {
		MWX_DEV_LOG(sc, "invalid patch firmware short header size=%zu regions=%u\n",
		    buflen, n_region);
		rv = EINVAL;
		goto out;
	}
	MWX_DEV_LOG(sc, "patch header hw_sw=0x%x patch_ver=0x%x regions=%u build=%.15s\n",
	    be32toh(hdr->hw_sw_ver), be32toh(hdr->patch_ver),
	    n_region, hdr->build_date);

	for (i = 0; i < n_region; i++) {
		struct mwx_patch_sec *sec;
		uint32_t len, addr, mode, sec_info;

		sec = (struct mwx_patch_sec *)(buf + sizeof(*hdr) +
		    i * sizeof(*sec));
		if ((be32toh(sec->type) & PATCH_SEC_TYPE_MASK) !=
		    PATCH_SEC_TYPE_INFO) {
			MWX_DEV_LOG(sc, "invalid patch region index=%d type=0x%x\n",
			    i, be32toh(sec->type));
			rv = EINVAL;
			goto out;
		}

		addr = be32toh(sec->info.addr);
		len = be32toh(sec->info.len);
		dl = buf + be32toh(sec->offs);
		sec_info = be32toh(sec->info.sec_key_idx);
		mode = mwx_get_data_mode(sc, sec_info);
		MWX_DEV_LOG(sc, "patch region index=%d addr=0x%08x len=%u mode=0x%08x sec=0x%08x\n",
		    i, addr, len, mode, sec_info);

		if (dl + len > buf + buflen) {
			MWX_DEV_LOG(sc, "patch region index=%d exceeds payload len=%u\n",
			    i, len);
			rv = EINVAL;
			goto out;
		}

		rv = mwx_mcu_init_download(sc, addr, len, mode);
		if (rv != 0) {
			MWX_DEV_LOG(sc, "patch download request failed index=%d error=%d\n",
			    i, rv);
			goto out;
		}
		rv = mwx_mcu_send_firmware(sc, MCU_CMD_FW_SCATTER,
		    dl, len, MWX_FW_CHUNK_LEN);
		if (rv != 0) {
			MWX_DEV_LOG(sc, "patch send failed index=%d error=%d\n",
			    i, rv);
			goto out;
		}
	}

	delay(5000);
	rv = mwx_mcu_start_patch(sc);
	if (rv != 0) {
		MWX_DEV_LOG(sc, "patch start failed error=%d\n", rv);
		goto out;
	}
	MWX_DEV_LOG(sc, "patch start ok\n");

out:
	sem = mwx_mcu_patch_sem_ctrl(sc, 0);
	if (sem != 0)
		rv = sem;
	if (rv != 0)
		goto fail;

load_ram:
	if (fwlen < sizeof(*fwhdr)) {
		MWX_DEV_LOG(sc, "invalid WM firmware size=%zu min=%zu\n",
		    fwlen, sizeof(*fwhdr));
		rv = EINVAL;
		goto fail;
	}
	fwhdr = (struct mwx_fw_trailer *)(fwbuf + fwlen - sizeof(*fwhdr));
	MWX_DEV_LOG(sc, "WM header version=%.10s build=%.15s chip=0x%02x eco=0x%02x regions=%u\n",
	    fwhdr->fw_ver, fwhdr->build_date, fwhdr->chip_id,
	    fwhdr->eco_code, fwhdr->n_region);

	if (fwlen < sizeof(*fwhdr) +
	    fwhdr->n_region * sizeof(struct mwx_fw_region)) {
		MWX_DEV_LOG(sc, "invalid WM firmware short header size=%zu regions=%u\n",
		    fwlen, fwhdr->n_region);
		rv = EINVAL;
		goto fail;
	}

	for (i = 0; i < fwhdr->n_region; i++) {
		struct mwx_fw_region *region;
		uint32_t len, addr, mode;

		region = (struct mwx_fw_region *)((u_char *)fwhdr -
		    (fwhdr->n_region - i) * sizeof(*region));

		addr = le32toh(region->addr);
		len = le32toh(region->len);
		mode = mwx_mcu_gen_dl_mode(region->feature_set);

		if (region->feature_set & FW_FEATURE_OVERRIDE_ADDR)
			override = addr;
		MWX_DEV_LOG(sc, "WM region index=%d addr=0x%08x len=%u mode=0x%08x feature=0x%02x offset=%zu\n",
		    i, addr, len, mode, region->feature_set, offset);

		if (offset + len > fwlen - sizeof(*fwhdr) -
		    fwhdr->n_region * sizeof(struct mwx_fw_region)) {
			MWX_DEV_LOG(sc, "WM region index=%d exceeds payload len=%u offset=%zu\n",
			    i, len, offset);
			rv = EINVAL;
			goto fail;
		}

		if (region->feature_set & FW_FEATURE_NON_DL) {
			offset += len;
			continue;
		}

		rv = mwx_mcu_init_download(sc, addr, len, mode);
		if (rv != 0) {
			MWX_DEV_LOG(sc, "WM download request failed index=%d error=%d\n",
			    i, rv);
			goto fail;
		}

		rv = mwx_mcu_send_firmware(sc, MCU_CMD_FW_SCATTER,
		    fwbuf + offset, len, MWX_FW_CHUNK_LEN);
		if (rv != 0) {
			MWX_DEV_LOG(sc, "WM send failed index=%d error=%d\n",
			    i, rv);
			goto fail;
		}
		offset += len;
	}

	if (override != 0)
		option |= FW_START_OVERRIDE;

	rv = mwx_mcu_start_firmware(sc, override, option);
	if (rv != 0) {
		MWX_DEV_LOG(sc, "firmware start failed override=0x%08x option=0x%08x error=%d\n",
		    override, option, rv);
		goto fail;
	}
	MWX_DEV_LOG(sc, "firmware start ok override=0x%08x option=0x%08x\n",
	    override, option);

	/* XXX should not busy poll here */
	if (mwx_poll(sc, MT_CONN_ON_MISC, MT_TOP_MISC2_FW_N9_RDY,
	    MT_TOP_MISC2_FW_N9_RDY, 1500) != 0) {
		MWX_DEV_LOG(sc, "firmware ready poll timed out\n");
		rv = EIO;
		goto fail;
	}

	MWX_DEV_LOG(sc, "firmware loaded and ready\n");
	rv = 0;

fail:
	if (buf != NULL)
		free(buf, M_DEVBUF, buflen);
	if (fwbuf != NULL)
		free(fwbuf, M_DEVBUF, fwlen);
	return rv;
}

int
mwx_mac_wtbl_update(struct mwx_softc *sc, int idx)
{
	mwx_rmw(sc, MT_WTBL_UPDATE,
	    (idx & MT_WTBL_UPDATE_WLAN_IDX_MASK) | MT_WTBL_UPDATE_ADM_COUNT_CLEAR,
	    MT_WTBL_UPDATE_WLAN_IDX_MASK);

	return mwx_poll(sc, MT_WTBL_UPDATE, 0, MT_WTBL_UPDATE_BUSY, 5000);
}

void
mwx_mac_init_band(struct mwx_softc *sc, uint32_t band)
{
	mwx_rmw(sc, MT_TMAC_CTCR0(band), 0x3f, MT_TMAC_CTCR0_INS_DDLMT_REFTIME);
	mwx_set(sc, MT_TMAC_CTCR0(band),
	    MT_TMAC_CTCR0_INS_DDLMT_VHT_SMPDU_EN |
	    MT_TMAC_CTCR0_INS_DDLMT_EN);

	mwx_set(sc, MT_WF_RMAC_MIB_TIME0(band), MT_WF_RMAC_MIB_RXTIME_EN);
	mwx_set(sc, MT_WF_RMAC_MIB_AIRTIME0(band), MT_WF_RMAC_MIB_RXTIME_EN);

	/* enable MIB tx-rx time reporting */
	mwx_set(sc, MT_MIB_SCR1(band), MT_MIB_TXDUR_EN);
	mwx_set(sc, MT_MIB_SCR1(band), MT_MIB_RXDUR_EN);

	mwx_rmw(sc, MT_DMA_DCR0(band),
	    1536 << MT_DMA_DCR0_MAX_RX_LEN_SHIFT, MT_DMA_DCR0_MAX_RX_LEN_MASK);
	/* disable rx rate report by default due to hw issues */
	mwx_clear(sc, MT_DMA_DCR0(band), MT_DMA_DCR0_RXD_G5_EN);

	/* filter out non-resp frames and get instantaneous signal reporting */
	mwx_rmw(sc, MT_WTBLOFF_TOP_RSCR(band),
	    0 << MT_WTBLOFF_TOP_RSCR_RCPI_MODE_SHIFT |
	    0x3 << MT_WTBLOFF_TOP_RSCR_RCPI_PARAM_SHIFT,
	    MT_WTBLOFF_TOP_RSCR_RCPI_MODE_MASK |
	    MT_WTBLOFF_TOP_RSCR_RCPI_PARAM_MASK);
}

void
mwx_mac_init(struct mwx_softc *sc)
{
	int i;

	mwx_rmw(sc, MT_MDP_DCR1, 1536 << MT_MDP_DCR1_MAX_RX_LEN_SHIFT,
	    MT_MDP_DCR1_MAX_RX_LEN_MASK);

	/* enable hardware de-agg */
	mwx_set(sc, MT_MDP_DCR0, MT_MDP_DCR0_DAMSDU_EN);
#if 0
	/* not enabled since our stack does not handle 802.3 frames */
	/* enable hardware rx header translation */
	mwx_set(sc, MT_MDP_DCR0, MT_MDP_DCR0_RX_HDR_TRANS_EN);
#endif

	for (i = 0; i < MWX_WTBL_SIZE; i++)
		mwx_mac_wtbl_update(sc, i);

	mwx_mac_init_band(sc, 0);
	mwx_mac_init_band(sc, 1);

	sc->sc_rxfilter = mwx_read(sc, MT_WF_RFCR(0));
}

int
mwx_mcu_patch_sem_ctrl(struct mwx_softc *sc, int semget)
{
#define	PATCH_SEM_RELEASE		0
#define PATCH_SEM_GET			1
#define	PATCH_NOT_DL_SEM_FAIL		0
#define	PATCH_IS_DL			1
#define	PATCH_NOT_DL_SEM_SUCCESS	2
#define	PATCH_REL_SEM_SUCCESS		3

	uint32_t op = semget ? PATCH_SEM_GET : PATCH_SEM_RELEASE;
	struct {
		uint32_t op;
	} req = {
		.op = htole32(op),
	};
	int rv, seq;
	uint32_t sem;

	rv = mwx_mcu_send_msg(sc, MCU_CMD_PATCH_SEM_CONTROL,
	    &req, sizeof(req), &seq);
	if (rv != 0)
		return rv;

	rv = mwx_mcu_wait_resp_int(sc, MCU_CMD_PATCH_SEM_CONTROL, seq, &sem);
	if (rv != 0)
		return rv;

	if (semget) {
		switch (sem) {
		case PATCH_IS_DL:
			return -1;
		case PATCH_NOT_DL_SEM_SUCCESS:
			return 0;
		default:
			DPRINTF("%s: failed to %s patch semaphore\n",
			    DEVNAME(sc), "get");
			return EAGAIN;
		}
	} else {
		switch (sem) {
		case PATCH_REL_SEM_SUCCESS:
			return 0;
		default:
			DPRINTF("%s: failed to %s patch semaphore\n",
			    DEVNAME(sc), "release");
			return EAGAIN;
		}
	}
}

int
mwx_mcu_init_download(struct mwx_softc *sc, uint32_t addr,
    uint32_t len, uint32_t mode)
{
	struct {
		uint32_t addr;
		uint32_t len;
		uint32_t mode;
	} req = {
		.addr = htole32(addr),
		.len = htole32(len),
		.mode = htole32(mode),
	};
	int cmd;

	if (addr == 0x200000 || addr == 0x900000)
		cmd = MCU_CMD_PATCH_START_REQ;
	else
		cmd = MCU_CMD_TARGET_ADDRESS_LEN_REQ;

	return mwx_mcu_send_wait(sc, cmd, &req, sizeof(req));
}

int
mwx_mcu_send_firmware(struct mwx_softc *sc, int cmd, u_char *data,
    size_t len, size_t max_len)
{
	size_t cur_len;
	int rv;

	size_t total = len;
	int chunks = 0;

	while (len > 0) {
		cur_len = len;
		if (cur_len > max_len)
			cur_len = max_len;

		rv = mwx_mcu_send_msg(sc, cmd, data, cur_len, NULL);
		if (rv != 0) {
			MWX_DEV_LOG(sc, "firmware chunk %d failed sent=%zu/%zu error=%d\n",
			    chunks, total - len, total, rv);
			return rv;
		}

		data += cur_len;
		len -= cur_len;
		chunks++;

		mwx_dma_tx_cleanup(sc, &sc->sc_txfwdlq);
	}
	MWX_DEV_LOG(sc, "firmware data sent len=%zu chunks=%d\n", total, chunks);

	return 0;
}

int
mwx_mcu_start_patch(struct mwx_softc *sc)
{
	struct {
		uint8_t check_crc;
		uint8_t reserved[3];
	} req = {
		.check_crc = 0,
	};

	return mwx_mcu_send_wait(sc, MCU_CMD_PATCH_FINISH_REQ, &req,
	    sizeof(req));
}

int
mwx_mcu_start_firmware(struct mwx_softc *sc, uint32_t addr, uint32_t option)
{
	struct {
		uint32_t option;
		uint32_t addr;
	} req = {
		.option = htole32(option),
		.addr = htole32(addr),
	};

	return mwx_mcu_send_wait(sc, MCU_CMD_FW_START_REQ, &req, sizeof(req));
}

int
mt7921_mcu_get_nic_capability(struct mwx_softc *sc)
{
	struct mt76_connac_cap_hdr {
		uint16_t	n_elements;
		uint16_t	pad;
	} __packed *hdr;
	struct tlv_hdr {
		uint32_t	type;
		uint32_t	len;
	} __packed *tlv;
	struct mt76_connac_phy_cap {
		uint8_t		ht;
		uint8_t		vht;
		uint8_t		_5g;
		uint8_t		max_bw;
		uint8_t		nss;
		uint8_t		dbdc;
		uint8_t		tx_ldpc;
		uint8_t		rx_ldpc;
		uint8_t		tx_stbc;
		uint8_t		rx_stbc;
		uint8_t		hw_path;
		uint8_t		he;
	} __packed *cap;
	struct mbuf *m;
	int rv, seq, count, i;

	rv = mwx_mcu_send_msg(sc, MCU_CE_CMD_GET_NIC_CAPAB, NULL, 0, &seq);
	if (rv != 0)
		return rv;

	rv = mwx_mcu_wait_resp_msg(sc, MCU_CE_CMD_GET_NIC_CAPAB, seq, &m);
	if (rv != 0)
		return rv;

	/* the message was already pulled up by mwx_mcu_rx_event() */
	if (mbuf_len(m) < sizeof(*hdr)) {
		printf("%s: GET_NIC_CAPAB response size error len=%zu\n",
		    DEVNAME(sc), (size_t)mbuf_len(m));
		m_freem(m);
		return EINVAL;
	}
	hdr = mtod(m, struct mt76_connac_cap_hdr *);
	count = le16toh(hdr->n_elements);
	m_adj(m, sizeof(*hdr));

	for (i = 0; i < count; i++) {
		uint32_t type, len;

		if (mbuf_len(m) < sizeof(*tlv)) {
			printf("%s: GET_NIC_CAPAB tlv size error\n",
			    DEVNAME(sc));
			m_freem(m);
			return EINVAL;
		}

		tlv = mtod(m, struct tlv_hdr *);
		type = le32toh(tlv->type);
		len = le32toh(tlv->len);
		m_adj(m, sizeof(*tlv));

		if (mbuf_len(m) < len)
			break;
		switch (type) {
		case MT_NIC_CAP_6G:
			/* TODO 6GHZ SUPPORT */
			sc->sc_capa.has_6ghz = /* XXX 1 */ 0;
			break;
		case MT_NIC_CAP_MAC_ADDR:
			if (len < ETHER_ADDR_LEN)
				break;
			memcpy(sc->sc_lladdr, mtod(m, caddr_t), ETHER_ADDR_LEN);
			break;
		case MT_NIC_CAP_PHY:
			if (len < sizeof(*cap))
				break;
			cap = mtod(m, struct mt76_connac_phy_cap *);

			sc->sc_capa.num_streams = cap->nss;
			sc->sc_capa.antenna_mask = (1U << cap->nss) - 1;
			sc->sc_capa.has_2ghz = cap->hw_path & 0x01;
			sc->sc_capa.has_5ghz = cap->hw_path & 0x02;
			break;
		case MT_NIC_CAP_TX_RESOURCE:
			/* unused on PCIe devices */
			break;
		}
		m_adj(m, len);
	}

	printf("%s: address %s\n", DEVNAME(sc), ether_sprintf(sc->sc_lladdr));

	m_freem(m);
	return 0;
}

int
mt7921_mcu_fw_log_2_host(struct mwx_softc *sc, uint8_t ctrl)
{
	struct {
		uint8_t	ctrl;
		uint8_t	pad[3];
	} req = {
		.ctrl = ctrl,
	};

	return mwx_mcu_send_msg(sc, MCU_CE_CMD_FWLOG_2_HOST, &req,
	    sizeof(req), NULL);
}

int
mt7921_mcu_set_eeprom(struct mwx_softc *sc)
{
	struct req_hdr {
		uint8_t	buffer_mode;
		uint8_t	format;
		uint8_t pad[2];
	} req = {
		.buffer_mode = EE_MODE_EFUSE,
		.format = EE_FORMAT_WHOLE,
	};

	return mwx_mcu_send_wait(sc, MCU_EXT_CMD_EFUSE_BUFFER_MODE, &req,
	    sizeof(req));
}

int
mt7921_mcu_set_rts_thresh(struct mwx_softc *sc, uint32_t val, uint8_t band)
{
	struct {
		uint8_t		prot_idx;
		uint8_t		band;
		uint8_t		rsv[2];
		uint32_t	len_thresh;
		uint32_t	pkt_thresh;
	} __packed req = {
		.prot_idx = 1,
		.band = band,
		.len_thresh = htole32(val),
		.pkt_thresh = htole32(0x2),
	};

	return mwx_mcu_send_wait(sc, MCU_EXT_CMD_PROTECT_CTRL, &req,
	    sizeof(req));
}

int
mwx_mcu_set_deep_sleep(struct mwx_softc *sc, int ena)
{
	struct mwx_connac_config req = {
		.resp_type = 0,
	};

	DPRINTF("%s: %s deep sleep\n", DEVNAME(sc), ena ? "enable" : "disable");
	snprintf((char *)req.data, sizeof(req.data), "KeepFullPwr %d", !ena);
	return mwx_mcu_send_msg(sc, MCU_CE_CMD_CHIP_CONFIG, &req,
	     sizeof(req), NULL);
}

void
mt7921_mcu_low_power_event(struct mwx_softc *sc, struct mbuf *m)
{
	struct mt7921_mcu_lp_event {
		uint8_t state;
		uint8_t reserved[3];
	} __packed *event;

	if (mbuf_len(m) < sizeof(*event))
		return;
	event = mtod(m, struct mt7921_mcu_lp_event *);
	DPRINTF("%s: low power event state %d\n", DEVNAME(sc), event->state);
}

void
mt7921_mcu_tx_done_event(struct mwx_softc *sc, struct mbuf *m)
{
	struct mt7921_mcu_tx_done_event {
		uint8_t		pid;
		uint8_t		status;
		uint16_t	seq;
		uint8_t		wlan_idx;
		uint8_t		tx_cnt;
		uint16_t	tx_rate;
		uint8_t		flag;
		uint8_t		tid;
		uint8_t		rsp_rate;
		uint8_t		mcs;
		uint8_t		bw;
		uint8_t		tx_pwr;
		uint8_t		reason;
		uint8_t		rsv0[1];
		uint32_t	delay;
		uint32_t	timestamp;
		uint32_t	applied_flag;
		uint8_t		txs[28];
		uint8_t		rsv1[32];
	} __packed *event;

	if (mbuf_len(m) < sizeof(*event))
		return;
	event = mtod(m, struct mt7921_mcu_tx_done_event *);
	// TODO mt7921_mac_add_txs(dev, event->txs);
}

int
mt7921_mcu_hw_scan(struct mwx_softc *sc, int bgscan)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_channel *c;
	struct mt76_connac_hw_scan_req	*req;
	struct mbuf *m;
	int n_ssids = 0;
	int rv;
	uint8_t nchan, n_2g = 0, n_5g = 0;

	m = mwx_mcu_alloc_msg(sizeof(*req));
	if (m == NULL)
		return ENOMEM;
	req = mtod(m, struct mt76_connac_hw_scan_req *);

	sc->sc_flags |= MWX_FLAG_SCANNING;
	sc->sc_scan_wd = 0;
	sc->sc_scan_seq_num = (sc->sc_scan_seq_num + 1) & 0x7f;

	req->seq_num = sc->sc_scan_seq_num /* | sc->sc_band_idx << 7 */;
	req->bss_idx = /* mvif->idx */ 0;
	req->scan_type = /* sreq->n_ssids ? 1 : */ 0;
	req->probe_req_num = /* sreq->n_ssids ? 2 : */ 0;
	req->version = 1;

#ifdef AIRPORT_WCL
	/*
	 * A scan macOS asked for (AirportMtkWlan): active when it wants probes,
	 * with its SSID (hidden networks), on its channels. Scans the driver
	 * starts itself stay passive over every channel, as with SimpleMtkWlan.
	 * Like Linux mt76: probing with no SSID is the wildcard (ssid_type 1).
	 */
	if (ic->ic_wcl_scan_requested &&
	    AirportWCL::scanUsesProbes(true, ic->ic_wcl_scan_active)) {
		req->scan_type = 1;
		req->probe_req_num = 2;
		if (ic->ic_wcl_scan_ssid_length > 0 &&
		    ic->ic_wcl_scan_ssid_length <= IEEE80211_NWID_LEN) {
			req->ssids[0].ssid_len = htole32(ic->ic_wcl_scan_ssid_length);
			memcpy(req->ssids[0].ssid, ic->ic_wcl_scan_ssid,
			    ic->ic_wcl_scan_ssid_length);
			n_ssids = 1;
		}
	}
#endif

#ifdef NOTYET
	for (i = 0; i < sreq->n_ssids; i++) {
		if (!sreq->ssids[i].ssid_len)
			continue;
		req->ssids[i].ssid_len = htole32(sreq->ssids[i].ssid_len);
		memcpy(req->ssids[i].ssid, sreq->ssids[i].ssid,
		    sreq->ssids[i].ssid_len);
		n_ssids++;
	}
#endif

	req->ssid_type = n_ssids ? 0x4 : 0x1;
	req->ssid_type_ext = n_ssids ? 1 : 0;
	req->ssids_num = n_ssids;

	for (nchan = 0, c = &ic->ic_channels[1];
	    c <= &ic->ic_channels[IEEE80211_CHAN_MAX] &&
	    nchan < 64; c++) {
		struct mt76_connac_mcu_scan_channel *chan;
		uint8_t channel_num;

		if (c->ic_flags == 0)
			continue;
#ifdef AIRPORT_WCL
		if (!AirportWCL::scanChannelSelected(ic->ic_wcl_scan_channels,
		    ic->ic_wcl_scan_requested && ic->ic_wcl_scan_restricted,
		    ieee80211_mhz2ieee(c->ic_freq, 0)))
			continue;
#endif

		if (nchan < 32)
			chan = &req->channels[nchan];
		else
			chan = &req->ext_channels[nchan - 32];

		channel_num = ieee80211_mhz2ieee(c->ic_freq, 0);
		/* TODO IEEE80211_IS_CHAN_6GHZ -> chan->band = 3*/
		if (IEEE80211_IS_CHAN_2GHZ(c)) {
			chan->band = 1;
			n_2g++;
		} else {
			chan->band = 2;
			n_5g++;
		}
		chan->channel_num = channel_num;
		nchan++;
	}

	if (nchan <= 32) {
		req->channels_num = nchan;
	} else {
		req->channels_num = 32;
		req->ext_channels_num = nchan - 32;
	}

	req->channel_type = nchan ? 4 : 0;
	req->timeout_value = htole16(nchan * 120);
	req->channel_min_dwell_time = htole16(120);
	req->channel_dwell_time = htole16(120);


#ifdef NOTYET
	if (sreq->ie_len > 0) {
		memcpy(req->ies, sreq->ie, sreq->ie_len);
		req->ies_len = htole16(sreq->ie_len);
	}
#endif

	req->scan_func |= SCAN_FUNC_SPLIT_SCAN;

	/* wildcard BSSID */
	memset(req->bssid, 0xff, ETHER_ADDR_LEN);
#ifdef NOTYET
	if (sreq->flags & NL80211_SCAN_FLAG_RANDOM_ADDR) {
		get_random_mask_addr(req->random_mac, sreq->mac_addr,
		    sreq->mac_addr_mask);
		req->scan_func |= SCAN_FUNC_RANDOM_MAC;
	}
#endif

	if (mtk_verbose_scans > 0)
	MWX_DEV_LOG(sc,
	    "scan request seq=%u channels=%u 2g=%u 5g=%u primary=%u ext=%u timeout=%u dwell=%u min_dwell=%u scan_func=0x%08x ssid_type=0x%x\n",
	    sc->sc_scan_seq_num, nchan, n_2g, n_5g, req->channels_num,
	    req->ext_channels_num, le16toh(req->timeout_value),
	    le16toh(req->channel_dwell_time),
	    le16toh(req->channel_min_dwell_time),
	    le32toh(req->scan_func), req->ssid_type);

	rv = mwx_mcu_send_mbuf(sc, MCU_CE_CMD_START_HW_SCAN, m, NULL);
	if (rv != 0)
		sc->sc_flags &= ~(MWX_FLAG_SCANNING | MWX_FLAG_BGSCAN);

	return rv;
}

int
mt7921_mcu_hw_scan_cancel(struct mwx_softc *sc)
{
	struct {
		uint8_t		seq_num;
		uint8_t		is_ext_channel;
		uint8_t		rsv[2];
	} __packed req = {
		.seq_num = sc->sc_scan_seq_num,
	};
	int rv;

	rv = mwx_mcu_send_msg(sc, MCU_CE_CMD_CANCEL_HW_SCAN, &req,
	    sizeof(req), NULL);
	if (rv == 0)
		sc->sc_flags &= ~(MWX_FLAG_SCANNING | MWX_FLAG_BGSCAN);
	return rv;
}

void
mwx_end_scan_task(void *arg)
{
	struct mwx_softc *sc = (struct mwx_softc *)arg;
	struct ieee80211com *ic = &sc->sc_ic;
	int s;

	s = splnet();
	ieee80211_end_scan(&ic->ic_if);
	splx(s);
}

void
mt7921_mcu_scan_event(struct mwx_softc *sc, struct mbuf *m)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct mt76_connac_hw_scan_done *event;
	uint32_t probe_sum = 0, mdrdy_sum = 0;
	int i, nodes_before;

	sc->sc_scan_done_count++;
	sc->sc_scan_wd = 0;
	nodes_before = mwx_count_nodes(ic);
	sc->sc_scan_last_ieee80211_nodes = nodes_before;

	if (mtk_verbose_scans <= 0 && sc->sc_scan_done_count % 64 != 0) {
		/* quiet idle rescan */
	} else if (mbuf_len(m) >= sizeof(*event)) {
		event = mtod(m, struct mt76_connac_hw_scan_done *);
		for (i = 0; i < MT76_HW_SCAN_DONE_MAX_CHANNEL_NUM; i++) {
			probe_sum += event->beacon_probe_num[i];
			mdrdy_sum += event->mdrdy_count[i];
		}
		MWX_DEV_LOG(sc,
		    "scan done #%u seq=%u complete_ch=%u state=%u version=%u beacon_scan=%u probe_sum=%u mdrdy_sum=%u beacon_2g=%u beacon_5g=%u nodes_before=%d rx_norm=%u rx_fail=%u\n",
		    sc->sc_scan_done_count, event->seq_num,
		    event->complete_channel_num, event->current_state,
		    event->version, le32toh(event->beacon_scan_num),
		    probe_sum, mdrdy_sum, le32toh(event->beacon_2g_num),
		    le32toh(event->beacon_5g_num), nodes_before,
		    sc->sc_rx_pkt_normal, sc->sc_rx_fill_fail);
	} else {
		MWX_DEV_LOG(sc,
		    "scan done #%u short event len=%zu nodes_before=%d rx_norm=%u rx_fail=%u\n",
		    sc->sc_scan_done_count, mbuf_len(m), nodes_before,
		    sc->sc_rx_pkt_normal, sc->sc_rx_fill_fail);
	}

	if (mtk_verbose_scans > 0)
		mtk_verbose_scans--;

	if (mt7921_mcu_hw_scan_cancel(sc) != 0)
		return;
	task_add(systq, &sc->sc_scan_task);
}

int
mt7921_mcu_set_mac_enable(struct mwx_softc *sc, int band, int enable)
{
	struct {
		uint8_t		enable;
		uint8_t		band;
		uint8_t		rsv[2];
	} __packed req = {
		.enable = (uint8_t)enable,
		.band = (uint8_t)band,
	};

	return mwx_mcu_send_wait(sc, MCU_EXT_CMD_MAC_INIT_CTRL, &req,
	    sizeof(req));
}

int
mt7921_mcu_set_channel_domain(struct mwx_softc *sc)
{
	struct {
		uint8_t		alpha2[4]; /* regulatory_request.alpha2 */
		uint8_t		bw_2g;	/* BW_20_40M		0
					 * BW_20M		1
					 * BW_20_40_80M		2
					 * BW_20_40_80_160M	3
					 * BW_20_40_80_8080M	4
					 */
		uint8_t		bw_5g;
		uint8_t		bw_6g;
		uint8_t		pad;
		uint8_t		n_2ch;
		uint8_t		n_5ch;
		uint8_t		n_6ch;
		uint8_t		pad2;
	} __packed *hdr;
	struct {
		uint16_t	hw_value;
		uint16_t	pad;
		uint32_t	flags;
	} __packed *channel;
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_channel *chan;
	struct mbuf *m;
	int i, len, rv;
	int n_2ch = 0, n_5ch = 0, n_6ch = 0;

	len = sizeof(*hdr) + IEEE80211_CHAN_MAX * sizeof(channel);
	m = mwx_mcu_alloc_msg(len);
	if (m == NULL)
		return ENOMEM;
	hdr = (decltype(hdr))mbuf_data(m);

	hdr->alpha2[0] = '0';
	hdr->alpha2[1] = '0';

	channel = (decltype(channel))(hdr + 1);

	hdr->bw_2g = 0;	/* BW_20_40M */
	for (i = 0; i <= IEEE80211_CHAN_MAX; i++) {
		chan = &ic->ic_channels[i];
		if (!IEEE80211_IS_CHAN_2GHZ(chan))
			continue;

		channel->hw_value = htole16(ieee80211_chan2ieee(ic, chan));
		channel->flags = htole32(0);	/* XXX */

		channel++;
		n_2ch++;
	}
	hdr->bw_5g = 3; /* BW_20_40_80_160M */
	for (i = 0; i <= IEEE80211_CHAN_MAX; i++) {
		chan = &ic->ic_channels[i];
		if (!IEEE80211_IS_CHAN_5GHZ(chan))
			continue;

		channel->hw_value = htole16(ieee80211_chan2ieee(ic, chan));
		channel->flags = htole32(0);	/* XXX */

		channel++;
		n_5ch++;
	}
#ifdef NOTYET
	/* 6GHz handling */
	hdr->bw_6g = 3; /* BW_20_40_80_160M */
	for (i = 0; i <= IEEE80211_CHAN_MAX; i++) {
		chan = &ic->ic_channels[i];
		if (!IEEE80211_IS_CHAN_6GHZ(chan))
			continue;

		channel->hw_value = htole16(ieee80211_chan2ieee(ic, chan));
		channel->flags = htole32(0);	/* XXX */

		channel++;
		n_6ch++;
	}
#endif

	memcpy(hdr->alpha2, sc->sc_alpha2, sizeof(sc->sc_alpha2));
	hdr->n_2ch = n_2ch;
	hdr->n_5ch = n_5ch;
	hdr->n_6ch = n_6ch;

	mwx_mcu_set_len(m, channel);
	rv = mwx_mcu_send_mbuf(sc, MCU_CE_CMD_SET_CHAN_DOMAIN, m, NULL);
	return rv;
}

/*
 * Center of the 40 or 80 MHz block a 5 GHz channel belongs to (802.11
 * channelization: 36/40, 44/48 ... and 36-48, 52-64, 100-112, 116-128,
 * 132-144, 149-161), 0 if the channel has no such block.
 */
int
mwx_5ghz_center(int chan, int width)
{
	static const uint8_t c40[] = { 38, 46, 54, 62, 102, 110, 118, 126,
	    134, 142, 151, 159, 167 };
	static const uint8_t c80[] = { 42, 58, 106, 122, 138, 155 };
	const uint8_t *c = width == 80 ? c80 : c40;
	int n = width == 80 ? nitems(c80) : nitems(c40);
	int half = width == 80 ? 6 : 2;
	int i;

	for (i = 0; i < n; i++)
		if (chan >= c[i] - half && chan <= c[i] + half)
			return c[i];
	return 0;
}

/* HT/VHT channel flags; 2.4 GHz HT40+ up to channel 9, HT40- from 5. */
uint32_t
mwx_chan_ht_flags(int chan, int is5g)
{
	uint32_t flags = IEEE80211_CHAN_HT20;
	int c40;

	if (!is5g) {
		if (chan <= 9)
			flags |= IEEE80211_CHAN_HT40U;
		if (chan >= 5 && chan <= 13)
			flags |= IEEE80211_CHAN_HT40D;
		return flags;
	}

	flags |= IEEE80211_CHAN_VHT20;
	c40 = mwx_5ghz_center(chan, 40);
	if (c40 > chan)
		flags |= IEEE80211_CHAN_HT40U | IEEE80211_CHAN_VHT40U;
	else if (c40 != 0)
		flags |= IEEE80211_CHAN_HT40D | IEEE80211_CHAN_VHT40D;
	if (mwx_5ghz_center(chan, 80) != 0)
		flags |= IEEE80211_CHAN_VHT80;
	return flags;
}

static const char *
mwx_cbw_name(uint8_t bw)
{
	switch (bw) {
	case CMD_CBW_40MHZ:
		return "40";
	case CMD_CBW_80MHZ:
		return "80";
	case CMD_CBW_160MHZ:
		return "160";
	default:
		return "20";
	}
}

/*
 * Width and center channel of the BSS, like mac80211's chandef. Before
 * association (negotiated == 0) it comes from the AP's HT/VHT operation
 * elements, as mac80211 tunes the full width before authenticating; after
 * association from what net80211 negotiated (ni_chw, ic_center_freq1).
 * Only widths up to 80 MHz are offered to the AP.
 */
void
mwx_bss_chandef(struct mwx_softc *sc, struct ieee80211_node *ni,
    int negotiated, uint8_t *ctrl, uint8_t *center, uint8_t *bw)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_channel *c = ni->ni_chan;
	int chan = ieee80211_chan2ieee(ic, c);
	int is5g = IEEE80211_IS_CHAN_5GHZ(c);
	int c80 = is5g ? mwx_5ghz_center(chan, 80) : 0;
	int c40 = 0, sco, ch;

	*ctrl = *center = chan;
	*bw = CMD_CBW_20MHZ;

	if (negotiated) {
		if ((ni->ni_flags & (IEEE80211_NODE_HT | IEEE80211_NODE_VHT |
		    IEEE80211_NODE_HE)) == 0)
			return;
		ch = c->ic_center_freq1 ?
		    ieee80211_mhz2ieee(c->ic_center_freq1, c->ic_flags) : chan;
		switch (ni->ni_chw) {
		case IEEE80211_CHAN_WIDTH_80:
		case IEEE80211_CHAN_WIDTH_80P80:
		case IEEE80211_CHAN_WIDTH_160:
			if (c80 != 0 && (ni->ni_flags &
			    (IEEE80211_NODE_VHT | IEEE80211_NODE_HE))) {
				*center = c80;
				*bw = CMD_CBW_80MHZ;
			}
			return;
		case IEEE80211_CHAN_WIDTH_40:
			if (ch == chan + 2 || ch == chan - 2) {
				*center = ch;
				*bw = CMD_CBW_40MHZ;
			}
			return;
		default:
			return;
		}
	}

	if (sc->sc_phy_limit >= MWX_PHY_LIMIT_VHT && is5g && c80 != 0 &&
	    ieee80211_node_supports_vht(ni) &&
	    ni->ni_vht_chanwidth >= IEEE80211_VHT_CHANWIDTH_80MHZ &&
	    ni->ni_vht_chan1 == c80) {
		*center = c80;
		*bw = CMD_CBW_80MHZ;
		return;
	}
	if (sc->sc_phy_limit < MWX_PHY_LIMIT_HT ||
	    !ieee80211_node_supports_ht(ni) ||
	    (ni->ni_htcaps & IEEE80211_HTCAP_CBW20_40) == 0)
		return;
	sco = ni->ni_htop0 & IEEE80211_HTOP0_SCO_MASK;
	if (sco == IEEE80211_HTOP0_SCO_SCA &&
	    (c->ic_flags & IEEE80211_CHAN_HT40U))
		c40 = chan + 2;
	else if (sco == IEEE80211_HTOP0_SCO_SCB &&
	    (c->ic_flags & IEEE80211_CHAN_HT40D))
		c40 = chan - 2;
	if (c40 != 0) {
		*center = c40;
		*bw = CMD_CBW_40MHZ;
	}
}

int
mt7921_mcu_set_chan_info(struct mwx_softc *sc, int cmd)
{
	struct ieee80211_channel *channel;
	struct {
		uint8_t		control_ch;
		uint8_t		center_ch;
		uint8_t		bw;
		uint8_t		tx_streams_num;
		uint8_t		rx_streams;	/* mask or num */
		uint8_t		switch_reason;
		uint8_t		band_idx;
		uint8_t		center_ch2;	/* for 80+80 only */
		uint16_t	cac_case;
		uint8_t		channel_band;
		uint8_t		rsv0;
		uint32_t	outband_freq;
		uint8_t		txpower_drop;
		uint8_t		ap_bw;
		uint8_t		ap_center_ch;
		uint8_t		rsv1[57];
	} __packed req = {
		.tx_streams_num = sc->sc_capa.num_streams,
		.rx_streams = sc->sc_capa.antenna_mask,
		.band_idx = 0,	/* XXX 0 or 1 */
	};

	if (sc->sc_ic.ic_opmode == IEEE80211_M_STA && sc->sc_ic.ic_bss != NULL)
		channel = sc->sc_ic.ic_bss->ni_chan;
	else
		channel = sc->sc_ic.ic_ibss_chan;

	req.control_ch = ieee80211_mhz2ieee(channel->ic_freq,
		    channel->ic_flags);
	req.center_ch = req.control_ch;
	req.bw = CMD_CBW_20MHZ;
	/* The joined BSS may be wider: see mwx_bss_chandef(). */
	if (sc->sc_chan_ctrl == req.control_ch && sc->sc_chan_center != 0) {
		req.center_ch = sc->sc_chan_center;
		req.bw = sc->sc_chan_bw;
	}

#ifdef NOTYET
	if (channel->ic_flags & IEEE80211_CHAN_6GHZ)
		req.channel_band = 2;
	else
#endif
	if (channel->ic_flags & IEEE80211_CHAN_5GHZ)
		req.channel_band = 1;
	else
		req.channel_band = 0;

#ifdef NOTYET
	if (dev->mt76.hw->conf.flags & IEEE80211_CONF_OFFCHANNEL)
		req.switch_reason = CH_SWITCH_SCAN_BYPASS_DPD;
	else if ((chandef->chan->flags & IEEE80211_CHAN_RADAR) &&
		 chandef->chan->dfs_state != NL80211_DFS_AVAILABLE)
		req.switch_reason = CH_SWITCH_DFS;
	else
#endif
		req.switch_reason = CH_SWITCH_NORMAL;

	if (cmd == MCU_EXT_CMD_CHANNEL_SWITCH)
		req.rx_streams = sc->sc_capa.num_streams;

#ifdef NOTYET
	if (chandef->width == NL80211_CHAN_WIDTH_80P80) {
		int freq2 = chandef->center_freq2;
		req.center_ch2 = ieee80211_frequency_to_channel(freq2);
	}
#endif

	return mwx_mcu_send_wait(sc, cmd, &req, sizeof(req));
}

/* hardcoded version of what linux does */
void
mt7921_mcu_build_sku(struct mwx_softc *sc, int band, int8_t *sku)
{
	int max_power = 127;
	int i, offset = 4;

	memset(sku, max_power, MT_SKU_POWER_LIMIT);

	if (band == MT_TX_PWR_BAND_2GHZ) {
		/* cck */
		memset(sku, 0x28, 4);
	}

	/* ofdm */
	memset(sku + offset, 0x28, 8);
	offset += 8;

	/* ht */
	for (i = 0; i < 2; i++) {
		memset(sku + offset, 0x28, 8);
		offset += 8;
	}
	sku[offset++] = 0x28;

	/* vht */
	for (i = 0; i < 4; i++) {
		/* this only sets 10 out of 12 bytes on purpose */
		memset(sku + offset, 0x28, 10);
		offset += 12;
	}

	/* he */
	for (i = 0; i < 7; i++) {
		memset(sku + offset, 0x28, 12);
		offset += 12;
	}
}

int
mt7921_mcu_rate_txpower_band(struct mwx_softc *sc, int band,
    const uint8_t *chans, int n_chans, int is_last)
{
	struct mt76_connac_sku_tlv *sku_tlv;
	struct mt76_connac_tx_power_limit_tlv *tx_power_tlv;
	struct mbuf *m;
	int batch_size = 8;
	const int len = sizeof(*tx_power_tlv) + batch_size * sizeof(*sku_tlv);
	int rv = 0, idx, j;

	for (idx = 0; idx < n_chans; ) {
		int num_ch = batch_size;

		m = mwx_mcu_alloc_msg(len);
		if (m == NULL)
			return ENOMEM;
		tx_power_tlv = mtod(m, struct mt76_connac_tx_power_limit_tlv *);
		tx_power_tlv->n_chan = num_ch;
		tx_power_tlv->band = band;
		memcpy(tx_power_tlv->alpha2, sc->sc_alpha2,
		    sizeof(sc->sc_alpha2));

		if (n_chans - idx < batch_size) {
			num_ch = n_chans - idx;
			if (is_last)
				tx_power_tlv->last_msg = 1;
		}

		sku_tlv = (struct mt76_connac_sku_tlv *)(tx_power_tlv + 1);

		for (j = 0; j < num_ch; j++, idx++) {
			sku_tlv->channel = chans[idx];
			mt7921_mcu_build_sku(sc, band, sku_tlv->pwr_limit);
			sku_tlv++;
		}

		mwx_mcu_set_len(m, sku_tlv);
		rv = mwx_mcu_send_mbuf(sc, MCU_CE_CMD_SET_RATE_TX_POWER, m,
		    NULL);
		if (rv != 0)
			break;
	}

	return rv;
}

int
mt7921_mcu_set_rate_txpower(struct mwx_softc *sc)
{
	static const uint8_t chan_list_2ghz[] = {
	    1, 2,  3,  4,  5,  6,  7,
	    8, 9, 10, 11, 12, 13, 14
	};
	static const uint8_t chan_list_5ghz[] = {
	    36,  38,  40,  42,  44,  46,  48,
	    50,  52,  54,  56,  58,  60,  62,
	    64, 100, 102, 104, 106, 108, 110,
	    112, 114, 116, 118, 120, 122, 124,
	    126, 128, 132, 134, 136, 138, 140,
	    142, 144, 149, 151, 153, 155, 157,
	    159, 161, 165
	};
	static const uint8_t chan_list_6ghz[] = {
	    1,   3,   5,   7,   9,  11,  13,
	    15,  17,  19,  21,  23,  25,  27,
	    29,  33,  35,  37,  39,  41,  43,
	    45,  47,  49,  51,  53,  55,  57,
	    59,  61,  65,  67,  69,  71,  73,
	    75,  77,  79,  81,  83,  85,  87,
	    89,  91,  93,  97,  99, 101, 103,
	    105, 107, 109, 111, 113, 115, 117,
	    119, 121, 123, 125, 129, 131, 133,
	    135, 137, 139, 141, 143, 145, 147,
	    149, 151, 153, 155, 157, 161, 163,
	    165, 167, 169, 171, 173, 175, 177,
	    179, 181, 183, 185, 187, 189, 193,
	    195, 197, 199, 201, 203, 205, 207,
	    209, 211, 213, 215, 217, 219, 221,
	    225, 227, 229, 233
	};
	int rv = 0;

	if (sc->sc_capa.has_2ghz)
		rv = mt7921_mcu_rate_txpower_band(sc, MT_TX_PWR_BAND_2GHZ,
		    chan_list_2ghz, nitems(chan_list_2ghz),
		    !(sc->sc_capa.has_5ghz || sc->sc_capa.has_6ghz));
	if (rv == 0 && sc->sc_capa.has_5ghz)
		rv = mt7921_mcu_rate_txpower_band(sc, MT_TX_PWR_BAND_5GHZ,
		    chan_list_5ghz, nitems(chan_list_5ghz),
		    !sc->sc_capa.has_6ghz);
	if (rv == 0 && sc->sc_capa.has_6ghz)
		rv = mt7921_mcu_rate_txpower_band(sc, MT_TX_PWR_BAND_6GHZ,
		    chan_list_6ghz, nitems(chan_list_6ghz), 1);
	return rv;
}

void
mt7921_mac_reset_counters(struct mwx_softc *sc)
{
	int i;

	for (i = 0; i < 4; i++) {
		mwx_read(sc, MT_TX_AGG_CNT(0, i));
		mwx_read(sc, MT_TX_AGG_CNT2(0, i));
	}

	/* XXX TODO stats in softc */

	/* reset airtime counters */
	mwx_read(sc, MT_MIB_SDR9(0));
	mwx_read(sc, MT_MIB_SDR36(0));
	mwx_read(sc, MT_MIB_SDR37(0));
	mwx_set(sc, MT_WF_RMAC_MIB_TIME0(0), MT_WF_RMAC_MIB_RXTIME_CLR);
	mwx_set(sc, MT_WF_RMAC_MIB_AIRTIME0(0), MT_WF_RMAC_MIB_RXTIME_CLR);
}

void
mt7921_mac_set_timing(struct mwx_softc *sc)
{
	uint16_t coverage_class = 0;		/* XXX */
	uint32_t val, reg_offset;
	uint32_t cck = MT_TIMEOUT_CCK_DEF_VAL;
	uint32_t ofdm = MT_TIMEOUT_OFDM_DEF_VAL;
	uint32_t offset;
	int is_2ghz = 1; /* XXX get from ic_bss node */
	uint32_t sifs = is_2ghz ? 10 : 16;
	uint32_t slottime = IEEE80211_DUR_DS_SHSLOT;	/* XXX get from stack */

#ifdef NOTYET
	if (!test_bit(MT76_STATE_RUNNING, &phy->mt76->state))
		return;
#endif

	mwx_set(sc, MT_ARB_SCR(0),
	    MT_ARB_SCR_TX_DISABLE | MT_ARB_SCR_RX_DISABLE);
	delay(1);

	offset = 3 * coverage_class;
	reg_offset = offset | (offset << 16);

	mwx_write(sc, MT_TMAC_CDTR(0), cck + reg_offset);
	mwx_write(sc, MT_TMAC_ODTR(0), ofdm + reg_offset);
	mwx_write(sc, MT_TMAC_ICR0(0),
	    MT_IFS_EIFS_DEF | MT_IFS_RIFS_DEF |
	    (sifs << MT_IFS_SIFS_SHIFT) |
	    (slottime << MT_IFS_SLOT_SHIFT));

	if (slottime < 20 || !is_2ghz)
		val = MT7921_CFEND_RATE_DEFAULT;
	else
		val = MT7921_CFEND_RATE_11B;

	mwx_rmw(sc, MT_AGG_ACR0(0), val, MT_AGG_ACR_CFEND_RATE_MASK);
	mwx_clear(sc, MT_ARB_SCR(0),
	    MT_ARB_SCR_TX_DISABLE | MT_ARB_SCR_RX_DISABLE);
}

int
mt7921_mcu_uni_add_dev(struct mwx_softc *sc, struct mwx_vif *mvif,
    struct mwx_node *mn, int enable)
{
	struct {
		struct {
			uint8_t		omac_idx;
			uint8_t		band_idx;
			uint16_t	pad;
		} __packed hdr;
		struct req_tlv {
			uint16_t	tag;
			uint16_t	len;
			uint8_t		active;
			uint8_t		pad;
			uint8_t		omac_addr[ETHER_ADDR_LEN];
		} __packed tlv;
	} dev_req = {
		.hdr = {
			.omac_idx = mvif->omac_idx,
			.band_idx = mvif->band_idx,
		},
		.tlv = {
			.tag = htole16(DEV_INFO_ACTIVE),
			.len = htole16(sizeof(dev_req.tlv)),
			.active = (uint8_t)enable,
		},
	};
	struct {
		struct {
			uint8_t		bss_idx;
			uint8_t		pad[3];
		} __packed hdr;
		struct mt76_connac_bss_basic_tlv basic;
	} basic_req = {
		.hdr = {
			.bss_idx = mvif->idx,
		},
		.basic = {
			.tag = htole16(UNI_BSS_INFO_BASIC),
			.len = htole16(
			    sizeof(struct mt76_connac_bss_basic_tlv)),
			.omac_idx = mvif->omac_idx,
			.band_idx = mvif->band_idx,
			.wmm_idx = mvif->wmm_idx,
			.active = (uint8_t)enable,
			.bmc_tx_wlan_idx = htole16(mn->wcid),
			.sta_idx = htole16(mn->wcid),
			.conn_state = 1,
		},
	};
	int rv, idx, cmd, len;
	void *data;

	switch (sc->sc_ic.ic_opmode) {
	case IEEE80211_M_MONITOR:
#ifndef IEEE80211_STA_ONLY
	case IEEE80211_M_HOSTAP:
#endif
		basic_req.basic.conn_type =
		    htole32(STA_TYPE_AP | NETWORK_INFRA);
		break;
	case IEEE80211_M_STA:
		basic_req.basic.conn_type =
		    htole32(STA_TYPE_STA | NETWORK_INFRA);
		break;
#ifndef IEEE80211_STA_ONLY
	case IEEE80211_M_IBSS:
		basic_req.basic.conn_type =
		    htole32(STA_TYPE_ADHOC | NETWORK_IBSS);
		break;
#endif
	default:
		panic("%s: unknown operation mode", DEVNAME(sc));
	}

	idx = mvif->omac_idx > EXT_BSSID_START ? HW_BSSID_0 : mvif->omac_idx;
	basic_req.basic.hw_bss_idx = idx;

	/* The address the vif answers to: the current one, not the factory one. */
	memcpy(dev_req.tlv.omac_addr,
	    IEEE80211_ADDR_EQ(sc->sc_ic.ic_myaddr, etheranyaddr) ?
	    sc->sc_lladdr : sc->sc_ic.ic_myaddr, ETHER_ADDR_LEN);

	if (enable) {
		cmd = MCU_UNI_CMD_DEV_INFO_UPDATE;
		data = &dev_req;
		len = sizeof(dev_req);
	} else {
		cmd = MCU_UNI_CMD_BSS_INFO_UPDATE;
		data = &basic_req;
		len = sizeof(basic_req);
	}

printf("%s: %s cmd %x mvif idx %d omac %d band %d wmm %d\n", DEVNAME(sc), __func__, cmd, mvif->idx, mvif->omac_idx, mvif->band_idx, mvif->wmm_idx);
	rv = mwx_mcu_send_wait(sc, cmd, data, len);
	if (rv < 0)
		return rv;

	if (enable) {
		cmd = MCU_UNI_CMD_BSS_INFO_UPDATE;
		data = &basic_req;
		len = sizeof(basic_req);
	} else {
		cmd = MCU_UNI_CMD_DEV_INFO_UPDATE;
		data = &dev_req;
		len = sizeof(dev_req);
	}

printf("%s: %s cmd %x wcid %d\n", DEVNAME(sc), __func__, cmd, mn->wcid);
	return mwx_mcu_send_wait(sc, cmd, data, len);
}

#define	MWX_PHY_MODE_A		(1U << 0)
#define	MWX_PHY_MODE_B		(1U << 1)
#define	MWX_PHY_MODE_G		(1U << 2)
#define	MWX_PHY_MODE_GN		(1U << 3)
#define	MWX_PHY_MODE_AN		(1U << 4)
#define	MWX_PHY_MODE_AC		(1U << 5)
#define	MWX_PHY_MODE_AX_24G	(1U << 6)
#define	MWX_PHY_MODE_AX_5G	(1U << 7)

/*
 * Linux mt76_connac_mcu_uni_add_bss() + mt76_connac_mcu_uni_set_chctx() for a
 * station: BSS basic + QoS TLVs, then the channel (RLM) TLV.
 */
void
mt7921_bss_conf_from_node(struct mwx_softc *sc, struct ieee80211_node *ni)
{
	struct mwx_bss_conf *bc = &sc->sc_bss_conf;
	int limit = sc->sc_phy_limit;

	memset(bc, 0, sizeof(*bc));
	if (ni == NULL || ni->ni_chan == NULL ||
	    ni->ni_chan == IEEE80211_CHAN_ANYC)
		return;
	memcpy(bc->bssid, ni->ni_bssid, IEEE80211_ADDR_LEN);
	bc->chan = ni->ni_chan;
	bc->intval = ni->ni_intval;
	bc->dtim = ni->ni_dtimperiod;
	bc->qos = (ni->ni_flags & IEEE80211_NODE_QOS) != 0;
	/*
	 * Linux mt76_connac_get_phy_mode(_v2)(..., sta = NULL): the BSS gets
	 * our own modes on this band, the AP's station record the negotiated
	 * ones (Linux capture: phymode 0xb1, nonht 0x78 for an 11ax AP).
	 */
	if (IEEE80211_IS_CHAN_2GHZ(ni->ni_chan)) {
		bc->phymode = MWX_PHY_MODE_B | MWX_PHY_MODE_G;
		bc->nonht_phy = PHY_TYPE_BIT_HR_DSSS | PHY_TYPE_BIT_ERP;
		if (limit >= MWX_PHY_LIMIT_HT) {
			bc->phymode |= MWX_PHY_MODE_GN;
			bc->nonht_phy |= PHY_TYPE_BIT_HT;
		}
		if (limit >= MWX_PHY_LIMIT_HE) {
			bc->phymode |= MWX_PHY_MODE_AX_24G;
			bc->nonht_phy |= PHY_TYPE_BIT_HE;
		}
	} else {
		bc->phymode = MWX_PHY_MODE_A;
		bc->nonht_phy = PHY_TYPE_BIT_OFDM;
		if (limit >= MWX_PHY_LIMIT_HT) {
			bc->phymode |= MWX_PHY_MODE_AN;
			bc->nonht_phy |= PHY_TYPE_BIT_HT;
		}
		if (limit >= MWX_PHY_LIMIT_VHT) {
			bc->phymode |= MWX_PHY_MODE_AC;
			bc->nonht_phy |= PHY_TYPE_BIT_VHT;
		}
		if (limit >= MWX_PHY_LIMIT_HE) {
			bc->phymode |= MWX_PHY_MODE_AX_5G;
			bc->nonht_phy |= PHY_TYPE_BIT_HE;
		}
	}
	bc->he = (ni->ni_flags & IEEE80211_NODE_HE) != 0;
	if (bc->he) {
		uint32_t op = ni->ni_he_oper_params;

		/* mac80211: htc_trig_based_pkt_ext, frame_time_rts_th */
		bc->he_pe = op & IEEE80211_HE_OPERATION_DFLT_PE_DURATION_MASK;
		if (bc->he_pe == 0)
			bc->he_pe = 4;		/* DEFAULT_HE_PE_DURATION */
		bc->he_rts = (op & IEEE80211_HE_OPERATION_RTS_THRESHOLD_MASK) >>
		    IEEE80211_HE_OPERATION_RTS_THRESHOLD_OFFSET;
		if (bc->he_rts == 0)
			bc->he_rts = 1023;	/* DEFAULT_HE_DURATION_RTS_THRES */
		bc->bss_color = (op & IEEE80211_HE_OPERATION_BSS_COLOR_MASK) >>
		    IEEE80211_HE_OPERATION_BSS_COLOR_OFFSET;
		bc->bss_color_en =
		    (op & IEEE80211_HE_OPERATION_BSS_COLOR_DISABLED) == 0;
	}
	bc->valid = 1;
}

int
mt7921_mcu_uni_add_bss(struct mwx_softc *sc, int enable)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct mwx_vif *mvif = &sc->sc_vif;
	struct mwx_bss_conf *bc = &sc->sc_bss_conf;
	struct {
		struct {
			uint8_t		bss_idx;
			uint8_t		pad[3];
		} __packed hdr;
		struct mt76_connac_bss_basic_tlv basic;
		struct {
			uint16_t	tag;
			uint16_t	len;
			uint8_t		qos;
			uint8_t		pad[3];
		} __packed qos;
	} __packed basic_req;
	struct {
		struct {
			uint8_t		bss_idx;
			uint8_t		pad[3];
		} __packed hdr;
		struct {
			uint16_t	tag;
			uint16_t	len;
			uint8_t		control_channel;
			uint8_t		center_chan;
			uint8_t		center_chan2;
			uint8_t		bw;
			uint8_t		tx_streams;
			uint8_t		rx_streams;
			uint8_t		short_st;
			uint8_t		ht_op_info;
			uint8_t		sco;
			uint8_t		band;
			uint8_t		pad[2];
		} __packed rlm;
	} __packed rlm_req;
	int is2g, rv;
	uint8_t chan;

	if (!bc->valid)
		return EINVAL;
	is2g = IEEE80211_IS_CHAN_2GHZ(bc->chan);
	chan = ieee80211_chan2ieee(ic, bc->chan);

	memset(&basic_req, 0, sizeof(basic_req));
	basic_req.hdr.bss_idx = mvif->idx;
	basic_req.basic.tag = htole16(UNI_BSS_INFO_BASIC);
	basic_req.basic.len = htole16(sizeof(basic_req.basic));
	basic_req.basic.active = 1;	/* Linux keeps a STA BSS active */
	basic_req.basic.omac_idx = mvif->omac_idx;
	basic_req.basic.hw_bss_idx = mvif->omac_idx > EXT_BSSID_START ?
	    HW_BSSID_0 : mvif->omac_idx;
	basic_req.basic.band_idx = mvif->band_idx;
	basic_req.basic.conn_type = htole32(STA_TYPE_STA | NETWORK_INFRA);
	basic_req.basic.conn_state = !enable;
	basic_req.basic.wmm_idx = mvif->wmm_idx;
	memcpy(basic_req.basic.bssid, bc->bssid, IEEE80211_ADDR_LEN);
	basic_req.basic.bmc_tx_wlan_idx = htole16(mvif->vif_mn.wcid);
	basic_req.basic.bcn_interval = htole16(bc->intval);
	basic_req.basic.dtim_period = bc->dtim;
	basic_req.basic.phymode = bc->phymode;
	basic_req.basic.sta_idx = htole16(mvif->vif_mn.wcid);
	basic_req.basic.nonht_basic_phy = htole16(bc->nonht_phy);
	basic_req.qos.tag = htole16(UNI_BSS_INFO_QBSS);
	basic_req.qos.len = htole16(sizeof(basic_req.qos));
	basic_req.qos.qos = bc->qos;

	rv = mwx_mcu_send_wait(sc, MCU_UNI_CMD_BSS_INFO_UPDATE, &basic_req,
	    sizeof(basic_req));
	if (rv != 0)
		return rv;

	if (bc->he) {
		struct {
			struct {
				uint8_t		bss_idx;
				uint8_t		pad[3];
			} __packed hdr;
			struct bss_info_uni_he he;
			struct bss_info_uni_bss_color bss_color;
		} __packed he_req;

		/* Linux mt76_connac_mcu_uni_add_bss() he_support part */
		memset(&he_req, 0, sizeof(he_req));
		he_req.hdr.bss_idx = mvif->idx;
		he_req.he.tag = htole16(UNI_BSS_INFO_HE_BASIC);
		he_req.he.len = htole16(sizeof(he_req.he));
		he_req.he.he_rts_thres = htole16(bc->he_rts);
		he_req.he.he_pe_duration = bc->he_pe;
		/* our TX MCS limits (Linux: own he_mcs_nss_supp.tx_mcs_*) */
		he_req.he.max_nss_mcs[CMD_HE_MCS_BW80] =
		    ic->ic_he_mcs_nss_supp.tx_mcs_80;
		he_req.he.max_nss_mcs[CMD_HE_MCS_BW160] =
		    ic->ic_he_mcs_nss_supp.tx_mcs_160;
		he_req.bss_color.tag = htole16(UNI_BSS_INFO_BSS_COLOR);
		he_req.bss_color.len = htole16(sizeof(he_req.bss_color));
		if (enable) {
			he_req.bss_color.enable = bc->bss_color_en;
			he_req.bss_color.bss_color = bc->bss_color;
		}
		rv = mwx_mcu_send_wait(sc, MCU_UNI_CMD_BSS_INFO_UPDATE,
		    &he_req, sizeof(he_req));
		if (rv != 0)
			return rv;
	}

	/* Linux mt76_connac_mcu_uni_set_chctx() */
	memset(&rlm_req, 0, sizeof(rlm_req));
	rlm_req.hdr.bss_idx = mvif->idx;
	rlm_req.rlm.tag = htole16(UNI_BSS_INFO_RLM);
	rlm_req.rlm.len = htole16(sizeof(rlm_req.rlm));
	rlm_req.rlm.control_channel = chan;
	rlm_req.rlm.center_chan = chan;
	rlm_req.rlm.bw = CMD_CBW_20MHZ;
	if (sc->sc_chan_ctrl == chan && sc->sc_chan_center != 0) {
		rlm_req.rlm.center_chan = sc->sc_chan_center;
		rlm_req.rlm.bw = sc->sc_chan_bw;
	}
	/* HT 40 MHz allowed; Linux clears it for 20 MHz channels */
	rlm_req.rlm.ht_op_info = rlm_req.rlm.bw == CMD_CBW_20MHZ ? 0 : 4;
	if (rlm_req.rlm.control_channel < rlm_req.rlm.center_chan)
		rlm_req.rlm.sco = 1;	/* SCA */
	else if (rlm_req.rlm.control_channel > rlm_req.rlm.center_chan)
		rlm_req.rlm.sco = 3;	/* SCB */
	rlm_req.rlm.tx_streams = sc->sc_capa.num_streams;
	rlm_req.rlm.rx_streams = sc->sc_capa.antenna_mask;
	rlm_req.rlm.short_st = 1;
	rlm_req.rlm.band = is2g ? 0 : 1;	/* nl80211_band: 2 GHz 0, 5 GHz 1 */

	return mwx_mcu_send_wait(sc, MCU_UNI_CMD_BSS_INFO_UPDATE, &rlm_req,
	    sizeof(rlm_req));
}

/* Linux mt7921_mcu_uni_bss_bcnft(): beacon interval and DTIM of the joined BSS. */
int
mt7921_mcu_uni_bss_bcnft(struct mwx_softc *sc)
{
	struct {
		struct {
			uint8_t		bss_idx;
			uint8_t		pad[3];
		} __packed hdr;
		struct {
			uint16_t	tag;
			uint16_t	len;
			uint16_t	bcn_interval;
			uint8_t		dtim_period;
			uint8_t		pad;
		} __packed bcnft;
	} __packed req;

	if (!sc->sc_bss_conf.valid)
		return EINVAL;
	memset(&req, 0, sizeof(req));
	req.hdr.bss_idx = sc->sc_vif.idx;
	req.bcnft.tag = htole16(UNI_BSS_INFO_BCNFT);
	req.bcnft.len = htole16(sizeof(req.bcnft));
	req.bcnft.bcn_interval = htole16(sc->sc_bss_conf.intval);
	req.bcnft.dtim_period = sc->sc_bss_conf.dtim;

	return mwx_mcu_send_wait(sc, MCU_UNI_CMD_BSS_INFO_UPDATE, &req,
	    sizeof(req));
}

/* Linux mt7921_mcu_uni_bss_ps(): 0 = device awake, 2 = dynamic power save. */
int
mt7921_mcu_uni_bss_ps(struct mwx_softc *sc, int ps_state)
{
	struct {
		struct {
			uint8_t		bss_idx;
			uint8_t		pad[3];
		} __packed hdr;
		struct {
			uint16_t	tag;
			uint16_t	len;
			uint8_t		ps_state;
			uint8_t		pad[3];
		} __packed ps;
	} __packed req;

	memset(&req, 0, sizeof(req));
	req.hdr.bss_idx = sc->sc_vif.idx;
	req.ps.tag = htole16(UNI_BSS_INFO_PS);
	req.ps.len = htole16(sizeof(req.ps));
	req.ps.ps_state = ps_state;

	return mwx_mcu_send_wait(sc, MCU_UNI_CMD_BSS_INFO_UPDATE, &req,
	    sizeof(req));
}

int
mt7921_mcu_set_sniffer(struct mwx_softc *sc, int enable)
{
	struct {
		uint8_t		band_idx;
		uint8_t		pad[3];
		struct sniffer_enable_tlv {
			uint16_t	tag;
			uint16_t	len;
			uint8_t		enable;
			uint8_t		pad[3];
		}		enable;
	} req = {
		.band_idx = 0,
		.enable = {
			.tag = htole16(0),
			.len = htole16(sizeof(req.enable)),
			.enable = (uint8_t)enable,
		},
	};

	return mwx_mcu_send_wait(sc, MCU_UNI_CMD_SNIFFER, &req, sizeof(req));
}

int
mt7921_mcu_set_beacon_filter(struct mwx_softc *sc, int enable)
{
	int rv;

	if (enable) {
#ifdef NOTYET
		rv = mt7921_mcu_uni_bss_bcnft(dev, vif, true);
		if (rv)
			return rv;
#endif
		mwx_set(sc, MT_WF_RFCR(0), MT_WF_RFCR_DROP_OTHER_BEACON);
	} else {
		rv = mt7921_mcu_set_bss_pm(sc, 0);
		if (rv)
			return rv;
		mwx_clear(sc, MT_WF_RFCR(0), MT_WF_RFCR_DROP_OTHER_BEACON);
	}
	return 0;
}

int
mt7921_mcu_set_bss_pm(struct mwx_softc *sc, int enable)
{
#ifdef NOTYET
	struct {
		uint8_t		bss_idx;
		uint8_t		dtim_period;
		uint16_t	aid;
		uint16_t	bcn_interval;
		uint16_t	atim_window;
		uint8_t		uapsd;
		uint8_t		bmc_delivered_ac;
		uint8_t		bmc_triggered_ac;
		uint8_t		pad;
	} req = {
		.bss_idx = mvif->mt76.idx,
		.aid = htole16(vif->cfg.aid),
		.dtim_period = vif->bss_conf.dtim_period,
		.bcn_interval = htole16(vif->bss_conf.beacon_int),
	};
#endif
	struct {
		uint8_t		bss_idx;
		uint8_t		pad[3];
	} req_hdr = {
		.bss_idx = /* mvif->mt76.idx XXX */ 0,
	};
	int rv;

	rv = mwx_mcu_send_msg(sc, MCU_CE_CMD_SET_BSS_ABORT,
		&req_hdr, sizeof(req_hdr), NULL);
#ifdef NOTYET
	if (rv != 0 || !enable)
		return rv;
	rv = mwx_mcu_send_msg(sc, MCU_CE_CMD_SET_BSS_CONNECTED,
		&req, sizeof(req), NULL);
#endif
	return rv;
}

#define IEEE80211_NUM_ACS	4
int
mt7921_mcu_set_tx(struct mwx_softc *sc, struct mwx_vif *mvif)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct edca {
		uint16_t	cw_min;
		uint16_t	cw_max;
		uint16_t	txop;
		uint16_t	aifs;
		uint8_t		guardtime;
		uint8_t		acm;
	} __packed;
	struct mt7921_mcu_tx {
		struct edca	edca[IEEE80211_NUM_ACS];
		uint8_t		bss_idx;
		uint8_t		qos;
		uint8_t		wmm_idx;
		uint8_t		pad;
	} __packed req = {
		.bss_idx = mvif->idx,
		.qos = ic->ic_bss != NULL && (ic->ic_bss->ni_flags & IEEE80211_NODE_QOS) != 0,
		.wmm_idx = mvif->wmm_idx,
	};
#ifdef NOTYET
	struct mu_edca {
		uint8_t		cw_min;
		uint8_t		cw_max;
		uint8_t		aifsn;
		uint8_t		acm;
		uint8_t		timer;
		uint8_t		padding[3];
	};
	struct mt7921_mcu_mu_tx {
		uint8_t		ver;
		uint8_t		pad0;
		uint16_t	len;
		uint8_t		bss_idx;
		uint8_t		qos;
		uint8_t		wmm_idx;
		uint8_t		pad1;
		struct mu_edca	edca[IEEE80211_NUM_ACS];
		uint8_t		pad3[32];
	} __packed req_mu = {
		.bss_idx = mvif->mt76.idx,
		.qos = vif->bss_conf.qos,
		.wmm_idx = mvif->mt76.wmm_idx,
	};
#endif
	static const int to_aci[] = { 1, 0, 2, 3 };
	int ac, rv;

	for (ac = 0; ac < IEEE80211_NUM_ACS; ac++) {
		struct ieee80211_edca_ac_params *q = &ic->ic_edca_ac[ac];
		struct edca *e = &req.edca[to_aci[ac]];

		e->aifs = htole16(q->ac_aifsn);
		e->txop = htole16(q->ac_txoplimit);
		e->cw_min = htole16((1U << MIN(q->ac_ecwmin, 15)) - 1);
		e->cw_max = htole16((1U << MIN(q->ac_ecwmax, 15)) - 1);
		e->acm = q->ac_acm;
	}

	rv = mwx_mcu_send_msg(sc, MCU_CE_CMD_SET_EDCA_PARMS, &req,
	    sizeof(req), NULL);

#ifdef NOTYET
	if (rv)
		return rv;
	if (!vif->bss_conf.he_support)
		return 0;

	for (ac = 0; ac < IEEE80211_NUM_ACS; ac++) {
		struct ieee80211_he_mu_edca_param_ac_rec *q;
		struct mu_edca *e;

		if (!mvif->queue_params[ac].mu_edca)
			break;

		q = &mvif->queue_params[ac].mu_edca_param_rec;
		e = &(req_mu.edca[to_aci[ac]]);

		e->cw_min = q->ecw_min_max & 0xf;
		e->cw_max = (q->ecw_min_max & 0xf0) >> 4;
		e->aifsn = q->aifsn;
		e->timer = q->mu_edca_timer;
	}

	rv = mt76_mcu_send_msg(&dev->mt76, MCU_CE_CMD(SET_MU_EDCA_PARMS),
	    &req_mu, sizeof(req_mu), false);
#endif
	return rv;
}

/* Decode P-RXV using the same mode/MCS/NSS/bandwidth fields as Linux mt7921. */
void
mwx_rx_rate(struct mwx_softc *sc, uint32_t v0)
{
	static const uint32_t ht[2][8] = {
		{ 6500, 13000, 19500, 26000, 39000, 52000, 58500, 65000 },
		{ 13500, 27000, 40500, 54000, 81000, 108000, 121500, 135000 },
	};
	static const uint32_t vht[3][10] = {
		{ 6500, 13000, 19500, 26000, 39000, 52000, 58500, 65000, 78000, 86667 },
		{ 13500, 27000, 40500, 54000, 81000, 108000, 121500, 135000, 162000, 180000 },
		{ 29250, 58500, 87750, 117000, 175500, 234000, 263250, 292500, 351000, 390000 },
	};
	static const uint8_t he_bits[] = { 1, 2, 2, 4, 4, 6, 6, 6, 8, 8, 10, 10 };
	static const uint8_t he_num[] = { 1, 1, 3, 1, 3, 2, 3, 5, 3, 5, 3, 5 };
	static const uint8_t he_den[] = { 2, 2, 4, 2, 4, 3, 4, 6, 4, 6, 4, 6 };
	static const uint16_t he_tones[] = { 234, 468, 980, 1960 };
	static const uint8_t he_gi[] = { 8, 16, 32 };
	uint32_t rate = 0;
	int mode = FIELD_GET(MT_PRXV_TX_MODE, v0);
	int mcs = FIELD_GET(MT_PRXV_TX_RATE, v0);
	int nss = FIELD_GET(MT_PRXV_NSTS, v0) + 1;
	int bw = FIELD_GET(MT_PRXV_FRAME_MODE, v0);
	int gi = FIELD_GET(MT_PRXV_HT_SGI, v0);
	u_int i;

	if (mode == MT_PHY_TYPE_CCK || mode == MT_PHY_TYPE_OFDM) {
		if (mode == MT_PHY_TYPE_CCK)
			mcs &= 3;
		for (i = 0; i < nitems(mt76_rates); i++)
			if (mt76_rates[i].hw_value == ((mode << 8) | mcs))
				rate = mt76_rates[i].rate * 500;
		nss = 1;
	} else if ((mode == MT_PHY_TYPE_HT || mode == MT_PHY_TYPE_HT_GF) &&
	    mcs < 32 && bw < 2) {
		nss = mcs / 8 + 1;
		rate = ht[bw][mcs % 8] * nss;
		if (gi)
			rate = (rate * 10 + 4) / 9;
	} else if (mode == MT_PHY_TYPE_VHT && mcs < 10 && nss <= 2 && bw < 3) {
		rate = vht[bw][mcs] * nss;
		if (gi)
			rate = (rate * 10 + 4) / 9;
	} else if (mode >= MT_PHY_TYPE_HE_SU && mode < __MT_PHY_TYPE_HE_MAX &&
	    (mcs & 15) < 12 && nss <= 2 && bw < 4 && gi < 3) {
		mcs &= 15;
		rate = (uint64_t)he_tones[bw] * he_bits[mcs] * he_num[mcs] * nss * 10000 /
		    (he_den[mcs] * (128 + he_gi[gi]));
	}
	if (rate == 0)
		return;
	sc->sc_rx_rate_kbps = rate;
	sc->sc_rx_rate_mode = mode;
	sc->sc_rx_rate_mcs = mcs;
	sc->sc_rx_rate_nss = nss;
	sc->sc_rx_rate_bw = bw;
	sc->sc_rx_rate_gi = gi;
}

int
mt7921_mac_fill_rx(struct mwx_softc *sc, struct mbuf *m,
    struct ieee80211_rxinfo *rxi)
{
	struct ieee80211com *ic = &sc->sc_ic;
	uint32_t *rxd, *rxv = NULL;
	uint32_t rxd0, rxd1, rxd2, rxd3, rxd4;
	uint32_t rate_v0 = 0;
	int have_rate = 0;
//	uint32_t mode = 0;
	uint16_t hdr_gap /*, seq_ctrl = 0, fc = 0 */;
	uint8_t chfnum, remove_pad /*, qos_ctl = 0, amsdu_info */;
	int idx, unicast, num_rxd = 6;
	int i;
//	bool insert_ccmp_hdr = false;

	if (mbuf_len(m) < num_rxd * sizeof(uint32_t)) {
		if (sc->sc_rx_fill_fail < 64) {
			MWX_DEV_LOG(sc,
			    "rx drop reason=short-rxd len=%zu need=%zu\n",
			    mbuf_len(m), num_rxd * sizeof(uint32_t));
		}
		return -1;
	}

	rxd = mtod(m, uint32_t *);
	rxd0 = le32toh(rxd[0]);
	rxd1 = le32toh(rxd[1]);
	rxd2 = le32toh(rxd[2]);
	rxd3 = le32toh(rxd[3]);
	rxd4 = le32toh(rxd[4]);
	sc->sc_last_rxd1 = rxd1;
	sc->sc_last_rxd3 = rxd3;
	sc->sc_last_rxd4 = rxd4;

	if (rxd1 & MT_RXD1_NORMAL_BAND_IDX) {
		MWX_RX_DROP_LOG(sc, "band-idx", -1, rxd0, rxd1, rxd2,
		    rxd3, rxd4);
		return -1;
	}

	if (rxd2 & MT_RXD2_NORMAL_AMSDU_ERR) {
		MWX_RX_DROP_LOG(sc, "amsdu-err", -1, rxd0, rxd1, rxd2,
		    rxd3, rxd4);
		return -1;
	}

	if (rxd2 & MT_RXD2_NORMAL_HDR_TRANS) {
		MWX_RX_DROP_LOG(sc, "hdr-trans", -1, rxd0, rxd1, rxd2,
		    rxd3, rxd4);
		return -1;
	}

	/* ICV error or CCMP/BIP/WPI MIC error */
	if (rxd1 & MT_RXD1_NORMAL_ICV_ERR) {
		MWX_RX_DROP_LOG(sc, "icv", -1, rxd0, rxd1, rxd2, rxd3,
		    rxd4);
		ic->ic_stats.is_rx_decryptcrc++;
		return -1;
	}

	if (rxd1 & MT_RXD1_NORMAL_FCS_ERR) {
		MWX_RX_DROP_LOG(sc, "fcs", -1, rxd0, rxd1, rxd2, rxd3,
		    rxd4);
		return -1;
	}

	if (rxd1 & MT_RXD1_NORMAL_TKIP_MIC_ERR) {
		/* report MIC failures to net80211 for TKIP */
		MWX_RX_DROP_LOG(sc, "tkip-mic", -1, rxd0, rxd1, rxd2,
		    rxd3, rxd4);
		ic->ic_stats.is_rx_locmicfail++;
		ieee80211_michael_mic_failure(ic, 0/* XXX */);
		return -1;
	}


	chfnum = (rxd3 & MT_RXD3_NORMAL_CH_NUM_MASK) >>
	    MT_RXD3_NORMAL_CH_NUM_SHIFT;
	unicast = (rxd3 & MT_RXD3_NORMAL_ADDR_TYPE_MASK) == MT_RXD3_NORMAL_U2M;
	idx = rxd1 & MT_RXD1_NORMAL_WLAN_IDX_MASK;

#if 0
	status->wcid = mt7921_rx_get_wcid(dev, idx, unicast);
	if (status->wcid) {
		struct mt7921_sta *msta;

		msta = container_of(status->wcid, struct mt7921_sta, wcid);
		spin_lock_bh(&dev->sta_poll_lock);
		if (list_empty(&msta->poll_list))
			list_add_tail(&msta->poll_list, &dev->sta_poll_list);
		spin_unlock_bh(&dev->sta_poll_lock);
	}
#endif

#if NOTYET
	if ((rxd0 & (MT_RXD0_NORMAL_IP_SUM | MT_RXD0_NORMAL_UDP_TCP_SUM)) ==
	    (MT_RXD0_NORMAL_IP_SUM | MT_RXD0_NORMAL_UDP_TCP_SUM)) {
		m->m_pkthdr.csum_flags = M_IPV4_CSUM_IN_OK |
		    M_TCP_CSUM_IN_OK | M_UDP_CSUM_IN_OK;
	}
#endif

	if ((rxd1 & MT_RXD1_NORMAL_SEC_MODE_MASK) != 0 &&
	    !(rxd1 & (MT_RXD1_NORMAL_CLM | MT_RXD1_NORMAL_CM))) {
		rxi->rxi_flags |= IEEE80211_RXI_HWDEC;
	}

	remove_pad = (rxd2 & MT_RXD2_NORMAL_HDR_OFFSET_MASK) >>
	    MT_RXD2_NORMAL_HDR_OFFSET_SHIFT;

	if (rxd2 & MT_RXD2_NORMAL_MAX_LEN_ERROR) {
		MWX_RX_DROP_LOG(sc, "max-len", -EINVAL, rxd0, rxd1, rxd2,
		    rxd3, rxd4);
		return -EINVAL;
	}

	rxd += 6;

	if (rxd1 & MT_RXD1_NORMAL_GROUP_4) {
		num_rxd += 4;
		rxd += 4;
	}
	if (rxd1 & MT_RXD1_NORMAL_GROUP_1) {
		num_rxd += 4;
		rxd += 4;
	}
	if (rxd1 & MT_RXD1_NORMAL_GROUP_2) {
		num_rxd += 2;
		rxd += 2;
	}
	if (rxd1 & MT_RXD1_NORMAL_GROUP_3) {	
		uint32_t v0, v1;
		int8_t chain[4], signal;

		rxv = rxd;
		num_rxd += 2;
		rxd += 2;

		v0 = le32toh(rxv[0]);
		v1 = le32toh(rxv[1]);
		rate_v0 = v0;
		have_rate = 1;

		if (rxd1 & MT_RXD1_NORMAL_GROUP_5) {
			num_rxd += 18;
			rxd += 6;
			rxv = rxd;
			v1 = le32toh(rxv[0]); 
			rxd += 12;
		}
	

		chain[0] = rcpi_to_rssi(MT_PRXV_RCPI0_SHIFT, v1);
		chain[1] = rcpi_to_rssi(MT_PRXV_RCPI1_SHIFT, v1);
		chain[2] = rcpi_to_rssi(MT_PRXV_RCPI2_SHIFT, v1);
		chain[3] = rcpi_to_rssi(MT_PRXV_RCPI3_SHIFT, v1);

		signal = -128;
		for (i = 0; i < sc->sc_capa.num_streams; i++) {
			if (!(sc->sc_capa.antenna_mask & (1U << i)))
				continue;	
			if (chain[i] >= 0)
				continue;
			signal = MAX(signal, chain[i]);
		}
		rxi->rxi_rssi = signal;
		}

	if (mbuf_len(m) < num_rxd * sizeof(uint32_t)) {
		if (sc->sc_rx_fill_fail < 64) {
			MWX_DEV_LOG(sc,
			    "rx drop reason=short-groups len=%zu need=%zu rxd=%08x/%08x/%08x/%08x/%08x\n",
			    mbuf_len(m), num_rxd * sizeof(uint32_t),
			    rxd0, rxd1, rxd2, rxd3, rxd4);
		}
		return -1;
	}

#if 0
	if (rxd1 & MT_RXD1_NORMAL_GROUP_4) {
		uint32_t v0 = le32toh(rxd[0]);
		uint32_t v2 = le32toh(rxd[2]);

		fc = htole16(FIELD_GET(MT_RXD6_FRAME_CONTROL, v0));
		seq_ctrl = FIELD_GET(MT_RXD8_SEQ_CTRL, v2);
		qos_ctl = FIELD_GET(MT_RXD8_QOS_CTL, v2);

		rxd += 4;
	}

	if (rxd1 & MT_RXD1_NORMAL_GROUP_1) {
		u8 *data = (u8 *)rxd;

		if (status->flag & RX_FLAG_DECRYPTED) {
			switch (FIELD_GET(MT_RXD1_NORMAL_SEC_MODE, rxd1)) {
			case MT_CIPHER_AES_CCMP:
			case MT_CIPHER_CCMP_CCX:
			case MT_CIPHER_CCMP_256:
				insert_ccmp_hdr =
					(rxd2 & MT_RXD2_NORMAL_FRAG);
				/* FALLTHROUGH */
			case MT_CIPHER_TKIP:
			case MT_CIPHER_TKIP_NO_MIC:
			case MT_CIPHER_GCMP:
			case MT_CIPHER_GCMP_256:
				status->iv[0] = data[5];
				status->iv[1] = data[4];
				status->iv[2] = data[3];
				status->iv[3] = data[2];
				status->iv[4] = data[1];
				status->iv[5] = data[0];
				break;
			default:
				break;
			}
		}
		rxd += 4;
	}

	if (rxd1 & MT_RXD1_NORMAL_GROUP_2) {
		status->timestamp = le32_to_cpu(rxd[0]);
		status->flag |= RX_FLAG_MACTIME_START;

		if (!(rxd2 & MT_RXD2_NORMAL_NON_AMPDU)) {
			status->flag |= RX_FLAG_AMPDU_DETAILS;

			/* all subframes of an A-MPDU have the same timestamp */
			if (phy->rx_ampdu_ts != status->timestamp) {
				if (!++phy->ampdu_ref)
					phy->ampdu_ref++;
			}
			phy->rx_ampdu_ts = status->timestamp;

			status->ampdu_ref = phy->ampdu_ref;
		}

		rxd += 2;
	}

	/* RXD Group 3 - P-RXV */
	if (rxd1 & MT_RXD1_NORMAL_GROUP_3) {
		u8 stbc, gi;
		u32 v0, v1;
		bool cck;

		rxv = rxd;
		rxd += 2;

		v0 = le32_to_cpu(rxv[0]);
		v1 = le32_to_cpu(rxv[1]);

		if (v0 & MT_PRXV_HT_AD_CODE)
			status->enc_flags |= RX_ENC_FLAG_LDPC;

		status->chains = mphy->antenna_mask;
		status->chain_signal[0] = to_rssi(MT_PRXV_RCPI0, v1);
		status->chain_signal[1] = to_rssi(MT_PRXV_RCPI1, v1);
		status->chain_signal[2] = to_rssi(MT_PRXV_RCPI2, v1);
		status->chain_signal[3] = to_rssi(MT_PRXV_RCPI3, v1);
		status->signal = -128;
		for (i = 0; i < hweight8(mphy->antenna_mask); i++) {
			if (!(status->chains & BIT(i)) ||
			    status->chain_signal[i] >= 0)
				continue;

			status->signal = max(status->signal,
					     status->chain_signal[i]);
		}

		stbc = FIELD_GET(MT_PRXV_STBC, v0);
		gi = FIELD_GET(MT_PRXV_SGI, v0);
		cck = false;

		idx = i = FIELD_GET(MT_PRXV_TX_RATE, v0);
		mode = FIELD_GET(MT_PRXV_TX_MODE, v0);

		switch (mode) {
		case MT_PHY_TYPE_CCK:
			cck = true;
			fallthrough;
		case MT_PHY_TYPE_OFDM:
			i = mt76_get_rate(&dev->mt76, sband, i, cck);
			break;
		case MT_PHY_TYPE_HT_GF:
		case MT_PHY_TYPE_HT:
			status->encoding = RX_ENC_HT;
			if (i > 31)
				return -EINVAL;
			break;
		case MT_PHY_TYPE_VHT:
			status->nss =
				FIELD_GET(MT_PRXV_NSTS, v0) + 1;
			status->encoding = RX_ENC_VHT;
			if (i > 9)
				return -EINVAL;
			break;
		case MT_PHY_TYPE_HE_MU:
		case MT_PHY_TYPE_HE_SU:
		case MT_PHY_TYPE_HE_EXT_SU:
		case MT_PHY_TYPE_HE_TB:
			status->nss =
				FIELD_GET(MT_PRXV_NSTS, v0) + 1;
			status->encoding = RX_ENC_HE;
			i &= GENMASK(3, 0);

			if (gi <= NL80211_RATE_INFO_HE_GI_3_2)
				status->he_gi = gi;

			status->he_dcm = !!(idx & MT_PRXV_TX_DCM);
			break;
		default:
			return -EINVAL;
		}

		status->rate_idx = i;

		switch (FIELD_GET(MT_PRXV_FRAME_MODE, v0)) {
		case IEEE80211_STA_RX_BW_20:
			break;
		case IEEE80211_STA_RX_BW_40:
			if (mode & MT_PHY_TYPE_HE_EXT_SU &&
			    (idx & MT_PRXV_TX_ER_SU_106T)) {
				status->bw = RATE_INFO_BW_HE_RU;
				status->he_ru =
					NL80211_RATE_INFO_HE_RU_ALLOC_106;
			} else {
				status->bw = RATE_INFO_BW_40;
			}
			break;
		case IEEE80211_STA_RX_BW_80:
			status->bw = RATE_INFO_BW_80;
			break;
		case IEEE80211_STA_RX_BW_160:
			status->bw = RATE_INFO_BW_160;
			break;
		default:
			return -EINVAL;
		}

		status->enc_flags |= RX_ENC_FLAG_STBC_MASK * stbc;
		if (mode < MT_PHY_TYPE_HE_SU && gi)
			status->enc_flags |= RX_ENC_FLAG_SHORT_GI;

		if (rxd1 & MT_RXD1_NORMAL_GROUP_5) {
			rxd += 18;
		}
	}

	amsdu_info = FIELD_GET(MT_RXD4_NORMAL_PAYLOAD_FORMAT, rxd4);
	status->amsdu = !!amsdu_info;
	if (status->amsdu) {
		status->first_amsdu = amsdu_info == MT_RXD4_FIRST_AMSDU_FRAME;
		status->last_amsdu = amsdu_info == MT_RXD4_LAST_AMSDU_FRAME;
	}
#endif

	hdr_gap = num_rxd * sizeof(uint32_t) + 2 * remove_pad;
	if (mbuf_len(m) < hdr_gap + sizeof(struct ieee80211_frame_min)) {
		if (sc->sc_rx_fill_fail < 64) {
			MWX_DEV_LOG(sc,
			    "rx drop reason=short-80211 len=%zu hdr_gap=%u rxd=%08x/%08x/%08x/%08x/%08x\n",
			    mbuf_len(m), hdr_gap, rxd0, rxd1, rxd2, rxd3,
			    rxd4);
		}
		return -1;
	}
	m_adj(m, hdr_gap);
	/* mt7921_mac_fill_rx(): split A-MSDUs have two bytes after the header. */
	if (rxd4 & MT_RXD4_NORMAL_PAYLOAD_FORMAT) {
		struct ieee80211_frame *wh = mtod(m, struct ieee80211_frame *);
		int len;

		if (!ieee80211_has_qos(wh))
			return -EINVAL;
		len = ieee80211_get_hdrlen(wh);
		if (mbuf_len(m) < (size_t)len + 2 + ETHER_HDR_LEN)
			return -EINVAL;
		memmove(mtod(m, uint8_t *) + 2, wh, len);
		m_adj(m, 2);
	}
#if 0
	if (status->amsdu) {
		memmove(skb->data + 2, skb->data,
			ieee80211_get_hdrlen_from_skb(skb));
		skb_pull(skb, 2);
	}

	struct ieee80211_hdr *hdr;

	if (insert_ccmp_hdr) {
		u8 key_id = FIELD_GET(MT_RXD1_NORMAL_KEY_ID, rxd1);

		mt76_insert_ccmp_hdr(skb, key_id);
	}

	hdr = mt76_skb_get_hdr(skb);
	fc = hdr->frame_control;
	if (ieee80211_is_data_qos(fc)) {
		seq_ctrl = le16_to_cpu(hdr->seq_ctrl);
		qos_ctl = *ieee80211_get_qos_ctl(hdr);
	}

	if (!status->wcid || !ieee80211_is_data_qos(fc))
		return 0;

	status->aggr = unicast && !ieee80211_is_qos_nullfunc(fc);
	status->seqno = IEEE80211_SEQ_TO_SN(seq_ctrl);
	status->qos_ctl = qos_ctl;
#endif
	rxi->rxi_chan = chfnum;
	if (have_rate && unicast && sc->sc_ic.ic_state == IEEE80211_S_RUN &&
	    (mtod(m, struct ieee80211_frame *)->i_fc[0] & IEEE80211_FC0_TYPE_MASK) == IEEE80211_FC0_TYPE_DATA)
		mwx_rx_rate(sc, rate_v0);

	if (rxi->rxi_flags & IEEE80211_RXI_HWDEC) {
		/*
		 * MT7921/MT7922 firmware decrypts and removes CCMP IV/MIC.
		 * Leave the protected bit clear so net80211 does not try to
		 * strip an IV which is no longer present.
		 */
		struct ieee80211_frame *wh;

		wh = mtod(m, struct ieee80211_frame *);
		wh->i_fc[1] &= ~IEEE80211_FC1_PROTECTED;
	}

	return 0;
}

uint32_t
mt7921_mac_tx_rate_val(struct mwx_softc *sc)
{
	int rateidx = 0, offset = 4;
	uint32_t rate, mode;

	/* XXX TODO basic_rates
	rateidx = ffs(vif->bss_conf.basic_rates) - 1;
	*/

	if (IEEE80211_IS_CHAN_2GHZ(sc->sc_ic.ic_bss->ni_chan))
		offset = 0;
	/* pick the lowest rate for hidden nodes */
	if (rateidx < 0)
		rateidx = 0;

	rateidx += offset;

	if (rateidx >= nitems(mt76_rates))
		rateidx = offset;

	rate =  mt76_rates[rateidx].hw_value;
	mode = (rate >> 8) << MT_TX_RATE_MODE_SHIFT;
	rate &= 0xff;

	return (rate & MT_TX_RATE_IDX_MASK) | (mode & MT_TX_RATE_MODE_MASK);
}

void
mt7921_mac_write_txwi_80211(struct mwx_softc *sc, struct mbuf *m,
    struct ieee80211_node *ni, struct mt76_txwi *txp)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_frame *wh;
	uint32_t val;
	uint8_t ftype, type, subtype, tid = 0;
	u_int hdrlen;
	int multicast, eapol = 0;


	wh = mtod(m, struct ieee80211_frame *);
	/*
	 * ftype keeps the IEEE80211_FC0_TYPE_* encoding for comparisons; type and
	 * subtype are shifted for the TXD fields. Comparing the shifted type with
	 * IEEE80211_FC0_TYPE_DATA sent every data frame at the fixed 6 Mbps rate.
	 */
	ftype = wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK;
	type = ftype >> IEEE80211_FC0_TYPE_SHIFT;
	subtype = wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK;
	subtype >>= IEEE80211_FC0_SUBTYPE_SHIFT;
	multicast = IEEE80211_IS_MULTICAST(wh->i_addr1);

	if (ftype == IEEE80211_FC0_TYPE_CTL)
		hdrlen = sizeof(struct ieee80211_frame_min);
	else
		hdrlen = ieee80211_get_hdrlen(wh);

	/* EAPOL keeps the robust basic rate, like mac80211's USE_MINRATE */
	if (ftype == IEEE80211_FC0_TYPE_DATA &&
	    mbuf_pkthdr_len(m) >= hdrlen + 8) {
		uint8_t llc[8];

		mbuf_copydata(m, hdrlen, sizeof(llc), llc);
		eapol = llc[6] == 0x88 && llc[7] == 0x8e;
	}

	/* Put QoS frames on the data queue which maps to their TID. */
	if (ieee80211_has_qos(wh)) {
		uint16_t qos = ieee80211_get_qos(wh);

		tid = qos & IEEE80211_QOS_TID;
		if ((__atomic_load_n(&sc->sc_tx_ba_mask, __ATOMIC_SEQ_CST) & (1U << tid)) == 0)
			txp->txwi[3] |= htole32(MT_TXD3_BA_DISABLE);
		if (__atomic_load_n(&sc->sc_tx_amsdu_mask, __ATOMIC_SEQ_CST) & (1U << tid))
			txp->txwi[7] |= htole32(MT_TXD7_HW_AMSDU);
	}

#ifdef NOTYET
	if (ieee80211_is_action(fc) &&
	    mgmt->u.action.category == WLAN_CATEGORY_BACK &&
	    mgmt->u.action.u.addba_req.action_code == WLAN_ACTION_ADDBA_REQ) {
		u16 capab = le16_to_cpu(mgmt->u.action.u.addba_req.capab);

		txp->txwi[5] |= htole32(MT_TXD5_ADD_BA);
		tid = (capab >> 2) & IEEE80211_QOS_CTL_TID_MASK;
	} else if (ieee80211_is_back_req(hdr->frame_control)) {
		struct ieee80211_bar *bar = (struct ieee80211_bar *)hdr;
		u16 control = le16_to_cpu(bar->control);

		tid = FIELD_GET(IEEE80211_BAR_CTRL_TID_INFO_MASK, control);
	}
#endif

	val = MT_HDR_FORMAT_802_11 | MT_TXD1_HDR_INFO(hdrlen / 2) |
	    MT_TXD1_TID(tid);
	txp->txwi[1] |= htole32(val);

	val = MT_TXD2_FRAME_TYPE(type) | MT_TXD2_SUB_TYPE(subtype);
	if (multicast)
		val |= MT_TXD2_MULTICAST;

#ifdef NOTYET
	if (key && multicast && ieee80211_is_robust_mgmt_frame(skb) &&
	    key->cipher == WLAN_CIPHER_SUITE_AES_CMAC) {
		val |= MT_TXD2_BIP;
		txp->txwi[3] &= ~htole32(MT_TXD3_PROTECT_FRAME);
	}
#endif

	if (multicast || ftype != IEEE80211_FC0_TYPE_DATA || eapol) {
		/* Fixed rata is available just for 802.11 txd */
		uint32_t rate, val6;

		val |= MT_TXD2_FIX_RATE;
		/* hardware won't add HTC for mgmt/ctrl frame */
		val |= htole32(MT_TXD2_HTC_VLD);

		rate = mt7921_mac_tx_rate_val(sc);

		val6 = MT_TXD6_FIXED_BW;
		val6 |= (rate << MT_TXD6_TX_RATE_SHIFT) & MT_TXD6_TX_RATE_MASK;
		txp->txwi[6] |= htole32(val6);
		txp->txwi[3] |= htole32(MT_TXD3_BA_DISABLE);
	}

	txp->txwi[2] |= htole32(val);

#ifdef NOTYET
	if (ieee80211_is_beacon(fc)) {
		txp->txwi[3] &= ~htole32(MT_TXD3_SW_POWER_MGMT);
		txp->txwi[3] |= htole32(MT_TXD3_REM_TX_COUNT);
	}

	if (info->flags & IEEE80211_TX_CTL_INJECTED) {
		u16 seqno = le16_to_cpu(hdr->seq_ctrl);

		if (ieee80211_is_back_req(hdr->frame_control)) {
			struct ieee80211_bar *bar;

			bar = (struct ieee80211_bar *)skb->data;
			seqno = le16_to_cpu(bar->start_seq_num);
		}

		val = MT_TXD3_SN_VALID |
			FIELD_PREP(MT_TXD3_SEQ, IEEE80211_SEQ_TO_SN(seqno));
		txp->txwi[3] |= htole32(val);
		txp->txwi[7] &= ~htole32(MT_TXD7_HW_AMSDU);
	}
#endif

	val = MT_TXD7_TYPE(type) | MT_TXD7_SUB_TYPE(subtype);
	txp->txwi[7] |= htole32(val);

#if NBPFILTER > 0
	if (__predict_false(sc->sc_drvbpf != NULL)) {
		struct mwx_tx_radiotap_header *tap = &sc->sc_txtap;
		uint16_t chan_flags;

		tap->wt_flags = 0;
		tap->wt_chan_freq = htole16(ni->ni_chan->ic_freq);
		chan_flags = ni->ni_chan->ic_flags;
		if (ic->ic_curmode != IEEE80211_MODE_11N &&
			ic->ic_curmode != IEEE80211_MODE_11AC) {
			chan_flags &= ~IEEE80211_CHAN_HT;
			chan_flags &= ~IEEE80211_CHAN_40MHZ;
		}
		if (ic->ic_curmode != IEEE80211_MODE_11AC)
			chan_flags &= ~IEEE80211_CHAN_VHT;
		tap->wt_chan_flags = htole16(chan_flags);
#ifdef NOTYET
		if ((ni->ni_flags & IEEE80211_NODE_HT) &&
		    !IEEE80211_IS_MULTICAST(wh->i_addr1) &&
		    type == IEEE80211_FC0_TYPE_DATA &&
		    rinfo->ht_plcp != MWX_RATE_HT_SISO_MCS_INV_PLCP) {
			tap->wt_rate = (0x80 | rinfo->ht_plcp);
		} else
			tap->wt_rate = rinfo->rate;
#endif
		tap->wt_rate = 2;
		if ((ic->ic_flags & IEEE80211_F_WEPON) &&
		    (wh->i_fc[1] & IEEE80211_FC1_PROTECTED))
			tap->wt_flags |= IEEE80211_RADIOTAP_F_WEP;

		bpf_mtap_hdr(sc->sc_drvbpf, tap, sc->sc_txtap_len,
		    m, BPF_DIRECTION_OUT);
	}
#endif

}

static inline uint8_t
mt7921_lmac_mapping(uint8_t ac)
{
	/* LMAC uses the reverse order of mac80211 AC indexes */
	return 3 - ac;
}

void
mt7921_mac_write_txwi(struct mwx_softc *sc, struct mbuf *m,
    struct ieee80211_node *ni, struct mt76_txwi *txp)
{
	struct mwx_node *mn = (struct mwx_node *)ni;
	struct ieee80211_frame *wh;
	uint32_t val, p_fmt, omac_idx;
	uint8_t q_idx, wmm_idx, band_idx;
	uint8_t phy_idx = 0;
	/* XXX hardcoded and wrong */
	int pid = MT_PACKET_ID_FIRST;
	enum mt76_txq_id qid = MT_TXQ_BE;
	static const uint8_t tid_to_ac[8] = {
		MT_TXQ_BE, MT_TXQ_BK, MT_TXQ_BK, MT_TXQ_BE,
		MT_TXQ_VI, MT_TXQ_VI, MT_TXQ_VO, MT_TXQ_VO,
	};

	wh = mtod(m, struct ieee80211_frame *);
	if (ieee80211_has_qos(wh))
		qid = (enum mt76_txq_id)tid_to_ac[ieee80211_get_qos(wh) & 7];
	else if ((wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK) != IEEE80211_FC0_TYPE_DATA)
		qid = MT_TXQ_VO;
	if ((wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK) == IEEE80211_FC0_TYPE_DATA)
		sc->sc_tx_data_frames++;
	omac_idx = sc->sc_vif.omac_idx << MT_TXD1_OWN_MAC_SHIFT;
	wmm_idx = sc->sc_vif.wmm_idx;
	band_idx = sc->sc_vif.band_idx;

	if (qid >= MT_TXQ_PSD) {
		p_fmt = MT_TX_TYPE_CT;
		q_idx = MT_LMAC_ALTX0;
	} else {
		p_fmt = MT_TX_TYPE_CT;
		q_idx = wmm_idx * MWX_MAX_WMM_SETS +
		    mt7921_lmac_mapping(qid);

#ifdef NOTYET
		/* counting non-offloading skbs */
		wcid->stats.tx_bytes += skb->len;
		wcid->stats.tx_packets++;
#endif
	}

	val = ((mbuf_pkthdr_len(m) + MT_TXD_SIZE) & MT_TXD0_TX_BYTES_MASK) |
		p_fmt | MT_TXD0_Q_IDX(q_idx);
	txp->txwi[0] = htole32(val);

	val = MT_TXD1_LONG_FORMAT | (mn->wcid & MT_TXD1_WLAN_IDX_MASK) |
	    (omac_idx & MT_TXD1_OWN_MAC_MASK);
	if (phy_idx || band_idx)
		val |= MT_TXD1_TGID;
	txp->txwi[1] = htole32(val);
	txp->txwi[2] = 0;

	val = 15 << MT_TXD3_REM_TX_COUNT_SHIFT;
	if (wh->i_fc[1] & IEEE80211_FC1_PROTECTED)
		val |= MT_TXD3_PROTECT_FRAME;
#ifdef NOTYET
	if (info->flags & IEEE80211_TX_CTL_NO_ACK)
		val |= MT_TXD3_NO_ACK;
#endif
	txp->txwi[3] = htole32(val);
	txp->txwi[4] = 0;

	val = pid & MT_TXD5_PID;
	if (pid >= MT_PACKET_ID_FIRST)
		val |= MT_TXD5_TX_STATUS_HOST;
	txp->txwi[5] = htole32(val);
	txp->txwi[6] = 0;
	txp->txwi[7] = /* XXX wcid->amsdu ? htole32(MT_TXD7_HW_AMSDU) : */ 0;

#ifdef NOTYET
	if (is_8023)
		mt76_connac2_mac_write_txwi_8023(txp, m, wcid);
	else
#endif
		mt7921_mac_write_txwi_80211(sc, m, ni, txp);
}

void
mwx_mac_tx_free(struct mwx_softc *sc, struct mbuf *m)
{
	struct mwx_txwi *mt;
	uint32_t *txfree;
	uint32_t txval;
	int count, i;

	mwx_dma_tx_cleanup(sc, &sc->sc_txq);

	if ((m = m_pullup(m, mbuf_pkthdr_len(m))) == NULL)
		return;

	txfree = mtod(m, uint32_t *);
	txval = le32toh(txfree[0]);
	m_adj(m, 2 * sizeof(txval));

	count = MT_TX_FREE0_MSDU_CNT_GET(txval);

	if (count * sizeof(txval) > mbuf_len(m))
		goto out;

	txfree = mtod(m, uint32_t *);
	for (i = 0; i < count; i++) {
		uint16_t msdu;

		txval = le32toh(txfree[i]);
		if (txval & MT_TX_FREE_PAIR) {
			count++;
			if (count * sizeof(txval) > mbuf_len(m))
				break;
			continue;
		}

		msdu = MT_TX_FREE_MSDU_ID_GET(txval);
		if (msdu >= sc->sc_txwi.mt_count)
			continue;
		lck_mtx_lock(sc->sc_txlock);
		mt = &sc->sc_txwi.mt_data[msdu];
		if (mt->mt_busy != 0)
			mwx_txwi_put(sc, mt);
		lck_mtx_unlock(sc->sc_txlock);
	}

out:
	m_freem(m);
}

int
mt7921_set_channel(struct mwx_softc *sc)
{
	int rv;

	/* stop queues, block other configs (MT76_RESET) */
	// XXX NOTYET mt76_set_channel(sc);

	rv = mt7921_mcu_set_chan_info(sc, MCU_EXT_CMD_CHANNEL_SWITCH);
	if (rv)
		return rv;
	mt7921_mac_set_timing(sc);
	mt7921_mac_reset_counters(sc);

	/* restart queues */
	return 0;
}

/*
 * Linux mt76_connac_get_phy_mode_v2() for a station: the modes negotiated
 * on the association response (before that only the legacy ones, as Linux
 * sends 0x08 at AUTH and 0x78 after associating to an 11ax AP).
 */
uint8_t
mt7921_get_phy_mode_v2(struct mwx_softc *sc, struct ieee80211_node *ni)
{
	uint8_t mode = 0;

	if (ni == NULL)
		ni = sc->sc_ic.ic_bss;

	if (IEEE80211_IS_CHAN_2GHZ(ni->ni_chan)) {
		mode |= PHY_TYPE_BIT_HR_DSSS | PHY_TYPE_BIT_ERP;
		if (ni->ni_flags & IEEE80211_NODE_HT)
			mode |= PHY_TYPE_BIT_HT;
		if (ni->ni_flags & IEEE80211_NODE_HE)
			mode |= PHY_TYPE_BIT_HE;
	} else if (IEEE80211_IS_CHAN_5GHZ(ni->ni_chan) /* || CHAN_6GHZ */) {
		mode |= PHY_TYPE_BIT_OFDM;
		if (ni->ni_flags & IEEE80211_NODE_HT)
			mode |= PHY_TYPE_BIT_HT;
		if (ni->ni_flags & IEEE80211_NODE_VHT)
			mode |= PHY_TYPE_BIT_VHT;
		if (ni->ni_flags & IEEE80211_NODE_HE)
			mode |= PHY_TYPE_BIT_HE;
	}
	return mode;
}

/*
 * Forget per-link state when leaving RUN: the channel width, Block Ack
 * work still queued for the old AP and a half-collected A-MSDU.
 */
void
mwx_link_reset(struct mwx_softc *sc)
{
	sc->sc_link_generation++;
	sc->sc_tx_ba_mask = sc->sc_tx_amsdu_mask = 0;
	sc->sc_chan_ctrl = sc->sc_chan_center = 0;
	sc->sc_chan_bw = CMD_CBW_20MHZ;
	__atomic_store_n(&sc->sc_ba_rx_start, 0, __ATOMIC_SEQ_CST);
	__atomic_store_n(&sc->sc_ba_rx_stop, 0, __ATOMIC_SEQ_CST);
	__atomic_store_n(&sc->sc_ba_tx_start, 0, __ATOMIC_SEQ_CST);
	__atomic_store_n(&sc->sc_ba_tx_stop, 0, __ATOMIC_SEQ_CST);
	if (sc->sc_amsdu_head != NULL) {
		m_freem(sc->sc_amsdu_head);
		sc->sc_amsdu_head = NULL;
	}
	sc->sc_rx_rate_kbps = 0;
	sc->sc_link_up_ns = 0;
}

/* Linux mt76_connac_mcu_sta_ba(): update WTBL first, then STA_REC_BA. */
int
mwx_mcu_set_ba(struct mwx_softc *sc, struct ieee80211_node *ni,
    uint8_t tid, int tx, int enable)
{
	struct mwx_node *mn = (struct mwx_node *)ni;
	struct mwx_vif *mvif = &sc->sc_vif;
	struct sta_rec_wtbl *wtbl;
	struct wtbl_ba *wb;
	struct sta_rec_ba *ba;
	struct mbuf *m;
	uint16_t tlvnum = 0, wnum = 0;
	uint16_t ssn = tx ? ni->ni_tx_ba[tid].ba_winstart : ni->ni_rx_ba[tid].ba_winstart;
	uint16_t winsize = tx ? ni->ni_tx_ba[tid].ba_winsize : ni->ni_rx_ba[tid].ba_winsize;
	uint16_t params = tx ? ni->ni_tx_ba[tid].ba_params : ni->ni_rx_ba[tid].ba_params;
	int rv;

	m = mwx_alloc_sta_req_tlv(sizeof(struct sta_req_hdr));
	if (m == NULL)
		return ENOBUFS;
	wtbl = (struct sta_rec_wtbl *)mwx_append_tlv(m, &tlvnum,
	    STA_REC_WTBL, sizeof(*wtbl));
	wtbl->wlan_idx_lo = mn->wcid & 0xff;
	wtbl->wlan_idx_hi = mn->wcid >> 8;
	wtbl->operation = WTBL_SET;
	wb = (struct wtbl_ba *)mwx_append_tlv(m, &wnum, WTBL_BA, sizeof(*wb));
	wb->tid = tid;
	wb->ba_type = tx ? MT_BA_TYPE_ORIGINATOR : MT_BA_TYPE_RECIPIENT;
	wb->ba_winsize = enable ? htole16(winsize) : 0;
	if (tx) {
		wb->sn = enable ? htole16(ssn) : 0;
		wb->ba_en = enable;
	} else {
		memcpy(wb->peer_addr, ni->ni_macaddr, sizeof(wb->peer_addr));
		wb->rst_ba_tid = tid;
		wb->rst_ba_sel = RST_BA_MAC_TID_MATCH;
		wb->rst_ba_sb = 1;
	}
	wtbl->tlv_num = htole16(wnum);
	wtbl->len = htole16(sizeof(*wtbl) + sizeof(*wb));
	m = mwx_fill_sta_req_hdr(m, mvif, mvif->omac_idx, mn->wcid, tlvnum);
	if (m == NULL)
		return ENOBUFS;
	rv = mwx_mcu_send_mbuf_wait(sc, MCU_UNI_CMD_STA_REC_UPDATE, m);
	if (rv != 0)
		return rv;

	m = mwx_alloc_sta_req_tlv(sizeof(struct sta_req_hdr));
	if (m == NULL)
		return ENOBUFS;
	tlvnum = 0;
	ba = (struct sta_rec_ba *)mwx_append_tlv(m, &tlvnum, STA_REC_BA, sizeof(*ba));
	ba->tid = tid;
	ba->ba_type = tx ? MT_BA_TYPE_ORIGINATOR : MT_BA_TYPE_RECIPIENT;
	ba->winsize = htole16(winsize);
	ba->ssn = htole16(ssn);
	ba->ba_en = enable << tid;
	ba->amsdu = (params & IEEE80211_ADDBA_AMSDU) != 0;
	m = mwx_fill_sta_req_hdr(m, mvif, mvif->omac_idx, mn->wcid, tlvnum);
	if (m == NULL)
		return ENOBUFS;
	return mwx_mcu_send_mbuf_wait(sc, MCU_UNI_CMD_STA_REC_UPDATE, m);
}

/* EBUSY tells net80211 to wait for the firmware task before accepting BA. */
int
mwx_ampdu_rx_start(struct ieee80211com *ic, struct ieee80211_node *ni,
    uint8_t tid)
{
	struct mwx_softc *sc = (struct mwx_softc *)ic->ic_softc;

	if (ni != ic->ic_bss || tid >= 8 || sc->sc_phy_limit < MWX_PHY_LIMIT_HT)
		return ENOTSUP;
	__atomic_fetch_or(&sc->sc_ba_rx_start, 1U << tid, __ATOMIC_SEQ_CST);
	task_add(sc->sc_nswq, &sc->sc_ba_task);
	return EBUSY;
}

void
mwx_ampdu_rx_stop(struct ieee80211com *ic, struct ieee80211_node *ni,
    uint8_t tid)
{
	struct mwx_softc *sc = (struct mwx_softc *)ic->ic_softc;

	if (ni != ic->ic_bss || tid >= 8)
		return;
	__atomic_fetch_or(&sc->sc_ba_rx_stop, 1U << tid, __ATOMIC_SEQ_CST);
	task_add(sc->sc_nswq, &sc->sc_ba_task);
}

int
mwx_ampdu_tx_start(struct ieee80211com *ic, struct ieee80211_node *ni,
    uint8_t tid)
{
	struct mwx_softc *sc = (struct mwx_softc *)ic->ic_softc;

	if (ni != ic->ic_bss || tid >= 8 || sc->sc_phy_limit < MWX_PHY_LIMIT_HT)
		return ENOTSUP;
	__atomic_fetch_or(&sc->sc_ba_tx_start, 1U << tid, __ATOMIC_SEQ_CST);
	task_add(sc->sc_nswq, &sc->sc_ba_task);
	return EBUSY;
}

void
mwx_ampdu_tx_stop(struct ieee80211com *ic, struct ieee80211_node *ni,
    uint8_t tid)
{
	struct mwx_softc *sc = (struct mwx_softc *)ic->ic_softc;

	if (ni != ic->ic_bss || tid >= 8)
		return;
	__atomic_fetch_and(&sc->sc_tx_ba_mask, ~(1U << tid), __ATOMIC_SEQ_CST);
	__atomic_fetch_and(&sc->sc_tx_amsdu_mask, ~(1U << tid), __ATOMIC_SEQ_CST);
	__atomic_fetch_or(&sc->sc_ba_tx_stop, 1U << tid, __ATOMIC_SEQ_CST);
	task_add(sc->sc_nswq, &sc->sc_ba_task);
}

void
mwx_ba_task(void *arg)
{
	struct mwx_softc *sc = (struct mwx_softc *)arg;
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_node *ni = ic->ic_bss;
	uint32_t start[2], stop[2], generation = sc->sc_link_generation;
	int tx, tid;

	start[0] = __atomic_exchange_n(&sc->sc_ba_rx_start, 0, __ATOMIC_SEQ_CST);
	start[1] = __atomic_exchange_n(&sc->sc_ba_tx_start, 0, __ATOMIC_SEQ_CST);
	stop[0] = __atomic_exchange_n(&sc->sc_ba_rx_stop, 0, __ATOMIC_SEQ_CST);
	stop[1] = __atomic_exchange_n(&sc->sc_ba_tx_stop, 0, __ATOMIC_SEQ_CST);
	if (ni == NULL || ic->ic_state != IEEE80211_S_RUN)
		return;
	ieee80211_ref_node(ni);
	for (tx = 0; tx < 2; tx++) {
		for (tid = 0; tid < 8; tid++) {
			uint32_t bit = 1U << tid;
			int enable = (stop[tx] & bit) == 0;
			int rv, state = tx ? ni->ni_tx_ba[tid].ba_state : ni->ni_rx_ba[tid].ba_state;

			if (((start[tx] | stop[tx]) & bit) == 0)
				continue;
			if (enable && state != IEEE80211_BA_REQUESTED)
				continue;
			rv = mwx_mcu_set_ba(sc, ni, tid, tx, enable);
			if (sc->sc_link_generation != generation || ic->ic_bss != ni ||
			    ic->ic_state != IEEE80211_S_RUN)
				goto out;
			if (rv != 0 || sc->sc_ba_log++ < 8)
				MWX_DEV_LOG(sc, "BA %s tid=%d enable=%d error=%d\n",
				    tx ? "tx" : "rx", tid, enable, rv);
			if (!enable)
				continue;
			state = tx ? ni->ni_tx_ba[tid].ba_state : ni->ni_rx_ba[tid].ba_state;
			if (state != IEEE80211_BA_REQUESTED)
				continue;
			if (rv != 0) {
				mwx_mcu_set_ba(sc, ni, tid, tx, 0);
				if (tx)
					ieee80211_addba_resp_refuse(ic, ni, tid, IEEE80211_STATUS_UNSPECIFIED);
				else
					ieee80211_addba_req_refuse(ic, ni, tid);
			} else if (tx) {
				__atomic_fetch_or(&sc->sc_tx_ba_mask, bit, __ATOMIC_SEQ_CST);
				if (ni->ni_tx_ba[tid].ba_params & IEEE80211_ADDBA_AMSDU)
					__atomic_fetch_or(&sc->sc_tx_amsdu_mask, bit, __ATOMIC_SEQ_CST);
				ieee80211_addba_resp_accept(ic, ni, tid);
			} else {
				ieee80211_addba_req_accept(ic, ni, tid);
			}
		}
	}
out:
	ieee80211_release_node(ic, ni);
}

struct mbuf *
mwx_alloc_sta_req_tlv(int len)
{
	struct mbuf *m;

	/* Allocate mbuf cluster with enough space */
	m = MCLGETL(NULL, M_DONTWAIT, MCLBYTES);
	if (m == NULL)
		return NULL;

	/* align to have space for the mcu header */
	mbuf_setdata(m, (uint8_t *)mbuf_data(m) + sizeof(struct mwx_mcu_txd) + len, 0);
	mbuf_setlen(m, 0);
	mbuf_pkthdr_setlen(m, 0);

	return m;
}

void *
mwx_append_len(struct mbuf *m, int len)
{
	caddr_t p;
	size_t oldlen;

	KASSERT(m_trailingspace(m) >= len);

	oldlen = mbuf_len(m);
	p = mtod(m, caddr_t) + oldlen;
	mbuf_setlen(m, oldlen + len);
	mbuf_pkthdr_setlen(m, mbuf_len(m));
	memset(p, 0, len);

	return p;
}

/*
 * Reserve len bytes at the end of mbuf m, return to start of that area
 * after initializing the data. It also sets the tag and len hdr.
 */
void *
mwx_append_tlv(struct mbuf *m, uint16_t *tlvnum, int tag, int len)
{
	struct {
		uint16_t	tag;
		uint16_t	len;
	} tlv = {
		.tag = htole16(tag),
		.len = htole16(len),
	};
	caddr_t p;

	p = (caddr_t)mwx_append_len(m, len);
	memcpy(p, &tlv, sizeof(tlv));

	*tlvnum += 1;

	return p;
}

struct mbuf *
mwx_fill_sta_req_hdr(struct mbuf *m, struct mwx_vif *mvif,
    uint8_t muar_idx, uint16_t wcid, uint16_t tlvnum)
{
	struct sta_req_hdr *hdr;

	KASSERT(m_leadingspace(m) >= sizeof(struct sta_req_hdr));
	m = m_prepend(m, sizeof(struct sta_req_hdr), M_DONTWAIT);
	if (m == NULL)
		return NULL;
	hdr = mtod(m, struct sta_req_hdr *);
	memset(hdr, 0, sizeof(struct sta_req_hdr));

	hdr->bss_idx = mvif->idx;
	hdr->wlan_idx_lo = wcid & 0xff;
	hdr->wlan_idx_hi = wcid >> 8;
	hdr->muar_idx = muar_idx;
	hdr->is_tlv_append = 1;
	hdr->tlv_num = htole16(tlvnum);
	return m;
}

void
mt7921_mcu_add_basic_tlv(struct mbuf *m, uint16_t *tlvnum, struct mwx_softc *sc,
    struct ieee80211_node *ni, int add, int isnew)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct sta_rec_basic *basic;

	basic = (struct sta_rec_basic *)mwx_append_tlv(m, tlvnum,
	    STA_REC_BASIC, sizeof(*basic));

	basic->extra_info = htole16(EXTRA_INFO_VER);
	if (add) {
		if (isnew)
			basic->extra_info |= htole16(EXTRA_INFO_NEW);
		basic->conn_state = CONN_STATE_PORT_SECURE;
	} else {
		basic->conn_state = CONN_STATE_DISCONNECT;
	}

	if (ni == NULL) {
		basic->conn_type = htole32(STA_TYPE_BC | NETWORK_INFRA);
		memset(basic->peer_addr, 0xff, sizeof(basic->peer_addr));
		return;
	}

	switch (ic->ic_opmode) {
#ifndef IEEE80211_STA_ONLY
	case IEEE80211_M_HOSTAP:
		basic->conn_type = htole32(STA_TYPE_STA | NETWORK_INFRA);
		break;
#endif
	case IEEE80211_M_STA:
		basic->conn_type = htole32(STA_TYPE_AP | NETWORK_INFRA);
		break;
#ifndef IEEE80211_STA_ONLY
	case IEEE80211_M_IBSS:
	case IEEE80211_M_AHDEMO:
		basic->conn_type = htole32(STA_TYPE_ADHOC | NETWORK_IBSS);
		break;
#endif
	case IEEE80211_M_MONITOR:
		panic("mt7921_mcu_sta_basic_tlv unexpected operation mode");
	}

	basic->aid = htole16(IEEE80211_AID(ni->ni_associd));
	memcpy(basic->peer_addr, ni->ni_macaddr, IEEE80211_ADDR_LEN);
	basic->qos = (ni->ni_flags & IEEE80211_NODE_QOS) != 0;
}

/* Linux mt76_connac_mcu_sta_he_tlv() / sta_he_tlv_v2(). */
void
mt7921_mcu_add_he_tlv(struct mbuf *m, uint16_t *tlvnum,
    struct ieee80211_node *ni)
{
	const struct ieee80211_he_cap_elem *elem = &ni->ni_he_cap_elem;
	struct sta_rec_he *he;
	struct sta_rec_he_v2 *v2;
	struct he_cap_bit {
		uint8_t byte;
		uint8_t mask;
		uint32_t flag;
	};
	static const struct he_cap_bit mac_bits[] = {
		{ 0, IEEE80211_HE_MAC_CAP0_HTC_HE, STA_REC_HE_CAP_HTC },
		{ 2, IEEE80211_HE_MAC_CAP2_BSR, STA_REC_HE_CAP_BSR },
		{ 3, IEEE80211_HE_MAC_CAP3_OMI_CONTROL, STA_REC_HE_CAP_OM },
		{ 4, IEEE80211_HE_MAC_CAP4_AMDSU_IN_AMPDU, STA_REC_HE_CAP_AMSDU_IN_AMPDU },
		{ 4, IEEE80211_HE_MAC_CAP4_BQR, STA_REC_HE_CAP_BQR },
	};
	static const struct he_cap_bit phy_bits[] = {
		{ 0, IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_RU_MAPPING_IN_2G |
		    IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_RU_MAPPING_IN_5G, STA_REC_HE_CAP_BW20_RU242_SUPPORT },
		{ 1, IEEE80211_HE_PHY_CAP1_LDPC_CODING_IN_PAYLOAD, STA_REC_HE_CAP_LDPC },
		{ 1, IEEE80211_HE_PHY_CAP1_HE_LTF_AND_GI_FOR_HE_PPDUS_0_8US, STA_REC_HE_CAP_SU_PPDU_1LTF_8US_GI },
		{ 2, IEEE80211_HE_PHY_CAP2_NDP_4x_LTF_AND_3_2US, STA_REC_HE_CAP_NDP_4LTF_3DOT2MS_GI },
		{ 2, IEEE80211_HE_PHY_CAP2_STBC_TX_UNDER_80MHZ, STA_REC_HE_CAP_LE_EQ_80M_TX_STBC },
		{ 2, IEEE80211_HE_PHY_CAP2_STBC_RX_UNDER_80MHZ, STA_REC_HE_CAP_LE_EQ_80M_RX_STBC },
		{ 6, IEEE80211_HE_PHY_CAP6_PARTIAL_BW_EXT_RANGE, STA_REC_HE_CAP_PARTIAL_BW_EXT_RANGE },
		{ 7, IEEE80211_HE_PHY_CAP7_HE_SU_MU_PPDU_4XLTF_AND_08_US_GI, STA_REC_HE_CAP_SU_MU_PPDU_4LTF_8US_GI },
		{ 7, IEEE80211_HE_PHY_CAP7_STBC_TX_ABOVE_80MHZ, STA_REC_HE_CAP_GT_80M_TX_STBC },
		{ 7, IEEE80211_HE_PHY_CAP7_STBC_RX_ABOVE_80MHZ, STA_REC_HE_CAP_GT_80M_RX_STBC },
		{ 8, IEEE80211_HE_PHY_CAP8_HE_ER_SU_PPDU_4XLTF_AND_08_US_GI, STA_REC_HE_CAP_ER_SU_PPDU_4LTF_8US_GI },
		{ 8, IEEE80211_HE_PHY_CAP8_HE_ER_SU_1XLTF_AND_08_US_GI, STA_REC_HE_CAP_ER_SU_PPDU_1LTF_8US_GI },
		{ 9, IEEE80211_HE_PHY_CAP9_NON_TRIGGERED_CQI_FEEDBACK, STA_REC_HE_CAP_TRIG_CQI_FK },
		{ 9, IEEE80211_HE_PHY_CAP9_TX_1024_QAM_LESS_THAN_242_TONE_RU, STA_REC_HE_CAP_TX_1024QAM_UNDER_RU242 },
		{ 9, IEEE80211_HE_PHY_CAP9_RX_1024_QAM_LESS_THAN_242_TONE_RU, STA_REC_HE_CAP_RX_1024QAM_UNDER_RU242 },
	};
	uint32_t cap = 0;
	u_int i;

	he = (struct sta_rec_he *)mwx_append_tlv(m, tlvnum, STA_REC_HE,
	    sizeof(*he));
	for (i = 0; i < nitems(mac_bits); i++)
		if (elem->mac_cap_info[mac_bits[i].byte] & mac_bits[i].mask)
			cap |= mac_bits[i].flag;
	for (i = 0; i < nitems(phy_bits); i++)
		if (elem->phy_cap_info[phy_bits[i].byte] & phy_bits[i].mask)
			cap |= phy_bits[i].flag;
	he->he_cap = htole32(cap);
	/* net80211 keeps the HE MCS structure in its on-wire byte order. */
	he->max_nss_mcs[CMD_HE_MCS_BW80] = ni->ni_he_mcs_nss_supp.rx_mcs_80;
	he->t_frame_dur = (elem->mac_cap_info[1] & 0x0c) >> 2;
	he->max_ampdu_exp = (elem->mac_cap_info[3] & 0x18) >> 3;
	he->bw_set = (elem->phy_cap_info[0] & 0xfe) >> 1;
	he->device_class = (elem->phy_cap_info[1] & 0x10) >> 4;
	he->punc_pream_rx = elem->phy_cap_info[1] & 0x0f;
	he->dcm_tx_mode = elem->phy_cap_info[3] & 0x03;
	he->dcm_tx_max_nss = (elem->phy_cap_info[3] & 0x04) >> 2;
	he->dcm_rx_mode = (elem->phy_cap_info[3] & 0x18) >> 3;
	he->dcm_rx_max_nss = (elem->phy_cap_info[3] & 0x20) >> 5;
	he->dcm_max_ru = (elem->phy_cap_info[8] & 0xc0) >> 6;
	he->pkt_ext = 2;

	v2 = (struct sta_rec_he_v2 *)mwx_append_tlv(m, tlvnum, STA_REC_HE_V2,
	    sizeof(*v2));
	memcpy(v2->he_mac_cap, elem->mac_cap_info, sizeof(v2->he_mac_cap));
	memcpy(v2->he_phy_cap, elem->phy_cap_info, sizeof(v2->he_phy_cap));
	v2->max_nss_mcs[CMD_HE_MCS_BW80] = ni->ni_he_mcs_nss_supp.rx_mcs_80;
	v2->pkt_ext = IEEE80211_HE_PHY_CAP9_NOMIMAL_PKT_PADDING_16US;
}

void
mt7921_mcu_add_sta_tlv(struct mbuf *m, uint16_t *tlvnum, struct mwx_softc *sc,
    struct ieee80211_node *ni, int add, int isnew, int sta_state)
{
	struct sta_rec_ra_info *ra_info;
	struct sta_rec_state *state;
	struct sta_rec_phy *phy;
	uint16_t supp_rates = 0, basic_rates = 0;
	int is2g = IEEE80211_IS_CHAN_2GHZ(ni->ni_chan);
	int ht = (ni->ni_flags & IEEE80211_NODE_HT) != 0;
	int vht = (ni->ni_flags & IEEE80211_NODE_VHT) != 0;
	int he = (ni->ni_flags & IEEE80211_NODE_HE) != 0;
	int i, j, offset = is2g ? 0 : 4;

	if (ht) {
		struct sta_rec_ht *rec = (struct sta_rec_ht *)mwx_append_tlv(m,
		    tlvnum, STA_REC_HT, sizeof(*rec));

		rec->ht_cap = htole16(ni->ni_htcaps);
	}
	if (vht) {
		struct sta_rec_vht *rec = (struct sta_rec_vht *)mwx_append_tlv(m,
		    tlvnum, STA_REC_VHT, sizeof(*rec));

		rec->vht_cap = htole32(ni->ni_vhtcaps);
		rec->vht_rx_mcs_map = htole16(ni->ni_vht_mcsinfo.rx_mcs_map);
		rec->vht_tx_mcs_map = htole16(ni->ni_vht_mcsinfo.tx_mcs_map);
	}
	if (ht || he) {
		struct sta_rec_amsdu *rec = (struct sta_rec_amsdu *)mwx_append_tlv(m,
		    tlvnum, STA_REC_HW_AMSDU, sizeof(*rec));

		rec->max_amsdu_num = 8;
		rec->amsdu_en = 1;
		rec->max_mpdu_size = 0; /* cap TX A-MSDU at 3839 bytes */
	}
	if (he)
		mt7921_mcu_add_he_tlv(m, tlvnum, ni);

	/* Linux's basic_rates uses band-relative indexes; RA uses CCK/OFDM bits. */
	for (i = 0; i < ni->ni_rates.rs_nrates; i++) {
		uint8_t rate = ni->ni_rates.rs_rates[i];

		for (j = offset; j < (int)nitems(mt76_rates); j++) {
			if ((rate & IEEE80211_RATE_VAL) != mt76_rates[j].rate)
				continue;
			if (rate & IEEE80211_RATE_BASIC)
				basic_rates |= 1U << (j - offset);
			supp_rates |= 1U << (j < 4 ? j : j + 2);
			break;
		}
	}
	phy = (struct sta_rec_phy *)mwx_append_tlv(m, tlvnum, STA_REC_PHY,
	    sizeof(*phy));
	phy->basic_rate = htole16(basic_rates);
	phy->phy_type = mt7921_get_phy_mode_v2(sc, ni);
	phy->ampdu = ht ? ni->ni_ampdu_param : 0;
	phy->rcpi = MIN(220, MAX(0, (ni->ni_rssi + 10) * 2));

	ra_info = (struct sta_rec_ra_info *)mwx_append_tlv(m, tlvnum,
	    STA_REC_RA, sizeof(*ra_info));
	ra_info->legacy = htole16(supp_rates);
	if (ht)
		memcpy(ra_info->rx_mcs_bitmask, ni->ni_rxmcs, HT_MCS_MASK_NUM);

	state = (struct sta_rec_state *)mwx_append_tlv(m, tlvnum,
	    STA_REC_STATE, sizeof(*state));
	state->state = sta_state;
	if (vht) {
		state->vht_opmode = ni->ni_chw == IEEE80211_CHAN_WIDTH_80 ? 2 :
		    ni->ni_chw == IEEE80211_CHAN_WIDTH_40 ? 1 : 0;
		state->vht_opmode |= (MAX(1, ni->ni_rx_nss) - 1) << 4;
	}
}

int
mt7921_mcu_wtbl_generic_tlv(struct mbuf *m, uint16_t *tlvnum,
    struct mwx_softc *sc, struct ieee80211_node *ni)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct wtbl_generic *generic;
	struct wtbl_rx *rx;

	generic = (struct wtbl_generic *)mwx_append_tlv(m, tlvnum,
	    WTBL_GENERIC, sizeof(*generic));

	if (ni) {
		generic->partial_aid = htole16(IEEE80211_AID(ni->ni_associd));
		memcpy(generic->peer_addr, ni->ni_macaddr, IEEE80211_ADDR_LEN);
		generic->muar_idx = sc->sc_vif.omac_idx;
		generic->qos = (ni->ni_flags & IEEE80211_NODE_QOS) != 0;
	} else {
		if (ic->ic_opmode == IEEE80211_M_STA && sc->sc_bss_conf.valid)
			memcpy(generic->peer_addr, sc->sc_bss_conf.bssid,
			    IEEE80211_ADDR_LEN);
		else
			memset(generic->peer_addr, 0xff, IEEE80211_ADDR_LEN);
		generic->muar_idx = 0xe;
	}

	rx = (struct wtbl_rx *)mwx_append_tlv(m, tlvnum, WTBL_RX,
	    sizeof(*rx));
#ifndef IEEE80211_STA_ONLY
	rx->rca1 = ni ? ic->ic_opmode != IEEE80211_M_HOSTAP : 1;
#else
	rx->rca1 = 1;
#endif
	rx->rca2 = 1;
	rx->rv = 1;

	return sizeof(*generic) + sizeof(*rx);
}

int
mt7921_mcu_wtbl_hdr_trans_tlv(struct mbuf *m, uint16_t *tlvnum,
    struct mwx_softc *sc, struct ieee80211_node *ni)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct wtbl_hdr_trans *htr;

	htr = (struct wtbl_hdr_trans *)mwx_append_tlv(m, tlvnum,
	    WTBL_HDR_TRANS, sizeof(*htr));

	/* no hdr decapsulation offload */
	htr->no_rx_trans = 1;

	if (ic->ic_opmode == IEEE80211_M_STA)
		htr->to_ds = 1;
	else
		htr->from_ds = 1;

	return sizeof(*htr);
}

int
mt7921_mcu_wtbl_ht_tlv(struct mbuf *m, uint16_t *tlvnum,
    struct mwx_softc *sc, struct ieee80211_node *ni)
{
	struct wtbl_ht *ht = NULL;
	struct wtbl_smps *smps;
	int len = 0;

	if (ni->ni_flags & IEEE80211_NODE_HT) {
		ht = (struct wtbl_ht *)mwx_append_tlv(m, tlvnum, WTBL_HT, sizeof(*ht));
		ht->ht = 1;
		ht->ldpc = (ni->ni_htcaps & IEEE80211_HTCAP_LDPC) != 0;
		ht->af = ni->ni_ampdu_param & IEEE80211_AMPDU_PARAM_LE;
		ht->mm = (ni->ni_ampdu_param & IEEE80211_AMPDU_PARAM_SS) >> 2;
		len += sizeof(*ht);
	}
	if (ni->ni_flags & IEEE80211_NODE_VHT) {
		struct wtbl_vht *vht = (struct wtbl_vht *)mwx_append_tlv(m,
		    tlvnum, WTBL_VHT, sizeof(*vht));
		uint8_t af = (ni->ni_vhtcaps >> IEEE80211_VHTCAP_MAX_A_MPDU_LENGTH_EXPONENT_SHIFT) & 7;

		vht->vht = 1;
		vht->ldpc = (ni->ni_vhtcaps & IEEE80211_VHTCAP_RXLDPC) != 0;
		if (ht != NULL)
			ht->af = MAX(ht->af, af);
		len += sizeof(*vht);
	}

	smps = (struct wtbl_smps *)mwx_append_tlv(m, tlvnum, WTBL_SMPS,
	    sizeof(*smps));
	smps->smps = (ni->ni_htcaps & IEEE80211_HTCAP_SMPS_MASK) ==
	    (IEEE80211_HTCAP_SMPS_DYN << IEEE80211_HTCAP_SMPS_SHIFT);

	return len + sizeof(*smps);
}

int
mt7921_mac_sta_update(struct mwx_softc *sc, struct ieee80211_node *ni,
    int add, int isnew, int sta_state)
{
	struct mwx_node *mw = (struct mwx_node *)ni;
	struct mwx_vif *mvif = &sc->sc_vif;
	struct sta_rec_wtbl *wtbl;
	struct mbuf *m = NULL;
	uint16_t tlvnum = 0, wnum = 0, wcid;
	int wlen = 0;

	m = mwx_alloc_sta_req_tlv(sizeof(struct sta_req_hdr));
	if (m == NULL)
		return ENOBUFS;

	if (ni != NULL)
		mt7921_mcu_add_basic_tlv(m, &tlvnum, sc, ni, add, isnew);

	if (ni != NULL && add)
		mt7921_mcu_add_sta_tlv(m, &tlvnum, sc, ni, add, isnew, sta_state);

	wtbl = (struct sta_rec_wtbl *)mwx_append_tlv(m, &tlvnum,
	    STA_REC_WTBL, sizeof(*wtbl));
	wcid = mw ? mw->wcid : mvif->vif_mn.wcid;
	wtbl->wlan_idx_lo = wcid & 0xff;
	wtbl->wlan_idx_hi = wcid >> 8;
	wtbl->operation = WTBL_RESET_AND_SET;

	if (add) {
		wlen += mt7921_mcu_wtbl_generic_tlv(m, &wnum, sc, ni);
		wlen += mt7921_mcu_wtbl_hdr_trans_tlv(m, &wnum, sc, ni);

		if (ni)
			wlen += mt7921_mcu_wtbl_ht_tlv(m, &wnum, sc, ni);
	}

	wtbl->tlv_num = htole16(wnum);
	wtbl->len = htole16(le16toh(wtbl->len) + wlen);

	/* Linux (captured): AP entry muar = omac, own broadcast entry muar = 0x0e */
	m = mwx_fill_sta_req_hdr(m, mvif, mw ? mvif->omac_idx : 0x0e, wcid,
	    tlvnum);
	if (m == NULL)
		return ENOBUFS;

	return mwx_mcu_send_mbuf_wait(sc, MCU_UNI_CMD_STA_REC_UPDATE, m);
}

static int
mt7921_key_to_cipher_id(struct ieee80211_key *k)
{
	switch (k->k_cipher) {
	case IEEE80211_CIPHER_CCMP:
		 return MCU_CIPHER_AES_CCMP;
	case IEEE80211_CIPHER_TKIP:
		return MCU_CIPHER_TKIP;
	case IEEE80211_CIPHER_WEP40:
		return MCU_CIPHER_WEP40;
	case IEEE80211_CIPHER_WEP104:
		 return MCU_CIPHER_WEP104;
	case IEEE80211_CIPHER_BIP:
		return MCU_CIPHER_BIP_CMAC_128;
	default:
		return MCU_CIPHER_NONE;
	}
}

void
mt7921_mcu_add_key_tlv(struct mbuf *m, uint16_t *tlvnum,
    struct ieee80211_key *k, int add)
{
	struct sta_rec_sec *sec;
	struct sta_rec_sec_key *key;

	sec = (struct sta_rec_sec *)mwx_append_tlv(m, tlvnum, STA_REC_KEY,
	    sizeof(*sec));

	if (add) {
		sec->n_cipher = 1;
		sec->add = 0;	/* connac2 has inverted logic */
		sec->len = htole16(sizeof(*sec) + sizeof(*key));

		key = (struct sta_rec_sec_key *)mwx_append_len(m, sizeof(*key));
		key->cipher_id = mt7921_key_to_cipher_id(k);
		key->cipher_len = sizeof(*key);
		key->key_id = k->k_id;
		key->key_len = k->k_len;

		KASSERT(k->k_len <= sizeof(key->key));
		memcpy(key->key, k->k_key, k->k_len);

		if (key->cipher_id == MCU_CIPHER_TKIP &&
		    k->k_len == sizeof(key->key)) {
			/* Rx/Tx MIC keys are swapped. */
			memcpy(key->key + 16, k->k_key + 24, 8);
			memcpy(key->key + 24, k->k_key + 16, 8);
		}
	} else {
		sec->add = 1;
		sec->n_cipher = 0;
	}
}

int
mt7921_mcu_sta_key_update(struct mwx_softc *sc, struct ieee80211_node *ni,
    struct ieee80211_key *k)
{
	struct mwx_vif *mvif = &sc->sc_vif;
	struct mwx_node *mn = (struct mwx_node *)ni;
	struct mbuf *m;
	int want_keymask = (MWX_NODE_FLAG_HAVE_PAIRWISE_KEY |
	    MWX_NODE_FLAG_HAVE_GROUP_KEY);
	int rv;
	uint16_t tlvnum = 0, wcid = 0;
	uint8_t muar_idx = 0;

	if (ni == NULL) {
		ni = sc->sc_ic.ic_bss;
		mn = (struct mwx_node *)ni;
	}
	if (ni == NULL)
		return ENXIO;

	if ((k->k_flags & IEEE80211_KEY_GROUP) != 0) {
		wcid = mvif->vif_mn.wcid;
		muar_idx = 0x0e;
	} else {
		wcid = mn->wcid;
	}

	m = mwx_alloc_sta_req_tlv(sizeof(struct sta_req_hdr));
	if (m == NULL)
		return ENOBUFS;

	DPRINTF("%s: %s: ni %p, k_id %d, k_flags %x k_cipher %d wcid %d\n",
	    DEVNAME(sc), "add key", ni, k->k_id, k->k_flags, k->k_cipher, wcid);
	MWX_DEV_LOG(sc,
	    "key add request id=%d flags=0x%x cipher=%d len=%d wcid=%u muar=%u ni=%p\n",
	    k->k_id, k->k_flags, k->k_cipher, k->k_len, wcid, muar_idx, ni);

	mt7921_mcu_add_key_tlv(m, &tlvnum, k, 1);
	m = mwx_fill_sta_req_hdr(m, mvif, muar_idx, wcid, tlvnum);
	if (m == NULL)
		return ENOBUFS;

	rv = mwx_mcu_send_mbuf_wait(sc, MCU_UNI_CMD_STA_REC_UPDATE, m);
	if (rv != 0)
		return rv;
	MWX_DEV_LOG(sc,
	    "key add done id=%d flags=0x%x cipher=%d wcid=%u node_flags_before=0x%x\n",
	    k->k_id, k->k_flags, k->k_cipher, wcid, mn->flags);

	/* Keep track of added keys to trigger link up when all are done. */
	if ((k->k_flags & IEEE80211_KEY_GROUP) != 0)
		mn->flags |= MWX_NODE_FLAG_HAVE_GROUP_KEY;
	else
		mn->flags |= MWX_NODE_FLAG_HAVE_PAIRWISE_KEY;

	if ((mn->flags & want_keymask) == want_keymask) {
		DPRINTF("marking port %s valid\n", ether_sprintf(ni->ni_macaddr));
		sc->sc_link_up_ns = mwx_uptime_ns();
		sc->sc_rx_data_frames = sc->sc_tx_data_frames = 0;
		MWX_DEV_LOG(sc, "key add complete port valid mode=%s width=%s MHz\n",
		    (ni->ni_flags & IEEE80211_NODE_HE) ? "ax" :
		    (ni->ni_flags & IEEE80211_NODE_VHT) ? "ac" :
		    (ni->ni_flags & IEEE80211_NODE_HT) ? "n" : "legacy",
		    mwx_cbw_name(sc->sc_chan_bw));
		if (ni->ni_flags & IEEE80211_NODE_MFP)
			ni->ni_flags |= IEEE80211_NODE_TXMGMTPROT;
		ni->ni_port_valid = 1;
		ieee80211_set_link_state(&sc->sc_ic, LINK_STATE_UP);
	}

	return 0;
}

void
mt7921_mcu_sta_key_delete(struct mwx_softc *sc, struct ieee80211_node *ni,
    struct ieee80211_key *k)
{
	struct mwx_vif *mvif = &sc->sc_vif;
	struct mwx_node *mn = (struct mwx_node *)ni;
	struct mbuf *m;
	uint16_t tlvnum = 0, wcid = 0;
	uint8_t muar_idx = 0;

	if (ni == NULL) {
		ni = sc->sc_ic.ic_bss;
		mn = (struct mwx_node *)ni;
	}
	if (ni == NULL)
		return;

	if ((k->k_flags & IEEE80211_KEY_GROUP) != 0) {
		wcid = mvif->vif_mn.wcid;
		muar_idx = 0x0e;
		mn->flags &= ~MWX_NODE_FLAG_HAVE_GROUP_KEY;
	} else {
		wcid = mn->wcid;
		mn->flags &= ~MWX_NODE_FLAG_HAVE_PAIRWISE_KEY;
	}

	m = mwx_alloc_sta_req_tlv(sizeof(struct sta_req_hdr));
	if (m == NULL)
		return;

	DPRINTF("%s: %s: ni %p, k_id %d, k_flags %x k_cipher %d wcid %d\n",
	    DEVNAME(sc), "delete key", ni, k->k_id, k->k_flags, k->k_cipher,
	    wcid);

	mt7921_mcu_add_key_tlv(m, &tlvnum, k, 0);
	m = mwx_fill_sta_req_hdr(m, mvif, muar_idx, wcid, tlvnum);
	if (m == NULL)
		return;

	mwx_mcu_send_mbuf(sc, MCU_UNI_CMD_STA_REC_UPDATE, m, NULL);
}

#undef free

bool
MtkMwx::mwx_match(IOPCIDevice *device)
{
	if (device == NULL) {
		MWX_LOG("probe skipped: null PCI device\n");
		return false;
	}
	uint16_t vendor = device->configRead16(kIOPCIConfigVendorID);
	uint16_t product = device->configRead16(kIOPCIConfigDeviceID);
	bool matched = pci_matchbyid(vendor, product, mwx_devices,
	    nitems(mwx_devices)) != 0;
	MWX_LOG("probe vendor=0x%04x product=0x%04x matched=%d\n",
	    vendor, product, matched);
	return matched;
}

bool
MtkMwx::attach(IOPCIDevice *device)
{
	MWX_LOG("HAL attach begin device=%p\n", (void *)device);
	if (com == NULL)
		com = (struct mwx_softc *)malloc(sizeof(*com), M_DEVBUF, M_WAITOK | M_ZERO);
	if (pci == NULL)
		pci = (struct pci_attach_args *)malloc(sizeof(*pci), M_DEVBUF, M_WAITOK | M_ZERO);
	if (com == NULL || pci == NULL) {
		MWX_LOG("HAL attach allocation failed com=%p pci=%p\n",
		    (void *)com, (void *)pci);
		releaseAll();
		return false;
	}
	MWX_LOG("HAL attach allocated com=%p pci=%p\n",
	    (void *)com, (void *)pci);

	gMwxHal = this;
	taskq_init();
	pci->pa_tag = device;
	pci->workloop = getMainWorkLoop();
	pci->pa_id = ((pcireg_t)device->configRead16(kIOPCIConfigDeviceID) << 16) |
	    device->configRead16(kIOPCIConfigVendorID);
	strlcpy(com->sc_dev.dv_xname, "mwx0", sizeof(com->sc_dev.dv_xname));
	MWX_LOG("HAL attach pci_id=0x%08x workloop=%p\n",
	    pci->pa_id, (void *)pci->workloop);
	if (!mwx_attach(NULL, (struct device *)com, pci)) {
		MWX_LOG("HAL attach failed inside mwx_attach\n");
		detach(device);
		return false;
	}
	MWX_LOG("HAL attach complete\n");
	return true;
}

void
MtkMwx::detach(IOPCIDevice *device)
{
	MWX_LOG("HAL detach begin device=%p com=%p\n",
	    (void *)device, (void *)com);
	if (com == NULL) {
		releaseAll();
		return;
	}

	struct ifnet *ifp = &com->sc_ic.ic_if;
	if (ifp->if_flags & IFF_RUNNING)
		mwx_stop(ifp);

	task_del(systq, &com->sc_reset_task);
	task_del(systq, &com->sc_scan_task);
	ieee80211_ifdetach(ifp);
	mwx_txwi_free(com);
	mwx_dma_free(com);
	if (com->sc_ih) {
		pci_intr_disestablish(pci->pa_pc, com->sc_ih);
		com->sc_ih = NULL;
	}
	mwx_txlock_free(com);
	if (com->sc_memh)
		bus_space_unmap(com->sc_st, com->sc_memh, com->sc_mems);
	releaseAll();
	MWX_LOG("HAL detach complete\n");
}

IOReturn
MtkMwx::enable(IONetworkInterface *interface)
{
	MWX_LOG("HAL enable begin interface=%p com=%p\n",
	    (void *)interface, (void *)com);
	if (com == NULL) {
		MWX_LOG("HAL enable failed: not attached\n");
		return kIOReturnNotAttached;
	}
	struct ifnet *ifp = &com->sc_ic.ic_if;
	if (ifp->if_flags & IFF_UP) {
		MWX_DEV_LOG(com, "HAL enable skipped: already up flags=0x%x\n",
		    ifp->if_flags);
		return kIOReturnSuccess;
	}
	ifp->if_flags |= IFF_UP;
	int rv = mwx_init(ifp);
	MWX_DEV_LOG(com, "HAL enable done mwx_init=%d flags=0x%x\n",
	    rv, ifp->if_flags);
	return rv == 0 ? kIOReturnSuccess : kIOReturnError;
}

IOReturn
MtkMwx::disable(IONetworkInterface *interface)
{
	MWX_LOG("HAL disable begin interface=%p com=%p\n",
	    (void *)interface, (void *)com);
	if (com == NULL)
		return kIOReturnSuccess;
	struct ifnet *ifp = &com->sc_ic.ic_if;
	if (!(ifp->if_flags & IFF_UP)) {
		MWX_DEV_LOG(com, "HAL disable skipped: already down flags=0x%x\n",
		    ifp->if_flags);
		return kIOReturnSuccess;
	}
	ifp->if_flags &= ~IFF_UP;
	if (ifp->if_flags & IFF_RUNNING)
		mwx_stop(ifp);
	MWX_DEV_LOG(com, "HAL disable complete flags=0x%x\n", ifp->if_flags);
	return kIOReturnSuccess;
}

struct ieee80211com *
MtkMwx::get80211Controller()
{
	if (com == NULL)
		com = (struct mwx_softc *)malloc(sizeof(*com), M_DEVBUF, M_WAITOK | M_ZERO);
	return com == NULL ? NULL : &com->sc_ic;
}

MtkDriverInfo *
MtkMwx::getDriverInfo()
{
	return this;
}

MtkDriverController *
MtkMwx::getDriverController()
{
	return this;
}

IOEthernetController *
MtkMwx::mwxController()
{
	return getController();
}

int
MtkMwx::mwxSleepNsec(void *ident, int priority, const char *wmesg,
    uint64_t timo)
{
	return MtkHalService::tsleep_nsec(ident, priority, wmesg, timo);
}

/*
 * Linux mt7921e_unregister_device(): take driver ownership, stop and clean
 * the DMA rings, reset the Wi-Fi subsystem. Leaves a clean card for the next
 * OS instead of firmware still holding our rings and BSS.
 */
void
MtkMwx::shutdownHardware()
{
	if (com == NULL)
		return;
	MWX_DEV_LOG(com, "shutdown: resetting the card for the next boot\n");
	mwx_write(com, MT_WFDMA0_HOST_INT_ENA, 0);
	mwx_mcu_drv_pmctrl(com);
	mwx_dma_disable(com, 1);
	mwx_wfsys_reset(com);
	/*
	 * No PCIe Function Level Reset here: after one, Linux's next probe failed
	 * with "driver own failed" until a cold power cycle. Linux's own teardown
	 * (mt7921e_unregister_device) also stops at the WFSYS reset.
	 */
}

bool
MtkMwx::mwxOnWorkLoop()
{
	IOWorkLoop *wl = getMainWorkLoop();

	return wl != NULL && wl->onThread();
}

void
MtkMwx::mwxWakeup(void *ident)
{
	wakeupOn(ident);
}

const char *
MtkMwx::getFirmwareVersion()
{
	if (com == NULL)
		return "MT792x";
	return com->sc_hwtype == MWX_HW_MT7922 ? "MT7922" : "MT7921";
}

int16_t
MtkMwx::getBSSNoise()
{
	return 0;
}

bool
MtkMwx::is5GBandSupport()
{
	return com != NULL && com->sc_capa.has_5ghz;
}

int
MtkMwx::getTxNSS()
{
	return (com != NULL && com->sc_capa.num_streams > 0) ? com->sc_capa.num_streams : 1;
}

const char *
MtkMwx::getFirmwareName()
{
	if (com == NULL || com->sc_hwtype == MWX_HW_MT7922)
		return MT7922_FIRMWARE_WM;
	return MT7921_FIRMWARE_WM;
}

uint32_t
MtkMwx::getRxRateKbps()
{
	if (com == NULL)
		return 0;
	return com->sc_rx_rate_kbps;
}

uint32_t
MtkMwx::getPhyLimit()
{
	if (com == NULL)
		return MWX_PHY_LIMIT_LEGACY;
	return com->sc_phy_limit;
}

IOReturn
MtkMwx::setPhyLimit(uint32_t limit)
{
	IOCommandGate *gate = getMainCommandGate();

	if (gate == NULL || com == NULL || limit > MWX_PHY_LIMIT_HE)
		return kIOReturnBadArgument;
	return gate->runAction(phyLimitAction, &limit, this);
}

IOReturn
MtkMwx::phyLimitAction(OSObject *owner, void *limit, void *target, void *, void *)
{
	MtkMwx *hal = (MtkMwx *)target;
	struct mwx_softc *sc = hal->com;
	struct ifnet *ifp = &sc->sc_ic.ic_if;
	uint32_t value = *(uint32_t *)limit;
	int running = (ifp->if_flags & IFF_RUNNING) != 0;

	if (sc->sc_phy_limit == (int)value)
		return kIOReturnSuccess;
	if (running)
		mwx_stop(ifp);
	mwx_link_reset(sc);
	sc->sc_phy_limit = value;
	mwx_apply_phy_limit(sc);
	MWX_DEV_LOG(sc, "PHY limit set to %s\n", mwx_phy_limit_name(value));
	if (running && mwx_init(ifp) != 0)
		return kIOReturnError;
	return kIOReturnSuccess;
}

UInt32
MtkMwx::supportedFeatures()
{
	return kIONetworkFeatureMultiPages;
}

const char *
MtkMwx::getFirmwareCountryCode()
{
	static char alpha2[3] = "00";
	if (com != NULL) {
		alpha2[0] = com->sc_alpha2[0];
		alpha2[1] = com->sc_alpha2[1];
		alpha2[2] = '\0';
	}
	return alpha2;
}

uint32_t
MtkMwx::getTxQueueSize()
{
	return 256;
}

void
MtkMwx::clearScanningFlags()
{
	if (com)
		com->sc_flags &= ~(MWX_FLAG_SCANNING | MWX_FLAG_BGSCAN);
}

IOReturn
MtkMwx::setMulticastList(IOEthernetAddress *addr, int count)
{
	return kIOReturnSuccess;
}

void
MtkMwx::releaseAll()
{
	MWX_LOG("HAL releaseAll com=%p pci=%p\n", (void *)com, (void *)pci);
	if (com) {
		mwx_free_mem(com);
		com = NULL;
	}
	if (pci) {
		mwx_free_mem(pci);
		pci = NULL;
	}
	if (gMwxHal == this)
		gMwxHal = NULL;
}

void
MtkMwx::free()
	{
		MWX_LOG("HAL free\n");
		releaseAll();
		MtkHalService::free();
	}

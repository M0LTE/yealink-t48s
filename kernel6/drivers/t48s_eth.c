// SPDX-License-Identifier: GPL-2.0
/*
 * t48s_eth: Ethernet for the Yealink SIP-T48S (Broadcom BCM11188).
 *
 * The SoC has a three-port switch (ESW): two external ports (LAN, PC) and an
 * internal port 8 (IMP) that faces the CPU. Frames to and from the CPU move
 * through two FIFOs on the switch, PTM (port to memory, receive) and MTP
 * (memory to port, transmit), each fed by a channel on DMA controller 1
 * (a Synopsys DW AHB DMAC): channel 2 receives, channel 3 transmits.
 *
 * Every frame on the IMP port carries a 4-byte Broadcom tag between the
 * source address and the type field, and a 4-byte FCS slot at the end.
 *
 * The register values below are the ones Yealink's own kernel was seen using
 * on this phone (read live from its DMA and switch registers), which match
 * Broadcom's GPL "Island" ethernet driver for the same switch IP.
 *
 * This driver keeps things simple: buffers are DMA-coherent and every frame
 * is copied, and one transmit is in flight at a time.
 *
 * The switch is set up here every time the interface comes up, whoever ran
 * before (esw_bring_up): its clocks and reset in chip control, the two
 * external ports forwarding, the address table entry that sends our own MAC
 * address to the CPU port, the CPU port's speed, and the few settings
 * Yealink's firmware changes from the chip's defaults. After a kexec from
 * Yealink's kernel, or after U-Boot has fetched us over the network, all of
 * it is found done already; after a start that never used the network it may
 * not be. To test that without such a start (interface down first):
 *   echo 1 > /sys/module/t48s_eth/parameters/cold
 * removes our address table entry, stops the CPU-side FIFOs, resets the
 * switch and gates its clocks. The next "up" then has everything to do.
 *
 * GPL v2, like the Broadcom driver it is modelled on.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/workqueue.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/rtnetlink.h>
#include <linux/delay.h>
#include <linux/moduleparam.h>

#define DRV_NAME	"t48s_eth"

/* physical addresses */
#define ESW_PHYS	0x30400000
#define ESW_SIZE	0x000b0000
#define DMA1_PHYS	0x30300000
#define DMA1_SIZE	0x1000

/* switch: page registers are 64 bits wide, at base + ((page << 8) + off) * 8 */
#define ESW_PAGE(p, o)		((((p) << 8) + (o)) * 8)
#define ESW_PORTCTRL(port)	ESW_PAGE(0x00, (port))
#define ESW_PORTCTRL_DISABLE	0x03		/* MAC TX and RX disable */
/* the IMP port's control register: which frames without an ARL entry reach the CPU */
#define ESW_IMPCTRL		ESW_PORTCTRL(ESW_PORT_IMP)
#define ESW_IMPCTRL_RX_UCAST	0x10
#define ESW_IMPCTRL_RX_MCAST	0x08
#define ESW_IMPCTRL_RX_BCAST	0x04
#define ESW_MIB(port, o)	ESW_PAGE(0x20 + (port), (o))
#define ESW_MIB_TX_DROP		0x08
#define ESW_MIB_TX_BCAST	0x10
#define ESW_MIB_TX_MCAST	0x14
#define ESW_MIB_TX_UCAST	0x18
#define ESW_GBLMGMT		ESW_PAGE(0x02, 0x00)
#define ESW_GBLMGMT_IMP_EN	0x80
#define ESW_IMPENABLE		ESW_PAGE(0x34, 0x06)
#define ESW_LINK		ESW_PAGE(0x01, 0x00)
#define ESW_LINK_EXT		0x03		/* link on external port 0 or 1 */
#define ESW_PORT_IMP		8
#define ESW_PORTS		2		/* external ports: 0 and 1 */
#define ESW_PORTCTRL_STP	0xe0		/* spanning tree state: */
#define ESW_PORTCTRL_FORWARD	0xa0		/* forwarding */
#define ESW_SWMODE		ESW_PAGE(0x00, 0x0b)
#define ESW_SWMODE_MANAGED	0x01
#define ESW_SWMODE_FORWARD	0x02		/* off after a reset: nothing is switched */
#define ESW_SPEED		ESW_PAGE(0x01, 0x04)	/* two bits a port: 0 = 10, 1 = 100, 2 = 1000 Mbit/s */
/* address table (ARL): put a MAC address in the index, read its bucket (two
 * entries), change one, write the bucket back */
#define ESW_ARL_CTL		ESW_PAGE(0x05, 0x00)
#define ESW_ARL_CTL_START	0x80		/* clears itself when done */
#define ESW_ARL_CTL_READ	0x01
#define ESW_ARL_MAC		ESW_PAGE(0x05, 0x02)
#define ESW_ARL_VID		ESW_PAGE(0x05, 0x08)
#define ESW_ARL_ENTRY(n)	ESW_PAGE(0x05, 0x10 + 0x10 * (n))	/* MAC address */
#define ESW_ARL_FWD(n)		ESW_PAGE(0x05, 0x18 + 0x10 * (n))
#define ESW_ARL_FWD_VALID	0x10000
#define ESW_ARL_FWD_CPU		0x1c008		/* valid, static, highest class, port 8: as
						 * Yealink's ethHw_arlEntrySet writes it */

/* switch: management block (plain 32-bit registers) */
#define ESW_STATUS		0xa0000
#define ESW_STATUS_READY	0x00800000	/* out of reset and usable */
#define ESW_IMPSPEED		0xa0090		/* low: the speed of the port in use
						 * (4 = 1000, 1 = 100, 2 = 10 Mbit/s);
						 * 0x40: the CPU port itself at 1000 */
#define ESW_IMPSPEED_CPU_1000	0x40

/* chip control: the switch's clock, bus clocks and reset */
#define CHIPC_PHYS		0x80000000
#define CHIPC_ESWCLK		0x0c		/* PLL1 channel 3 */
#define CHIPC_ESWCLK_ON		0x00010014	/* 2 GHz / 20 = 100 MHz, output on: as found
						 * after U-Boot and after Yealink */
#define CHIPC_CLK_OUTPUT	0x00010000
#define CHIPC_BUSCLK		0x5c
#define CHIPC_BUSCLK_DMAC1	0x00000040
#define CHIPC_BUSCLK_ESW	0x00000100
#define CHIPC_SOFTRESET2	0x84
#define CHIPC_SOFTRESET2_ESW	0x00000060	/* the switch and its clock domain */

/* switch: CPU interface (plain 32-bit registers) */
#define ESW_PTM_CONFIG		0x80008
#define ESW_PTM_CONFIG_VAL	0x90040010	/* enable, burst 8, block 256 */
#define ESW_PTM_DMASTAT		0x80010
#define ESW_PTM_FIFO		0x88000
#define ESW_MTP_CONFIG		0x90000
#define ESW_MTP_CONFIG_VAL	0x90000013	/* enable, burst 8 (+1, non-A0) */
#define ESW_MTP_FIFO_CFG	0x90008
#define ESW_MTP_DMASTAT		0x90010
#define ESW_MTP_FIFO_DATA	0x98000

/* DW AHB DMAC */
#define CH_RX			2
#define CH_TX			3
#define DMAC_CH(c)		((c) * 0x58)
#define DMAC_SAR		0x00
#define DMAC_DAR		0x08
#define DMAC_LLP		0x10
#define DMAC_CTL_LO		0x18
#define DMAC_CTL_HI		0x1c
#define DMAC_SSTATAR		0x30
#define DMAC_DSTATAR		0x38
#define DMAC_CFG_LO		0x40
#define DMAC_CFG_HI		0x44
#define DMAC_MASK_TFR		0x310
#define DMAC_MASK_BLOCK		0x318
#define DMAC_MASK_SRCTRAN	0x320
#define DMAC_MASK_DSTTRAN	0x328
#define DMAC_MASK_ERR		0x330
#define DMAC_CLEAR_TFR		0x338
#define DMAC_CLEAR_BLOCK	0x340
#define DMAC_CLEAR_SRCTRAN	0x348
#define DMAC_CLEAR_DSTTRAN	0x350
#define DMAC_CLEAR_ERR		0x358
#define DMAC_CFG		0x398
#define DMAC_CHEN		0x3a0
#define DMAC_CTL_DONE		0x00001000	/* in CTL_HI, written back to the LLI */

/*
 * Values seen in Yealink's kernel (DMA1 channels 2 and 3):
 *  RX CTL_LO: LLP src+dst, SMS master 2, peripheral-to-memory with the
 *             peripheral as flow controller, src burst 8, 64-bit widths
 *  TX CTL_LO: LLP src+dst, DMS master 2, memory-to-peripheral with the DMAC
 *             as flow controller, dst burst 8, 64-bit widths
 *  RX CFG_HI: status update from SSTATAR, HPROT 1, FIFO mode
 *  TX CFG_HI: dst handshake interface 1, status update to DSTATAR, HPROT 1,
 *             FIFO mode
 */
#define RX_CTL_LO		0x1a408036
#define TX_CTL_LO		0x18901036
#define CTL_INT_EN		0x00000001
#define RX_CFG_LO		0x000000e0	/* priority 7 */
#define RX_CFG_HI		0x00000046
#define TX_CFG_LO		0x000000e0
#define TX_CFG_HI		0x00000826

#define BRCM_HDR_LEN		4
#define FCS_LEN			4
#define RX_RING			32
#define RX_BUF_LEN		2048
#define TX_BUF_LEN		2048
#define RX_LEN(sstat)		(((sstat) >> 13) & 0xffff)

struct lli {			/* DMAC linked list item, as the hardware reads it */
	u32 sar;
	u32 dar;
	u32 llp;
	u32 ctl_lo;
	u32 ctl_hi;		/* written back with DONE */
	u32 sstat;		/* written back: PTM status (frame length) */
	u32 dstat;
	u32 pad;		/* keep each item 8-byte aligned */
};

struct t48s_eth {
	struct net_device *dev;
	struct napi_struct napi;
	void __iomem *esw;
	void __iomem *dmac;
	int irq_rx, irq_tx;		/* DMA1 channels 2 and 3 */
	spinlock_t lock;

	struct lli *rx_lli;		/* RX_RING items */
	dma_addr_t rx_lli_dma;
	u8 *rx_buf;			/* RX_RING * RX_BUF_LEN */
	dma_addr_t rx_buf_dma;
	int rx_next;

	struct lli *tx_lli;		/* 2 items: config word, then frame */
	dma_addr_t tx_lli_dma;
	u8 *tx_buf;			/* 8-byte config word, then the frame at +8 */
	dma_addr_t tx_buf_dma;
	int tx_busy;
	unsigned int tx_len;

	struct delayed_work guard;	/* keeps an eye on the IMP port's settings */
	unsigned int imp_fixes;		/* times the guard had to put them back */

	bool off;			/* the cold test has switched the switch off */
	unsigned int cold_tests;
	unsigned int set_last;		/* settings the last bring-up had to change */
	int arl_bin;			/* where our address sits in its bucket, -1 = not written */
};

static u8 t48s_mac[ETH_ALEN] = { 0x80, 0x5e, 0xc0, 0x03, 0x1c, 0x05 };

/* debugging: echo N > /sys/module/t48s_eth/parameters/dump prints the next N frames */
static int dump;
module_param(dump, int, 0644);

/* ---- switch registers: 64-bit accesses, low word holds the value ---- */

static u32 esw_rd(struct t48s_eth *p, u32 off)
{
	register u32 lo asm("r2");
	register u32 hi asm("r3");
	asm volatile("ldrd %0, [%2]" : "=&r"(lo), "=&r"(hi) : "r"(p->esw + off) : "memory");
	(void)hi;
	return lo;
}

static void esw_wr(struct t48s_eth *p, u32 off, u32 val)
{
	register u32 lo asm("r2") = val;
	register u32 hi asm("r3") = 0;
	asm volatile("strd %0, [%2]" : : "r"(lo), "r"(hi), "r"(p->esw + off) : "memory");
}

/* the few registers that use the high word: MAC addresses */
static u64 esw_rd64(struct t48s_eth *p, u32 off)
{
	register u32 lo asm("r2");
	register u32 hi asm("r3");
	asm volatile("ldrd %0, [%2]" : "=&r"(lo), "=&r"(hi) : "r"(p->esw + off) : "memory");
	return ((u64)hi << 32) | lo;
}

static void esw_wr64(struct t48s_eth *p, u32 off, u64 val)
{
	register u32 lo asm("r2") = (u32)val;
	register u32 hi asm("r3") = (u32)(val >> 32);
	asm volatile("strd %0, [%2]" : : "r"(lo), "r"(hi), "r"(p->esw + off) : "memory");
}

static inline void dmac_wr(struct t48s_eth *p, u32 off, u32 val)
{
	writel(val, p->dmac + off);
}

static inline u32 dmac_rd(struct t48s_eth *p, u32 off)
{
	return readl(p->dmac + off);
}

/* DW DMAC per-channel bit registers take a write-enable bit at ch + 8 */
static inline void dmac_chbit(struct t48s_eth *p, u32 reg, int ch, int on)
{
	dmac_wr(p, reg, (1 << (ch + 8)) | (on ? (1 << ch) : 0));
}

static void dmac_stop(struct t48s_eth *p, int ch)
{
	int n = 100000;

	dmac_chbit(p, DMAC_CHEN, ch, 0);
	while ((dmac_rd(p, DMAC_CHEN) & (1 << ch)) && --n)
		cpu_relax();
}

static void dmac_setup(struct t48s_eth *p, int ch, u32 ctl_lo, u32 cfg_lo,
		       u32 cfg_hi, u32 sstatar, u32 dstatar)
{
	u32 c = DMAC_CH(ch);

	dmac_stop(p, ch);
	dmac_wr(p, c + DMAC_CTL_LO, ctl_lo);
	dmac_wr(p, c + DMAC_CTL_HI, 0);
	dmac_wr(p, c + DMAC_CFG_LO, cfg_lo);
	dmac_wr(p, c + DMAC_CFG_HI, cfg_hi);
	dmac_wr(p, c + DMAC_SSTATAR, sstatar);
	dmac_wr(p, c + DMAC_DSTATAR, dstatar);
	dmac_wr(p, DMAC_CLEAR_TFR, 1 << ch);
	dmac_wr(p, DMAC_CLEAR_BLOCK, 1 << ch);
	dmac_wr(p, DMAC_CLEAR_SRCTRAN, 1 << ch);
	dmac_wr(p, DMAC_CLEAR_DSTTRAN, 1 << ch);
	dmac_wr(p, DMAC_CLEAR_ERR, 1 << ch);
	dmac_chbit(p, DMAC_MASK_TFR, ch, 0);
	dmac_chbit(p, DMAC_MASK_BLOCK, ch, 0);
	dmac_chbit(p, DMAC_MASK_SRCTRAN, ch, 0);
	dmac_chbit(p, DMAC_MASK_DSTTRAN, ch, 0);
	dmac_chbit(p, DMAC_MASK_ERR, ch, 0);
}

static void dmac_start(struct t48s_eth *p, int ch, dma_addr_t first)
{
	wmb();
	dmac_wr(p, DMAC_CH(ch) + DMAC_LLP, first);
	dmac_chbit(p, DMAC_CHEN, ch, 1);
}

/* ---- receive ---- */

static void rx_ring_init(struct t48s_eth *p)
{
	int i;

	for (i = 0; i < RX_RING; i++) {
		struct lli *l = &p->rx_lli[i];

		l->sar = ESW_PHYS + ESW_PTM_FIFO;
		l->dar = p->rx_buf_dma + i * RX_BUF_LEN;
		l->llp = p->rx_lli_dma + ((i + 1) % RX_RING) * sizeof(struct lli);
		l->ctl_lo = RX_CTL_LO | CTL_INT_EN;
		l->ctl_hi = RX_BUF_LEN;
		l->sstat = 0;
		l->dstat = 0;
		l->pad = 0;
	}
	p->rx_next = 0;
}

static void rx_frame(struct t48s_eth *p, const u8 *buf, int len)
{
	struct net_device *dev = p->dev;
	struct sk_buff *skb;

	if (dump > 0) {
		dump--;
		pr_info(DRV_NAME " rx %d (desc %d)\n", len, p->rx_next);
		print_hex_dump(KERN_INFO, DRV_NAME " rx: ", DUMP_PREFIX_OFFSET,
			       16, 1, buf, min(len, 48), false);
	}

	/* [DA SA][BRCM tag][type ... payload][FCS] */
	if (len < 12 + BRCM_HDR_LEN + 2 + FCS_LEN || len > RX_BUF_LEN) {
		dev->stats.rx_length_errors++;
		return;
	}
	len -= BRCM_HDR_LEN + FCS_LEN;

	skb = netdev_alloc_skb_ip_align(dev, len);
	if (!skb) {
		dev->stats.rx_dropped++;
		return;
	}
	memcpy(skb_put(skb, 12), buf, 12);
	memcpy(skb_put(skb, len - 12), buf + 12 + BRCM_HDR_LEN, len - 12);
	skb->protocol = eth_type_trans(skb, dev);
	dev->stats.rx_packets++;
	dev->stats.rx_bytes += len;
	netif_receive_skb(skb);
}

static int t48s_poll(struct napi_struct *napi, int budget)
{
	struct t48s_eth *p = container_of(napi, struct t48s_eth, napi);
	int done = 0;

	while (done < budget) {
		struct lli *l = &p->rx_lli[p->rx_next];
		u32 ctl_hi = READ_ONCE(l->ctl_hi);

		if (!(ctl_hi & DMAC_CTL_DONE))
			break;
		rmb();
		rx_frame(p, p->rx_buf + p->rx_next * RX_BUF_LEN, RX_LEN(l->sstat));
		l->sstat = 0;
		wmb();
		l->ctl_hi = RX_BUF_LEN;
		p->rx_next = (p->rx_next + 1) % RX_RING;
		done++;
	}

	if (done < budget) {
		napi_complete(napi);
		dmac_wr(p, DMAC_CLEAR_BLOCK, 1 << CH_RX);
		dmac_chbit(p, DMAC_MASK_BLOCK, CH_RX, 1);
		/* a frame may have landed between the last check and the unmask */
		if (p->rx_lli[p->rx_next].ctl_hi & DMAC_CTL_DONE) {
			dmac_chbit(p, DMAC_MASK_BLOCK, CH_RX, 0);
			napi_schedule(napi);
		}
	}
	return done;
}

static irqreturn_t t48s_rx_irq(int irq, void *data)
{
	struct t48s_eth *p = data;

	dmac_chbit(p, DMAC_MASK_BLOCK, CH_RX, 0);
	dmac_wr(p, DMAC_CLEAR_BLOCK, 1 << CH_RX);
	dmac_wr(p, DMAC_CLEAR_ERR, 1 << CH_RX);
	napi_schedule(&p->napi);
	return IRQ_HANDLED;
}

/* ---- transmit ---- */

static irqreturn_t t48s_tx_irq(int irq, void *data)
{
	struct t48s_eth *p = data;
	struct net_device *dev = p->dev;
	unsigned long flags;

	dmac_wr(p, DMAC_CLEAR_BLOCK, 1 << CH_TX);
	dmac_wr(p, DMAC_CLEAR_ERR, 1 << CH_TX);
	dmac_wr(p, DMAC_CLEAR_TFR, 1 << CH_TX);

	spin_lock_irqsave(&p->lock, flags);
	if (p->tx_busy) {
		int n = 1000;

		/* only complete a frame whose channel has really stopped: this may be a
		 * late interrupt for a frame that t48s_xmit already reclaimed */
		while ((dmac_rd(p, DMAC_CHEN) & (1 << CH_TX)) && --n)
			cpu_relax();
	}
	if (p->tx_busy && !(dmac_rd(p, DMAC_CHEN) & (1 << CH_TX))) {
		p->tx_busy = 0;
		dev->stats.tx_packets++;
		dev->stats.tx_bytes += p->tx_len;
		netif_wake_queue(dev);
	}
	spin_unlock_irqrestore(&p->lock, flags);
	return IRQ_HANDLED;
}

static int t48s_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct t48s_eth *p = netdev_priv(dev);
	u8 *f = p->tx_buf + 8;
	unsigned int len = skb->len;
	unsigned int dlen;
	unsigned long flags;
	u32 *cfg = (u32 *)p->tx_buf;

	if (len < 12 + 2 || len > ETH_FRAME_LEN + 4) {
		dev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	spin_lock_irqsave(&p->lock, flags);
	if (p->tx_busy && !(dmac_rd(p, DMAC_CHEN) & (1 << CH_TX))) {
		/* the previous frame went but its interrupt has not been handled yet */
		p->tx_busy = 0;
		dev->stats.tx_packets++;
		dev->stats.tx_bytes += p->tx_len;
	}
	if (p->tx_busy) {
		netif_stop_queue(dev);
		spin_unlock_irqrestore(&p->lock, flags);
		return NETDEV_TX_BUSY;
	}

	/* [DA SA][BRCM tag: tc 7, let the ARL forward][type ... payload][pad][FCS slot] */
	skb_copy_bits(skb, 0, f, 12);
	f[12] = 7 << 2;
	f[13] = 0;
	f[14] = 0;
	f[15] = 0;
	skb_copy_bits(skb, 12, f + 12 + BRCM_HDR_LEN, len - 12);
	if (len < ETH_ZLEN) {
		memset(f + BRCM_HDR_LEN + len, 0, ETH_ZLEN - len);
		len = ETH_ZLEN;
	}
	p->tx_len = len;
	dlen = len + BRCM_HDR_LEN + FCS_LEN;

	/* config word: offset of the last byte in its 64-bit word, end of packet */
	cfg[0] = (((dlen - 1) % 8) << 8) | 0x00010000;
	cfg[1] = 0;

	p->tx_lli[0].sar = p->tx_buf_dma;
	p->tx_lli[0].dar = ESW_PHYS + ESW_MTP_FIFO_CFG;
	p->tx_lli[0].llp = p->tx_lli_dma + sizeof(struct lli);
	p->tx_lli[0].ctl_lo = TX_CTL_LO;
	p->tx_lli[0].ctl_hi = 1;
	p->tx_lli[0].sstat = 0;
	p->tx_lli[0].dstat = 0;

	p->tx_lli[1].sar = p->tx_buf_dma + 8;
	p->tx_lli[1].dar = ESW_PHYS + ESW_MTP_FIFO_DATA;
	p->tx_lli[1].llp = 0;
	p->tx_lli[1].ctl_lo = TX_CTL_LO | CTL_INT_EN;
	p->tx_lli[1].ctl_hi = (dlen + 7) / 8;
	p->tx_lli[1].sstat = 0;
	p->tx_lli[1].dstat = 0;

	p->tx_busy = 1;
	netif_stop_queue(dev);
	dmac_wr(p, DMAC_CH(CH_TX) + DMAC_CTL_LO, TX_CTL_LO);
	dmac_wr(p, DMAC_CLEAR_TFR, 1 << CH_TX);
	dmac_chbit(p, DMAC_MASK_TFR, CH_TX, 1);	/* whole chain done */
	dmac_start(p, CH_TX, p->tx_lli_dma);
	spin_unlock_irqrestore(&p->lock, flags);

	netif_trans_update(dev);
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static void t48s_tx_timeout(struct net_device *dev, unsigned int txqueue)
{
	struct t48s_eth *p = netdev_priv(dev);
	unsigned long flags;

	netdev_warn(dev, "transmit timed out (CHEN 0x%x)\n", dmac_rd(p, DMAC_CHEN));
	spin_lock_irqsave(&p->lock, flags);
	dmac_stop(p, CH_TX);
	p->tx_busy = 0;
	dev->stats.tx_errors++;
	netif_wake_queue(dev);
	spin_unlock_irqrestore(&p->lock, flags);
}

/* ---- open / close ---- */

/*
 * Turn the switch's CPU (IMP) port on or off.
 *
 * The IMP control register also decides whether broadcasts reach the CPU. On
 * the first open after a kexec from Yealink's kernel that register was seen
 * to end up as 0 although 0x0c had just been written to it (issue 34): no
 * broadcasts, so no ARP requests, and the phone could only be reached by
 * hosts that already knew its MAC address. A write that is read back
 * afterwards has always held, so: say exactly which frames we want, as
 * Broadcom's driver does, read every write back, and let imp_guard() below
 * check again from time to time.
 */
#define ESW_IMPCTRL_WANTED	(ESW_IMPCTRL_RX_BCAST | ESW_IMPCTRL_RX_MCAST)

static void esw_wr_check(struct t48s_eth *p, u32 off, u32 val)
{
	esw_wr(p, off, val);
	if (esw_rd(p, off) != val) {
		esw_wr(p, off, val);
		netdev_warn(p->dev, "switch register 0x%x did not take 0x%x at once, now 0x%x\n",
			    off / 8, val, esw_rd(p, off));
	}
}

static void esw_imp_enable(struct t48s_eth *p, int on)
{
	u32 v;

	v = esw_rd(p, ESW_GBLMGMT);
	v = on ? (v | ESW_GBLMGMT_IMP_EN) : (v & ~ESW_GBLMGMT_IMP_EN);
	esw_wr_check(p, ESW_GBLMGMT, v);

	v = esw_rd(p, ESW_IMPENABLE);
	v = on ? (v | 1) : (v & ~1);
	esw_wr_check(p, ESW_IMPENABLE, v);

	v = esw_rd(p, ESW_IMPCTRL);
	if (on)
		v = (v & ~ESW_PORTCTRL_DISABLE) | ESW_IMPCTRL_WANTED;
	else
		v |= ESW_PORTCTRL_DISABLE;
	esw_wr_check(p, ESW_IMPCTRL, v);
}

static u32 esw_impspeed_wanted(struct t48s_eth *p);

/*
 * eth0 is the switch's CPU port, which is always up. Its carrier follows the
 * external ports instead (link register bits 0 and 1, the LAN and PC jacks):
 * on while either has a link, so a cable pulled and put back is seen as a
 * carrier change (issue #77: the phone registers and asks DHCP again).
 */
static void link_follow(struct t48s_eth *p)
{
	bool up = esw_rd(p, ESW_LINK) & ESW_LINK_EXT;

	if (up == netif_carrier_ok(p->dev))
		return;
	netdev_info(p->dev, "switch: external link %s\n", up ? "up" : "down");
	if (up)
		netif_carrier_on(p->dev);
	else
		netif_carrier_off(p->dev);
}

/* every two seconds while the interface is up: are broadcasts still coming our
 * way, and does the CPU port's speed setting still match the link? */
static void imp_guard(struct work_struct *work)
{
	struct t48s_eth *p = container_of(work, struct t48s_eth, guard.work);
	u32 v = esw_rd(p, ESW_IMPCTRL);

	if ((v & (ESW_IMPCTRL_WANTED | ESW_PORTCTRL_DISABLE)) != ESW_IMPCTRL_WANTED) {
		p->imp_fixes++;
		netdev_warn(p->dev, "switch: IMP control was 0x%02x, putting broadcasts back (time %u)\n",
			    v, p->imp_fixes);
		esw_wr_check(p, ESW_IMPCTRL, (v & ~ESW_PORTCTRL_DISABLE) | ESW_IMPCTRL_WANTED);
	}
	v = esw_impspeed_wanted(p);
	if (readl(p->esw + ESW_IMPSPEED) != v)
		writel(v, p->esw + ESW_IMPSPEED);
	link_follow(p);
	schedule_delayed_work(&p->guard, 2 * HZ);
}

/* ---- the switch from cold ---- */

/* set a register's bits if they are not as wanted; counts what it changed */
static void esw_want(struct t48s_eth *p, u32 off, u32 mask, u32 val)
{
	u32 v = esw_rd(p, off);

	if ((v & mask) == val)
		return;
	esw_wr_check(p, off, (v & ~mask) | val);
	p->set_last++;
}

static void esw_want64(struct t48s_eth *p, u32 off, u64 val)
{
	if (esw_rd64(p, off) == val)
		return;
	esw_wr64(p, off, val);
	p->set_last++;
}

static int esw_arl_wait(struct t48s_eth *p)
{
	int n = 1000;

	while ((esw_rd(p, ESW_ARL_CTL) & ESW_ARL_CTL_START) && --n)
		udelay(10);
	return n ? 0 : -ETIMEDOUT;
}

/*
 * The address table entry that sends frames for our MAC address to the CPU
 * port (on = 1), or its removal (on = 0). Without it the switch treats our
 * address as unknown and, as the CPU port is set (broadcast and multicast
 * only), keeps unicast frames from us. Returns the bin used, or an error.
 */
static int esw_arl_cpu(struct t48s_eth *p, const u8 *mac, int on)
{
	u64 m = 0;
	int i, bin = -1, rc;

	for (i = 0; i < ETH_ALEN; i++)
		m = (m << 8) | mac[i];
	esw_wr64(p, ESW_ARL_MAC, m);
	esw_wr(p, ESW_ARL_VID, 0);
	esw_wr(p, ESW_ARL_CTL, ESW_ARL_CTL_START | ESW_ARL_CTL_READ);
	rc = esw_arl_wait(p);
	if (rc)
		return rc;
	for (i = 1; i >= 0; i--) {
		u32 fwd = esw_rd(p, ESW_ARL_FWD(i));

		if ((fwd & ESW_ARL_FWD_VALID) && esw_rd64(p, ESW_ARL_ENTRY(i)) == m) {
			bin = i;	/* ours already */
			break;
		}
		if (!(fwd & ESW_ARL_FWD_VALID))
			bin = i;	/* free */
	}
	if (!on) {
		if (bin < 0 || esw_rd64(p, ESW_ARL_ENTRY(bin)) != m ||
		    !(esw_rd(p, ESW_ARL_FWD(bin)) & ESW_ARL_FWD_VALID))
			return -ENOENT;
		esw_wr(p, ESW_ARL_FWD(bin), 0);
	} else {
		if (bin < 0)
			bin = 0;	/* both taken by other addresses: ours matters more */
		if (esw_rd64(p, ESW_ARL_ENTRY(bin)) == m &&
		    esw_rd(p, ESW_ARL_FWD(bin)) == ESW_ARL_FWD_CPU)
			return bin;
		esw_wr64(p, ESW_ARL_ENTRY(bin), m);
		esw_wr(p, ESW_ARL_FWD(bin), ESW_ARL_FWD_CPU);
		p->set_last++;
	}
	esw_wr(p, ESW_ARL_CTL, ESW_ARL_CTL_START);
	rc = esw_arl_wait(p);
	return rc ? rc : bin;
}

/* the CPU port's speed register follows the external port that has a link, as
 * Yealink's driver keeps it (ethHw_impSpeedSet) */
static u32 esw_impspeed_wanted(struct t48s_eth *p)
{
	static const u8 code[4] = { 2, 1, 4, 4 };	/* 10, 100, 1000 Mbit/s */
	u32 link = esw_rd(p, ESW_LINK), speed = esw_rd(p, ESW_SPEED);
	int port;

	for (port = ESW_PORTS - 1; port >= 0; port--)
		if (link & (1 << port))
			return ESW_IMPSPEED_CPU_1000 | code[(speed >> (2 * port)) & 3];
	return ESW_IMPSPEED_CPU_1000 | 4;
}

/*
 * What Yealink's firmware (ethHw_Init and what follows it) changes from the
 * values a reset leaves, read from the running switch before and after a
 * reset: the spanning tree, 802.1X and LLDP group addresses go to both ports
 * and the CPU, and the VLAN settings of the three ports.
 */
static const struct { u8 page, off; u64 val; } esw_settings[] = {
	{ 0x04, 0x20, 0x0180c200000eULL }, { 0x04, 0x28, 0x103 },
	{ 0x04, 0x30, 0x0180c2000003ULL }, { 0x04, 0x38, 0x103 },
	{ 0x04, 0x40, 0x0180c2000000ULL }, { 0x04, 0x48, 0x103 },
	{ 0x04, 0x0e, 0xa8 },
	{ 0x34, 0x01, 0x0e }, { 0x34, 0x02, 0x70 }, { 0x34, 0x05, 0x40 },
	{ 0x34, 0x06, 0x1d },
	{ 0x34, 0x10, 0xfff }, { 0x34, 0x12, 0xfff }, { 0x34, 0x20, 0xfff },
};

/*
 * Bring the switch up, from whatever state it is in. Called with the
 * interface down, before the CPU-side FIFOs and the DMA are started.
 */
static int esw_bring_up(struct t48s_eth *p)
{
	void __iomem *chipc = ioremap(CHIPC_PHYS, 0x1000);
	unsigned long flags;
	u32 busclk, clk, reset;
	int i, n, rc;

	if (!chipc)
		return -ENOMEM;
	local_irq_save(flags);
	busclk = readl(chipc + CHIPC_BUSCLK);
	clk = readl(chipc + CHIPC_ESWCLK);
	reset = readl(chipc + CHIPC_SOFTRESET2);
	writel(busclk | CHIPC_BUSCLK_ESW | CHIPC_BUSCLK_DMAC1, chipc + CHIPC_BUSCLK);
	if (!(clk & CHIPC_CLK_OUTPUT))
		writel(CHIPC_ESWCLK_ON, chipc + CHIPC_ESWCLK);
	local_irq_restore(flags);
	if (reset & CHIPC_SOFTRESET2_ESW) {
		udelay(100);
		local_irq_save(flags);
		writel(readl(chipc + CHIPC_SOFTRESET2) & ~CHIPC_SOFTRESET2_ESW,
		       chipc + CHIPC_SOFTRESET2);
		local_irq_restore(flags);
	}
	iounmap(chipc);
	if ((busclk & (CHIPC_BUSCLK_ESW | CHIPC_BUSCLK_DMAC1)) != (CHIPC_BUSCLK_ESW | CHIPC_BUSCLK_DMAC1) ||
	    !(clk & CHIPC_CLK_OUTPUT) || (reset & CHIPC_SOFTRESET2_ESW))
		netdev_info(p->dev, "switch was off (bus clocks 0x%08x, clock 0x%08x, reset 0x%08x): started\n",
			    busclk, clk, reset);
	for (n = 0; n < 200 && !(readl(p->esw + ESW_STATUS) & ESW_STATUS_READY); n++)
		usleep_range(1000, 2000);
	p->off = false;
	if (!(readl(p->esw + ESW_STATUS) & ESW_STATUS_READY)) {
		netdev_err(p->dev, "switch not ready (status 0x%08x)\n", readl(p->esw + ESW_STATUS));
		return -EIO;
	}

	p->set_last = 0;
	for (i = 0; i < ARRAY_SIZE(esw_settings); i++)
		esw_want64(p, ESW_PAGE(esw_settings[i].page, esw_settings[i].off), esw_settings[i].val);
	/* both external ports: MAC on, forwarding. Then let the switch switch. */
	for (i = 0; i < ESW_PORTS; i++)
		esw_want(p, ESW_PORTCTRL(i), ESW_PORTCTRL_STP | ESW_PORTCTRL_DISABLE,
			 ESW_PORTCTRL_FORWARD);
	esw_want(p, ESW_SWMODE, ESW_SWMODE_MANAGED | ESW_SWMODE_FORWARD,
		 ESW_SWMODE_MANAGED | ESW_SWMODE_FORWARD);
	rc = esw_arl_cpu(p, p->dev->dev_addr, 1);
	p->arl_bin = rc;
	if (rc < 0)
		netdev_warn(p->dev, "switch: could not enter our address in the address table (%d)\n", rc);
	if (readl(p->esw + ESW_IMPSPEED) != esw_impspeed_wanted(p)) {
		writel(esw_impspeed_wanted(p), p->esw + ESW_IMPSPEED);
		p->set_last++;
	}
	if (p->set_last)
		netdev_info(p->dev, "switch: %u settings were not as wanted and have been set\n", p->set_last);
	return 0;
}

/*
 * The cold test: with the interface down, take the switch as far down as
 * this kernel can. Our address leaves the address table (which survives a
 * reset), the CPU-side FIFOs are stopped, the switch is put in reset and its
 * clocks are gated. Nothing may read a switch register until the next "up".
 */
static struct t48s_eth *the_eth;

static int cold_set(const char *val, const struct kernel_param *kp)
{
	struct t48s_eth *p = the_eth;
	void __iomem *chipc;
	unsigned long flags;
	int rc;

	if (!p || val[0] != '1')
		return -EINVAL;
	rtnl_lock();
	if (netif_running(p->dev) || p->off) {
		rtnl_unlock();
		return -EBUSY;
	}
	chipc = ioremap(CHIPC_PHYS, 0x1000);
	if (!chipc) {
		rtnl_unlock();
		return -ENOMEM;
	}
	rc = esw_arl_cpu(p, p->dev->dev_addr, 0);
	netdev_info(p->dev, "test: our address %s the address table (%d)\n",
		    rc >= 0 ? "removed from" : "was not in", rc);
	writel(0, p->esw + ESW_PTM_CONFIG);
	writel(0, p->esw + ESW_MTP_CONFIG);
	writel(0, p->esw + ESW_IMPSPEED);
	p->off = true;
	local_irq_save(flags);
	writel(readl(chipc + CHIPC_SOFTRESET2) | CHIPC_SOFTRESET2_ESW, chipc + CHIPC_SOFTRESET2);
	writel(readl(chipc + CHIPC_ESWCLK) & ~CHIPC_CLK_OUTPUT, chipc + CHIPC_ESWCLK);
	writel(readl(chipc + CHIPC_BUSCLK) & ~CHIPC_BUSCLK_ESW, chipc + CHIPC_BUSCLK);
	local_irq_restore(flags);
	netdev_info(p->dev, "test: switch in reset, clocks gated (reset 0x%08x, clock 0x%08x, bus clocks 0x%08x)\n",
		    readl(chipc + CHIPC_SOFTRESET2), readl(chipc + CHIPC_ESWCLK), readl(chipc + CHIPC_BUSCLK));
	iounmap(chipc);
	p->cold_tests++;
	rtnl_unlock();
	return 0;
}

static const struct kernel_param_ops cold_ops = { .set = cold_set };
module_param_cb(cold, &cold_ops, NULL, 0200);

/*
 * cat /sys/module/t48s_eth/parameters/diag: the switch's CPU port settings,
 * what it has sent our way, and where the receive ring and the DMA stand.
 */
static int diag_get(char *buf, const struct kernel_param *kp)
{
	struct t48s_eth *p = the_eth;
	u32 llp;

	if (!p)
		return sprintf(buf, "no device\n");
	if (p->off)
		return sprintf(buf, "switch off (cold test %u)\n", p->cold_tests);
	llp = dmac_rd(p, DMAC_CH(CH_RX) + DMAC_LLP);
	return sprintf(buf,
		"imp ctrl 0x%02x (bcast %s, put back %u times), gblmgmt 0x%02x, imp enable 0x%02x, link 0x%04x\n"
		"to cpu: bcast %u mcast %u ucast %u dropped %u\n"
		"rx ring: driver at %d, dma at %d, channels on 0x%x\n"
		"last up: %u settings set; our address in table bin %d; ports 0x%02x 0x%02x, mode 0x%02x, cpu speed 0x%02x; cold tests %u\n",
		esw_rd(p, ESW_IMPCTRL),
		(esw_rd(p, ESW_IMPCTRL) & ESW_IMPCTRL_RX_BCAST) ? "on" : "OFF", p->imp_fixes,
		esw_rd(p, ESW_GBLMGMT), esw_rd(p, ESW_IMPENABLE), esw_rd(p, ESW_LINK),
		esw_rd(p, ESW_MIB(ESW_PORT_IMP, ESW_MIB_TX_BCAST)),
		esw_rd(p, ESW_MIB(ESW_PORT_IMP, ESW_MIB_TX_MCAST)),
		esw_rd(p, ESW_MIB(ESW_PORT_IMP, ESW_MIB_TX_UCAST)),
		esw_rd(p, ESW_MIB(ESW_PORT_IMP, ESW_MIB_TX_DROP)),
		p->rx_next,
		llp ? (int)((llp - p->rx_lli_dma) / sizeof(struct lli)) : -1,
		dmac_rd(p, DMAC_CHEN) & 0xff,
		p->set_last, p->arl_bin, esw_rd(p, ESW_PORTCTRL(0)), esw_rd(p, ESW_PORTCTRL(1)),
		esw_rd(p, ESW_SWMODE), readl(p->esw + ESW_IMPSPEED), p->cold_tests);
}

static const struct kernel_param_ops diag_ops = { .get = diag_get };
module_param_cb(diag, &diag_ops, NULL, 0444);

static int t48s_open(struct net_device *dev)
{
	struct t48s_eth *p = netdev_priv(dev);
	int rc;

	rc = esw_bring_up(p);
	if (rc)
		return rc;
	netdev_info(dev, "switch: link 0x%04x, gblmgmt 0x%02x, imp ctrl 0x%02x, ptm 0x%08x, mtp 0x%08x\n",
		    esw_rd(p, ESW_LINK), esw_rd(p, ESW_GBLMGMT),
		    esw_rd(p, ESW_PORTCTRL(ESW_PORT_IMP)),
		    readl(p->esw + ESW_PTM_CONFIG), readl(p->esw + ESW_MTP_CONFIG));

	dmac_wr(p, DMAC_CFG, 1);	/* controller enable */
	dmac_setup(p, CH_RX, RX_CTL_LO, RX_CFG_LO, RX_CFG_HI,
		   ESW_PHYS + ESW_PTM_DMASTAT, 0);
	dmac_setup(p, CH_TX, TX_CTL_LO, TX_CFG_LO, TX_CFG_HI,
		   0, ESW_PHYS + ESW_MTP_DMASTAT);
	writel(ESW_PTM_CONFIG_VAL, p->esw + ESW_PTM_CONFIG);
	writel(ESW_MTP_CONFIG_VAL, p->esw + ESW_MTP_CONFIG);

	rx_ring_init(p);
	p->tx_busy = 0;

	rc = request_irq(p->irq_rx, t48s_rx_irq, 0, "t48s_eth rx", p);
	if (rc)
		return rc;
	rc = request_irq(p->irq_tx, t48s_tx_irq, 0, "t48s_eth tx", p);
	if (rc) {
		free_irq(p->irq_rx, p);
		return rc;
	}

	napi_enable(&p->napi);
	esw_imp_enable(p, 1);
	dmac_chbit(p, DMAC_MASK_BLOCK, CH_RX, 1);
	dmac_chbit(p, DMAC_MASK_ERR, CH_RX, 1);
	dmac_start(p, CH_RX, p->rx_lli_dma);
	netdev_info(dev, "switch: imp ctrl now 0x%02x, gblmgmt 0x%02x\n",
		    esw_rd(p, ESW_IMPCTRL), esw_rd(p, ESW_GBLMGMT));
	netif_carrier_off(dev);
	link_follow(p);
	netif_start_queue(dev);
	schedule_delayed_work(&p->guard, HZ);
	return 0;
}

static int t48s_stop(struct net_device *dev)
{
	struct t48s_eth *p = netdev_priv(dev);

	cancel_delayed_work_sync(&p->guard);
	netif_stop_queue(dev);
	esw_imp_enable(p, 0);
	dmac_chbit(p, DMAC_MASK_BLOCK, CH_RX, 0);
	dmac_chbit(p, DMAC_MASK_TFR, CH_TX, 0);
	dmac_stop(p, CH_RX);
	dmac_stop(p, CH_TX);
	/* the CPU-side FIFOs off too: a switch reset with them on leaves the
	 * receive side stuck (seen: nothing reached the CPU afterwards) */
	writel(0, p->esw + ESW_PTM_CONFIG);
	writel(0, p->esw + ESW_MTP_CONFIG);
	napi_disable(&p->napi);
	free_irq(p->irq_rx, p);
	free_irq(p->irq_tx, p);
	netif_carrier_off(dev);
	return 0;
}

static const struct net_device_ops t48s_ops = {
	.ndo_open		= t48s_open,
	.ndo_stop		= t48s_stop,
	.ndo_start_xmit		= t48s_xmit,
	.ndo_tx_timeout		= t48s_tx_timeout,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_set_mac_address	= eth_mac_addr,
};

static int t48s_eth_probe(struct platform_device *pdev)
{
	struct device *d = &pdev->dev;
	struct net_device *dev;
	struct t48s_eth *p;
	int rc;

	dev = devm_alloc_etherdev(d, sizeof(*p));
	if (!dev)
		return -ENOMEM;
	SET_NETDEV_DEV(dev, d);
	p = netdev_priv(dev);
	p->dev = dev;
	spin_lock_init(&p->lock);
	INIT_DELAYED_WORK(&p->guard, imp_guard);
	p->arl_bin = -1;

	/*
	 * Plain ioremap, no claim on the regions: the DMA controller's registers
	 * are shared in principle (we use two of its eight channels).
	 */
	p->esw = devm_ioremap(d, ESW_PHYS, ESW_SIZE);
	p->dmac = devm_ioremap(d, DMA1_PHYS, DMA1_SIZE);
	if (!p->esw || !p->dmac)
		return -ENOMEM;
	p->irq_rx = platform_get_irq_byname(pdev, "rx");
	p->irq_tx = platform_get_irq_byname(pdev, "tx");
	if (p->irq_rx < 0 || p->irq_tx < 0)
		return -ENODEV;

	rc = dma_set_mask_and_coherent(d, DMA_BIT_MASK(32));
	if (rc)
		return rc;
	p->rx_lli = dmam_alloc_coherent(d, RX_RING * sizeof(struct lli), &p->rx_lli_dma, GFP_KERNEL);
	p->rx_buf = dmam_alloc_coherent(d, RX_RING * RX_BUF_LEN, &p->rx_buf_dma, GFP_KERNEL);
	p->tx_lli = dmam_alloc_coherent(d, 2 * sizeof(struct lli), &p->tx_lli_dma, GFP_KERNEL);
	p->tx_buf = dmam_alloc_coherent(d, TX_BUF_LEN, &p->tx_buf_dma, GFP_KERNEL);
	if (!p->rx_lli || !p->rx_buf || !p->tx_lli || !p->tx_buf)
		return -ENOMEM;

	/* "local-mac-address" in the device tree if there is one, else the built-in address */
	if (device_get_ethdev_address(d, dev))
		eth_hw_addr_set(dev, t48s_mac);
	dev->netdev_ops = &t48s_ops;
	dev->watchdog_timeo = 2 * HZ;
	netif_napi_add_weight(dev, &p->napi, t48s_poll, 16);

	rc = register_netdev(dev);
	if (rc)
		return rc;
	the_eth = p;
	netdev_info(dev, "T48S switch ethernet, MAC %pM\n", dev->dev_addr);
	return 0;
}

static const struct of_device_id t48s_eth_of_match[] = {
	{ .compatible = "brcm,bcm11107-esw-cpu" },
	{ }
};

static struct platform_driver t48s_eth_driver = {
	.probe = t48s_eth_probe,
	.driver = {
		.name = DRV_NAME,
		.of_match_table = t48s_eth_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(t48s_eth_driver);
MODULE_LICENSE("GPL");

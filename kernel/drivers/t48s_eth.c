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
 * is copied, one transmit is in flight at a time, and the switch is assumed
 * to be configured already (it is, after a kexec from Yealink's kernel, or
 * after U-Boot; ports, ARL entry for our MAC and IMP speed are left as found).
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
#include <mach/irqs.h>

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
#define ESW_GBLMGMT		ESW_PAGE(0x02, 0x00)
#define ESW_GBLMGMT_IMP_EN	0x80
#define ESW_IMPENABLE		ESW_PAGE(0x34, 0x06)
#define ESW_LINK		ESW_PAGE(0x01, 0x00)
#define ESW_PORT_IMP		8
#define ESW_PORT_LAN		0

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
#define IRQ_RX			IRQ_DMA1C2
#define IRQ_TX			IRQ_DMA1C3
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
		u32 ctl_hi = ACCESS_ONCE(l->ctl_hi);

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

	dev->trans_start = jiffies;
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static void t48s_tx_timeout(struct net_device *dev)
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

static void esw_imp_enable(struct t48s_eth *p, int on)
{
	u32 v;

	v = esw_rd(p, ESW_GBLMGMT);
	v = on ? (v | ESW_GBLMGMT_IMP_EN) : (v & ~ESW_GBLMGMT_IMP_EN);
	esw_wr(p, ESW_GBLMGMT, v);

	v = esw_rd(p, ESW_IMPENABLE);
	v = on ? (v | 1) : (v & ~1);
	esw_wr(p, ESW_IMPENABLE, v);

	v = esw_rd(p, ESW_PORTCTRL(ESW_PORT_IMP));
	v = on ? (v & ~ESW_PORTCTRL_DISABLE) : (v | ESW_PORTCTRL_DISABLE);
	esw_wr(p, ESW_PORTCTRL(ESW_PORT_IMP), v);
}

static int t48s_open(struct net_device *dev)
{
	struct t48s_eth *p = netdev_priv(dev);
	int rc;

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

	rc = request_irq(IRQ_RX, t48s_rx_irq, 0, "t48s_eth rx", p);
	if (rc)
		return rc;
	rc = request_irq(IRQ_TX, t48s_tx_irq, 0, "t48s_eth tx", p);
	if (rc) {
		free_irq(IRQ_RX, p);
		return rc;
	}

	napi_enable(&p->napi);
	esw_imp_enable(p, 1);
	dmac_chbit(p, DMAC_MASK_BLOCK, CH_RX, 1);
	dmac_chbit(p, DMAC_MASK_ERR, CH_RX, 1);
	dmac_start(p, CH_RX, p->rx_lli_dma);
	netif_carrier_on(dev);
	netif_start_queue(dev);
	return 0;
}

static int t48s_stop(struct net_device *dev)
{
	struct t48s_eth *p = netdev_priv(dev);

	netif_stop_queue(dev);
	esw_imp_enable(p, 0);
	dmac_chbit(p, DMAC_MASK_BLOCK, CH_RX, 0);
	dmac_chbit(p, DMAC_MASK_TFR, CH_TX, 0);
	dmac_stop(p, CH_RX);
	dmac_stop(p, CH_TX);
	napi_disable(&p->napi);
	free_irq(IRQ_RX, p);
	free_irq(IRQ_TX, p);
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
	.ndo_change_mtu		= eth_change_mtu,
};

static int __init t48s_eth_init(void)
{
	struct net_device *dev;
	struct t48s_eth *p;
	int rc = -ENOMEM;

	dev = alloc_etherdev(sizeof(*p));
	if (!dev)
		return -ENOMEM;
	p = netdev_priv(dev);
	p->dev = dev;
	spin_lock_init(&p->lock);

	p->esw = ioremap(ESW_PHYS, ESW_SIZE);
	p->dmac = ioremap(DMA1_PHYS, DMA1_SIZE);
	if (!p->esw || !p->dmac)
		goto fail;

	p->rx_lli = dma_alloc_coherent(NULL, RX_RING * sizeof(struct lli), &p->rx_lli_dma, GFP_KERNEL);
	p->rx_buf = dma_alloc_coherent(NULL, RX_RING * RX_BUF_LEN, &p->rx_buf_dma, GFP_KERNEL);
	p->tx_lli = dma_alloc_coherent(NULL, 2 * sizeof(struct lli), &p->tx_lli_dma, GFP_KERNEL);
	p->tx_buf = dma_alloc_coherent(NULL, TX_BUF_LEN, &p->tx_buf_dma, GFP_KERNEL);
	if (!p->rx_lli || !p->rx_buf || !p->tx_lli || !p->tx_buf)
		goto fail;

	memcpy(dev->dev_addr, t48s_mac, ETH_ALEN);
	dev->netdev_ops = &t48s_ops;
	dev->watchdog_timeo = 2 * HZ;
	netif_napi_add(dev, &p->napi, t48s_poll, 16);

	rc = register_netdev(dev);
	if (rc)
		goto fail;
	netdev_info(dev, "T48S switch ethernet, MAC %pM\n", dev->dev_addr);
	return 0;

fail:
	pr_err(DRV_NAME ": init failed (%d)\n", rc);
	return rc;	/* the few allocations are left behind; this is built in */
}
device_initcall(t48s_eth_init);
MODULE_LICENSE("GPL");

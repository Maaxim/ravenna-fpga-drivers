// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/irq.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <uapi/linux/net_tstamp.h>

#include "main.h"

void ra_net_tx_ts_irq(struct ra_net_priv *priv)
{
	struct ptp_packet_fpga_timestamp *ts_packet;
	struct device *dev = priv->dev;
	u32 ctr, sot;
	u16 wr_idx;

	/* Must be called with priv->lock held! */

	dev_dbg(dev, "%s()\n", __func__);

	spin_lock(&priv->tx_ts.lock);

	wr_idx = priv->tx_ts.ts_wr_idx;
	wr_idx++;
	wr_idx %= RA_NET_TX_TS_LIST_SIZE;

	if (unlikely(wr_idx == priv->tx_ts.ts_rd_idx)) {
		/* timestamp buffer full */
		dev_err(dev, "tx timestamp buffer full, IRQ disabled\n");

		priv->tx_ts.reenable_irq = true;
		ra_net_pp_irq_disable(priv, RA_NET_PP_IRQ_PTP_TX_TS_IRQ_AVAILABLE);
		spin_unlock(&priv->tx_ts.lock);
		return;
	}

	ts_packet = &priv->tx_ts.fpga_ts[wr_idx];
	priv->tx_ts.ts_wr_idx = wr_idx;

	spin_unlock(&priv->tx_ts.lock);

	dev_dbg(dev, "TX_TS_COUNT: 0x%08X\n",
		ra_net_ior(priv, RA_NET_PTP_TX_TS_CNT));

	for (ctr = sizeof(*ts_packet) / sizeof(u32); ctr >= 0; ctr--) {
		sot = ra_net_ior(priv, RA_NET_TX_TIMESTAMP_FIFO);

		if ((sot >> 16) == RA_NET_TX_TIMESTAMP_START_OF_TS)
			break;
	}

	if (ctr != sizeof(*ts_packet) / sizeof(u32))
		dev_dbg(dev, "misaligned timestamp for tx packet found\n");

	if (unlikely(ctr == 0)) {
		dev_err(dev, "%s(): no start of timestamp found\n", __func__);
		return;
	}

	dev_dbg(dev, "reading timestamp for tx packet\n");

	ts_packet->seconds_hi = sot & 0xffff;

	/* Pull the remaining data */
	ra_net_ior_rep(priv, RA_NET_TX_TIMESTAMP_FIFO,
			&ts_packet->seconds,
			sizeof(*ts_packet) - sizeof(ts_packet->seconds_hi));

	/* schedule always in case of remaining timestamps in list */
	schedule_work(&priv->tx_ts.work);
}

static void ra_net_stamp_tx_skb(struct ra_net_priv *priv, struct sk_buff *skb,
				const struct ptp_packet_fpga_timestamp *ts,
				bool *ts_consumed, bool *skb_consumed)
{
	struct device *dev = priv->dev;
	u8 *data = skb->data;
	u16 packet_seq_id;
	u32 offset;

	offset = ETH_HLEN + IPV4_HLEN(data) + UDP_HLEN;

	*ts_consumed = false;
	*skb_consumed = true;

	/* assumptions:
	 *  - PTP packets are PTPV2 IPV4
	 *  - sequence ID is unique and sufficient to associate timestamp and packet
	 *    (FIXME: is this always true ?)
	 */

	if (skb->len + ETH_HLEN < offset + OFF_PTP_SEQUENCE_ID + sizeof(packet_seq_id)) {
		dev_dbg(dev,  "packet does not contain ptp sequence id (length invalid)\n");
		return;
	}

	packet_seq_id = ntohs(*(__be16*)(data + offset + OFF_PTP_SEQUENCE_ID));

	if (likely(ts->sequence_id == packet_seq_id)) {
		/* OK, timestamp is valid */
		struct skb_shared_hwtstamps shhwtstamps;

		dev_dbg(dev, "found valid timestamp for tx packet; sequence id 0x%04X\n",
			packet_seq_id);

		shhwtstamps.hwtstamp =
			(s64)ts->seconds * NSEC_PER_SEC + ts->nanoseconds;

		*ts_consumed = true;

		skb_tstamp_tx(skb, &shhwtstamps);

		return;
	}

	if (ts->sequence_id > packet_seq_id) {
		/* corresponding timestamp seems to be lost ! => "remove" packet from list */
		dev_dbg(dev, "timestamp sequence id (0x%04X) > packet sequence id (0x%04X) => discard packet\n",
			ts->sequence_id, packet_seq_id);

		*ts_consumed = true;
		*skb_consumed = false;

		return;
	}

	/* ts_seq_id < packet_seq_id) */
	/* timestamp without packet ! => remove from list */
	dev_dbg(dev, "timestamp sequence id (0x%04X) < packet sequence id (0x%04X) => discard timestamp\n",
		ts->sequence_id, packet_seq_id);
}

static void ra_net_tx_ts_work(struct work_struct *work)
{
	struct ra_net_priv *priv =
		container_of(work, struct ra_net_priv, tx_ts.work);
	int skb_wr_idx, skb_rd_idx;
	int ts_wr_idx, ts_rd_idx;
	unsigned long flags;

	dev_dbg(priv->dev, "%s()\n", __func__);

	spin_lock_irqsave(&priv->tx_ts.lock, flags);

	skb_wr_idx = priv->tx_ts.skb_wr_idx;
	ts_wr_idx  = priv->tx_ts.ts_wr_idx;
	skb_rd_idx = priv->tx_ts.skb_rd_idx;
	ts_rd_idx  = priv->tx_ts.ts_rd_idx;

	if (ts_wr_idx == ts_rd_idx)	// nothing to do
		goto out;

	do {
		struct ptp_packet_fpga_timestamp *ts;
		bool ts_consumed, skb_consumed;
		struct sk_buff *skb;

		ts_rd_idx++;
		ts_rd_idx %= RA_NET_TX_TS_LIST_SIZE;

		skb_rd_idx++;
		skb_rd_idx %= RA_NET_TX_SKB_LIST_SIZE;

		skb = priv->tx_ts.skb_ptr[skb_rd_idx];
		ts = &priv->tx_ts.fpga_ts[ts_rd_idx];

		ra_net_stamp_tx_skb(priv, skb, ts,
				    &ts_consumed, &skb_consumed);

		if (skb_consumed) {
			dev_kfree_skb_any(skb);
		} else {
			skb_rd_idx += RA_NET_TX_SKB_LIST_SIZE-1;
			skb_rd_idx %= RA_NET_TX_SKB_LIST_SIZE;
		}

		if (!ts_consumed) {
			ts_rd_idx += RA_NET_TX_TS_LIST_SIZE-1;
			ts_rd_idx %= RA_NET_TX_TS_LIST_SIZE;
		}
	} while ((ts_rd_idx != ts_wr_idx) && (skb_rd_idx != skb_wr_idx));

	priv->tx_ts.ts_rd_idx = ts_rd_idx;
	priv->tx_ts.skb_rd_idx = skb_rd_idx;

out:
	spin_unlock_irqrestore(&priv->tx_ts.lock, flags);

	if (priv->tx_ts.reenable_irq) {
		priv->tx_ts.reenable_irq = false;
		ra_net_pp_irq_enable(priv, RA_NET_PP_IRQ_PTP_TX_TS_IRQ_AVAILABLE);
	}

}

void ra_net_flush_tx_ts(struct ra_net_priv *priv)
{
	unsigned long flags;
	int rd_idx;

	cancel_work_sync(&priv->tx_ts.work);

	spin_lock_irqsave(&priv->tx_ts.lock, flags);

	for (;;) {
		u32 pp_irqs;
		struct ptp_packet_fpga_timestamp ts_packet;

		pp_irqs = ra_net_ior(priv, RA_NET_PP_IRQS);
		if (!(pp_irqs & RA_NET_PP_IRQ_PTP_TX_TS_IRQ_AVAILABLE))
			break;

		ra_net_ior_rep(priv, RA_NET_TX_TIMESTAMP_FIFO,
			       &ts_packet, sizeof(ts_packet));
	}

	/* Tx skb list */
	while (priv->tx_ts.skb_rd_idx != priv->tx_ts.skb_wr_idx) {
		dev_kfree_skb(priv->tx_ts.skb_ptr[rd_idx]);

		priv->tx_ts.skb_rd_idx++;
		priv->tx_ts.skb_rd_idx %= RA_NET_TX_SKB_LIST_SIZE;
	}

	priv->tx_ts.skb_rd_idx = 0;
	priv->tx_ts.skb_wr_idx = 0;

	priv->tx_ts.ts_rd_idx = 0;
	priv->tx_ts.ts_wr_idx = 0;

	spin_unlock_irqrestore(&priv->tx_ts.lock, flags);
}

bool ra_net_tx_ts_send(struct ra_net_priv *priv, struct sk_buff *skb)
{
	struct skb_shared_info *skb_sh = skb_shinfo(skb);
	unsigned long flags;

	/* Must be called with priv->lock held! */

	if (!priv->tx_ts.enable)
		return true;

	if (skb_sh->tx_flags & SKBTX_HW_TSTAMP) {
		spin_lock_irqsave(&priv->tx_ts.lock, flags);

		priv->tx_ts.skb_wr_idx++;
		priv->tx_ts.skb_wr_idx %= RA_NET_TX_SKB_LIST_SIZE;

		if (priv->tx_ts.skb_wr_idx == priv->tx_ts.skb_rd_idx) {
			struct sk_buff *old_skb;

			/* no space left in ringbuffer!
			 * => discard oldest entry
			 */
			priv->tx_ts.skb_rd_idx++;
			priv->tx_ts.skb_rd_idx %= RA_NET_TX_SKB_LIST_SIZE;

			old_skb = priv->tx_ts.skb_ptr[priv->tx_ts.skb_rd_idx];
			dev_kfree_skb_any(old_skb);

			net_err_ratelimited("%s: skb ringbuffer for timestamping full "
						"=> discarding oldest entry\n",
						priv->ndev->name);
		}

		dev_dbg(priv->dev, "Requesting timestamp for tx packet\n");

		priv->tx_ts.skb_ptr[priv->tx_ts.skb_wr_idx] = skb;

		spin_unlock_irqrestore(&priv->tx_ts.lock, flags);

		skb_sh->tx_flags |= SKBTX_IN_PROGRESS;

		return false;
	}

	return true;
}

void ra_net_rx_skb_stamp(struct ra_net_priv *priv, struct sk_buff *skb,
			 struct ptp_packet_fpga_timestamp *ts)
{
	struct skb_shared_hwtstamps *ts_ptr = skb_hwtstamps(skb);
	u64 ns;

	if (!priv->rx_ts_enable)
		return;

	if (ts->start_of_ts != RA_NET_TX_TIMESTAMP_START_OF_TS) {
		dev_dbg(priv->dev, "Rx timestamp has no SOT\n");
		return;
	}

	dev_dbg(priv->dev, "Valid rx timestamp found\n");

	ns = (s64)ts->seconds * NSEC_PER_SEC + ts->nanoseconds;
	ts_ptr->hwtstamp = ns_to_ktime(ns);
}

static void ra_net_tx_ts_config(struct ra_net_priv *priv)
{
	u32 val = ra_net_ior(priv, RA_NET_PP_CONFIG);
	bool have = !!(val & RA_NET_PP_CONFIG_ENABLE_PTP_TIMESTAMPS);
	bool want = priv->tx_ts.enable || priv->rx_ts_enable;

	if (have == want)
		return;

	netif_stop_queue(priv->ndev);

	ra_net_iow_mask(priv, RA_NET_PP_CONFIG,
			RA_NET_PP_CONFIG_ENABLE_PTP_TIMESTAMPS,
			want ? RA_NET_PP_CONFIG_ENABLE_PTP_TIMESTAMPS : 0);

	if (want)
		ra_net_pp_irq_enable(priv, RA_NET_PP_IRQ_PTP_TX_TS_IRQ_AVAILABLE);
	else
		ra_net_pp_irq_disable(priv, RA_NET_PP_IRQ_PTP_TX_TS_IRQ_AVAILABLE);

	netif_start_queue(priv->ndev);
}

void ra_net_tx_ts_init(struct ra_net_priv *priv)
{
	spin_lock_init(&priv->tx_ts.lock);
	INIT_WORK(&priv->tx_ts.work, ra_net_tx_ts_work);
}

int ra_net_hwtstamp_ioctl(struct net_device *ndev, struct ifreq *ifr, int cmd)
{
	struct ra_net_priv *priv = netdev_priv(ndev);
	struct hwtstamp_config config;
	struct device *dev = priv->dev;

	dev_dbg(dev, "%s()\n", __func__);

	if (copy_from_user(&config, ifr->ifr_data, sizeof(config)))
		return -EFAULT;

	/* reserved for future extensions */
	if (config.flags) {
		dev_err(dev, "%s(): got config.flags 0x%08X which should be 0.",
		       __func__, config.flags);
		return -EINVAL;
	}

	switch(config.tx_type) {
	case HWTSTAMP_TX_OFF:
		dev_dbg(dev, "%s(): HWTSTAMP_TX_OFF\n", __func__);
		priv->tx_ts.enable = false;
		ra_net_tx_ts_config(priv);
		break;

	case HWTSTAMP_TX_ON:
		dev_dbg(dev, "%s(): HWTSTAMP_TX_ON\n", __func__);
		priv->tx_ts.enable = true;
		ra_net_tx_ts_config(priv);
		break;

	default:
		dev_err(dev, "%s() config.tx_type %d not supported\n",
			__func__, config.tx_type);
		return -EINVAL;
	}

	switch(config.rx_filter) {
	case HWTSTAMP_FILTER_NONE:
		dev_dbg(dev, "%s(): HWTSTAMP_FILTER_NONE\n", __func__);
		priv->rx_ts_enable = false;
		ra_net_tx_ts_config(priv);
		break;

	case HWTSTAMP_FILTER_PTP_V2_L4_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L4_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ:
		dev_dbg(dev, "%s(): HWTSTAMP_FILTER_PTP_V2_L4_xxx\n",
			__func__);

		priv->rx_ts_enable = true;
		ra_net_tx_ts_config(priv);

		config.rx_filter = HWTSTAMP_FILTER_PTP_V2_L4_EVENT;
		break;

	default:
		dev_err(dev, "%s() config.rx_filter %i not supported\n",
			__func__, config.rx_filter);
		return -EINVAL;
	}

	if (copy_to_user(ifr->ifr_data, &config, sizeof(config)))
		return -EFAULT;

	return 0;
}

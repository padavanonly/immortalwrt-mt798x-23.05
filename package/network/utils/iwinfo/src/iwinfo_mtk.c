#include <ctype.h>
#include <inttypes.h>
#include "iwinfo.h"
#include "iwinfo_wext.h"
#include "mtwifi.h"

#include "iwinfo_mtk_ccode.c"

static int mtk_get_freqlist(const char *dev, char *buf, int *len);
static int mtk_get_assoclist(const char *dev, char *buf, int *len);
static int mtk_get_assoc_bitrate(const char *dev, int *buf);

static inline int mtk_ioctl(const char *ifname, int cmd, struct iwreq *wrq)
{
	strncpy(wrq->ifr_name, ifname, IFNAMSIZ);
	return iwinfo_ioctl(cmd, wrq);
}

static int mtk_is_ifname(const char *devname)
{
	const char *suffix;

	if (!strncmp(devname, "apcli", 5))
		suffix = devname + 5;
	else if (!strncmp(devname, "ra", 2) &&
		 strncmp(devname, "radio", 5))
		suffix = devname + 2;
	else
		return 0;

	if (*suffix == 'i' || *suffix == 'x' || *suffix == 'e')
		suffix++;

	if (!isdigit((unsigned char)*suffix))
		return 0;

	while (isdigit((unsigned char)*suffix))
		suffix++;

	return *suffix == '\0';
}

static int mtk_dev2phy(const char *devname, char ifname[IFNAMSIZ])
{
	struct uci_section *s;
	const char *phy = NULL;
	int ret = -1;

	if (mtk_is_ifname(devname)) {
		if (strlen(devname) >= IFNAMSIZ)
			return -1;

		strncpy(ifname, devname, IFNAMSIZ - 1);
		ifname[IFNAMSIZ - 1] = '\0';
		return 0;
	}

	s = iwinfo_uci_get_radio(devname, "mtwifi");
	if (!s)
		goto out;

	phy = uci_lookup_option_string(uci_ctx, s, "phy");

	if (phy && *phy && strlen(phy) < IFNAMSIZ) {
		strncpy(ifname, phy, IFNAMSIZ - 1);
		ifname[IFNAMSIZ - 1] = '\0';
		ret = 0;
	}

out:
	iwinfo_uci_free();
	return ret;
}

static int mtk_probe(const char *dev)
{
	char ifname[IFNAMSIZ];

	return !mtk_dev2phy(dev, ifname);
}

static void mtk_close(void)
{
	iwinfo_close();
}

static int mtk_is_ifup(const char *ifname)
{
	struct ifreq ifr = {};

	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
	ifr.ifr_name[IFNAMSIZ - 1] = '\0';

	if (iwinfo_ioctl(SIOCGIFFLAGS, &ifr) >= 0)
	{
		if (ifr.ifr_flags & IFF_UP)
			return 1;
	}

	return 0;
}

static int mtk_get_band_from_uci(const char *dev, int *buf)
{
	struct uci_section *s;
	const char *band = NULL;
	int ret = -1;

	s = iwinfo_uci_get_radio(dev, "mtwifi");
	if (!s)
		goto out;

	band = uci_lookup_option_string(uci_ctx, s, "band");
	if (!band)
		goto out;

	if (!strcmp(band, "2g"))
		*buf = MTK_CH_BAND_24G;
	else if (!strcmp(band, "5g"))
		*buf = MTK_CH_BAND_5G;
	else if (!strcmp(band, "6g"))
		*buf = MTK_CH_BAND_6G;
	else
		goto out;

	ret = 0;

out:
	iwinfo_uci_free();
	return ret;
}

static int mtk_get_band(const char *dev, int *buf)
{
	char ifname[IFNAMSIZ];
	struct iwreq wrq = {};
	unsigned char band = 0;

	if (!mtk_get_band_from_uci(dev, buf))
		return 0;

	if (mtk_dev2phy(dev, ifname))
		return -1;

	if (!mtk_is_ifup(ifname))
		return -1;

	wrq.u.data.length = sizeof(band);
	wrq.u.data.pointer = &band;
	wrq.u.data.flags = OID_GET_WIRELESS_BAND;

	if (mtk_ioctl(ifname, RT_PRIV_IOCTL, &wrq) >= 0 &&
	    band <= MTK_CH_BAND_6G) {
		*buf = band;
		return 0;
	}

	return -1;
}

static int mtk_get_phy_mode(const char *dev, unsigned long *buf)
{
	char ifname[IFNAMSIZ];
	struct iwreq wrq = {};

	if (mtk_dev2phy(dev, ifname))
		return -1;

	if (!mtk_is_ifup(ifname))
		return -1;

	wrq.u.data.length = sizeof(*buf);
	wrq.u.data.pointer = buf;
	wrq.u.data.flags = RT_OID_802_11_PHY_MODE;

	if (mtk_ioctl(ifname, RT_PRIV_IOCTL, &wrq) >= 0)
		return 0;

	return -1;
}

static int mtk_band_from_frequency(int mhz)
{
	if (mhz >= 5925 && mhz <= 7125)
		return MTK_CH_BAND_6G;
	if (mhz >= 4910 && mhz < 5925)
		return MTK_CH_BAND_5G;
	if (mhz >= 2400 && mhz < 2500)
		return MTK_CH_BAND_24G;

	return -1;
}

static int mtk_channel_to_frequency(int channel, int band)
{
	if (channel <= 0)
		return -1;

	switch (band) {
	case MTK_CH_BAND_24G:
		return (channel == 14) ? 2484 : 2407 + channel * 5;

	case MTK_CH_BAND_5G:
		if (channel >= 182 && channel <= 196)
			return 4000 + channel * 5;
		return 5000 + channel * 5;

	case MTK_CH_BAND_6G:
		return (channel == 2) ? 5935 : 5950 + channel * 5;

	default:
		if (channel <= 13)
			return 2407 + channel * 5;
		if (channel == 14)
			return 2484;
		if (channel >= 182 && channel <= 196)
			return 4000 + channel * 5;
		return 5000 + channel * 5;
	}
}

static int mtk_iwinfo_band(int band)
{
	switch (band) {
	case MTK_CH_BAND_24G:
		return IWINFO_BAND_24;

	case MTK_CH_BAND_5G:
		return IWINFO_BAND_5;

	case MTK_CH_BAND_6G:
		return IWINFO_BAND_6;

	default:
		return 0;
	}
}

static int mtk_band_supports_n(int band, unsigned long wmode)
{
	switch (band) {
	case MTK_CH_BAND_24G:
		return !!(wmode & WMODE_GN);

	case MTK_CH_BAND_5G:
		return !!(wmode & WMODE_AN);

	default:
		return 0;
	}
}

static int mtk_band_supports_ax(int band, unsigned long wmode)
{
	switch (band) {
	case MTK_CH_BAND_24G:
		return !!(wmode & WMODE_AX_24G);

	case MTK_CH_BAND_5G:
		return !!(wmode & WMODE_AX_5G);

	case MTK_CH_BAND_6G:
		return !!(wmode & WMODE_AX_6G);

	default:
		return 0;
	}
}

static int mtk_band_supports_be(int band, unsigned long wmode)
{
	switch (band) {
	case MTK_CH_BAND_24G:
		return !!(wmode & WMODE_BE_24G);

	case MTK_CH_BAND_5G:
		return !!(wmode & WMODE_BE_5G);

	case MTK_CH_BAND_6G:
		return !!(wmode & WMODE_BE_6G);

	default:
		return 0;
	}
}

static void mtk_set_hwmodelist_for_band(int band, unsigned long wmode, int *buf)
{
	*buf = 0;

	switch (band) {
	case MTK_CH_BAND_24G:
		*buf = (IWINFO_80211_B | IWINFO_80211_G);
		break;

	case MTK_CH_BAND_6G:
		break;

	case MTK_CH_BAND_5G:
	default:
		*buf = IWINFO_80211_A;
		break;
	}

	if (!wmode) {
		switch (band) {
		case MTK_CH_BAND_24G:
			*buf |= (IWINFO_80211_N | IWINFO_80211_AX |
				 IWINFO_80211_BE);
			break;

		case MTK_CH_BAND_6G:
			*buf |= (IWINFO_80211_AX | IWINFO_80211_BE);
			break;

		case MTK_CH_BAND_5G:
		default:
			*buf |= (IWINFO_80211_N | IWINFO_80211_AC |
				 IWINFO_80211_AX | IWINFO_80211_BE);
			break;
		}

		return;
	}

	if (mtk_band_supports_n(band, wmode))
		*buf |= IWINFO_80211_N;

	if (band != MTK_CH_BAND_6G && WMODE_CAP_AC(wmode))
		*buf |= IWINFO_80211_AC;

	if (mtk_band_supports_ax(band, wmode))
		*buf |= IWINFO_80211_AX;

	if (mtk_band_supports_be(band, wmode))
		*buf |= IWINFO_80211_BE;

	if (!*buf) {
		switch (band) {
		case MTK_CH_BAND_24G:
			*buf = (IWINFO_80211_B | IWINFO_80211_G);
			break;

		case MTK_CH_BAND_5G:
			*buf = IWINFO_80211_A;
			break;

		default:
			break;
		}
	}

	if (band == MTK_CH_BAND_6G)
		*buf &= ~(IWINFO_80211_A | IWINFO_80211_B |
			  IWINFO_80211_G | IWINFO_80211_N |
			  IWINFO_80211_AC);
	else if (band == MTK_CH_BAND_24G)
		*buf &= ~IWINFO_80211_A;
}

static void mtk_set_htmodelist_for_band(int band, unsigned long wmode,
					int *buf)
{
	*buf = 0;

	if (!wmode) {
		switch (band) {
		case MTK_CH_BAND_24G:
			*buf = (IWINFO_HTMODE_HT20 | IWINFO_HTMODE_HT40 |
				IWINFO_HTMODE_HE20 | IWINFO_HTMODE_HE40 |
				IWINFO_HTMODE_EHT20 | IWINFO_HTMODE_EHT40);
			break;

		case MTK_CH_BAND_6G:
			*buf = (IWINFO_HTMODE_HE20 | IWINFO_HTMODE_HE40 |
				IWINFO_HTMODE_HE80 | IWINFO_HTMODE_HE80_80 |
				IWINFO_HTMODE_HE160 |
				IWINFO_HTMODE_EHT20 | IWINFO_HTMODE_EHT40 |
				IWINFO_HTMODE_EHT80 | IWINFO_HTMODE_EHT80_80 |
				IWINFO_HTMODE_EHT160 | IWINFO_HTMODE_EHT320);
			break;

		case MTK_CH_BAND_5G:
		default:
			*buf = (IWINFO_HTMODE_HT20 | IWINFO_HTMODE_HT40 |
				IWINFO_HTMODE_VHT20 | IWINFO_HTMODE_VHT40 |
				IWINFO_HTMODE_VHT80 | IWINFO_HTMODE_VHT80_80 |
				IWINFO_HTMODE_VHT160 |
				IWINFO_HTMODE_HE20 | IWINFO_HTMODE_HE40 |
				IWINFO_HTMODE_HE80 | IWINFO_HTMODE_HE80_80 |
				IWINFO_HTMODE_HE160 |
				IWINFO_HTMODE_EHT20 | IWINFO_HTMODE_EHT40 |
				IWINFO_HTMODE_EHT80 | IWINFO_HTMODE_EHT80_80 |
				IWINFO_HTMODE_EHT160);
			break;
		}

		return;
	}

	if (mtk_band_supports_n(band, wmode))
		*buf |= (IWINFO_HTMODE_HT20 | IWINFO_HTMODE_HT40);

	if (band != MTK_CH_BAND_6G && WMODE_CAP_AC(wmode)) {
		*buf |= (IWINFO_HTMODE_VHT20 | IWINFO_HTMODE_VHT40);

		if (band == MTK_CH_BAND_5G)
			*buf |= (IWINFO_HTMODE_VHT80 |
				 IWINFO_HTMODE_VHT80_80 |
				 IWINFO_HTMODE_VHT160);
	}

	if (mtk_band_supports_ax(band, wmode)) {
		*buf |= (IWINFO_HTMODE_HE20 | IWINFO_HTMODE_HE40);

		if (band != MTK_CH_BAND_24G)
			*buf |= (IWINFO_HTMODE_HE80 |
				 IWINFO_HTMODE_HE80_80 |
				 IWINFO_HTMODE_HE160);
	}

	if (mtk_band_supports_be(band, wmode)) {
		*buf |= (IWINFO_HTMODE_EHT20 | IWINFO_HTMODE_EHT40);

		if (band != MTK_CH_BAND_24G)
			*buf |= (IWINFO_HTMODE_EHT80 |
				 IWINFO_HTMODE_EHT80_80 |
				 IWINFO_HTMODE_EHT160);

		if (band == MTK_CH_BAND_6G)
			*buf |= IWINFO_HTMODE_EHT320;
	}

	if (!*buf) {
		switch (band) {
		case MTK_CH_BAND_24G:
		case MTK_CH_BAND_5G:
			*buf = (IWINFO_HTMODE_HT20 | IWINFO_HTMODE_HT40);
			break;

		default:
			break;
		}
	}
}

static int mtk_get_mode(const char *dev, int *buf)
{
	struct iwreq wrq;
	char ifname[IFNAMSIZ];

	if (mtk_dev2phy(dev, ifname))
		return -1;

	if(mtk_ioctl(ifname, SIOCGIWMODE, &wrq) >= 0)
	{
		switch(wrq.u.mode)
		{
			case 1:
				*buf = IWINFO_OPMODE_ADHOC;
				break;

			case 2:
				*buf = IWINFO_OPMODE_CLIENT;
				break;

			case 3:
				*buf = IWINFO_OPMODE_MASTER;
				break;

			case 6:
				*buf = IWINFO_OPMODE_MONITOR;
				break;

			default:
				*buf = IWINFO_OPMODE_UNKNOWN;
				break;
		}

		return 0;
	}

	return -1;
}

static int mtk_get_ssid(const char *dev, char *buf)
{
	struct iwreq wrq = {};
	char ifname[IFNAMSIZ];

	if (mtk_dev2phy(dev, ifname))
		return -1;

	wrq.u.essid.pointer = buf;
	wrq.u.essid.length  = IW_ESSID_MAX_SIZE;

	if(mtk_ioctl(ifname, SIOCGIWESSID, &wrq) >= 0)
		return 0;

	return -1;
}

static int mtk_get_bssid(const char *dev, char *buf)
{
	struct iwreq wrq;
	char ifname[IFNAMSIZ];

	if (mtk_dev2phy(dev, ifname))
		return -1;

	if(mtk_ioctl(ifname, SIOCGIWAP, &wrq) >= 0)
	{
		sprintf(buf, "%02X:%02X:%02X:%02X:%02X:%02X",
			(uint8_t)wrq.u.ap_addr.sa_data[0], (uint8_t)wrq.u.ap_addr.sa_data[1],
			(uint8_t)wrq.u.ap_addr.sa_data[2], (uint8_t)wrq.u.ap_addr.sa_data[3],
			(uint8_t)wrq.u.ap_addr.sa_data[4], (uint8_t)wrq.u.ap_addr.sa_data[5]);

		return 0;
	}

	return -1;
}

static int mtk_is_eht320(const char *dev)
{
	struct uci_section *s;
	const char *htmode = NULL;
	int ret = 0;

	s = iwinfo_uci_get_radio(dev, "mtwifi");
	if (!s)
		goto out;

	htmode = uci_lookup_option_string(uci_ctx, s, "htmode");
	if (htmode && !strcmp(htmode, "EHT320"))
		ret = 1;

out:
	iwinfo_uci_free();
	return ret;
}

static int mtk_get_bitrate(const char *dev, int *buf)
{
	struct iwreq wrq;
	char ifname[IFNAMSIZ];
	uint64_t bitrate;
	int eht320;

	if (!mtk_get_assoc_bitrate(dev, buf))
		return 0;

	if (mtk_dev2phy(dev, ifname))
		return -1;

	if(mtk_ioctl(ifname, SIOCGIWRATE, &wrq) >= 0)
	{
		bitrate = wrq.u.bitrate.value;
		eht320 = mtk_is_eht320(dev);

		if (eht320 &&
		    bitrate >= 500000000ULL && bitrate <= 530000000ULL)
			bitrate = 8647000000ULL;

		*buf = (bitrate / 1000);
		return 0;
	}

	return -1;
}

static int mtk_get_channel(const char *dev, int *buf)
{
	struct iwreq wrq;
	char ifname[IFNAMSIZ];

	if (mtk_dev2phy(dev, ifname))
		return -1;

	if (mtk_ioctl(ifname, SIOCGIWFREQ, &wrq) >= 0)
	{
		*buf = wrq.u.freq.m;
		return 0;
	}

	return -1;
}

static int mtk_get_center_chan1(const char *dev, int *buf)
{
	/* Not Supported */
	return -1;
}

static int mtk_get_center_chan2(const char *dev, int *buf)
{
	/* Not Supported */
	return -1;
}

static int mtk_get_frequency(const char *dev, int *buf)
{
	char chans[IWINFO_BUFSIZE] = { 0 };
	struct iwinfo_freqlist_entry *entry;
	int channel;
	int band;
	int len = 0;
	size_t offset;
	struct iwreq wrq;
	char ifname[IFNAMSIZ];

	if (mtk_dev2phy(dev, ifname))
		return -1;

	if (mtk_ioctl(ifname, SIOCGIWFREQ, &wrq) >= 0)
	{
		channel = wrq.u.freq.m;

		if (channel <= 0)
			return -1;

		if (!mtk_get_band(dev, &band)) {
			*buf = mtk_channel_to_frequency(channel, band);
			return (*buf > 0) ? 0 : -1;
		}

		if (!mtk_get_freqlist(dev, chans, &len)) {
			for (offset = 0;
			     offset + sizeof(*entry) <= (size_t)len;
			     offset += sizeof(*entry)) {
				entry = (struct iwinfo_freqlist_entry *)
					&chans[offset];

				if (entry->channel == channel) {
					*buf = entry->mhz;
					return 0;
				}
			}
		}

		*buf = mtk_channel_to_frequency(channel, -1);
		return (*buf > 0) ? 0 : -1;
	}

	return -1;
}

static int mtk_get_txpower(const char *dev, int *buf)
{
	struct iwreq wrq;
	char ifname[IFNAMSIZ];

	if (mtk_dev2phy(dev, ifname))
		return -1;

	wrq.u.txpower.flags = 0;

	if(mtk_ioctl(ifname, SIOCGIWTXPOW, &wrq) >= 0)
	{
		*buf = wrq.u.txpower.value;
		return 0;
	}

	return -1;
}

static int mtk_get_signal(const char *dev, int *buf)
{
	return mtk_get_txpower(dev, buf);
}

static int mtk_get_noise(const char *dev, int *buf)
{
	return -1;
}

static int mtk_get_quality(const char *dev, int *buf)
{
	*buf = 100;
	return 0;
}

static int mtk_get_quality_max(const char *dev, int *buf)
{
	*buf = 100;
	return 0;
}

static void fill_rate_info(HTTRANSMIT_SETTING HTSetting, struct iwinfo_rate_entry *re,
	unsigned int mcs, unsigned int nss)
{
	unsigned long DataRate = 0;
	uint8_t mode = HTSetting.field.MODE;
	uint8_t bw = HTSetting.field.BW;
	uint8_t short_gi = HTSetting.field.ShortGI;
	uint8_t dcm = HTSetting.field.MCS & 0x10 ? 1 : 0;

	re->is_eht = 0;
	re->is_he = 0;
	re->is_vht = 0;
	re->is_ht = 0;

	if (mode >= MODE_HTMIX && mode <= MODE_VHT)
	{
		if (short_gi)
			re->is_short_gi = 1;
	}

	if (mode >= MODE_HE && mode < MODE_UNKNOWN) {
		if (mode == MODE_EHT || mode == MODE_EHT_ER_SU || mode == MODE_EHT_TB || mode == MODE_EHT_MU) {
			re->is_eht = 1;
			re->eht_gi = short_gi;
			re->eht_dcm = dcm;
		} else {
			re->is_he = 1;
			re->he_gi = short_gi;
			re->he_dcm = dcm;
		}
	} else if (mode == MODE_VHT) {
		re->is_vht = 1;
	} else if (mode >= MODE_HTMIX && mode <= MODE_HTGREENFIELD) {
		re->is_ht = 1;
	}

	switch (bw) {
		case BW_20:
			re->mhz = 20;
			break;
		case BW_40:
			re->mhz = 40;
			break;
		case BW_80:
			re->mhz = 80;
			break;
		case BW_160:
			re->mhz = 160;
			break;
		case BW_320:
			re->mhz_hi = 320 / 256, re->mhz = 320 % 256;
			break;
		default:
			re->mhz = 20;
			break;
	}

	re->is_40mhz = (re->mhz == 40);

	if (mode >= MODE_HE && mode < MODE_UNKNOWN) {
		if (mode == MODE_EHT || mode == MODE_EHT_ER_SU || mode == MODE_EHT_TB || mode == MODE_EHT_MU) {
			get_rate_eht((mcs & 0xf), bw, nss, 0, &DataRate);
		} else {
			get_rate_he((mcs & 0xf), bw, nss, 0, &DataRate);
		}
		if (short_gi == 1)
			DataRate = (DataRate * 967) >> 10;
		else if (short_gi == 2)
			DataRate = (DataRate * 870) >> 10;
	} else {
		getRate(HTSetting, &DataRate);
	}
	re->rate = (uint32_t)(DataRate * 1000);
}

static void mtk_parse_rateinfo(RT_802_11_MAC_ENTRY *pe, 
	struct iwinfo_rate_entry *rx_rate, struct iwinfo_rate_entry *tx_rate)
{
	HTTRANSMIT_SETTING TxRate;
	HTTRANSMIT_SETTING RxRate;

	unsigned int mcs = 0, mcs_r = 0;
	unsigned int nss = 0, nss_r = 0;

	memset(rx_rate, 0, sizeof(struct iwinfo_rate_entry));
	memset(tx_rate, 0, sizeof(struct iwinfo_rate_entry));

	TxRate.word = pe->TxRate.word;
	RxRate.word = pe->LastRxRate.word;

	if (TxRate.field.MODE != MODE_UNKNOWN) {
		mcs = TxRate.field.MCS;

		if (TxRate.field.MODE >= MODE_VHT) {
			nss = ((mcs & (0x3 << 4)) >> 4) + 1;
			mcs = mcs & 0xF;
			tx_rate->nss = nss;
		} else {
			mcs = mcs & 0x3f;
			tx_rate->nss = 1;
		}

		tx_rate->mcs = mcs;
		fill_rate_info(TxRate, tx_rate, mcs, nss);
	}

	if (RxRate.field.MODE != MODE_UNKNOWN) {
		mcs_r = RxRate.field.MCS;

		if (RxRate.field.MODE >= MODE_VHT) {
			nss_r = (((mcs_r & (0x3 << 4)) >> 4) + 1) /
				(RxRate.field.STBC + 1);
			mcs_r = mcs_r & 0xF;
			rx_rate->nss = nss_r;
		} else {
			rx_rate->nss = 1;
			if (RxRate.field.MODE >= MODE_HTMIX) {
				mcs_r = mcs_r & 0x3f;
			} else if (RxRate.field.MODE == MODE_OFDM) {
				mcs_r = mcs_r & 0xf;
				RxRate.field.MCS = mcs_r;
			} else if (RxRate.field.MODE == MODE_CCK) {
				mcs_r = cck_to_mcs(mcs_r & 0x7);
				RxRate.field.MCS = mcs_r;
			}
		}

		rx_rate->mcs = mcs_r;
		fill_rate_info(RxRate, rx_rate, mcs_r, nss_r);
	}
}

static RT_802_11_MAC_TABLE *mtk_get_mac_table(const char *dev)
{
	struct iwreq wrq = {};
	RT_802_11_MAC_TABLE *table;
	char ifname[IFNAMSIZ];

	if (mtk_dev2phy(dev, ifname))
		return NULL;

	table = calloc(1, sizeof(RT_802_11_MAC_TABLE));
	if (!table)
		return NULL;

	wrq.u.data.pointer = (caddr_t)table;
	wrq.u.data.length  = sizeof(RT_802_11_MAC_TABLE);

	if (mtk_ioctl(ifname, RTPRIV_IOCTL_GET_MAC_TABLE_STRUCT, &wrq) < 0) {
		free(table);
		return NULL;
	}

	return table;
}

static size_t mtk_mac_table_count(const RT_802_11_MAC_TABLE *table)
{
	if (table->Num > MAX_NUMBER_OF_MAC)
		return MAX_NUMBER_OF_MAC;

	return table->Num;
}

static int mtk_get_assoclist(const char *dev, char *buf, int *len)
{
	RT_802_11_MAC_TABLE *table;
	size_t count;
	size_t max_entries;
	size_t i;

	table = mtk_get_mac_table(dev);
	if (!table)
		return -1;

	count = mtk_mac_table_count(table);
	max_entries = IWINFO_BUFSIZE / sizeof(struct iwinfo_assoclist_entry);
	if (count > max_entries)
		count = max_entries;

	for (i = 0; i < count; i++) {
		RT_802_11_MAC_ENTRY *pe = &(table->Entry[i]);
		struct iwinfo_assoclist_entry *e = (struct iwinfo_assoclist_entry *)buf + i;

		memcpy(e->mac, pe->Addr, 6);
		e->signal = pe->AvgRssi0;
		e->signal_avg = pe->AvgRssi0;
		e->noise = pe->AvgRssi0 - pe->AvgSnr;
		e->inactive = pe->InactiveTime;
		e->connected_time = pe->ConnectedTime;
		e->rx_packets = pe->RxPackets;
		e->tx_packets = pe->TxPackets;
		e->rx_bytes = pe->RxBytes;
		e->tx_bytes = pe->TxBytes;
		mtk_parse_rateinfo(pe, &e->rx_rate, &e->tx_rate);
	}

	*len = count * sizeof(struct iwinfo_assoclist_entry);
	free(table);
	return 0;
}

static int mtk_get_assoc_bitrate(const char *dev, int *buf)
{
	RT_802_11_MAC_TABLE *table;
	struct iwinfo_rate_entry rx_rate;
	struct iwinfo_rate_entry tx_rate;
	uint32_t best = 0;
	size_t count;
	size_t i;

	table = mtk_get_mac_table(dev);
	if (!table)
		return -1;

	count = mtk_mac_table_count(table);
	for (i = 0; i < count; i++) {
		mtk_parse_rateinfo(&table->Entry[i], &rx_rate, &tx_rate);

		if (tx_rate.rate > best)
			best = tx_rate.rate;
		if (rx_rate.rate > best)
			best = rx_rate.rate;
	}

	free(table);

	if (!best)
		return -1;

	*buf = best;
	return 0;
}

static int mtk_get_txpwrlist(const char *dev, char *buf, int *len)
{
	return -1;
}

static int mtk_get_scanlist_dump(const char *ifname, int index, char *data, size_t len)
{
	struct iwreq wrq = {};

	snprintf(data, len, "%d", index);

	wrq.u.data.pointer = data;
	wrq.u.data.length = len;
	wrq.u.data.flags = GET_MAC_TABLE_STRUCT_FLAG_RAW_SSID;

	return mtk_ioctl(ifname, RTPRIV_IOCTL_GSITESURVEY, &wrq);
}

enum {
	SCAN_DATA_CH,
	SCAN_DATA_SSID,
	SCAN_DATA_BSSID,
	SCAN_DATA_SECURITY,
	SCAN_DATA_RSSI,
	SCAN_DATA_SIG,
	SCAN_DATA_NT,
	SCAN_DATA_SSID_LEN,
	SCAN_DATA_MAX
};

static int mtk_get_scanlist(const char *dev, char *buf, int *len)
{
	struct iwinfo_scanlist_entry *e = (struct iwinfo_scanlist_entry *)buf;
	char *data = NULL;
	unsigned int data_len = 5000;
	int offsets[SCAN_DATA_MAX];
	char cmd[128];
	int index = 0;
	int total = -1;
	char *pos;
	char ifname[IFNAMSIZ];

	if (mtk_dev2phy(dev, ifname))
		return -1;

	*len = 0;

	if ((data = (char *)malloc(data_len)) == NULL)
		return -1;

	sprintf(cmd, "iwpriv %s set SiteSurvey=", ifname);
	system(cmd);

	sleep(5);

	while (1) {
		memset(data, 0, data_len);
		if (mtk_get_scanlist_dump(ifname, index, data, sizeof(data))) {
			free(data);
			return -1;
		}

		//printf("%s\n", data);

		sscanf(data, "\nTotal=%d", &total);

		strtok(data, "\n");
		pos = strtok(NULL, "\n");

		offsets[SCAN_DATA_CH] = strstr(pos, "Ch ") - pos;
		offsets[SCAN_DATA_SSID] = strstr(pos, "SSID ") - pos;
		offsets[SCAN_DATA_BSSID] = strstr(pos, "BSSID ") - pos;
		offsets[SCAN_DATA_SECURITY] = strstr(pos, "Security ") - pos;
		offsets[SCAN_DATA_RSSI] = strstr(pos, "Rssi") - pos;
		offsets[SCAN_DATA_SIG] = strstr(pos, "Siganl") - pos;
		offsets[SCAN_DATA_NT] = strstr(pos, "NT") - pos;
		offsets[SCAN_DATA_SSID_LEN] = strstr(pos, "SSID_Len") - pos;

		while (1) {
			struct iwinfo_crypto_entry *crypto = &e->crypto;
			const char *security;
			uint8_t *mac = e->mac;
			int ssid_len;

			pos = strtok(NULL, "\n");
			if (!pos)
				break;

			sscanf(pos, "%d", &index);

			if (strncmp(pos + offsets[SCAN_DATA_NT], "In", 2))
				continue;

			security = pos + offsets[SCAN_DATA_SECURITY];
			if (!strstr(security, "PSK") && !strstr(security, "OPEN") && !strstr(security, "OWE"))
				continue;

			memset(crypto, 0, sizeof(struct iwinfo_crypto_entry));

			if (strstr(security, "PSK") || strstr(security, "OWE")) {
				crypto->enabled = true;

				if (strstr(security, "WPAPSK")) {
					crypto->wpa_version |= 1 << 0;
					crypto->auth_suites |= IWINFO_KMGMT_PSK;
				}

				if (strstr(security, "WPA2PSK")) {
					crypto->wpa_version |= 1 << 1;
					crypto->auth_suites |= IWINFO_KMGMT_PSK;
				}

				if (strstr(security, "WPA3PSK")) {
					crypto->wpa_version |= 1 << 2;
					crypto->auth_suites |= IWINFO_KMGMT_SAE;
				}

				if (strstr(security, "OWE")) {
					crypto->wpa_version |= 1 << 2;
					crypto->auth_suites |= IWINFO_KMGMT_OWE;
				}

				if (strstr(security, "AES")) {
					crypto->pair_ciphers |= IWINFO_CIPHER_CCMP;
				}

				if (strstr(security, "CCMP256")) {
					crypto->pair_ciphers |= IWINFO_CIPHER_CCMP256;
				}

				if (strstr(security, "TKIP")) {
					crypto->pair_ciphers |= IWINFO_CIPHER_TKIP;
				}

				if (strstr(security, "GCMP128")) {
					crypto->pair_ciphers |= IWINFO_CIPHER_GCMP;
				}

				if (strstr(security, "GCMP256")) {
					crypto->pair_ciphers |= IWINFO_CIPHER_GCMP256;
				}
			}

			e->mode = IWINFO_OPMODE_MASTER;

			sscanf(pos + offsets[SCAN_DATA_CH], "%"SCNu8, &e->channel);
			sscanf(pos + offsets[SCAN_DATA_RSSI], "%"SCNu8, &e->signal);
			sscanf(pos + offsets[SCAN_DATA_SIG], "%"SCNu8, &e->quality);
			e->quality_max = 100;

			sscanf(pos + offsets[SCAN_DATA_BSSID], "%02"SCNx8":%02"SCNx8":%02"SCNx8":%02"SCNx8":%02"SCNx8":%02"SCNx8"",
				mac + 0, mac + 1, mac + 2, mac + 3, mac + 4, mac + 5);

			sscanf(pos + offsets[SCAN_DATA_SSID_LEN], "%d", &ssid_len);
			memcpy(e->ssid, pos + offsets[SCAN_DATA_SSID], ssid_len);

			*len += sizeof(struct iwinfo_scanlist_entry);
			e++;

			if (index + 1 == total)
				break;
		}

		if (index + 1 == total)
			break;
		else
			index++;
	}

	free(data);
	return 0;
}

static int mtk_get_freqlist(const char *dev, char *buf, int *len)
{
	int device_band = -1;
	struct iwreq wrq;
	struct iw_range range;
	struct iwinfo_freqlist_entry entry;
	char ifname[IFNAMSIZ];
	int band;
	int i, bl;

	if (mtk_dev2phy(dev, ifname))
		return -1;

	if (!mtk_is_ifup(ifname))
		return -1;

	wrq.u.data.pointer = (caddr_t) &range;
	wrq.u.data.length  = sizeof(struct iw_range);
	wrq.u.data.flags   = 0;

	if (mtk_ioctl(ifname, SIOCGIWRANGE, &wrq) >= 0)
	{
		bl = 0;
		mtk_get_band(dev, &device_band);

		for (i = 0; i < range.num_frequency; i++)
		{
			memset(&entry, 0, sizeof(entry));
			entry.mhz        = range.freq[i].m;
			entry.channel    = range.freq[i].i;
			entry.restricted = 0;

			if (device_band >= 0)
				band = mtk_iwinfo_band(device_band);
			else
				band = mtk_iwinfo_band(
					mtk_band_from_frequency(entry.mhz));

			if (band)
				entry.band = band;

			memcpy(&buf[bl], &entry, sizeof(struct iwinfo_freqlist_entry));
			bl += sizeof(struct iwinfo_freqlist_entry);
		}

		*len = bl;
		return 0;
	}

	return -1;
}

static int mtk_get_country(const char *dev, char *buf)
{
	char ifname[IFNAMSIZ];
	char data[4] = {0};
	struct iwreq wrq;

	if (mtk_dev2phy(dev, ifname))
		return -1;

	wrq.u.data.length = sizeof(data);
	wrq.u.data.pointer = &data;
	wrq.u.data.flags = OID_802_11_COUNTRYCODE;

	if (mtk_ioctl(ifname, RT_PRIV_IOCTL, &wrq) >= 0)
	{
		memcpy(buf, data, 2);
		return 0;
	}
	return -1;
}

static int mtk_get_countrylist(const char *dev, char *buf, int *len)
{
	int count = sizeof(mtk_country_codes)/sizeof(mtk_country_codes[0]);
	struct iwinfo_country_entry *c = (struct iwinfo_country_entry *)buf;

	for (int i=0; i<count; i++) {
		c->iso3166 = mtk_country_codes[i][0]<<8 | mtk_country_codes[i][1];
		snprintf(c->ccode, sizeof(c->ccode), "%s", mtk_country_codes[i]);
		c++;
	}

	*len = (count * sizeof(struct iwinfo_country_entry));
	return 0;
}

static int mtk_get_hwmodelist(const char *dev, int *buf)
{
	char chans[IWINFO_BUFSIZE] = { 0 };
	struct iwinfo_freqlist_entry *e = NULL;
	int band;
	int len = 0;
	size_t offset;
	unsigned long wmode = 0;

	*buf = 0;
	mtk_get_phy_mode(dev, &wmode);

	if (!mtk_get_band(dev, &band)) {
		mtk_set_hwmodelist_for_band(band, wmode, buf);
		return 0;
	}

	if (!mtk_get_freqlist(dev, chans, &len))
	{
		for (offset = 0;
		     offset + sizeof(*e) <= (size_t)len;
		     offset += sizeof(*e)) {
			e = (struct iwinfo_freqlist_entry *)&chans[offset];
			band = mtk_band_from_frequency(e->mhz);
			if (band >= 0) {
				mtk_set_hwmodelist_for_band(band, wmode, buf);
				return 0;
			}
		}
	}

	return -1;
}

static int mtk_get_htmodelist(const char *dev, int *buf)
{
	char chans[IWINFO_BUFSIZE] = { 0 };
	struct iwinfo_freqlist_entry *e = NULL;
	int band;
	int len = 0;
	size_t offset;
	unsigned long wmode = 0;

	*buf = 0;
	mtk_get_phy_mode(dev, &wmode);

	if (!mtk_get_band(dev, &band)) {
		mtk_set_htmodelist_for_band(band, wmode, buf);
		return 0;
	}

	if (!mtk_get_freqlist(dev, chans, &len))
	{
		for (offset = 0;
		     offset + sizeof(*e) <= (size_t)len;
		     offset += sizeof(*e)) {
			e = (struct iwinfo_freqlist_entry *)&chans[offset];
			band = mtk_band_from_frequency(e->mhz);
			if (band >= 0) {
				mtk_set_htmodelist_for_band(band, wmode, buf);
				return 0;
			}
		}
	}

	return -1;
}

static int mtk_get_htmode(const char *dev, int *buf)
{
	char ifname[IFNAMSIZ];
	struct iwreq wrq;
	unsigned char bw = 0;
	unsigned long wmode = 0;

	if (mtk_dev2phy(dev, ifname))
		return -1;

	if (!mtk_is_ifup(ifname))
		return -1;

	wrq.u.data.length = sizeof(bw);
	wrq.u.data.pointer = &bw;
	wrq.u.data.flags = OID_802_11_BW;

	if (mtk_ioctl(ifname, RT_PRIV_IOCTL, &wrq) >= 0)
	{
		wrq.u.data.length = sizeof(wmode);
		wrq.u.data.pointer = &wmode;
		wrq.u.data.flags = RT_OID_802_11_PHY_MODE;

		if (mtk_ioctl(ifname, RT_PRIV_IOCTL, &wrq) >= 0)
		{
			if (WMODE_CAP_BE(wmode)) {
				switch (bw) {
					case BW_20: *buf = IWINFO_HTMODE_EHT20; break;
					case BW_40: *buf = IWINFO_HTMODE_EHT40; break;
					case BW_80: *buf = IWINFO_HTMODE_EHT80; break;
					case BW_8080: *buf = IWINFO_HTMODE_EHT80_80; break;
					case BW_160: *buf = IWINFO_HTMODE_EHT160; break;
					case BW_320: *buf = IWINFO_HTMODE_EHT320; break;
				}
			} else if (WMODE_CAP_AX(wmode)) {
				switch (bw) {
					case BW_20: *buf = IWINFO_HTMODE_HE20; break;
					case BW_40: *buf = IWINFO_HTMODE_HE40; break;
					case BW_80: *buf = IWINFO_HTMODE_HE80; break;
					case BW_8080: *buf = IWINFO_HTMODE_HE80_80; break;
					case BW_160: *buf = IWINFO_HTMODE_HE160; break;
				}
			} else if (WMODE_CAP_AC(wmode)) {
				switch (bw) {
					case BW_20: *buf = IWINFO_HTMODE_VHT20; break;
					case BW_40: *buf = IWINFO_HTMODE_VHT40; break;
					case BW_80: *buf = IWINFO_HTMODE_VHT80; break;
					case BW_8080: *buf = IWINFO_HTMODE_VHT80_80; break;
					case BW_160: *buf = IWINFO_HTMODE_VHT160; break;
				}
			} else if (WMODE_CAP_N(wmode)) {
				switch (bw) {
					case BW_20: *buf = IWINFO_HTMODE_HT20; break;
					case BW_40: *buf = IWINFO_HTMODE_HT40; break;
				}
			} else {
				*buf = IWINFO_HTMODE_NOHT;
			}
			return 0;
		}
	}

	return -1;
}

static int mtk_get_encryption(const char *dev, char *buf)
{
	char ifname[IFNAMSIZ];
	struct iwreq wrq;
	struct security_info secinfo;
	unsigned int authMode, encryMode;
	struct iwinfo_crypto_entry *c = (struct iwinfo_crypto_entry *)buf;

	if (mtk_dev2phy(dev, ifname))
		return -1;

	if (!mtk_is_ifup(ifname))
		return -1;

	wrq.u.data.length = sizeof(secinfo);
	wrq.u.data.pointer = &secinfo;
	wrq.u.data.flags = OID_802_11_SECURITY_TYPE;

	if (mtk_ioctl(ifname, RT_PRIV_IOCTL, &wrq) >= 0)
	{
		authMode = secinfo.auth_mode;
		encryMode = secinfo.encryp_type;

		c->auth_algs |= IWINFO_AUTH_OPEN;
		if (IS_AKM_SHARED(authMode))
			c->auth_algs |= IWINFO_AUTH_SHARED;			

		if (IS_AKM_WPA1(authMode) || IS_AKM_FT_WPA2(authMode) || IS_AKM_WPANONE(authMode) ||
			IS_AKM_WPA3(authMode) || IS_AKM_WPA2(authMode) || IS_AKM_WPA3_192BIT(authMode))
			c->auth_suites |= IWINFO_KMGMT_8021x;

		if (IS_AKM_WPA1PSK(authMode) || IS_AKM_WPA2PSK(authMode) || IS_AKM_FT_WPA2PSK(authMode) ||
			IS_AKM_WPA2PSK_SHA256(authMode))
			c->auth_suites |= IWINFO_KMGMT_PSK;

		if (IS_AKM_FT_SAE_SHA256(authMode) || IS_AKM_SAE_SHA256(authMode))
			c->auth_suites |= IWINFO_KMGMT_SAE;

		if (IS_AKM_OWE(authMode))
			c->auth_suites |= IWINFO_KMGMT_OWE;

		if (IS_AKM_WPA1(authMode) || IS_AKM_WPA1PSK(authMode))
			c->wpa_version |= 1 << 0;

		if (IS_AKM_WPA2(authMode) || IS_AKM_WPA2PSK(authMode) || IS_AKM_FT_WPA2(authMode) ||
			IS_AKM_FT_WPA2PSK(authMode) || IS_AKM_WPA2_SHA256(authMode) || IS_AKM_WPA2PSK_SHA256(authMode) ||
			IS_AKM_FT_WPA2_SHA384(authMode))
			c->wpa_version |= 1 << 1;

		if (IS_AKM_WPA3(authMode) || IS_AKM_WPA3PSK(authMode) ||
			IS_AKM_WPA3_192BIT(authMode) || IS_AKM_OWE(authMode))
			c->wpa_version |= 1 << 2;

		if (IS_CIPHER_NONE(encryMode))
			c->pair_ciphers |= IWINFO_CIPHER_NONE;

		if (IS_CIPHER_WEP40(encryMode))
			c->pair_ciphers |= IWINFO_CIPHER_WEP40;

		if (IS_CIPHER_WEP104(encryMode))
			c->pair_ciphers |= IWINFO_CIPHER_WEP104;

		if (IS_CIPHER_TKIP(encryMode))
			c->pair_ciphers |= IWINFO_CIPHER_TKIP;

		if (IS_CIPHER_CCMP128(encryMode))
			c->pair_ciphers |= IWINFO_CIPHER_CCMP;

		if (IS_CIPHER_GCMP128(encryMode))
			c->pair_ciphers |= IWINFO_CIPHER_GCMP;

		if (IS_CIPHER_CCMP256(encryMode))
			c->pair_ciphers |= IWINFO_CIPHER_CCMP256;

		if (IS_CIPHER_GCMP256(encryMode))
			c->pair_ciphers |= IWINFO_CIPHER_GCMP256;

		c->group_ciphers = c->pair_ciphers;

		if (IS_AKM_OPEN(authMode) && IS_CIPHER_NONE(encryMode))
			c->enabled = 0;
		else
			c->enabled = 1;

		return 0;
	}

	return -1;
}

static int mtk_get_phyname(const char *dev, char *buf)
{
	char ifname[IFNAMSIZ];

	if (mtk_dev2phy(dev, ifname))
		return -1;

	strcpy(buf, ifname);

	return 0;
}

static int mtk_get_mbssid_support(const char *dev, int *buf)
{
	*buf = 1;
	return 0;
}

static int mtk_get_l1profile_attr(const char *attr, char *data, int len)
{
	FILE *fp;
	char *key, *val, buf[512];

	fp = fopen(MTK_L1_PROFILE_PATH, "r");
	if (!fp)
		return -1;

	while (fgets(buf, sizeof(buf), fp))
	{
		key = strtok(buf, " =\n");
		val = strtok(NULL, "\n");
		
		if (!key || !val || !*key || *key == '#')
			continue;

		if (!strcmp(key, attr))
		{
			//printf("l1profile key=%s, val=%s\n", key, val);
			snprintf(data, len, "%s", val);
			fclose(fp);
			return 0;
		}
	}

	fclose(fp);
	return -1;
}

static int mtk_get_hardware_id_from_l1profile(struct iwinfo_hardware_id *id)
{
	const char *attr = "INDEX0";
	char buf[16] = {0};

	if (mtk_get_l1profile_attr(attr, buf, sizeof(buf)) < 0)
		return -1;
	
	if (!strcmp(buf, "MT7981")) {
		id->vendor_id = 0x14c3;
		id->device_id = 0x7981;
		id->subsystem_vendor_id = id->vendor_id;
		id->subsystem_device_id = id->device_id;
	} else if (!strcmp(buf, "MT7992")) {
		id->vendor_id = 0x14c3;
		id->device_id = 0x7992;
		id->subsystem_vendor_id = id->vendor_id;
		id->subsystem_device_id = id->device_id;
	} else if (!strcmp(buf, "MT7990")) {
		id->vendor_id = 0x14c3;
		id->device_id = 0x7990;
		id->subsystem_vendor_id = id->vendor_id;
		id->subsystem_device_id = id->device_id;
	} else {
		return -1;
	}

	return 0;
}

static int mtk_get_hardware_id(const char *dev, char *buf)
{
	struct iwinfo_hardware_id *id = (struct iwinfo_hardware_id *)buf;
	int ret = 0;

	memset(id, 0, sizeof(*id));

	ret = iwinfo_hardware_id_from_mtd(id);
	if (ret != 0)
		ret = mtk_get_hardware_id_from_l1profile(id);

	return ret;
}

static const struct iwinfo_hardware_entry *
mtk_get_hardware_entry(const char *dev)
{
	struct iwinfo_hardware_id id;

	if (mtk_get_hardware_id(dev, (char *)&id))
		return NULL;

	return iwinfo_hardware(&id);
}

static int mtk_get_hardware_name(const char *dev, char *buf)
{
	const struct iwinfo_hardware_entry *hw;

	if (!(hw = mtk_get_hardware_entry(dev)))
		sprintf(buf, "%s", "MediaTek");
	else
		sprintf(buf, "%s %s", hw->vendor_name, hw->device_name);

	return 0;
}

static int mtk_get_txpower_offset(const char *dev, int *buf)
{
	/* Stub */
	*buf = 0;
	return -1;
}

static int mtk_get_frequency_offset(const char *dev, int *buf)
{
	/* Stub */
	*buf = 0;
	return -1;
}

const struct iwinfo_ops mtk_ops = {
	.name             = "mtk",
	.probe            = mtk_probe,
	.channel          = mtk_get_channel,
	.center_chan1     = mtk_get_center_chan1,
	.center_chan2     = mtk_get_center_chan2,
	.frequency        = mtk_get_frequency,
	.frequency_offset = mtk_get_frequency_offset,
	.txpower          = mtk_get_txpower,
	.txpower_offset   = mtk_get_txpower_offset,
	.bitrate          = mtk_get_bitrate,
	.signal           = mtk_get_signal,
	.noise            = mtk_get_noise,
	.quality          = mtk_get_quality,
	.quality_max      = mtk_get_quality_max,
	.mbssid_support   = mtk_get_mbssid_support,
	.hwmodelist       = mtk_get_hwmodelist,
	.htmodelist       = mtk_get_htmodelist,
	.htmode           = mtk_get_htmode,
	.mode             = mtk_get_mode,
	.ssid             = mtk_get_ssid,
	.bssid            = mtk_get_bssid,
	.country          = mtk_get_country,
	.countrylist      = mtk_get_countrylist,
	.hardware_id      = mtk_get_hardware_id,
	.hardware_name    = mtk_get_hardware_name,
	.encryption       = mtk_get_encryption,
	.phyname          = mtk_get_phyname,
	.assoclist        = mtk_get_assoclist,
	.txpwrlist        = mtk_get_txpwrlist,
	.scanlist         = mtk_get_scanlist,
	.freqlist         = mtk_get_freqlist,
	.close            = mtk_close,
};

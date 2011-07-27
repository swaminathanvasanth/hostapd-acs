#ifndef __ACS_H
#define __ACS_H

#include "utils/common.h"
#include "ap/hostapd.h"
#include "list.h"

#ifdef CONFIG_ACS
enum hostapd_chan_status acs_init(struct hostapd_iface *iface);
void hostapd_notify_acs_roc(struct hostapd_iface *iface,
			    unsigned int freq,
			    unsigned int duration,
			    int roc_status);
void hostapd_notify_acs_roc_cancel(struct hostapd_iface *iface,
				   unsigned int freq,
				   unsigned int duration,
				   int roc_status);
int hostapd_acs_completed(struct hostapd_iface *iface);
#else
static inline enum hostapd_chan_status acs_init(struct hostapd_iface *iface)
{
	wpa_printf(MSG_ERROR, "ACS was disabled on your build, "
		   "rebuild hostapd with CONFIG_ACS=1");
	return HOSTAPD_CHAN_INVALID;
}
static inline void hostapd_notify_acs_roc(struct hostapd_iface *iface,
					  unsigned int freq,
					  unsigned int duration,
					  int roc_status)
{
}
static inline void hostapd_notify_acs_roc_cancel(struct hostapd_iface *iface,
						 unsigned int freq,
						 unsigned int duration,
						 int roc_status)
{
}
#endif /* CONFIG_ACS */

#endif /* __ACS_H */

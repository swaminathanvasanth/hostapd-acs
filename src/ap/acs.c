/*
 * Copyright (C) 2011 Qualcomm Atheros
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

/*
 * ACS Maintainer: Luis R. Rodriguez <mcgrof@kernel.org>
 */

#include "includes.h"
#include "acs.h"
#include "drivers/driver.h"
#include "ap/ap_drv_ops.h"
#include "ap/ap_config.h"
#include "ap/hw_features.h"

/*
 * Until we get really bored and implement OS support for
 * others on os.[ch]
 */
#include <math.h>

/*
 * Automatic Channel Selection
 *
 * http://wireless.kernel.org/en/users/Documentation/acs
 *
 * This is the hostapd AP ACS implementation
 *
 * You get automatic channel selection when you configure hostapd
 * with a channel=acs_survey or channel=0 on the hostapd.conf file.
 *
 * TODO:
 *
 * - The current algorithm is heavily based on the amount of time
 *   we are willing to spend offchannel configurable via acs_roc_duration_ms,
 *   and acs_num_req_surveys, this will work for the period of time we do
 *   the analysis, so if these values are too low you'd use an ideal channel
 *   only based on the short bursts of traffic on the channel. We can also take
 *   into consideration other data to help us further make a better analysis and
 *   speed out our decision:
 *
 *	* Do a scan and count the number of BSSes on each channel:
 * 	  * Assign an HT40 primary channel a high interference aggregate value
 *	  * Assign an HT40 secondary channel a lower interference aggregate value
 *	* Use a frequency broker to collect other PHY RF interference:
 *	  * BT devices, etc, assign interference value aggregates to these
 *
 * - An ideal result would continue surveying the channels and collect a
 *   histogram, the ideal channel then will remain ideal for most of the
 *   collected history.
 *
 * - Add wpa_supplicant support for ACS, ideal for P2P
 *
 * - Randomize channel study
 *
 * - Enable the use passive scan instead of offchannel operation to enable
 *   drivers that do not support offchannel support
 *
 * - Get more drivers / firmware to implement / export survey dump
 *
 * - Any other OSes interested?
 */

static void acs_clean_chan_surveys(struct hostapd_channel_data *chan)
{
	struct freq_survey *survey, *tmp;

	if (dl_list_empty(&chan->survey_list))
		return;

	dl_list_for_each_safe(survey, tmp, &chan->survey_list, struct freq_survey, list_member) {
		dl_list_del(&survey->list_member);
		os_free(survey);
	}
}

static void acs_cleanup(struct hostapd_iface *iface)
{
	unsigned int i;
	struct hostapd_channel_data *chan;

	for (i = 0; i < iface->current_mode->num_channels; i++) {
		chan = &iface->current_mode->channels[i];

		if (chan->survey_count)
			acs_clean_chan_surveys(chan);

		dl_list_init(&chan->survey_list);
		chan->min_nf = 0;
		chan->survey_count = 0;
	}

	iface->chans_surveyed = 0;
	iface->off_channel_freq_idx = 0;
	iface->acs_num_completed_surveys = 0;
}

void acs_fail(struct hostapd_iface *iface)
{
	wpa_printf(MSG_ERROR, "ACS: failed to start");
	acs_cleanup(iface);
}

/*
 * XXX: Use libm's pow thingy? If we can implement our own log2() then
 * we can keep this here and remove libm support from hostapd, not
 * sure if its worth it.
 */
static u64 base_to_power(u64 base, u64 pow)
{
	u64 result = base;

	if (pow == 0)
		return 1;

	pow--;
	while (pow--)
		result *= base;

	return result;
}

static long double acs_survey_interference_factor(struct freq_survey *survey, s8 min_nf)
{
	long double factor;

	factor = survey->channel_time_busy - survey->channel_time_tx;
	factor /= (survey->channel_time - survey->channel_time_tx);
	factor *= (base_to_power(2, survey->nf - min_nf));
	factor = log2(factor);

	return factor;
}

static void acs_chan_interference_factor(struct hostapd_iface *iface,
					 struct hostapd_channel_data *chan)
{
	struct freq_survey *survey;
	unsigned int i = 0;
	long double int_factor = 0;

	if (dl_list_empty(&chan->survey_list) || chan->flag & HOSTAPD_CHAN_DISABLED)
		return;

	dl_list_for_each(survey, &chan->survey_list, struct freq_survey, list_member) {
		int_factor = acs_survey_interference_factor(survey, iface->lowest_nf);
		chan->survey_interference_factor += int_factor;
		wpa_printf(MSG_DEBUG, "\tsurvey_id: %d"
			   "\tchan_min_nf: %d\tsurvey_interference_factor: %Lf",
			   ++i, chan->min_nf, int_factor);
	}

	/* XXX: remove survey count */

	chan->survey_interference_factor = chan->survey_interference_factor / chan->survey_count;
}

static int acs_usable_chan(struct hostapd_channel_data *chan)
{
	if (!chan->survey_count)
		return 0;
	if (dl_list_empty(&chan->survey_list))
		return 0;
	if (chan->flag & HOSTAPD_CHAN_DISABLED)
		return 0;
	return 1;
}

/*
 * At this point its assumed we have the iface->lowest_nf
 * and all chan->min_nf values
 */
struct hostapd_channel_data *acs_find_ideal_chan(struct hostapd_iface *iface)
{
	unsigned int i;
	struct hostapd_channel_data *chan, *ideal_chan = NULL;

	for (i = 0; i < iface->current_mode->num_channels; i++) {
		chan = &iface->current_mode->channels[i];

		if (!acs_usable_chan(chan))
			continue;

		wpa_printf(MSG_DEBUG, "------------------------- "
			   "Survey analysis for channel %d (%d MHz) "
			   "--------------------------------",
			    chan->chan,
			    chan->freq);

		acs_chan_interference_factor(iface, chan);

		wpa_printf(MSG_DEBUG, "\tChannel survey interference factor average: %Lf",
			   chan->survey_interference_factor);

		if (!ideal_chan)
			ideal_chan = chan;
		else {
			if (chan->survey_interference_factor < ideal_chan->survey_interference_factor)
				ideal_chan = chan;
		}
	}

	return ideal_chan;
}

static enum hostapd_chan_status acs_study_next_freq(struct hostapd_iface *iface)
{
	int err;
	unsigned int i;
	struct hostapd_channel_data *chan;
	struct hostapd_data *hapd = iface->bss[0];

	if (iface->off_channel_freq_idx > iface->current_mode->num_channels) {
		wpa_printf(MSG_ERROR, "ACS: channel index out of bounds");
		return HOSTAPD_CHAN_INVALID;
	}

	for (i = iface->off_channel_freq_idx; i < iface->current_mode->num_channels; i++) {
		chan = &iface->current_mode->channels[i];
		if (chan->flag & HOSTAPD_CHAN_DISABLED)
			continue;

		err = hostapd_drv_remain_on_channel(hapd, chan->freq, iface->conf->acs_roc_duration_ms);
		if (err < 0) {
			wpa_printf(MSG_ERROR, "ACS: request to go offchannel "
				   "on freq %d MHz failed",
				   chan->freq);
			return HOSTAPD_CHAN_INVALID;
		}

		iface->off_channel_freq_idx = i;

		return HOSTAPD_CHAN_ACS;
	}

	if (!iface->chans_surveyed) {
		wpa_printf(MSG_ERROR, "ACS: unable to survey any channel");
		return HOSTAPD_CHAN_INVALID;
	}

	return HOSTAPD_CHAN_VALID;
}

static void acs_study_complete(struct hostapd_iface *iface)
{
	struct hostapd_channel_data *ideal_chan;

	iface->acs_num_completed_surveys++;

	if (iface->acs_num_completed_surveys < iface->conf->acs_num_req_surveys) {
		iface->off_channel_freq_idx = 0;

		switch (acs_study_next_freq(iface)) {
		case HOSTAPD_CHAN_ACS:
			return;
		case HOSTAPD_CHAN_VALID:
			/*
			 * Bullshit, we were expected to do some more bg work
			 * due to the acs_num_req_surveys.
			 */
			wpa_printf(MSG_ERROR, "ACS: odd loop bug, report this...");
		case HOSTAPD_CHAN_INVALID:
		default:
			goto fail;
		}
	}

	if (!iface->chans_surveyed) {
		wpa_printf(MSG_ERROR, "ACS: unable to collect any "
			   "useful survey data");
		goto fail;
	}

	ideal_chan = acs_find_ideal_chan(iface);
	if (!ideal_chan) {
		wpa_printf(MSG_ERROR, "ACS: although survey data was collected we "
			   "were unable to compute an ideal channel");
		goto fail;
	}

	wpa_printf(MSG_DEBUG, "-------------------------------------------------------------------------");
	wpa_printf(MSG_INFO, "ACS: Ideal chan: %d (%d MHz) Average interference factor: %Lf",
		   ideal_chan->chan,
		   ideal_chan->freq,
		   ideal_chan->survey_interference_factor);
	wpa_printf(MSG_DEBUG, "-------------------------------------------------------------------------");

	iface->conf->channel = ideal_chan->chan;
	/*
	 * Note that iface->conf->secondary_channel will be left as
	 * per your preference for enabling HT40+, HT40- or not using
	 * HT40 at all.
	 */

	/*
	 * hostapd_setup_interface_complete() will return -1 on failure,
	 * 0 on success and 0 is HOSTAPD_CHAN_VALID :)
	 */
	switch (hostapd_acs_completed(iface)) {
	case HOSTAPD_CHAN_VALID:
		acs_cleanup(iface);
		return;
	case HOSTAPD_CHAN_INVALID:
	case HOSTAPD_CHAN_ACS:
	default:
		wpa_printf(MSG_ERROR, "ACS: although things seemed "
			   "fine we failed in the end");
		goto fail;
	}

fail:
	acs_fail(iface);
}


static void acs_roc_next(struct hostapd_iface *iface,
			 unsigned int freq,
			 unsigned int duration)
{
	struct hostapd_channel_data *chan;
	struct hostapd_data *hapd = iface->bss[0];
	int err;

	chan = &iface->current_mode->channels[iface->off_channel_freq_idx];

	wpa_printf(MSG_EXCESSIVE, "ACS: offchannel on freq %d MHz", freq);

	err = hostapd_drv_survey_freq(hapd, freq);
	if (err) {
		/* XXX: figure out why we are not getting out of here */
		wpa_printf(MSG_ERROR, "ACS: failed to get any survey "
			   "data for freq %d MHz", freq);
		goto fail;
	}

	wpa_printf(MSG_EXCESSIVE, "ACS: going to next channel...");

	iface->off_channel_freq_idx++;

	switch (acs_study_next_freq(iface)) {
	case HOSTAPD_CHAN_VALID:
		acs_study_complete(iface);
		return;
	case HOSTAPD_CHAN_ACS:
		return;
	case HOSTAPD_CHAN_INVALID:
	default:
		goto fail;
	}

fail:
	acs_fail(iface);
}

void hostapd_notify_acs_roc(struct hostapd_iface *iface,
			    unsigned int freq,
			    unsigned int duration,
			    int roc_status)
{
	if (roc_status) {
		acs_fail(iface);
		return;
	}

	/* We'll get an event once completed or cancelled */
}

void hostapd_notify_acs_roc_cancel(struct hostapd_iface *iface,
				   unsigned int freq,
				   unsigned int duration,
				   int roc_status)
{
	if (roc_status) {
		acs_fail(iface);
		return;
	}

	acs_roc_next(iface, freq, duration);
}

static void acs_init_scan_complete(struct hostapd_iface *iface)
{
	wpa_printf(MSG_DEBUG, "ACS: using survey based algorithm "
		   "(acs_num_req_surveys=%d acs_roc_duration_ms=%d)",
		   iface->conf->acs_num_req_surveys,
		   iface->conf->acs_roc_duration_ms);

	acs_cleanup(iface);

	iface->acs_num_completed_surveys = 0;

	switch (acs_study_next_freq(iface)) {
	case HOSTAPD_CHAN_ACS:
		return;
	case HOSTAPD_CHAN_VALID:
	case HOSTAPD_CHAN_INVALID:
	default:
		acs_fail(iface);
	}
}

static int acs_init_scan(struct hostapd_iface *iface)
{
	struct wpa_driver_scan_params params;

	wpa_printf(MSG_DEBUG, "ACS: initial scan just to kick off the hw a bit...");
	os_memset(&params, 0, sizeof(params));

	if (hostapd_driver_scan(iface->bss[0], &params) < 0) {
		wpa_printf(MSG_ERROR, "ACS: Failed to request initial scan");
		return -1;
	}

	iface->scan_cb = acs_init_scan_complete;
	return 1;
}

static int acs_sanity_check(struct hostapd_iface *iface)
{

	/* Meh? */
	if (iface->chans_surveyed) {
		wpa_printf(MSG_ERROR, "ACS: no usable channels found");
		return -1;
        }

	if (!(iface->drv_flags & WPA_DRIVER_FLAGS_OFFCHANNEL_TX)) {
		wpa_printf(MSG_ERROR, "ACS: offchannel TX support required");
		return -1;
        }

	return 0;
}

enum hostapd_chan_status acs_init(struct hostapd_iface *iface)
{
	int err;

	wpa_printf(MSG_INFO, "ACS: automatic channel selection started, this may take a bit");

	err = acs_sanity_check(iface);
	if (err < 0)
		return HOSTAPD_CHAN_INVALID;

	err = acs_init_scan(iface);
	if (err < 0)
		return HOSTAPD_CHAN_INVALID;
	else if (err == 1)
		return HOSTAPD_CHAN_ACS;
	else
		return HOSTAPD_CHAN_INVALID;
}

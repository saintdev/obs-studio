/*
 * Copyright (C) 2014 by
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <util/bmem.h>
#include <util/platform.h>
#include <util/threading.h>
#include <obs-module.h>

#include <alsa/asoundlib.h>
#include <libavutil/common.h>

#define ALSA_DATA(voidptr) struct alsa_data *data = voidptr;
#define blog(level, msg, ...) blog(level, "alsa-input: " msg, ##__VA_ARGS__)

#define OBS_MIN(a,b) ((a)<(b)?(a):(b))
#define OBS_MAX(a,b) ((a)>(b)?(a):(b))
#define MAX_BUFFER_TIME_US 500000
#define CHECK_RETURN(msg) \
do { \
	if (ret < 0) { \
		blog(LOG_ERROR, msg ": %s", snd_strerror(ret)); \
		return ret; \
	} \
} while(0)

/* TODO:
 * Non blocking mode?
 * MMAP access?
 *     Not all drivers support MMAP.
 * Configurable sample rate/channels/etc..
 */

struct alsa_data {
	obs_source_t      *source;
	pthread_t          thread;
	os_event_t        *event;
	unsigned           num_pollfds;

	char              *device;
	snd_pcm_t         *pcm;
	snd_pcm_format_t   format;
	unsigned           channels;
	unsigned           sample_rate;
	snd_pcm_uframes_t  buffer_size;
	snd_pcm_uframes_t  period_size;
};

/**
 * get obs audio format from ALSA format
 */
static inline enum audio_format alsa_to_obs_audio_format(
	snd_pcm_format_t format)
{
	switch (format) {
	case SND_PCM_FORMAT_U8:       return AUDIO_FORMAT_U8BIT_PLANAR;
	case SND_PCM_FORMAT_S16_LE:   return AUDIO_FORMAT_16BIT_PLANAR;
	case SND_PCM_FORMAT_S32_LE:   return AUDIO_FORMAT_32BIT_PLANAR;
	case SND_PCM_FORMAT_FLOAT_LE: return AUDIO_FORMAT_FLOAT_PLANAR;
	default:                      return AUDIO_FORMAT_UNKNOWN;
	}

	return AUDIO_FORMAT_UNKNOWN;
}

static const char* alsa_getname(void)
{
	return obs_module_text("ALSA Input");
}

static void alsa_device_list(obs_property_t *prop)
{
	int ret;
	int card = -1;
	snd_ctl_card_info_t *card_info;
	snd_pcm_info_t *pcm_info;

	obs_property_list_clear(prop);

	obs_property_list_add_string(prop, "Default Audio Device", "default");

	snd_ctl_card_info_alloca(&card_info);
	snd_pcm_info_alloca(&pcm_info);

	while (1) {
		char device_str[32];
		snd_ctl_t *card_ctl;
		int device = -1;

		if ((ret = snd_card_next(&card))) {
			blog(LOG_ERROR, "Unable to get next card: %s",
			     snd_strerror(ret));
			break;
		}

		if (card < 0)
			break;

		snprintf(device_str, sizeof(device_str), "hw:%i", card);
		if ((ret = snd_ctl_open(&card_ctl, device_str, 0))) {
			blog(LOG_INFO, "Couldn't open card %i: %s",
			     card, snd_strerror(ret));
			continue;
		}

		if ((ret = snd_ctl_card_info(card_ctl, card_info))) {
			blog(LOG_INFO, "Couldn't open card CTL: %s",
				snd_strerror(ret));
			snd_ctl_close(card_ctl);
			continue;
		}

		while (1) {
			const char *pcm_name, *card_name;
			char *description;

			if ((ret = snd_ctl_pcm_next_device(card_ctl, &device))) {
				blog(LOG_DEBUG, "Unable to find next device: %s",
					snd_strerror(ret));
				break;
			}

			if (device < 0)
				break;

			snd_pcm_info_set_device(pcm_info, device);
			snd_pcm_info_set_stream(pcm_info, SND_PCM_STREAM_CAPTURE);
			if (snd_ctl_pcm_info(card_ctl, pcm_info))
				continue;

			card_name = snd_ctl_card_info_get_name(card_info);
			pcm_name = snd_pcm_info_get_name(pcm_info);
			snprintf(device_str, sizeof(device_str),
				"plughw:%i,%i", card, device);

			description = bzalloc(strlen(card_name) +
				strlen(pcm_name) + strlen(device_str) + 10);

			sprintf(description, "%s (%s, %s)",
				device_str, card_name, pcm_name);

			obs_property_list_add_string(prop,
					description,
					(void *)device_str);
			bfree(description);
		}

		snd_ctl_close(card_ctl);
	}
}

static obs_properties_t *alsa_properties(void *unused)
{
	UNUSED_PARAMETER(unused);

	obs_properties_t *props = obs_properties_create();
	obs_property_t *devices = obs_properties_add_list(props, "pcm_name",
		obs_module_text("Device"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);
	obs_properties_add_bool(props, "force_mono", obs_module_text("Force Mono"));

	alsa_device_list(devices);

	return props;
}

static void alsa_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "pcm_name", "default");
	obs_data_set_default_bool(settings, "force_mono", false);

	return;
}

static int alsa_handle_xrun(snd_pcm_t *pcm)
{
	int ret;

	switch(snd_pcm_state(pcm)) {
		case SND_PCM_STATE_SUSPENDED:
			while ((ret = snd_pcm_resume(pcm)) == -EAGAIN)
				sleep(1);
			if (ret >= 0)
				break;
		case SND_PCM_STATE_XRUN:
			ret = snd_pcm_prepare(pcm);
			CHECK_RETURN("XRUN: Error handling XRUN");
			break;
		/* arecord also handles SND_PCM_STATE_DRAINING */
		default:
			blog(LOG_ERROR, "XRUN: Unhandled state.");
			return -1;
	}
	return 0;
}

static void alsa_terminate(struct alsa_data *data)
{
	if (data->thread) {
		os_event_signal(data->event);
		pthread_join(data->thread, NULL);
		os_event_destroy(data->event);
		data->thread = 0;
	}

	if (data->pcm) {
		snd_pcm_close(data->pcm);
		data->pcm = NULL;
	}
}

#define MSEC_PER_SEC 1000
static void *alsa_thread(void *vptr)
{
	ALSA_DATA(vptr);
	void *audio;
	struct obs_source_audio obs_audio;
	size_t bytes_per_period = snd_pcm_format_width(data->format) / 8 * data->period_size;
	audio = bzalloc(bytes_per_period * data->channels);
	int ret;

	if ((ret = snd_pcm_start(data->pcm)) < 0) {
		blog(LOG_ERROR, "Not able to start PCM: %s",
			snd_strerror(ret));
		return NULL;
	}

	obs_audio.speakers        = get_speaker_layout(data->channels);
	obs_audio.samples_per_sec = data->sample_rate;
	obs_audio.format          = alsa_to_obs_audio_format(data->format);
	for (int ch = 0; ch < data->channels; ch++)
		obs_audio.data[ch] = audio + bytes_per_period * ch;

	while (os_event_try(data->event) == EAGAIN) {
		snd_pcm_sframes_t frames, delay = 0;
		snd_pcm_uframes_t count = data->period_size;

		ret = snd_pcm_wait(data->pcm, MSEC_PER_SEC);

		if (!ret || ret == -EAGAIN)
			continue;

		if (ret < 0)
			if (alsa_handle_xrun(data->pcm) < 0)
				break;

		while (count > 0) {
			frames = snd_pcm_mmap_readn(data->pcm, (void **)obs_audio.data, count);

			if (frames == -EAGAIN)
				continue;
			if (frames < 0)
				if (alsa_handle_xrun(data->pcm) < 0)
					goto exit;

			snd_pcm_delay(data->pcm, &delay);

			obs_audio.frames    = frames;
			obs_audio.timestamp = get_audio_sample_time(frames + delay,
								    data->sample_rate);

			obs_source_output_audio(data->source, &obs_audio);

			count -= frames;
		}

	}

exit:
	bfree(audio);
	return NULL;
}

int alsa_set_hwparams(snd_pcm_t *pcm, struct alsa_data *data)
{
	int ret = 0;
	snd_pcm_hw_params_t *params;
	unsigned buffer_time = UINT_MAX;
	unsigned period_time = 0;

	snd_pcm_hw_params_alloca(&params);

	ret = snd_pcm_hw_params_any(pcm, params);
	CHECK_RETURN("No hwparams available");
	ret = snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_MMAP_NONINTERLEAVED);
	CHECK_RETURN("Unable to set access type");
	ret = snd_pcm_hw_params_set_format(pcm, params, data->format);
	CHECK_RETURN("Unable to set PCM format");
	ret = snd_pcm_hw_params_set_channels(pcm, params, data->channels);
	CHECK_RETURN("Unable to set channels");
	ret = snd_pcm_hw_params_set_rate_near(pcm, params, &data->sample_rate, 0);
	CHECK_RETURN("Unable to set sample rate");
	snd_pcm_hw_params_get_buffer_time_max(params, &buffer_time, 0);
	buffer_time = OBS_MIN(buffer_time, MAX_BUFFER_TIME_US);
	snd_pcm_hw_params_get_period_time_min(params, &period_time, 0);
	period_time = OBS_MAX(period_time, buffer_time / 4);
	ret = snd_pcm_hw_params_set_buffer_time_near(pcm, params, &buffer_time, 0);
	CHECK_RETURN("Unable to set buffer time");
	ret = snd_pcm_hw_params_set_period_time_near(pcm, params, &period_time, 0);
	CHECK_RETURN("Unable to set period time");
	snd_pcm_hw_params_get_period_size(params, &data->period_size, 0);
	snd_pcm_hw_params_get_buffer_size(params, &data->buffer_size);
	snd_pcm_hw_params(pcm, params);
	CHECK_RETURN("Unable to set hwparams");

	blog(LOG_INFO, "Channels: %i", data->channels);
	blog(LOG_INFO, "Sample rate: %iHz", data->sample_rate);
	blog(LOG_INFO, "Period size: %lu", data->period_size);
	blog(LOG_INFO, "Buffer size: %lu", data->buffer_size);

	return 0;
}

static bool alsa_init(struct alsa_data *data)
{
	int ret;

	/* Initialize and start thread */
	blog(LOG_INFO, "Attempting to open PCM (%s)", data->device);
	ret = snd_pcm_open(&data->pcm, data->device, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
	if (ret < 0) {
		blog(LOG_ERROR, "Unable to open PCM: %s",
			snd_strerror(ret));
		return false;
	}

	if (alsa_set_hwparams(data->pcm, data) < 0)
		return false;

	if (os_event_init(&data->event, OS_EVENT_TYPE_MANUAL))
		return false;
	if (pthread_create(&data->thread, NULL, alsa_thread, data))
		return false;

	return true;
}

static void alsa_update(void *vptr, obs_data_t *settings)
{
	ALSA_DATA(vptr);

	alsa_terminate(data);

	bfree(data->device);

	data->device = bstrdup(obs_data_get_string(settings, "pcm_name"));
	data->channels = obs_data_get_bool(settings, "force_mono") ? 1 : 2;

	alsa_init(data);

	return;
}

static void *alsa_create(obs_data_t *settings, obs_source_t *source)
{
	struct alsa_data *data = bzalloc(sizeof(*data));

	data->sample_rate = 48000;
	data->format      = SND_PCM_FORMAT_S16_LE;
	data->source      = source;

	alsa_update(data, settings);

	return data;
}

static void alsa_destroy(void *vptr)
{
	ALSA_DATA(vptr);

	if (!data)
		return;

	alsa_terminate(data);

	bfree(data->device);

	bfree(data);
}

struct obs_source_info alsa_capture = {
	.id             = "alsa_capture",
	.type           = OBS_SOURCE_TYPE_INPUT,
	.output_flags   = OBS_SOURCE_AUDIO,
	.get_name       = alsa_getname,
	.create         = alsa_create,
	.destroy        = alsa_destroy,
	.update         = alsa_update,
	.get_defaults   = alsa_defaults,
	.get_properties = alsa_properties
};

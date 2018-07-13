/* hfp.c
Copyright (c) 2012-2017, The Linux Foundation. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above
      copyright notice, this list of conditions and the following
      disclaimer in the documentation and/or other materials provided
      with the distribution.
    * Neither the name of The Linux Foundation nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.*/

#define LOG_TAG "audio_hw_hfp"
/*#define LOG_NDEBUG 0*/
#define LOG_NDDEBUG 0

#include <errno.h>
#include <math.h>
#include <cutils/log.h>

#include "audio_hw.h"
#include "platform.h"
#include "platform_api.h"
#include <stdlib.h>
#include <cutils/str_parms.h>
#include "audio_extn.h"

#ifdef HFP_ENABLED
#define AUDIO_PARAMETER_HFP_ENABLE      "hfp_enable"
#define AUDIO_PARAMETER_HFP_SET_SAMPLING_RATE "hfp_set_sampling_rate"
#define AUDIO_PARAMETER_KEY_HFP_VOLUME "hfp_volume"

#ifdef PLATFORM_MSM8994
#define HFP_RX_VOLUME     "SEC AUXPCM LOOPBACK Volume"
#elif defined PLATFORM_MSM8996
#define HFP_RX_VOLUME     "PRI AUXPCM LOOPBACK Volume"
#else
#define HFP_RX_VOLUME     "Internal HFP RX Volume"
#endif

static int32_t start_hfp(struct audio_device *adev,
                               struct str_parms *parms);

static int32_t stop_hfp(struct audio_device *adev);

struct hfp_module {
    struct pcm *hfp_ul_rx;
    struct pcm *hfp_ul_tx;
    struct pcm *hfp_dl_rx;
    struct pcm *hfp_dl_tx;
    bool is_hfp_running;
    float hfp_volume;
    audio_usecase_t ucid;
};

static struct hfp_module hfpmod = {
    .hfp_ul_rx = NULL,
    .hfp_ul_tx = NULL,
    .hfp_dl_rx = NULL,
    .hfp_dl_tx = NULL,
    .hfp_volume = 0,
    .is_hfp_running = 0,
    .ucid = USECASE_AUDIO_HFP_SCO_UPLINK,
};
static struct pcm_config pcm_config_hfp = {
    .channels = 1,
    .rate = 8000,
    .period_size = 240,
    .period_count = 2,
    .format = PCM_FORMAT_S16_LE,
    .start_threshold = 0,
    .stop_threshold = INT_MAX,
    .avail_min = 0,
};

static int32_t hfp_set_volume(struct audio_device *adev, float value)
{
    int32_t vol, ret = 0;
    struct mixer_ctl *ctl;
    char mixer_ctl_name[128];
    int pcm_device_id;

    ALOGV("%s: entry", __func__);
    ALOGD("%s: (%f)\n", __func__, value);

    hfpmod.hfp_volume = value;
    if (value < 0.0) {
        ALOGW("%s: (%f) Under 0.0, assuming 0.0\n", __func__, value);
        value = 0.0;
    } else {
        value = ((value > 15.000000) ? 1.0 : (value / 15));
        ALOGW("%s: Volume brought with in range (%f)\n", __func__, value);
    }
    vol  = lrint((value * 0x2000) + 0.5);

    if (!hfpmod.is_hfp_running) {
        ALOGV("%s: HFP not active, ignoring set_hfp_volume call", __func__);
        return -EIO;
    }

    ALOGD("%s: Setting HFP volume to %d \n", __func__, vol);

    switch (hfpmod.ucid) {
    case USECASE_AUDIO_HFP_SCO_UPLINK:
        pcm_device_id = platform_get_pcm_device_id(USECASE_AUDIO_HFP_SCO_DOWNLINK,
                                                   PCM_PLAYBACK);
        break;
    case USECASE_AUDIO_HFP_SCO_WB_UPLINK:
        pcm_device_id = platform_get_pcm_device_id(USECASE_AUDIO_HFP_SCO_WB_DOWNLINK,
                                                   PCM_PLAYBACK);
        break;
    default:
        ALOGE("%s: Invalid use-case %d", __func__, hfpmod.ucid);
        return -EINVAL;
    }

    snprintf(mixer_ctl_name, sizeof(mixer_ctl_name),
             "Playback %d Volume", pcm_device_id);
    ctl = mixer_get_ctl_by_name(adev->mixer, mixer_ctl_name);
    if (!ctl) {
        ALOGE("%s: Could not get ctl for mixer cmd - %s",
              __func__, mixer_ctl_name);
        return -EINVAL;
    }
    if(mixer_ctl_set_value(ctl, 0, vol) < 0) {
        ALOGE("%s: Couldn't set HFP Volume: [%d]", __func__, vol);
        return -EINVAL;
    }

    ALOGV("%s: exit", __func__);
    return ret;
}

static int32_t start_hfp(struct audio_device *adev,
                         struct str_parms *parms __unused)
{
    int32_t i, ret = 0;
    struct audio_usecase *uc_uplink_info;
    struct audio_usecase uc_downlink_info;
    int32_t pcm_ul_rx_id, pcm_ul_tx_id, pcm_dl_rx_id, pcm_dl_tx_id;

    ALOGD("%s: enter", __func__);

    uc_uplink_info = (struct audio_usecase *)calloc(1, sizeof(struct audio_usecase));

    if (!uc_uplink_info)
        return -ENOMEM;

    uc_uplink_info->id = hfpmod.ucid;
    uc_uplink_info->type = PCM_HFP_CALL;
    uc_uplink_info->stream.out = adev->primary_output;
    uc_uplink_info->devices = adev->primary_output->devices;
    uc_uplink_info->in_snd_device = SND_DEVICE_NONE;
    uc_uplink_info->out_snd_device = SND_DEVICE_NONE;

    list_add_tail(&adev->usecase_list, &uc_uplink_info->list);

    select_devices(adev, hfpmod.ucid);
    memcpy(&uc_downlink_info, uc_uplink_info, sizeof(struct audio_usecase));

    switch (uc_uplink_info->id) {
    case USECASE_AUDIO_HFP_SCO_UPLINK:
        uc_downlink_info.id = USECASE_AUDIO_HFP_SCO_DOWNLINK;
        break;
    case USECASE_AUDIO_HFP_SCO_WB_UPLINK:
        uc_downlink_info.id = USECASE_AUDIO_HFP_SCO_WB_DOWNLINK;
        break;
    default:
        ALOGE("%s: Invalid use-case %d", __func__, uc_uplink_info->id);
        ret = -EINVAL;
        goto exit;
    }

    pcm_ul_rx_id = platform_get_pcm_device_id(uc_uplink_info->id, PCM_PLAYBACK);
    pcm_ul_tx_id = platform_get_pcm_device_id(uc_uplink_info->id, PCM_CAPTURE);
    pcm_dl_rx_id = platform_get_pcm_device_id(uc_downlink_info.id, PCM_PLAYBACK);
    pcm_dl_tx_id = platform_get_pcm_device_id(uc_downlink_info.id, PCM_CAPTURE);
    if (pcm_ul_rx_id < 0 || pcm_ul_tx_id < 0 ||
        pcm_dl_rx_id < 0 || pcm_dl_tx_id < 0 ) {
        ALOGE("%s: Invalid PCM devices (UL: rx: %d tx: %d DL: rx: %d tx %d) for the usecase(%d)",
              __func__, pcm_ul_rx_id, pcm_ul_tx_id, pcm_dl_rx_id, pcm_dl_tx_id, hfpmod.ucid);
        ret = -EIO;
        goto exit;
    }

    ALOGV("%s: HFP UL PCM devices (UL pcm rx: %d UL pcm tx: %d) for the usecase(%d)",
              __func__, pcm_ul_rx_id, pcm_ul_tx_id, hfpmod.ucid);

    if ((uc_uplink_info->out_snd_device != SND_DEVICE_NONE) ||
        (uc_uplink_info->in_snd_device != SND_DEVICE_NONE)) {
        if (audio_extn_ext_hw_plugin_usecase_start(adev->ext_hw_plugin, uc_uplink_info))
            ALOGE("%s: failed to start ext hw plugin", __func__);
        /* Set cached mic mute upon hfp start */
        bool mute = false;
        audio_extn_ext_hw_plugin_get_mic_mute(adev->ext_hw_plugin, &mute);
        if (audio_extn_ext_hw_plugin_set_mic_mute(adev->ext_hw_plugin, mute))
            ALOGE("%s: failed to set mic mute", __func__);
    }
    ALOGD("%s: Opening PCM playback device card_id(%d) device_id(%d)",
          __func__, adev->snd_card, pcm_ul_rx_id);
    hfpmod.hfp_ul_rx = pcm_open(adev->snd_card,
                                  pcm_ul_rx_id,
                                  PCM_OUT, &pcm_config_hfp);
    if (hfpmod.hfp_ul_rx && !pcm_is_ready(hfpmod.hfp_ul_rx)) {
        ALOGE("%s: %s", __func__, pcm_get_error(hfpmod.hfp_ul_rx));
        ret = -EIO;
        goto exit;
    }
    ALOGD("%s: Opening PCM capture device card_id(%d) device_id(%d)",
          __func__, adev->snd_card, pcm_ul_tx_id);
    hfpmod.hfp_ul_tx = pcm_open(adev->snd_card,
                                  pcm_ul_tx_id,
                                  PCM_IN, &pcm_config_hfp);
    if (hfpmod.hfp_ul_tx && !pcm_is_ready(hfpmod.hfp_ul_tx)) {
        ALOGE("%s: %s", __func__, pcm_get_error(hfpmod.hfp_ul_tx));
        ret = -EIO;
        goto exit;
    }
    if (pcm_start(hfpmod.hfp_ul_rx) < 0) {
        ALOGE("%s: pcm start for hfp ul rx failed", __func__);
        ret = -EINVAL;
        goto exit;
    }
    if (pcm_start(hfpmod.hfp_ul_tx) < 0) {
        ALOGE("%s: pcm start for hfp ul tx failed", __func__);
        ret = -EINVAL;
        goto exit;
    }

    ALOGV("%s: HFP DL PCM devices (DL pcm rx: %d DL pcm tx: %d) for the usecase(%d)",
          __func__, pcm_dl_rx_id, pcm_dl_tx_id, uc_downlink_info.id);

    //Calibrate the downlink
    audio_extn_utils_update_stream_app_type_cfg_for_usecase(adev, &uc_downlink_info);
    audio_extn_utils_send_audio_calibration(adev, &uc_downlink_info);
    audio_extn_utils_send_app_type_cfg(adev, &uc_downlink_info);

    ALOGD("%s: Opening PCM playback device card_id(%d) device_id(%d)",
          __func__, adev->snd_card, pcm_dl_rx_id);
    hfpmod.hfp_dl_rx = pcm_open(adev->snd_card,
                                   pcm_dl_rx_id,
                                   PCM_OUT, &pcm_config_hfp);
    if (hfpmod.hfp_dl_rx && !pcm_is_ready(hfpmod.hfp_dl_rx)) {
        ALOGE("%s: %s", __func__, pcm_get_error(hfpmod.hfp_dl_rx));
        ret = -EIO;
        goto exit;
    }
    ALOGD("%s: Opening PCM capture device card_id(%d) device_id(%d)",
          __func__, adev->snd_card, pcm_dl_tx_id);
    hfpmod.hfp_dl_tx = pcm_open(adev->snd_card,
                                   pcm_dl_tx_id,
                                   PCM_IN, &pcm_config_hfp);
    if (hfpmod.hfp_dl_tx && !pcm_is_ready(hfpmod.hfp_dl_tx)) {
        ALOGE("%s: %s", __func__, pcm_get_error(hfpmod.hfp_dl_tx));
        ret = -EIO;
        goto exit;
    }

    if (pcm_start(hfpmod.hfp_dl_rx) < 0) {
        ALOGE("%s: pcm start for hfp dl rx failed", __func__);
        ret = -EINVAL;
        goto exit;
    }
    if (pcm_start(hfpmod.hfp_dl_tx) < 0) {
        ALOGE("%s: pcm start for hfp dl tx failed", __func__);
        ret = -EINVAL;
        goto exit;
    }

    hfpmod.is_hfp_running = true;
    hfp_set_volume(adev, hfpmod.hfp_volume);

    ALOGD("%s: exit: status(%d)", __func__, ret);
    return 0;

exit:
    stop_hfp(adev);
    ALOGE("%s: Problem in HFP start: status(%d)", __func__, ret);
    return ret;
}

static int32_t stop_hfp(struct audio_device *adev)
{
    int32_t i, ret = 0;
    struct audio_usecase *uc_info;

    ALOGD("%s: enter", __func__);
    hfpmod.is_hfp_running = false;

    /* 1. Close the PCM devices */
    if (hfpmod.hfp_ul_rx) {
        pcm_close(hfpmod.hfp_ul_rx);
        hfpmod.hfp_ul_rx = NULL;
    }
    if (hfpmod.hfp_ul_tx) {
        pcm_close(hfpmod.hfp_ul_tx);
        hfpmod.hfp_ul_tx = NULL;
    }
    if (hfpmod.hfp_dl_rx) {
        pcm_close(hfpmod.hfp_dl_rx);
        hfpmod.hfp_dl_rx = NULL;
    }
    if (hfpmod.hfp_dl_tx) {
        pcm_close(hfpmod.hfp_dl_tx);
        hfpmod.hfp_dl_tx = NULL;
    }

    uc_info = get_usecase_from_list(adev, hfpmod.ucid);
    if (uc_info == NULL) {
        ALOGE("%s: Could not find the usecase (%d) in the list",
              __func__, hfpmod.ucid);
        return -EINVAL;
    }

    if ((uc_info->out_snd_device != SND_DEVICE_NONE) ||
        (uc_info->in_snd_device != SND_DEVICE_NONE)) {
        if (audio_extn_ext_hw_plugin_usecase_stop(adev->ext_hw_plugin, uc_info))
            ALOGE("%s: failed to stop ext hw plugin", __func__);
    }

    /* 2. Disable echo reference while stopping hfp */
    platform_set_echo_reference(adev, false, uc_info->devices);

    /* 3. Get and set stream specific mixer controls */
    disable_audio_route(adev, uc_info);

    /* 4. Disable the rx and tx devices */
    disable_snd_device(adev, uc_info->out_snd_device);
    disable_snd_device(adev, uc_info->in_snd_device);

    list_remove(&uc_info->list);
    free(uc_info);

    ALOGD("%s: exit: status(%d)", __func__, ret);
    return ret;
}

bool audio_extn_hfp_is_active(struct audio_device *adev)
{
    struct audio_usecase *hfp_usecase = NULL;
    hfp_usecase = get_usecase_from_list(adev, hfpmod.ucid);

    if (hfp_usecase != NULL)
        return true;
    else
        return false;
}

audio_usecase_t audio_extn_hfp_get_usecase()
{
    return hfpmod.ucid;
}

int audio_extn_hfp_set_parameters(struct audio_device *adev, struct str_parms *parms)
{
    int ret;
    int rate;
    int val;
    float vol;
    char value[32]={0};

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_HFP_ENABLE, value,
                            sizeof(value));

    if (ret >= 0) {
           if ((!strncmp(value,"true",sizeof(value))) && (!hfpmod.is_hfp_running))
               ret = start_hfp(adev,parms);
           else if((!strncmp(value,"false",sizeof(value))) && (hfpmod.is_hfp_running))
               stop_hfp(adev);
           else {
               ALOGE("%s: concurrent start_hfp() and concurrent stop_hfp() is not valid", __func__);
               ret = -EINVAL;
               goto exit;
           }
    }

    memset(value, 0, sizeof(value));
    ret = str_parms_get_str(parms,AUDIO_PARAMETER_HFP_SET_SAMPLING_RATE, value,
                            sizeof(value));
    if (ret >= 0) {
           rate = atoi(value);
           if (rate == 8000){
               hfpmod.ucid = USECASE_AUDIO_HFP_SCO_UPLINK;
               pcm_config_hfp.rate = rate;
           } else if (rate == 16000){
               hfpmod.ucid = USECASE_AUDIO_HFP_SCO_WB_UPLINK;
               pcm_config_hfp.rate = rate;
           } else
               ALOGE("Unsupported rate..");
    }

    if (hfpmod.is_hfp_running) {
        memset(value, 0, sizeof(value));
        ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING,
                                value, sizeof(value));
        if (ret >= 0) {
            val = atoi(value);
            if (val > 0)
                select_devices(adev, hfpmod.ucid);
        }
    }

    memset(value, 0, sizeof(value));
    ret = str_parms_get_str(parms, AUDIO_PARAMETER_KEY_HFP_VOLUME,
                            value, sizeof(value));
    if (ret >= 0) {
        if (sscanf(value, "%f", &vol) != 1){
            ALOGE("%s: error in retrieving hfp volume", __func__);
            ret = -EIO;
            goto exit;
        }
        ALOGD("%s: set_hfp_volume usecase, Vol: [%f]", __func__, vol);
        hfp_set_volume(adev, vol);
    }
exit:
    ALOGV("%s Exit",__func__);
    return ret;
}
#endif /*HFP_ENABLED*/
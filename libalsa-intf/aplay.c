/*
** Copyright 2010, The Android Open-Source Project
** Copyright (c) 2011-2012, The Linux Foundation. All rights reserved.
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <sys/poll.h>
#include <sys/ioctl.h>
#include <getopt.h>

#include <sound/asound.h>
#include <sound/compress_params.h>
#include <sound/compress_offload.h>
#include "alsa_audio.h"

#ifndef ANDROID
#define strlcat g_strlcat
#define strlcpy g_strlcpy
#endif

#define ID_RIFF 0x46464952
#define ID_WAVE 0x45564157
#define ID_FMT  0x20746d66
#define ID_DATA 0x61746164

#define FORMAT_PCM 1
#define FORMAT_PCM_24 65534
#define LOG_NDEBUG 1

struct output_metadata_handle_t {
    uint32_t            metadataLength;
    uint32_t            bufferLength;
    uint64_t            timestamp;
    uint32_t            reserved[12];
};

static struct output_metadata_handle_t outputMetadataTunnel;

static pcm_flag = 1;
static debug = 0;
static uint32_t play_max_sz = 2147483648LL;
static int format = SNDRV_PCM_FORMAT_S16_LE;
static int period = 0;
static int compressed = 0;
static int set_channel_map = 0;
static char channel_map[8];
static char *compr_codec;
static int piped = 0;
static int outputMetadataLength = 0;
static int eosSet = 0;

static struct option long_options[] =
{
    {"pcm", 0, 0, 'P'},
    {"debug", 0, 0, 'V'},
    {"Mmap", 0, 0, 'M'},
    {"HW", 1, 0, 'D'},
    {"Rate", 1, 0, 'R'},
    {"channel", 1, 0, 'C'},
    {"format", 1, 0, 'F'},
    {"period", 1, 0, 'B'},
    {"compressed", 0, 0, 'T'},
    {"channelMap", 0, 0, 'X'},
    {0, 0, 0, 0}
};

struct wav_header {
    uint32_t riff_id;
    uint32_t riff_sz;
    uint32_t riff_fmt;
    uint32_t fmt_id;
    uint32_t fmt_sz;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;       /* sample_rate * num_channels * bps / 8 */
    uint16_t block_align;     /* num_channels * bps / 8 */
    uint16_t bits_per_sample;
    uint32_t data_id;
    uint32_t data_sz;
};

void updateMetaData(size_t bytes) {
   outputMetadataTunnel.metadataLength = sizeof(outputMetadataTunnel);
   outputMetadataTunnel.timestamp = 0;
   outputMetadataTunnel.bufferLength =  bytes;
   fprintf(stderr, "bytes = %d\n", bytes);
}
static int set_params(struct pcm *pcm)
{
     struct snd_pcm_hw_params *params;
     struct snd_pcm_sw_params *sparams;

     unsigned long periodSize, bufferSize, reqBuffSize;
     unsigned int periodTime, bufferTime;
     unsigned int requestedRate = pcm->rate;
     int channels;
     if(pcm->flags & PCM_MONO)
         channels = 1;
     else if(pcm->flags & PCM_QUAD)
         channels = 4;
     else if(pcm->flags & PCM_5POINT1)
         channels = 6;
     else if(pcm->flags & PCM_7POINT1)
         channels = 8;
     else
         channels = 2;

     params = (struct snd_pcm_hw_params*) calloc(1, sizeof(struct snd_pcm_hw_params));
     if (!params) {
          fprintf(stderr, "Aplay:Failed to allocate ALSA hardware parameters!");
          return -ENOMEM;
     }

     param_init(params);

     param_set_mask(params, SNDRV_PCM_HW_PARAM_ACCESS,
                    (pcm->flags & PCM_MMAP)? SNDRV_PCM_ACCESS_MMAP_INTERLEAVED : SNDRV_PCM_ACCESS_RW_INTERLEAVED);
     param_set_mask(params, SNDRV_PCM_HW_PARAM_FORMAT, pcm->format);
     param_set_mask(params, SNDRV_PCM_HW_PARAM_SUBFORMAT,
                    SNDRV_PCM_SUBFORMAT_STD);
     if (period)
         param_set_min(params, SNDRV_PCM_HW_PARAM_PERIOD_BYTES, period);
     else
         param_set_min(params, SNDRV_PCM_HW_PARAM_PERIOD_TIME, 10);

     switch (format) {
     case SNDRV_PCM_FORMAT_S16_LE:
         param_set_int(params, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, 16);
         param_set_int(params, SNDRV_PCM_HW_PARAM_FRAME_BITS, pcm->channels * 16);
         break;
     case SNDRV_PCM_FORMAT_S24_LE:
         param_set_int(params, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, 32);
         param_set_int(params, SNDRV_PCM_HW_PARAM_FRAME_BITS, pcm->channels * 32);
         break;
     }

     param_set_int(params, SNDRV_PCM_HW_PARAM_CHANNELS,
                    pcm->channels);
     param_set_int(params, SNDRV_PCM_HW_PARAM_RATE, pcm->rate);
     param_set_hw_refine(pcm, params);

     if (param_set_hw_params(pcm, params)) {
         fprintf(stderr, "Aplay:cannot set hw params\n");
         return -errno;
     }
     if (debug)
         param_dump(params);

     pcm->buffer_size = pcm_buffer_size(params);
     pcm->period_size = pcm_period_size(params);
     pcm->period_cnt = pcm->buffer_size/pcm->period_size;
     if (debug) {
        fprintf (stderr,"period_cnt = %d\n", pcm->period_cnt);
        fprintf (stderr,"period_size = %d\n", pcm->period_size);
        fprintf (stderr,"buffer_size = %d\n", pcm->buffer_size);
     }
     sparams = (struct snd_pcm_sw_params*) calloc(1, sizeof(struct snd_pcm_sw_params));
     if (!sparams) {
         fprintf(stderr, "Aplay:Failed to allocate ALSA software parameters!\n");
         return -ENOMEM;
     }
     // Get the current software parameters
    sparams->tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
    sparams->period_step = 1;

    sparams->avail_min = pcm->period_size/(channels * 2) ;
    sparams->start_threshold =  pcm->period_size/(channels * 2) ;
    sparams->stop_threshold =  pcm->buffer_size ;
    sparams->xfer_align =  pcm->period_size/(channels * 2) ; /* needed for old kernels */

    sparams->silence_size = 0;
    sparams->silence_threshold = 0;

    if (param_set_sw_params(pcm, sparams)) {
        fprintf(stderr, "Aplay:cannot set sw params");
        return -errno;
    }
    if (debug) {
       fprintf (stderr,"sparams->avail_min= %lu\n", sparams->avail_min);
       fprintf (stderr," sparams->start_threshold= %lu\n", sparams->start_threshold);
       fprintf (stderr," sparams->stop_threshold= %lu\n", sparams->stop_threshold);
       fprintf (stderr," sparams->xfer_align= %lu\n", sparams->xfer_align);
       fprintf (stderr," sparams->boundary= %lu\n", sparams->boundary);
    }
    return 0;
}

void send_channel_map_driver(struct pcm *pcm)
{
    int i, ret;
    struct mixer *mixer;
    const char* device = "/dev/snd/controlC0";

    mixer = mixer_open(device);
    if (!mixer) {
        fprintf(stderr,"oops: %s: %d\n", strerror(errno), __LINE__);
        return;
    }
    ret = pcm_set_channel_map(pcm, mixer, 8, channel_map);
    if (ret < 0)
        fprintf(stderr, "could not set channel mask\n");
    mixer_close(mixer);

    return;
}

int buffer_data(struct pcm *pcm, void *data, unsigned count)
{
    int bufsize;
    int copy = 0;
    int i, j;
    void *inflate_data;

    if (pcm->format != SNDRV_PCM_FORMAT_S24_LE || (pcm->flags & PCM_TUNNEL))
       return 0;
    printf("Buffering data: count =%d", count);
    bufsize = (count/4)*3;
    inflate_data = calloc(1, count);
    if (pcm->buf->residue_buf == NULL)
       pcm->buf->residue_buf = calloc(1, bufsize);
    if (!inflate_data || !pcm->buf->residue_buf) {
       printf("Could not allocate buffer");
       if (inflate_data != NULL)
           free(inflate_data);
       pcm_close(pcm);
       return -ENOMEM;
    }
    memcpy(inflate_data, pcm->buf->residue_buf,
    pcm->buf->residue_buf_ptr);
    copy = bufsize - pcm->buf->residue_buf_ptr;
    memcpy(inflate_data + pcm->buf->residue_buf_ptr, data, copy);
    memcpy(pcm->buf->residue_buf, data + copy, count - copy);
    pcm->buf->residue_buf_ptr = count - copy;

    j = bufsize - 1;
    for (i = count-1; i >= 0 && j >= 0; i--) {
       if (i%4 == 0)
       continue;
       *((char *)(inflate_data) + i) = *((char *)(inflate_data) + j);
       j--;
    }
    memcpy(data, inflate_data, count);
    if (inflate_data != NULL)
       free(inflate_data);
    return 0;
}


int is_buffer_available(struct pcm *pcm, void *data, int count, int format)
{
    int i, j, copy;
    if (format != SNDRV_PCM_FORMAT_S24_LE || (pcm->flags & PCM_TUNNEL))
       return 0;
    copy = (count/4)*3;
    if (pcm->buf->residue_buf_ptr >= copy) {
       pcm->buf->residue_buf_ptr -= copy;
       for (i = 0, j = 0; j < count; j++) {
           if (j%4 == 0)
               continue;
           *((char *)data + j) = *((char *)pcm->buf->residue_buf + i);
           i++;
       }
       printf("Extra buffer available");
       return 1;
    }
    return 0;
}

int hw_pcm_write(struct pcm *pcm, void *data, unsigned count)
{
    int ret = 0, n = 0;
    ret = buffer_data(pcm, data, count);
    if (ret)
        return ret;
    do {
        n = pcm_write(pcm, data, count);
    } while (is_buffer_available(pcm, data, pcm->period_size, pcm->format));
    return ret;
}

static int play_file(unsigned rate, unsigned channels, int fd,
              unsigned flags, const char *device, unsigned data_sz)
{
    struct pcm *pcm;
    struct mixer *mixer;
    struct pcm_ctl *ctl = NULL;
    unsigned bufsize;
    char *data;
    long avail;
    long frames;
    int nfds = 1;
    struct snd_xferi x;
    unsigned offset = 0;
    int err;
    static int start = 0;
    struct pollfd pfd[1];
    int remainingData = 0;

    flags |= PCM_OUT;

    if (channels == 1)
        flags |= PCM_MONO;
    else if (channels == 4)
	flags |= PCM_QUAD;
    else if (channels == 6)
	flags |= PCM_5POINT1;
    else if (channels == 8)
	flags |= PCM_7POINT1;
    else
        flags |= PCM_STEREO;

    if (debug)
        flags |= DEBUG_ON;
    else
        flags |= DEBUG_OFF;

    pcm = pcm_open(flags, device);
    if (pcm < 0)
        return pcm;

    if (!pcm_ready(pcm)) {
        pcm_close(pcm);
        return -EBADFD;
    }

    if (compressed) {
       struct snd_compr_caps compr_cap;
       struct snd_compr_params compr_params;
       if (ioctl(pcm->fd, SNDRV_COMPRESS_GET_CAPS, &compr_cap)) {
          fprintf(stderr, "Aplay: SNDRV_COMPRESS_GET_CAPS, failed Error no %d \n", errno);
          pcm_close(pcm);
          return -errno;
       }
       if (!period)
           period = compr_cap.min_fragment_size;
           switch (get_compressed_format(compr_codec)) {
           case SND_AUDIOCODEC_MP3:
               compr_params.codec.id = SND_AUDIOCODEC_MP3;
               break;
           case SND_AUDIOCODEC_AC3_PASS_THROUGH:
               compr_params.codec.id = SND_AUDIOCODEC_AC3_PASS_THROUGH;
               printf("codec -d = %x\n", SND_AUDIOCODEC_AC3_PASS_THROUGH);
               break;
           case SND_AUDIOCODEC_AAC:
               compr_params.codec.id = SND_AUDIOCODEC_AAC;
               printf("codec -d = %x\n", SND_AUDIOCODEC_AAC);
               break;
           default:
               break;
           }
       compr_params.codec.transcode_dts = 0;
       if (ioctl(pcm->fd, SNDRV_COMPRESS_SET_PARAMS, &compr_params)) {
          fprintf(stderr, "Aplay: SNDRV_COMPRESS_SET_PARAMS,failed Error no %d \n", errno);
          pcm_close(pcm);
          return -errno;
       }
       outputMetadataLength = sizeof(struct output_metadata_handle_t);
    } else if (channels > 2) {
        if(set_channel_map) {
            send_channel_map_driver(pcm);
        }
    }
    pcm->channels = channels;
    pcm->rate = rate;
    pcm->flags = flags;
    pcm->format = format;
    if (set_params(pcm)) {
        fprintf(stderr, "Aplay:params setting failed\n");
        pcm_close(pcm);
        return -errno;
    }

    if (!pcm_flag) {
       if (pcm_prepare(pcm)) {
          fprintf(stderr, "Aplay:Failed in pcm_prepare\n");
          pcm_close(pcm);
          return -errno;
       }
       if (ioctl(pcm->fd, SNDRV_PCM_IOCTL_START)) {
          fprintf(stderr, "Aplay: Hostless IOCTL_START Error no %d \n", errno);
          pcm_close(pcm);
          return -errno;
       }
        while(1);
    }

    remainingData = data_sz;

    if (flags & PCM_MMAP) {
        u_int8_t *dst_addr = NULL;
        struct snd_pcm_sync_ptr *sync_ptr1 = pcm->sync_ptr;
        if (mmap_buffer(pcm)) {
             fprintf(stderr, "Aplay:params setting failed\n");
             pcm_close(pcm);
             return -errno;
        }
        if (pcm_prepare(pcm)) {
          fprintf(stderr, "Aplay:Failed in pcm_prepare\n");
          pcm_close(pcm);
          return -errno;
        }

        bufsize = pcm->period_size;
        if (debug)
          fprintf(stderr, "Aplay:bufsize = %d\n", bufsize);

        pfd[0].fd = pcm->timer_fd;
        pfd[0].events = POLLIN;

        frames = bufsize / (2*channels);
        for (;;) {
             if (!pcm->running) {
                  if (pcm_prepare(pcm)) {
                      fprintf(stderr, "Aplay:Failed in pcm_prepare\n");
                      pcm_close(pcm);
                      return -errno;
                  }
                  pcm->running = 1;
                  start = 0;
             }
             /* Sync the current Application pointer from the kernel */
             pcm->sync_ptr->flags = SNDRV_PCM_SYNC_PTR_APPL | SNDRV_PCM_SYNC_PTR_AVAIL_MIN;//SNDRV_PCM_SYNC_PTR_HWSYNC;
             err = sync_ptr(pcm);
             if (err == EPIPE) {
                 fprintf(stderr, "Aplay:Failed in sync_ptr \n");
                 /* we failed to make our window -- try to restart */
                 pcm->underruns++;
                 pcm->running = 0;
                 continue;
             }
             /*
              * Check for the available buffer in driver. If available buffer is
              * less than avail_min we need to wait
              */
             avail = pcm_avail(pcm);
             if (avail < 0) {
                 fprintf(stderr, "Aplay:Failed in pcm_avail\n");
                 pcm_close(pcm);
                 return avail;
             }
             if (avail < pcm->sw_p->avail_min) {
                 poll(pfd, nfds, TIMEOUT_INFINITE);
                 continue;
             }
             /*
              * Now that we have buffer size greater than avail_min available to
              * to be written we need to calcutate the buffer offset where we can
              * start writting.
              */
             dst_addr = dst_address(pcm);

             if (debug) {
                 fprintf(stderr, "dst_addr = 0x%08x\n", dst_addr);
                 fprintf(stderr, "Aplay:avail = %d frames = %d\n",avail, frames);
                 fprintf(stderr, "Aplay:sync_ptr->s.status.hw_ptr %ld  pcm->buffer_size %d  sync_ptr->c.control.appl_ptr %ld\n",
                            pcm->sync_ptr->s.status.hw_ptr,
                            pcm->buffer_size,
                            pcm->sync_ptr->c.control.appl_ptr);
             }

             /*
              * Read from the file to the destination buffer in kernel mmaped buffer
              * This reduces a extra copy of intermediate buffer.
              */
             memset(dst_addr, 0x0, bufsize);

             if (data_sz && !piped) {
                 if (remainingData < bufsize) {
                     bufsize = remainingData;
                     frames = remainingData / (2*channels);
                 }
             }
             fprintf(stderr, "addr = %d, size = %d \n", (dst_addr + outputMetadataLength),(bufsize - outputMetadataLength));
             err = read(fd, (dst_addr + outputMetadataLength) , (bufsize - outputMetadataLength));
             if(compressed) {
                 updateMetaData(err);
                 memcpy(dst_addr, &outputMetadataTunnel, outputMetadataLength);
             }

             if (debug)
                 fprintf(stderr, "read %d bytes from file\n", err);
             if (err <= 0 ) {
                 fprintf(stderr," EOS set\n ");
                 eosSet = 1;
                 break;
             }
             if (data_sz && !piped) {
                 remainingData -= bufsize;
                 if (remainingData <= 0)
                     break;
             }

             /*
              * Increment the application pointer with data written to kernel.
              * Update kernel with the new sync pointer.
              */
             pcm->sync_ptr->c.control.appl_ptr += frames;
             pcm->sync_ptr->flags = 0;

             err = sync_ptr(pcm);
             if (err == EPIPE) {
                 fprintf(stderr, "Aplay:Failed in sync_ptr 2 \n");
                 /* we failed to make our window -- try to restart */
                 pcm->underruns++;
                 pcm->running = 0;
                 continue;
             }

             if (debug) {
                 fprintf(stderr, "Aplay:sync_ptr->s.status.hw_ptr %ld  sync_ptr->c.control.appl_ptr %ld\n",
                            pcm->sync_ptr->s.status.hw_ptr,
                            pcm->sync_ptr->c.control.appl_ptr);
                 if (compressed && start) {
                    struct snd_compr_tstamp tstamp;
        if (ioctl(pcm->fd, SNDRV_COMPRESS_TSTAMP, &tstamp))
      fprintf(stderr, "Aplay: failed SNDRV_COMPRESS_TSTAMP\n");
                    else
                  fprintf(stderr, "timestamp = %lld\n", tstamp.timestamp);
    }
             }
             /*
              * If we have reached start threshold of buffer prefill,
              * its time to start the driver.
              */
                 if(start)
                     goto start_done;
                 if (ioctl(pcm->fd, SNDRV_PCM_IOCTL_START)) {
                     err = -errno;
                     if (errno == EPIPE) {
                         fprintf(stderr, "Aplay:Failed in SNDRV_PCM_IOCTL_START\n");
                         /* we failed to make our window -- try to restart */
                         pcm->underruns++;
                         pcm->running = 0;
                         continue;
                    } else {
                        fprintf(stderr, "Aplay:Error no %d \n", errno);
                        pcm_close(pcm);
                        return -errno;
                    }
                } else
                    start = 1;

start_done:
                offset += frames;
        }

        while(1) {
            pcm->sync_ptr->flags = SNDRV_PCM_SYNC_PTR_APPL | SNDRV_PCM_SYNC_PTR_AVAIL_MIN;//SNDRV_PCM_SYNC_PTR_HWSYNC;
            sync_ptr(pcm);
            /*
             * Check for the available buffer in driver. If available buffer is
             * less than avail_min we need to wait
             */
            if (pcm->sync_ptr->s.status.hw_ptr >= pcm->sync_ptr->c.control.appl_ptr) {
                fprintf(stderr, "Aplay:sync_ptr->s.status.hw_ptr %ld  sync_ptr->c.control.appl_ptr %ld\n",
                           pcm->sync_ptr->s.status.hw_ptr,
                           pcm->sync_ptr->c.control.appl_ptr);

                if(compressed && eosSet) {
                    fprintf(stderr,"Audio Drain DONE ++\n");
                    if ( ioctl(pcm->fd, SNDRV_COMPRESS_DRAIN) < 0 ) {
                        fprintf(stderr,"Audio Drain failed\n");
                    }
                    fprintf(stderr,"Audio Drain DONE --\n");
                }
                break;
            } else
                poll(pfd, nfds, TIMEOUT_INFINITE);
        }
    } else {
        if (pcm_prepare(pcm)) {
            fprintf(stderr, "Aplay:Failed in pcm_prepare\n");
            pcm_close(pcm);
            return -errno;
        }

        bufsize = pcm->period_size;

        data = calloc(1, bufsize);
        if (!data) {
            fprintf(stderr, "Aplay:could not allocate %d bytes\n", bufsize);
            pcm_close(pcm);
            return -ENOMEM;
        }

        if (data_sz && !piped) {
            if (remainingData < bufsize)
                bufsize = remainingData;
        }

        while (read(fd, data, bufsize) > 0) {
            if (hw_pcm_write(pcm, data, bufsize)){
                fprintf(stderr, "Aplay: pcm_write failed\n");
                free(data);
                pcm_close(pcm);
                return -errno;
            }
            memset(data, 0, bufsize);

            if (data_sz && !piped) {
                remainingData -= bufsize;
                if (remainingData <= 0)
                    break;
                if (remainingData < bufsize)
                       bufsize = remainingData;
            }
        }
        free(data);
    }

    fprintf(stderr, "Aplay: Done playing\n");
    pcm_close(pcm);
    return 0;
}

int play_raw(const char *fg, int rate, int ch, const char *device, const char *fn)
{
    int fd;
    unsigned flag = 0;

    if(!fn) {
        fd = fileno(stdin);
        piped = 1;
    } else {
        fd = open(fn, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "Aplay:aplay: cannot open '%s'\n", fn);
            return fd;
        }
    }

    if (!strncmp(fg, "M", sizeof("M")))
        flag = PCM_MMAP;
    else if (!strncmp(fg, "N", sizeof("N")))
        flag = PCM_NMMAP;

    fprintf(stderr, "aplay: Playing '%s': format %s ch = %d\n",
        fn, get_format_desc(format), ch );
    return play_file(rate, ch, fd, flag, device, 0);
}

int play_wav(const char *fg, int rate, int ch, const char *device, const char *fn)
{
    struct wav_header hdr;
    int fd;
    unsigned flag = 0;

    if (pcm_flag) {
        if(!fn) {
            fd = fileno(stdin);
            piped = 1;
        } else {
            fd = open(fn, O_RDONLY);
            if (fd < 0) {
                fprintf(stderr, "Aplay:aplay: cannot open '%s'\n", fn);
                return fd;
            }
        }
        if (compressed) {
            hdr.sample_rate = rate;
            hdr.num_channels = ch;
            hdr.data_sz = 0;
            goto ignore_header;
        }

        if (read(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
            fprintf(stderr, "Aplay:aplay: cannot read header\n");
            return -errno;
        }
        if (hdr.bits_per_sample == 16) {
            format = SNDRV_PCM_FORMAT_S16_LE;
        } else if(hdr.bits_per_sample == 24) {
            format = SNDRV_PCM_FORMAT_S24_LE;
        }
        if (hdr.bits_per_sample > 16) {
            int temp = 0;
            /* parse the file until 'data' chunk is found */
            while (temp != 0x61746164) {
                if(read(fd, &temp, 4) != 4) {
                    fprintf(stderr, "DATA ID is not found in the header");
                    return -EINVAL;
                }
                fprintf(stderr, "temp 0x%x", temp);
            }
            hdr.data_id = 0x61746164;
            read(fd, &hdr.data_sz, 4);
            fprintf(stderr, "data_id 0x%x data_sz = 0x%x",hdr.data_id,hdr.data_sz);
        }
        if ((hdr.riff_id != ID_RIFF) ||
            (hdr.riff_fmt != ID_WAVE) ||
            (hdr.fmt_id != ID_FMT)) {
            fprintf(stderr, "Aplay:aplay: '%s' is not a riff/wave file\n", fn);
            return -EINVAL;
        }
        fprintf(stderr, "hdr.fmt_sz %d hdr.bits_per_sample = %d"
                        "hdr.audio_format = %d\n",
                        hdr.fmt_sz, hdr.bits_per_sample, hdr.audio_format);
        if ((hdr.audio_format != FORMAT_PCM && hdr.audio_format != FORMAT_PCM_24) ||
            (hdr.fmt_sz != 16 && hdr.fmt_sz != 40)) {
            fprintf(stderr, "Aplay:aplay: '%s' is not pcm format\n", fn);
            return -EINVAL;
        }
        if (hdr.bits_per_sample != 16 && hdr.bits_per_sample != 24) {
            fprintf(stderr, "Aplay:aplay: '%s' is not 16bit per sample\n", fn);
            return -EINVAL;
        }
    } else {
        fd = -EBADFD;
        hdr.sample_rate = rate;
        hdr.num_channels = ch;
        hdr.data_sz = 0;
    }

ignore_header:
    if (!strncmp(fg, "M", sizeof("M")))
        flag = PCM_MMAP;
    else if (!strncmp(fg, "N", sizeof("N")))
        flag = PCM_NMMAP;
    fprintf(stderr, "aplay: Playing '%s':%s\n", fn, get_format_desc(format) );

    fprintf(stderr, "aplay: Samplerate[%d]Channels[%d]\n", hdr.sample_rate, hdr.num_channels);
    return play_file(hdr.sample_rate, hdr.num_channels, fd, flag, device, hdr.data_sz);
}

char get_channel_map_val(char *string)
{
    char retval = 0;
    if( !strncmp(string, "RRC", sizeof(string)) )
        retval = 16;
    else if( !strncmp(string, "RLC", sizeof(string)) )
        retval = 15;
    else if( !strncmp(string, "FRC", sizeof(string)) )
        retval = 14;
    else if( !strncmp(string, "FLC", sizeof(string)) )
        retval = 13;
    else if( !strncmp(string, "MS", sizeof(string)) )
        retval = 12;
    else if( !strncmp(string, "CVH", sizeof(string)) )
        retval = 11;
    else if( !strncmp(string, "TS", sizeof(string)) )
        retval = 10;
    else if( !strncmp(string, "RB", sizeof(string)) )
        retval = 9;
    else if( !strncmp(string, "LB", sizeof(string)) )
        retval = 8;
    else if( !strncmp(string, "CS", sizeof(string)) )
        retval = 7;
    else if( !strncmp(string, "LFE", sizeof(string)) )
        retval = 6;
    else if( !strncmp(string, "RS", sizeof(string)) )
        retval = 5;
    else if( !strncmp(string, "LS", sizeof(string)) )
        retval = 4;
    else if( !strncmp(string, "FC", sizeof(string)) )
        retval = 3;
    else if( !strncmp(string, "FR", sizeof(string)) )
        retval = 2;
    else if( !strncmp(string, "FL", sizeof(string)) )
        retval = 1;

    return retval;
}

int main(int argc, char **argv)
{
    int option_index = 0;
    int c,i;
    int ch = 2;
    int rate = 44100;
    char *mmap = "N";
    char *device = "hw:0,0";
    char *filename;
    char *ptr;
    int rc = 0;

    if (argc <2) {
          printf("\nUsage: aplay [options] <file>\n"
                "options:\n"
                "-D <hw:C,D>  -- Alsa PCM by name\n"
                "-M   -- Mmap stream\n"
                "-P   -- Hostless steam[No PCM]\n"
    "-C             -- Channels\n"
    "-R             -- Rate\n"
                "-V   -- verbose\n"
    "-F             -- Format\n"
                "-B             -- Period\n"
                "-T <MP3, AAC, AC3_PASS_THROUGH>  -- Compressed\n"
                "-X <\"FL,FR,FC,Ls,Rs,LFE\" for 5.1 configuration\n"
                "     supported channels: \n"
                "     FL, FR, FC, LS, RS, LFE, CS, TS \n"
                "     LB, RB, FLC, FRC, RLC, RRC, CVH, MS\n"
                "<file> \n");
           fprintf(stderr, "Formats Supported:\n");
           for (i = 0; i <= SNDRV_PCM_FORMAT_LAST; ++i)
               if (get_format_name(i))
                   fprintf(stderr, "%s ", get_format_name(i));
           fprintf(stderr, "\nSome of these may not be available on selected hardware\n");
           return 0;
     }
     while ((c = getopt_long(argc, argv, "PVMD:R:C:F:B:T:X:", long_options, &option_index)) != -1) {
       switch (c) {
       case 'P':
          pcm_flag = 0;
          break;
       case 'V':
          debug = 1;
          break;
       case 'M':
          mmap = "M";
          break;
       case 'D':
          device = optarg;
          break;
       case 'R':
          rate = (int)strtol(optarg, NULL, 0);
          break;
       case 'C':
          ch = (int)strtol(optarg, NULL, 0);
          break;
       case 'F':
          printf("optarg = %s\n", optarg);
          format = get_format(optarg);
          break;
       case 'B':
          period = (int)strtol(optarg, NULL, 0);
          break;
       case 'T':
          compressed = 1;
          printf("compressed codec type requested = %s\n", optarg);
          compr_codec = optarg;
          break;
       case 'X':
          set_channel_map = 1; i = 0;
          memset(channel_map, 0, sizeof(channel_map));
          ptr = strtok(optarg, ",");
          while((ptr != NULL) && (i < sizeof(channel_map))) {
              channel_map[i] = get_channel_map_val(ptr);
              if (channel_map[i] < 0 || channel_map[i] > 16) {
                  set_channel_map = 0;
                  break;
              }
              ptr = strtok(NULL,","); i++;
          }
          break;
       default:
          printf("\nUsage: aplay [options] <file>\n"
                "options:\n"
                "-D <hw:C,D>  -- Alsa PCM by name\n"
                "-M   -- Mmap stream\n"
                "-P   -- Hostless steam[No PCM]\n"
                "-V   -- verbose\n"
                "-C   -- Channels\n"
    "-R             -- Rate\n"
    "-F             -- Format\n"
                "-B             -- Period\n"
                "-T             -- Compressed\n"
                "-X <\"FL,FR,FC,Ls,Rs,LFE\" for 5.1 configuration\n"
                "     supported channels: \n"
                "     FL, FR, FC, LS, RS, LFE, CS, TS \n"
                "     LB, RB, FLC, FRC, RLC, RRC, CVH, MS\n"
                "<file> \n");
           fprintf(stderr, "Formats Supported:\n");
           for (i = 0; i < SNDRV_PCM_FORMAT_LAST; ++i)
               if (get_format_name(i))
                   fprintf(stderr, "%s ", get_format_name(i));
           fprintf(stderr, "\nSome of these may not be available on selected hardware\n");
          return -EINVAL;
       }

    }
    filename = (char*) calloc(1, 128);
    if (!filename) {
          fprintf(stderr, "Aplay:Failed to allocate filename!");
          return -ENOMEM;
    }
    if (optind > argc - 1) {
       free(filename);
       filename = NULL;
    } else {
       strlcpy(filename, argv[optind++], 128);
    }

    if (pcm_flag) {
        if (format == SNDRV_PCM_FORMAT_S16_LE || format == SNDRV_PCM_FORMAT_S24_LE)
             rc = play_wav(mmap, rate, ch, device, filename);
         else
             rc = play_raw(mmap, rate, ch, device, filename);
    } else {
        rc = play_wav(mmap, rate, ch, device, "dummy");
    }
    if (filename)
        free(filename);

    return rc;
}


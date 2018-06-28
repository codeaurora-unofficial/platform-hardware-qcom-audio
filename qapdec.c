#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h>
#include <assert.h>
#include <signal.h>
#include <pthread.h>

#include <dolby_ms12.h>
#include <dts_m8.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

#define print(l, msg, ...)						\
	do {								\
		if (debug_level >= l)					\
			fprintf(stderr, msg, ##__VA_ARGS__);		\
	} while (0)

#define err(msg, ...) \
	print(1, "error: " msg "\n", ##__VA_ARGS__)

#define info(msg, ...) \
	print(2, msg "\n", ##__VA_ARGS__)

#define dbg(msg, ...) \
	print(3, msg "\n", ##__VA_ARGS__)

#define av_err(errnum, fmt, ...) \
	err(fmt ": %s", ##__VA_ARGS__, av_err2str(errnum))

#define CASESTR(ENUM) case ENUM: return #ENUM;

#define ARRAY_SIZE(x)	(sizeof (x) / sizeof (*(x)))

#ifndef QAP_LIB_DTS_M8
# define QAP_LIB_DTS_M8 "libdts_m8_wrapper.so"
#endif

#ifndef QAP_LIB_DOLBY_MS12
# define QAP_LIB_DOLBY_MS12 "/usr/lib64/libdolby_ms12_wrapper.so"
#endif

#define AUDIO_OUTPUT_ID_BASE 0x100

static AVFormatContext *avctx;
static AVStream *avstream;

qap_lib_handle_t qap_lib;
qap_session_handle_t qap_session;
qap_module_handle_t qap_module;

bool wrote_wav_header;
int debug_level = 1;
volatile bool quit;

static int wav_channel_count;
static int wav_channel_offset[QAP_AUDIO_MAX_CHANNELS];
static int wav_block_size;

static pthread_mutex_t qap_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qap_cond = PTHREAD_COND_INITIALIZER;
static bool qap_eos_received;
static bool qap_buffer_full;
static int qap_input_sample_rate;

static FILE *output_stream;

#define WAV_SPEAKER_FRONT_LEFT			0x1
#define WAV_SPEAKER_FRONT_RIGHT			0x2
#define WAV_SPEAKER_FRONT_CENTER		0x4
#define WAV_SPEAKER_LOW_FREQUENCY		0x8
#define WAV_SPEAKER_BACK_LEFT			0x10
#define WAV_SPEAKER_BACK_RIGHT			0x20
#define WAV_SPEAKER_FRONT_LEFT_OF_CENTER	0x40
#define WAV_SPEAKER_FRONT_RIGHT_OF_CENTER	0x80
#define WAV_SPEAKER_BACK_CENTER			0x100
#define WAV_SPEAKER_SIDE_LEFT			0x200
#define WAV_SPEAKER_SIDE_RIGHT			0x400
#define WAV_SPEAKER_TOP_CENTER			0x800
#define WAV_SPEAKER_TOP_FRONT_LEFT		0x1000
#define WAV_SPEAKER_TOP_FRONT_CENTER		0x2000
#define WAV_SPEAKER_TOP_FRONT_RIGHT		0x4000
#define WAV_SPEAKER_TOP_BACK_LEFT		0x8000
#define WAV_SPEAKER_TOP_BACK_CENTER		0x10000
#define WAV_SPEAKER_TOP_BACK_RIGHT		0x20000
#define WAV_SPEAKER_INVALID			0

#define packed_struct struct __attribute__((packed))

packed_struct wav_header {
	uint8_t			riff_magic[4];
	uint32_t		riff_chunk_size;
	uint8_t			wave_magic[4];
	packed_struct {
		uint8_t		chunk_label[4];
		uint32_t	chunk_size;
		uint16_t	audio_format;
		uint16_t	num_channels;
		uint32_t	sample_rate;
		uint32_t	byte_rate;
		uint16_t	block_align;
		uint16_t	bits_per_sample;
		uint16_t	cb_size;
		packed_struct {
			uint16_t	valid_bits_per_sample;
			uint32_t	channel_mask;
			uint8_t		sub_format[16];
		} ext;
	} fmt;
	packed_struct {
		uint8_t		chunk_label[4];
		uint32_t	chunk_size;
	} data;
};

static struct {
	uint32_t wav_channel;
	uint8_t qap_channel;
} wav_channel_table[] = {
	/* keep in order of wav channels in pcm sample */
	{ WAV_SPEAKER_FRONT_LEFT, QAP_AUDIO_PCM_CHANNEL_L },
	{ WAV_SPEAKER_FRONT_RIGHT, QAP_AUDIO_PCM_CHANNEL_R },
	{ WAV_SPEAKER_FRONT_CENTER, QAP_AUDIO_PCM_CHANNEL_C },
	{ WAV_SPEAKER_LOW_FREQUENCY, QAP_AUDIO_PCM_CHANNEL_LFE },
	{ WAV_SPEAKER_BACK_LEFT, QAP_AUDIO_PCM_CHANNEL_LS },
	{ WAV_SPEAKER_BACK_RIGHT, QAP_AUDIO_PCM_CHANNEL_RS },
	{ WAV_SPEAKER_BACK_LEFT, QAP_AUDIO_PCM_CHANNEL_LB },
	{ WAV_SPEAKER_BACK_RIGHT, QAP_AUDIO_PCM_CHANNEL_RB },
	{ WAV_SPEAKER_FRONT_LEFT_OF_CENTER, QAP_AUDIO_PCM_CHANNEL_FLC },
	{ WAV_SPEAKER_FRONT_RIGHT_OF_CENTER, QAP_AUDIO_PCM_CHANNEL_FRC },
	{ WAV_SPEAKER_BACK_CENTER, QAP_AUDIO_PCM_CHANNEL_CS },
	{ WAV_SPEAKER_SIDE_LEFT, QAP_AUDIO_PCM_CHANNEL_SL },
	{ WAV_SPEAKER_SIDE_RIGHT, QAP_AUDIO_PCM_CHANNEL_SR },
	{ WAV_SPEAKER_TOP_CENTER, QAP_AUDIO_PCM_CHANNEL_TC },
	{ WAV_SPEAKER_TOP_FRONT_LEFT, QAP_AUDIO_PCM_CHANNEL_TFL },
	{ WAV_SPEAKER_TOP_FRONT_CENTER, QAP_AUDIO_PCM_CHANNEL_TFC },
	{ WAV_SPEAKER_TOP_FRONT_RIGHT, QAP_AUDIO_PCM_CHANNEL_TFR },
	{ WAV_SPEAKER_TOP_BACK_LEFT, QAP_AUDIO_PCM_CHANNEL_TBL },
	{ WAV_SPEAKER_TOP_BACK_CENTER, QAP_AUDIO_PCM_CHANNEL_TBC },
	{ WAV_SPEAKER_TOP_BACK_RIGHT, QAP_AUDIO_PCM_CHANNEL_TBR },
};

static int write_header(FILE *out, qap_output_config_t *cfg)
{
	struct wav_header hdr;
	uint32_t channel_mask = 0;

	for (unsigned i = 0; i < ARRAY_SIZE(wav_channel_table); i++) {
		uint8_t qap_ch = wav_channel_table[i].qap_channel;
		uint32_t wav_ch = wav_channel_table[i].wav_channel;

		for (int pos = 0; pos < cfg->channels; pos++) {
			if (cfg->ch_map[pos] == qap_ch) {
				wav_channel_offset[wav_channel_count++] =
					pos * cfg->bit_width / 8;
				channel_mask |= wav_ch;
			}
		}
	}

	if (wav_channel_count != cfg->channels) {
		fprintf(stderr, "dropping %d channels from output",
			cfg->channels - wav_channel_count);
	}

	memcpy(&hdr.riff_magic, "RIFF", 4);
	hdr.riff_chunk_size = 0xffffffff;
	memcpy(&hdr.wave_magic, "WAVE", 4);

	memcpy(&hdr.fmt.chunk_label, "fmt ", 4);
	hdr.fmt.chunk_size = sizeof (hdr.fmt) - 8;
	hdr.fmt.audio_format = 0xfffe; // WAVE_FORMAT_EXTENSIBLE
	hdr.fmt.num_channels = wav_channel_count;
	hdr.fmt.sample_rate = cfg->sample_rate;
	hdr.fmt.byte_rate = cfg->sample_rate * wav_channel_count *
		cfg->bit_width / 8;
	hdr.fmt.block_align = wav_channel_count * cfg->bit_width / 8;
	hdr.fmt.bits_per_sample = cfg->bit_width;

	hdr.fmt.cb_size = sizeof (hdr.fmt.ext);
	hdr.fmt.ext.valid_bits_per_sample = cfg->bit_width;
	hdr.fmt.ext.channel_mask = channel_mask;
	memcpy(&hdr.fmt.ext.sub_format,
	       "\x01\x00\x00\x00\x00\x00\x10\x00\x80\x00\x00\xAA\x00\x38\x9B\x71",
	       16);

	memcpy(&hdr.data.chunk_label, "data", 4);
	hdr.data.chunk_size = 0xffffffff;

	if (fwrite(&hdr, sizeof (hdr), 1, out) != 1) {
		fprintf(stderr, "failed to write wav header\n");
		return -1;
	}

	wav_block_size = hdr.fmt.block_align;

	return 0;
}

static int write_buffer(FILE *out, qap_buffer_common_t *buffer)
{
	const uint8_t *src;
	int sample_size;
	int n_blocks;

	src = buffer->data;

	assert(buffer->size % wav_block_size == 0);
	n_blocks = buffer->size / wav_block_size;
	sample_size = wav_block_size / wav_channel_count;

	for (int i = 0; i < n_blocks; i++) {
		for (int ch = 0; ch < wav_channel_count; ch++) {
			fwrite(src + wav_channel_offset[ch],
			       sample_size, 1, out);
		}
		src += wav_block_size;
	}

	return 0;
}

static const char *audio_format_to_str(qap_audio_format_t format)
{
	switch (format) {
	CASESTR(QAP_AUDIO_FORMAT_PCM_16_BIT)
	CASESTR(QAP_AUDIO_FORMAT_PCM_8_24_BIT)
	CASESTR(QAP_AUDIO_FORMAT_PCM_24_BIT_PACKED)
	CASESTR(QAP_AUDIO_FORMAT_PCM_32_BIT)
	CASESTR(QAP_AUDIO_FORMAT_AC3)
	CASESTR(QAP_AUDIO_FORMAT_AC4)
	CASESTR(QAP_AUDIO_FORMAT_EAC3)
	CASESTR(QAP_AUDIO_FORMAT_AAC)
	CASESTR(QAP_AUDIO_FORMAT_AAC_ADTS)
	CASESTR(QAP_AUDIO_FORMAT_MP2)
	CASESTR(QAP_AUDIO_FORMAT_MP3)
	CASESTR(QAP_AUDIO_FORMAT_FLAC)
	CASESTR(QAP_AUDIO_FORMAT_ALAC)
	CASESTR(QAP_AUDIO_FORMAT_APE)
	CASESTR(QAP_AUDIO_FORMAT_DTS)
	CASESTR(QAP_AUDIO_FORMAT_DTS_HD)
	}
	return "unknown";
}

static const char *audio_channel_to_str(qap_pcm_chmap channel)
{
	switch (channel) {
	case QAP_AUDIO_PCM_CHANNEL_L: return "L";
	case QAP_AUDIO_PCM_CHANNEL_R: return "R";
	case QAP_AUDIO_PCM_CHANNEL_C: return "C";
	case QAP_AUDIO_PCM_CHANNEL_LS: return "LS";
	case QAP_AUDIO_PCM_CHANNEL_RS: return "RS";
	case QAP_AUDIO_PCM_CHANNEL_LFE: return "LFE";
	case QAP_AUDIO_PCM_CHANNEL_CS: return "CS";
	case QAP_AUDIO_PCM_CHANNEL_LB: return "LB";
	case QAP_AUDIO_PCM_CHANNEL_RB: return "RB";
	case QAP_AUDIO_PCM_CHANNEL_TS: return "TS";
	case QAP_AUDIO_PCM_CHANNEL_CVH: return "CVH";
	case QAP_AUDIO_PCM_CHANNEL_MS: return "MS";
	case QAP_AUDIO_PCM_CHANNEL_FLC: return "FLC";
	case QAP_AUDIO_PCM_CHANNEL_FRC: return "FRC";
	case QAP_AUDIO_PCM_CHANNEL_RLC: return "RLC";
	case QAP_AUDIO_PCM_CHANNEL_RRC: return "RRC";
	case QAP_AUDIO_PCM_CHANNEL_LFE2: return "LFE2";
	case QAP_AUDIO_PCM_CHANNEL_SL: return "SL";
	case QAP_AUDIO_PCM_CHANNEL_SR: return "SR";
	case QAP_AUDIO_PCM_CHANNEL_TFL: return "TFL";
	case QAP_AUDIO_PCM_CHANNEL_TFR: return "TFR";
	case QAP_AUDIO_PCM_CHANNEL_TC: return "TC";
	case QAP_AUDIO_PCM_CHANNEL_TBL: return "TBL";
	case QAP_AUDIO_PCM_CHANNEL_TBR: return "TBR";
	case QAP_AUDIO_PCM_CHANNEL_TSL: return "TSL";
	case QAP_AUDIO_PCM_CHANNEL_TSR: return "TSR";
	case QAP_AUDIO_PCM_CHANNEL_TBC: return "TBC";
	case QAP_AUDIO_PCM_CHANNEL_BFC: return "BFC";
	case QAP_AUDIO_PCM_CHANNEL_BFL: return "BFL";
	case QAP_AUDIO_PCM_CHANNEL_BFR: return "BFR";
	case QAP_AUDIO_PCM_CHANNEL_LW: return "LW";
	case QAP_AUDIO_PCM_CHANNEL_RW: return "RW";
	case QAP_AUDIO_PCM_CHANNEL_LSD: return "LSD";
	case QAP_AUDIO_PCM_CHANNEL_RSD: return "RSD";
	}
	return "??";
}

static const char *audio_chmap_to_str(int channels, uint8_t *map)
{
	static char buf[256];
	int len = sizeof (buf);
	int offset = 0;

	for (int i = 0; i < channels && offset < len; i++) {
		int r = snprintf(buf + offset, len - offset, "%s%s",
				 audio_channel_to_str(map[i]),
				 i == channels  - 1 ? "" : ",");
		if (r > 0)
			offset += r;
	}

	buf[offset] = '\0';

	return buf;
}

static void handle_buffer(qap_audio_buffer_t *buffer)
{
	dbg("qap: output 0x%x pcm buffer size=%u pts=%" PRIi64,
	    buffer->buffer_parms.output_buf_params.output_id,
	    buffer->common_params.size, buffer->common_params.timestamp);

	if (buffer->buffer_parms.output_buf_params.output_id == AUDIO_OUTPUT_ID_BASE) {
		assert(wrote_wav_header);

		write_buffer(output_stream, &buffer->common_params);
	}
}

static void handle_output_config(qap_output_buff_params_t *out_buffer)
{
	qap_output_config_t *cfg = &out_buffer->output_config;

	info("qap: output 0x%x config: id=0x%x format=%s sr=%d ss=%d "
	     "interleaved=%d channels=%d chmap[%s]",
	     out_buffer->output_id, cfg->id,
	     audio_format_to_str(cfg->format),
	     cfg->sample_rate, cfg->bit_width, cfg->is_interleaved,
	     cfg->channels, audio_chmap_to_str(cfg->channels, cfg->ch_map));

	if (!cfg->sample_rate) {
		assert(qap_input_sample_rate != 0);
		cfg->sample_rate = qap_input_sample_rate;
	}

	if (!wrote_wav_header && out_buffer->output_id == AUDIO_OUTPUT_ID_BASE) {
		write_header(output_stream, cfg);
		wrote_wav_header = true;
	}
}

static void handle_qap_session_event(qap_session_handle_t session, void *priv,
				     qap_callback_event_t event_id,
				     int size, void *data)
{
	switch (event_id) {
	case QAP_CALLBACK_EVENT_DATA:
		if (size != sizeof (qap_audio_buffer_t)) {
			err("QAP_CALLBACK_EVENT_DATA "
			    "size=%d expected=%zu", size,
			    sizeof (qap_audio_buffer_t));
		}
		handle_buffer((qap_audio_buffer_t *)data);
		break;
	case QAP_CALLBACK_EVENT_OUTPUT_CFG_CHANGE:
		if (size != sizeof (qap_audio_buffer_t)) {
			err("QAP_CALLBACK_EVENT_OUTPUT_CFG_CHANGE "
			    "size=%d expected=%zu", size,
			    sizeof (qap_audio_buffer_t));
		}
		handle_output_config(&((qap_audio_buffer_t *)data)->buffer_parms.output_buf_params);
		break;
	case QAP_CALLBACK_EVENT_EOS:
		info("qap: EOS for primary");
		pthread_mutex_lock(&qap_lock);
		qap_eos_received = true;
		pthread_cond_signal(&qap_cond);
		pthread_mutex_unlock(&qap_lock);
		break;
	case QAP_CALLBACK_EVENT_MAIN_2_EOS:
		info("qap: EOS for secondary");
		break;
	case QAP_CALLBACK_EVENT_EOS_ASSOC:
		info("qap: EOS for assoc");
		break;
	case QAP_CALLBACK_EVENT_ERROR:
		info("qap: error");
		break;
	case QAP_CALLBACK_EVENT_SUCCESS:
		info("qap: success");
		break;
	case QAP_CALLBACK_EVENT_METADATA:
		info("qap: metadata");
		break;
	default:
		err("unknown QAP session event %u", event_id);
		break;
	}
}

static void handle_input_config(qap_input_config_t *cfg)
{
	info("qap: input stream format sr=%u ss=%u channels=%u",
	     cfg->sample_rate, cfg->bit_width, cfg->channels);

	qap_input_sample_rate = cfg->sample_rate;
}

static void handle_qap_module_event(qap_module_handle_t module, void *priv,
				    qap_module_callback_event_t event_id,
				    int size, void *data)
{
	switch (event_id) {
	case QAP_MODULE_CALLBACK_EVENT_SEND_INPUT_BUFFER:
		if (size != sizeof (qap_send_buffer_t)) {
			err("QAP_MODULE_CALLBACK_EVENT_SEND_INPUT_BUFFER "
			    "size=%d expected=%zu", size,
			    sizeof (qap_send_buffer_t));
		} else {
			qap_send_buffer_t *buf = data;
			dbg("qap: %u bytes avail", buf->bytes_available);
		}
		pthread_mutex_lock(&qap_lock);
		qap_buffer_full = false;
		pthread_cond_signal(&qap_cond);
		pthread_mutex_unlock(&qap_lock);
		break;
	case QAP_MODULE_CALLBACK_EVENT_INPUT_CFG_CHANGE:
		if (size != sizeof (qap_input_config_t)) {
			err("QAP_MODULE_CALLBACK_EVENT_INPUT_CFG_CHANGE "
			    "size=%d expected=%zu", size,
			    sizeof (qap_input_config_t));
		}
		handle_input_config((qap_input_config_t *)data);
		break;
	default:
		err("unknown QAP module event %u", event_id);
		break;
	}
}

static int wait_buffer_available(qap_module_handle_t module)
{
	pthread_mutex_lock(&qap_lock);
	while (qap_buffer_full)
		pthread_cond_wait(&qap_cond, &qap_lock);
	pthread_mutex_unlock(&qap_lock);

	return 0;
}

static int get_av_log_level(void)
{
	if (debug_level >= 5)
		return AV_LOG_TRACE;
	if (debug_level >= 4)
		return AV_LOG_DEBUG;
	if (debug_level >= 3)
		return AV_LOG_VERBOSE;
	if (debug_level >= 2)
		return AV_LOG_INFO;
	if (debug_level >= 1)
		return AV_LOG_ERROR;
	return AV_LOG_QUIET;
}

static void usage(void)
{
	fprintf(stderr, "usage: qapdec [OPTS] <input>\n"
		"Where OPTS is a combination of:\n"
		"  -s <stream>    audio stream number to decode\n"
		"  -o <filename>  output data to file instead of stdout\n"
		"  -v             increase debug verbosity\n"
		"  -c <channels>  maximum number of channels to output\n"
		"  -k <kvpairs>   pass kvpairs string to the decoder backend\n"
		"\n");
}

static void handle_quit(int sig)
{
	quit = true;
}

int main(int argc, char **argv)
{
	const char *url;
	int opt;
	int ret;
	int stream_index = -1;
	int channels;
	int max_channels[3] = { -1, -1, -1 };
	int num_outputs = 0;
	char *kvpairs = NULL;
	const char *qap_lib_name;
	size_t written = 0;
	qap_audio_format_t qap_format;
	qap_module_config_t qap_mod_cfg;
	qap_session_outputs_config_t qap_session_cfg;
	qap_audio_buffer_t qap_buffer;
	AVPacket pkt;

	output_stream = stdout;

	while ((opt = getopt(argc, argv, "c:hk:o:s:v")) != -1) {
		switch (opt) {
		case 'c':
			channels = atoi(optarg);
			if (channels <= 0 || channels > 8) {
				err("invalid number of channels %s", optarg);
				usage();
				return 1;
			}
			if (num_outputs >= 3) {
				err("too many outputs");
				usage();
				return 1;
			}
			max_channels[num_outputs++] = channels;
			break;
		case 'v':
			debug_level++;
			break;
		case 'k':
			kvpairs = optarg;
			break;
		case 's':
			stream_index = atoi(optarg);
			break;
		case 'o':
			output_stream = fopen(optarg, "w");
			if (!output_stream) {
				err("cannot open file `%s' for writing: %m",
				    optarg);
				return 1;
			}
			break;
		case 'h':
			usage();
			return 0;
		default:
			err("unknown option %c", opt);
			usage();
			return 1;
		}
	}

	if (num_outputs == 0) {
		num_outputs = 1;
		max_channels[0] = 8;
	}

	if (optind >= argc) {
		err("missing url to play\n");
		usage();
		return 1;
	}

	url = argv[optind];

	av_log_set_level(get_av_log_level());
	av_register_all();
	avformat_network_init();

	info("QAP library version %u", qap_get_version());

	ret = avformat_open_input(&avctx, url, NULL, NULL);
	if (ret < 0) {
		av_err(ret, "failed to open %s", url);
		return 1;
	}

	ret = avformat_find_stream_info(avctx, NULL);
	if (ret < 0) {
		av_err(ret, "failed to get streams info");
		return 1;
	}

	av_dump_format(avctx, -1, url, 0);

	ret = av_find_best_stream(avctx, AVMEDIA_TYPE_AUDIO, stream_index, -1,
				  NULL, 0);
	if (ret < 0) {
		av_err(ret, "stream does not seem to contain audio");
		return 1;
	}

	avstream = avctx->streams[ret];

	switch (avstream->codecpar->codec_id) {
	case AV_CODEC_ID_AC3:
		qap_lib_name = QAP_LIB_DOLBY_MS12;
		qap_format = QAP_AUDIO_FORMAT_AC3;
		break;
	case AV_CODEC_ID_EAC3:
		qap_lib_name = QAP_LIB_DOLBY_MS12;
		qap_format = QAP_AUDIO_FORMAT_EAC3;
		break;
	case AV_CODEC_ID_AAC:
	case AV_CODEC_ID_AAC_LATM:
		qap_lib_name = QAP_LIB_DOLBY_MS12;
		qap_format = QAP_AUDIO_FORMAT_AAC_ADTS;
		break;
	case AV_CODEC_ID_DTS:
		qap_lib_name = QAP_LIB_DTS_M8;
		qap_format = QAP_AUDIO_FORMAT_DTS;
		break;
	default:
		err("cannot decode %s format",
		    avcodec_get_name(avstream->codecpar->codec_id));
		return 1;
	}

	qap_lib = qap_load_library(qap_lib_name);
	if (!qap_lib) {
		err("qap: failed to load library %s", qap_lib_name);
		return 1;
	}

	qap_session = qap_session_open(QAP_SESSION_BROADCAST, qap_lib);
	if (!qap_session) {
		err("qap: failed to open decode session");
		return 1;
	}

	qap_session_set_callback(qap_session, handle_qap_session_event, NULL);

	memset(&qap_session_cfg, 0, sizeof (qap_session_cfg));
	qap_session_cfg.num_output = num_outputs;
	for (int i = 0; i < num_outputs; i++) {
		qap_session_cfg.output_config[i].id = AUDIO_OUTPUT_ID_BASE + i;
		qap_session_cfg.output_config[i].channels = max_channels[i];
	}

	ret = qap_session_cmd(qap_session, QAP_SESSION_CMD_SET_OUTPUTS,
			      sizeof (qap_session_cfg), &qap_session_cfg,
			      NULL, NULL);
	if (ret) {
		err("qap: QAP_SESSION_CMD_SET_CONFIG command failed");
		return 1;
	}

	if (kvpairs) {
		ret = qap_session_cmd(qap_session, QAP_SESSION_CMD_SET_KVPAIRS,
				      strlen(kvpairs) + 1, kvpairs, NULL, NULL);
		if (ret) {
			err("qap: QAP_SESSION_CMD_SET_KVPAIRS command failed");
			return 1;
		}
	}

	memset(&qap_mod_cfg, 0, sizeof (qap_mod_cfg));
	qap_mod_cfg.module_type = QAP_MODULE_DECODER;
	qap_mod_cfg.flags = QAP_MODULE_FLAG_PRIMARY;
	qap_mod_cfg.format = qap_format;

	if (qap_module_init(qap_session, &qap_mod_cfg, &qap_module)) {
		err("qap: failed to init module");
		return 1;
	}

	if (qap_module_set_callback(qap_module, handle_qap_module_event, NULL)) {
		err("qap: failed to set module callback");
		return 1;
	}

	ret = qap_module_cmd(qap_module, QAP_MODULE_CMD_START,
			     0, NULL, NULL, NULL);
	if (ret) {
		err("qap: QAP_SESSION_CMD_START command failed");
		return 1;
	}

	av_init_packet(&pkt);

	signal(SIGINT, handle_quit);
	signal(SIGTERM, handle_quit);

	while (!quit) {
		AVRational av_timebase = avstream->time_base;
		AVRational qap_timebase = { 1, 1000000 };

		ret = av_read_frame(avctx, &pkt);
		if (ret == AVERROR_EOF) {
			break;
		}

		if (ret < 0) {
			av_err(ret, "failed to read frame from input");
			return ret;
		}

		if (pkt.stream_index != avstream->index) {
			av_packet_unref(&pkt);
			continue;
		}

		memset(&qap_buffer, 0, sizeof (qap_buffer));
		qap_buffer.common_params.data = pkt.data;
		qap_buffer.common_params.size = pkt.size;
		qap_buffer.common_params.offset = 0;

		if (pkt.pts == AV_NOPTS_VALUE) {
			qap_buffer.common_params.timestamp = 0;
			qap_buffer.buffer_parms.input_buf_params.flags =
				QAP_BUFFER_NO_TSTAMP;
		} else {
			qap_buffer.common_params.timestamp =
				av_rescale_q(pkt.pts, av_timebase, qap_timebase);
			qap_buffer.buffer_parms.input_buf_params.flags =
				QAP_BUFFER_TSTAMP;
		}

		dbg(" in: buffer size=%d pts=%" PRIi64 " -> %" PRIi64,
		    pkt.size, pkt.pts, qap_buffer.common_params.timestamp);

		while (qap_buffer.common_params.offset <
		       qap_buffer.common_params.size) {

			pthread_mutex_lock(&qap_lock);
			qap_buffer_full = true;
			pthread_mutex_unlock(&qap_lock);

			ret = qap_module_process(qap_module, &qap_buffer);
			if (ret < 0) {
				if (wait_buffer_available(qap_module) < 0)
					return 1;
			} else if (ret == 0) {
				err("decoder returned zero size");
				return 1;
			} else {
				qap_buffer.common_params.offset += ret;
				dbg("dec: written %d bytes", ret);
			}
		}

		written += pkt.size;

		av_packet_unref(&pkt);
	}

	memset(&qap_buffer, 0, sizeof (qap_buffer));
	qap_buffer.buffer_parms.input_buf_params.flags = QAP_BUFFER_EOS;
	qap_module_process(qap_module, &qap_buffer);

	ret = qap_module_cmd(qap_module, QAP_MODULE_CMD_STOP,
			     0, NULL, NULL, NULL);
	if (ret) {
		err("qap: QAP_MODULE_CMD_STOP command failed");
		return 1;
	}

	// wait eos
	pthread_mutex_lock(&qap_lock);
	while (!qap_eos_received)
		pthread_cond_wait(&qap_cond, &qap_lock);
	pthread_mutex_unlock(&qap_lock);

	info("written %zu bytes", written);

	if (qap_session_close(qap_session))
		err("qap: failed to close session");

	if (qap_unload_library(qap_lib))
		err("qap: failed to unload library");

	if (output_stream != stdout)
		fclose(output_stream);

	return 0;
}
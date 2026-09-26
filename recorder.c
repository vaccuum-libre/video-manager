#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>

#define RECORD_DURATION_SECONDS 180
#define DEFAULT_OUTPUT "recording.mp4"
#define DEFAULT_DEVICE "/dev/video0"
#define DEFAULT_ENCODER "libx264"
#define DEFAULT_WIDTH 1280
#define DEFAULT_HEIGHT 720
#define DEFAULT_FRAMERATE 30
#define DEFAULT_BITRATE 4000000LL

static volatile sig_atomic_t stop_requested;

#define MAX_DIMENSION 16384
#define MAX_FRAMERATE 240
#define MAX_BITRATE 1000000000000LL

typedef struct {
    const char *output_path;
    const char *device_path;
    const char *encoder_name;
    const char *input_pixel_format;
    int width;
    int height;
    int frame_rate;
    int64_t bitrate;
} Config;

typedef struct {
    AVFormatContext *input_format;
    AVCodecContext *decoder;
    int video_stream_index;
    AVRational input_time_base;

    AVFormatContext *output_format;
    AVCodecContext *encoder;
    AVStream *output_stream;
    int output_opened;
    int header_written;

    struct SwsContext *sws;
    AVFrame *decoded_frame;
    AVFrame *converted_frame;
    AVPacket *input_packet;
    AVPacket *output_packet;

    int64_t first_input_timestamp;
    int64_t last_encoder_pts;
    int64_t synthetic_frame_count;
    int warned_missing_timestamps;
} Recorder;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static int interrupt_callback(void *opaque)
{
    (void)opaque;
    return stop_requested != 0;
}

static void print_ffmpeg_error(const char *operation, int error)
{
    char message[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(error, message, sizeof(message));
    fprintf(stderr, "%s: %s\n", operation, message);
}

static void print_usage(FILE *stream, const char *program)
{
    fprintf(stream,
            "Usage: %s [OPTIONS]\n"
            "Record 180 seconds of camera video to an H.264 MP4 file.\n\n"
            "Options:\n"
            "  -o, --output FILE          Output file (default: %s)\n"
            "  -d, --device DEVICE        V4L2 camera device (default: %s)\n"
            "  -s, --size WIDTHxHEIGHT    Capture size (default: %dx%d)\n"
            "  -r, --framerate FPS        Capture frame rate (default: %d)\n"
            "  -b, --bitrate RATE         H.264 bitrate; K/M suffix allowed (default: 4M)\n"
            "  -p, --pixel-format FORMAT  V4L2 format, e.g. yuyv422 or mjpeg\n"
            "  -e, --encoder NAME         FFmpeg H.264 encoder (default: %s)\n"
            "  -h, --help                 Show this help\n\n"
            "Example:\n"
            "  %s -d /dev/video0 -s 1920x1080 -r 30 -p mjpeg -o clip.mp4\n",
            program, DEFAULT_OUTPUT, DEFAULT_DEVICE, DEFAULT_WIDTH, DEFAULT_HEIGHT,
            DEFAULT_FRAMERATE, DEFAULT_ENCODER, program);
}

static int parse_positive_int(const char *text, int *value)
{
    char *end = NULL;
    long parsed;

    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed <= 0 || parsed > INT_MAX)
        return -1;
    *value = (int)parsed;
    return 0;
}

static int parse_size(const char *text, int *width, int *height)
{
    char *end = NULL;
    long parsed_width;
    long parsed_height;

    errno = 0;
    parsed_width = strtol(text, &end, 10);
    if (errno != 0 || end == text || (*end != 'x' && *end != 'X') ||
        parsed_width <= 0 || parsed_width > INT_MAX)
        return -1;

    text = end + 1;
    errno = 0;
    parsed_height = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        parsed_height <= 0 || parsed_height > INT_MAX)
        return -1;

    *width = (int)parsed_width;
    *height = (int)parsed_height;
    return 0;
}

static int parse_bitrate(const char *text, int64_t *value)
{
    char *end = NULL;
    int64_t multiplier = 1;
    unsigned long long parsed;

    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || parsed == 0)
        return -1;

    if (*end == 'k' || *end == 'K') {
        multiplier = 1000;
        ++end;
    } else if (*end == 'm' || *end == 'M') {
        multiplier = 1000000;
        ++end;
    }

    if (*end != '\0' || parsed > (unsigned long long)(INT64_MAX / multiplier))
        return -1;

    *value = (int64_t)parsed * multiplier;
    return 0;
}

static int parse_arguments(int argc, char **argv, Config *config)
{
    static const struct option options[] = {
        {"output", required_argument, NULL, 'o'},
        {"device", required_argument, NULL, 'd'},
        {"size", required_argument, NULL, 's'},
        {"framerate", required_argument, NULL, 'r'},
        {"bitrate", required_argument, NULL, 'b'},
        {"pixel-format", required_argument, NULL, 'p'},
        {"encoder", required_argument, NULL, 'e'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    int option;

    while ((option = getopt_long(argc, argv, "o:d:s:r:b:p:e:h", options, NULL)) != -1) {
        switch (option) {
        case 'o':
            config->output_path = optarg;
            break;
        case 'd':
            config->device_path = optarg;
            break;
        case 's':
            if (parse_size(optarg, &config->width, &config->height) < 0) {
                fprintf(stderr, "Invalid size '%s'; expected WIDTHxHEIGHT\n", optarg);
                return -1;
            }
            break;
        case 'r':
            if (parse_positive_int(optarg, &config->frame_rate) < 0) {
                fprintf(stderr, "Invalid frame rate '%s'\n", optarg);
                return -1;
            }
            break;
        case 'b':
            if (parse_bitrate(optarg, &config->bitrate) < 0) {
                fprintf(stderr, "Invalid bitrate '%s'\n", optarg);
                return -1;
            }
            break;
        case 'p':
            config->input_pixel_format = optarg;
            break;
        case 'e':
            config->encoder_name = optarg;
            break;
        case 'h':
            print_usage(stdout, argv[0]);
            return 1;
        default:
            print_usage(stderr, argv[0]);
            return -1;
        }
    }

    if (optind != argc) {
        fprintf(stderr, "Unexpected argument: %s\n", argv[optind]);
        return -1;
    }
    if (!*config->output_path || !*config->device_path || !*config->encoder_name ||
        (config->input_pixel_format && !*config->input_pixel_format)) {
        fprintf(stderr, "Output, device, encoder, and pixel format values cannot be empty\n");
        return -1;
    }
    if (config->width > MAX_DIMENSION || config->height > MAX_DIMENSION ||
        config->width % 2 != 0 || config->height % 2 != 0) {
        fprintf(stderr, "Width and height must be even and no greater than %d\n",
                MAX_DIMENSION);
        return -1;
    }
    if (config->frame_rate > MAX_FRAMERATE) {
        fprintf(stderr, "Frame rate must not exceed %d fps\n", MAX_FRAMERATE);
        return -1;
    }
    if (config->bitrate > MAX_BITRATE) {
        fprintf(stderr, "Bitrate must not exceed 1T\n");
        return -1;
    }
    return 0;
}

static int set_dictionary_value(AVDictionary **dictionary, const char *key, const char *value)
{
    int result = av_dict_set(dictionary, key, value, 0);
    if (result < 0)
        print_ffmpeg_error("Could not allocate input option", result);
    return result;
}

static int open_input_device(Recorder *recorder, const Config *config)
{
    const AVInputFormat *input = av_find_input_format("v4l2");
    const AVCodec *decoder_codec;
    AVStream *stream;
    AVDictionary *options = NULL;
    char video_size[64];
    char frame_rate[32];
    int result;

    if (!input) {
        fprintf(stderr, "This FFmpeg build does not provide the v4l2 input device\n");
        return AVERROR_DEMUXER_NOT_FOUND;
    }

    recorder->input_format = avformat_alloc_context();
    if (!recorder->input_format)
        return AVERROR(ENOMEM);
    recorder->input_format->interrupt_callback.callback = interrupt_callback;

    snprintf(video_size, sizeof(video_size), "%dx%d", config->width, config->height);
    snprintf(frame_rate, sizeof(frame_rate), "%d", config->frame_rate);
    if ((result = set_dictionary_value(&options, "video_size", video_size)) < 0 ||
        (result = set_dictionary_value(&options, "framerate", frame_rate)) < 0)
        goto done;
    if (config->input_pixel_format &&
        (result = set_dictionary_value(&options, "input_format", config->input_pixel_format)) < 0)
        goto done;

    result = avformat_open_input(&recorder->input_format, config->device_path, input, &options);
    if (result < 0) {
        fprintf(stderr, "Could not open camera '%s'\n", config->device_path);
        print_ffmpeg_error("Camera open failed", result);
        goto done;
    }

    if (av_dict_count(options) != 0) {
        AVDictionaryEntry *entry = NULL;
        while ((entry = av_dict_get(options, "", entry, AV_DICT_IGNORE_SUFFIX)))
            fprintf(stderr, "Unsupported camera option: %s=%s\n", entry->key, entry->value);
        result = AVERROR_OPTION_NOT_FOUND;
        goto done;
    }

    result = avformat_find_stream_info(recorder->input_format, NULL);
    if (result < 0) {
        print_ffmpeg_error("Could not read camera stream information", result);
        goto done;
    }

    result = av_find_best_stream(recorder->input_format, AVMEDIA_TYPE_VIDEO, -1, -1,
                                 &decoder_codec, 0);
    if (result < 0) {
        print_ffmpeg_error("Camera has no usable video stream", result);
        goto done;
    }
    recorder->video_stream_index = result;
    stream = recorder->input_format->streams[recorder->video_stream_index];
    recorder->input_time_base = stream->time_base;

    if (stream->codecpar->width != config->width || stream->codecpar->height != config->height) {
        fprintf(stderr, "Camera negotiated %dx%d instead of requested %dx%d\n",
                stream->codecpar->width, stream->codecpar->height,
                config->width, config->height);
        result = AVERROR(EINVAL);
        goto done;
    }
    if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0 &&
        av_cmp_q(stream->avg_frame_rate, (AVRational){config->frame_rate, 1}) != 0) {
        fprintf(stderr, "Camera negotiated frame rate %d/%d instead of requested %d/1\n",
                stream->avg_frame_rate.num, stream->avg_frame_rate.den,
                config->frame_rate);
        result = AVERROR(EINVAL);
        goto done;
    }

    recorder->decoder = avcodec_alloc_context3(decoder_codec);
    if (!recorder->decoder) {
        result = AVERROR(ENOMEM);
        goto done;
    }
    result = avcodec_parameters_to_context(recorder->decoder, stream->codecpar);
    if (result < 0) {
        print_ffmpeg_error("Could not configure camera decoder", result);
        goto done;
    }
    result = avcodec_open2(recorder->decoder, decoder_codec, NULL);
    if (result < 0)
        print_ffmpeg_error("Could not open camera decoder", result);

done:
    av_dict_free(&options);
    return result;
}

static int encoder_supports_pixel_format(const AVCodec *codec, enum AVPixelFormat format)
{
    const enum AVPixelFormat *supported;

    if (!codec->pix_fmts)
        return 1;
    for (supported = codec->pix_fmts; *supported != AV_PIX_FMT_NONE; ++supported) {
        if (*supported == format)
            return 1;
    }
    return 0;
}

static int initialize_output(Recorder *recorder, const Config *config)
{
    const AVCodec *encoder_codec;
    int result;

    result = avformat_alloc_output_context2(&recorder->output_format, NULL, "mp4",
                                            config->output_path);
    if (result < 0 || !recorder->output_format) {
        if (result >= 0)
            result = AVERROR_UNKNOWN;
        print_ffmpeg_error("Could not create MP4 output context", result);
        return result;
    }

    encoder_codec = avcodec_find_encoder_by_name(config->encoder_name);
    if (!encoder_codec && strcmp(config->encoder_name, DEFAULT_ENCODER) == 0)
        encoder_codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!encoder_codec) {
        fprintf(stderr, "H.264 encoder '%s' is unavailable\n", config->encoder_name);
        return AVERROR_ENCODER_NOT_FOUND;
    }
    if (encoder_codec->id != AV_CODEC_ID_H264) {
        fprintf(stderr, "Encoder '%s' does not encode H.264\n", encoder_codec->name);
        return AVERROR(EINVAL);
    }
    if (!encoder_supports_pixel_format(encoder_codec, AV_PIX_FMT_YUV420P)) {
        fprintf(stderr, "Encoder '%s' does not support the required yuv420p pixel format\n",
                encoder_codec->name);
        return AVERROR(EINVAL);
    }

    recorder->encoder = avcodec_alloc_context3(encoder_codec);
    if (!recorder->encoder)
        return AVERROR(ENOMEM);

    recorder->encoder->codec_id = encoder_codec->id;
    recorder->encoder->codec_type = AVMEDIA_TYPE_VIDEO;
    recorder->encoder->width = config->width;
    recorder->encoder->height = config->height;
    recorder->encoder->pix_fmt = AV_PIX_FMT_YUV420P;
    recorder->encoder->time_base = (AVRational){1, config->frame_rate};
    recorder->encoder->framerate = (AVRational){config->frame_rate, 1};
    recorder->encoder->bit_rate = config->bitrate;
    recorder->encoder->gop_size = config->frame_rate * 2;
    recorder->encoder->max_b_frames = 0;

    if (recorder->output_format->oformat->flags & AVFMT_GLOBALHEADER)
        recorder->encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (strcmp(encoder_codec->name, "libx264") == 0) {
        result = av_opt_set(recorder->encoder->priv_data, "preset", "veryfast", 0);
        if (result < 0) {
            print_ffmpeg_error("Could not set libx264 preset", result);
            return result;
        }
        result = av_opt_set(recorder->encoder->priv_data, "tune", "zerolatency", 0);
        if (result < 0) {
            print_ffmpeg_error("Could not set libx264 tune", result);
            return result;
        }
    }

    result = avcodec_open2(recorder->encoder, encoder_codec, NULL);
    if (result < 0) {
        print_ffmpeg_error("Could not open H.264 encoder", result);
        return result;
    }

    recorder->output_stream = avformat_new_stream(recorder->output_format, NULL);
    if (!recorder->output_stream)
        return AVERROR(ENOMEM);
    recorder->output_stream->time_base = recorder->encoder->time_base;
    recorder->output_stream->avg_frame_rate = recorder->encoder->framerate;

    result = avcodec_parameters_from_context(recorder->output_stream->codecpar,
                                             recorder->encoder);
    if (result < 0) {
        print_ffmpeg_error("Could not copy encoder parameters", result);
        return result;
    }
    recorder->output_stream->codecpar->codec_tag = 0;

    if (!(recorder->output_format->oformat->flags & AVFMT_NOFILE)) {
        result = avio_open(&recorder->output_format->pb, config->output_path, AVIO_FLAG_WRITE);
        if (result < 0) {
            fprintf(stderr, "Could not open output file '%s'\n", config->output_path);
            print_ffmpeg_error("Output open failed", result);
            return result;
        }
        recorder->output_opened = 1;
    }

    result = avformat_write_header(recorder->output_format, NULL);
    if (result < 0) {
        print_ffmpeg_error("Could not write MP4 header", result);
        return result;
    }
    recorder->header_written = 1;
    return 0;
}

static int allocate_working_objects(Recorder *recorder, const Config *config)
{
    int result;

    recorder->decoded_frame = av_frame_alloc();
    recorder->converted_frame = av_frame_alloc();
    recorder->input_packet = av_packet_alloc();
    recorder->output_packet = av_packet_alloc();
    if (!recorder->decoded_frame || !recorder->converted_frame ||
        !recorder->input_packet || !recorder->output_packet)
        return AVERROR(ENOMEM);

    recorder->converted_frame->format = recorder->encoder->pix_fmt;
    recorder->converted_frame->width = config->width;
    recorder->converted_frame->height = config->height;
    result = av_frame_get_buffer(recorder->converted_frame, 32);
    if (result < 0)
        print_ffmpeg_error("Could not allocate conversion frame", result);
    return result;
}

static int encode_and_write(Recorder *recorder, AVFrame *frame)
{
    int result = avcodec_send_frame(recorder->encoder, frame);

    if (result < 0) {
        print_ffmpeg_error(frame ? "Could not send frame to encoder"
                                  : "Could not flush encoder", result);
        return result;
    }

    for (;;) {
        result = avcodec_receive_packet(recorder->encoder, recorder->output_packet);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
            return 0;
        if (result < 0) {
            print_ffmpeg_error("Could not receive encoded packet", result);
            return result;
        }

        av_packet_rescale_ts(recorder->output_packet, recorder->encoder->time_base,
                             recorder->output_stream->time_base);
        recorder->output_packet->stream_index = recorder->output_stream->index;
        result = av_interleaved_write_frame(recorder->output_format,
                                            recorder->output_packet);
        av_packet_unref(recorder->output_packet);
        if (result < 0) {
            print_ffmpeg_error("Could not write encoded packet", result);
            return result;
        }
    }
}

static int64_t frame_input_timestamp(Recorder *recorder, const AVFrame *frame)
{
    int64_t timestamp = frame->best_effort_timestamp;

    if (timestamp == AV_NOPTS_VALUE) {
        if (!recorder->warned_missing_timestamps) {
            fprintf(stderr, "Camera supplied no timestamps; deriving them from the configured frame rate\n");
            recorder->warned_missing_timestamps = 1;
        }
        timestamp = av_rescale_q(recorder->synthetic_frame_count,
                                 recorder->encoder->time_base,
                                 recorder->input_time_base);
    }
    ++recorder->synthetic_frame_count;
    return timestamp;
}

static int convert_and_encode_frame(Recorder *recorder, const Config *config, AVFrame *input)
{
    const int64_t input_timestamp = frame_input_timestamp(recorder, input);
    const int64_t duration = av_rescale_q(RECORD_DURATION_SECONDS, (AVRational){1, 1},
                                          recorder->input_time_base);
    int64_t relative_timestamp;
    int64_t encoder_pts;
    const int64_t end_encoder_pts =
        (int64_t)RECORD_DURATION_SECONDS * config->frame_rate;
    AVFrame *encoding_frame;
    int result;

    if (recorder->first_input_timestamp == AV_NOPTS_VALUE)
        recorder->first_input_timestamp = input_timestamp;
    relative_timestamp = input_timestamp - recorder->first_input_timestamp;
    if (relative_timestamp < 0) {
        fprintf(stderr, "Camera timestamp moved before the recording start\n");
        return AVERROR_INVALIDDATA;
    }
    if (relative_timestamp >= duration)
        return AVERROR_EOF;

    encoder_pts = av_rescale_q_rnd(relative_timestamp, recorder->input_time_base,
                                   recorder->encoder->time_base,
                                   AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
    if (encoder_pts <= recorder->last_encoder_pts)
        encoder_pts = recorder->last_encoder_pts + 1;
    if (encoder_pts >= end_encoder_pts)
        return AVERROR_EOF;
    recorder->last_encoder_pts = encoder_pts;

    if (input->width != config->width || input->height != config->height) {
        fprintf(stderr, "Camera frame changed size to %dx%d; expected %dx%d\n",
                input->width, input->height, config->width, config->height);
        return AVERROR(EINVAL);
    }

    if (input->format == recorder->encoder->pix_fmt) {
        encoding_frame = input;
    } else {
        result = av_frame_make_writable(recorder->converted_frame);
        if (result < 0) {
            print_ffmpeg_error("Could not reuse conversion frame", result);
            return result;
        }
        recorder->sws = sws_getCachedContext(recorder->sws,
                                             input->width, input->height,
                                             (enum AVPixelFormat)input->format,
                                             config->width, config->height,
                                             recorder->encoder->pix_fmt,
                                             SWS_BILINEAR, NULL, NULL, NULL);
        if (!recorder->sws) {
            const char *pixel_format =
                av_get_pix_fmt_name((enum AVPixelFormat)input->format);
            fprintf(stderr, "Could not initialize pixel format conversion from %s to yuv420p\n",
                    pixel_format ? pixel_format : "unknown");
            return AVERROR(EINVAL);
        }
        result = sws_scale(recorder->sws, (const uint8_t *const *)input->data,
                           input->linesize, 0, input->height,
                           recorder->converted_frame->data,
                           recorder->converted_frame->linesize);
        if (result != config->height) {
            fprintf(stderr, "Pixel format conversion produced %d of %d rows\n",
                    result, config->height);
            return result < 0 ? result : AVERROR_INVALIDDATA;
        }
        encoding_frame = recorder->converted_frame;
    }

    encoding_frame->pts = encoder_pts;
    return encode_and_write(recorder, encoding_frame);
}

static int receive_decoded_frames(Recorder *recorder, const Config *config, int *finished)
{
    int result;

    for (;;) {
        result = avcodec_receive_frame(recorder->decoder, recorder->decoded_frame);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
            return 0;
        if (result < 0) {
            print_ffmpeg_error("Could not decode camera frame", result);
            return result;
        }

        result = convert_and_encode_frame(recorder, config, recorder->decoded_frame);
        av_frame_unref(recorder->decoded_frame);
        if (result == AVERROR_EOF) {
            *finished = 1;
            return 0;
        }
        if (result < 0)
            return result;
    }
}

static int record_frames(Recorder *recorder, const Config *config)
{
    int finished = 0;
    int result;

    while (!stop_requested && !finished) {
        result = av_read_frame(recorder->input_format, recorder->input_packet);
        if (result == AVERROR(EAGAIN)) {
            av_usleep(10000);
            continue;
        }
        if (result == AVERROR(EINTR))
            continue;
        if (result == AVERROR_EXIT && stop_requested)
            break;
        if (result == AVERROR_EOF) {
            fprintf(stderr, "Camera stream ended before 180 seconds\n");
            return AVERROR_EOF;
        }
        if (result < 0) {
            print_ffmpeg_error("Could not read camera packet", result);
            return result;
        }

        if (recorder->input_packet->stream_index != recorder->video_stream_index) {
            av_packet_unref(recorder->input_packet);
            continue;
        }

        result = avcodec_send_packet(recorder->decoder, recorder->input_packet);
        av_packet_unref(recorder->input_packet);
        if (result < 0) {
            print_ffmpeg_error("Could not send camera packet to decoder", result);
            return result;
        }
        result = receive_decoded_frames(recorder, config, &finished);
        if (result < 0)
            return result;
    }

    return 0;
}

static int flush_encoder(Recorder *recorder)
{
    return encode_and_write(recorder, NULL);
}

static int cleanup_resources(Recorder *recorder)
{
    int result = 0;

    av_packet_free(&recorder->output_packet);
    av_packet_free(&recorder->input_packet);
    av_frame_free(&recorder->converted_frame);
    av_frame_free(&recorder->decoded_frame);
    sws_freeContext(recorder->sws);
    avcodec_free_context(&recorder->encoder);
    avcodec_free_context(&recorder->decoder);
    avformat_close_input(&recorder->input_format);

    if (recorder->output_format) {
        if (recorder->output_opened) {
            result = avio_closep(&recorder->output_format->pb);
            if (result < 0)
                print_ffmpeg_error("Could not close output file", result);
        }
        avformat_free_context(recorder->output_format);
    }
    return result;
}

int main(int argc, char **argv)
{
    Config config = {
        .output_path = DEFAULT_OUTPUT,
        .device_path = DEFAULT_DEVICE,
        .encoder_name = DEFAULT_ENCODER,
        .input_pixel_format = NULL,
        .width = DEFAULT_WIDTH,
        .height = DEFAULT_HEIGHT,
        .frame_rate = DEFAULT_FRAMERATE,
        .bitrate = DEFAULT_BITRATE,
    };
    Recorder recorder = {
        .video_stream_index = -1,
        .first_input_timestamp = AV_NOPTS_VALUE,
        .last_encoder_pts = -1,
    };
    struct sigaction action = {0};
    int result;
    int exit_code = EXIT_FAILURE;

    result = parse_arguments(argc, argv, &config);
    if (result > 0)
        return EXIT_SUCCESS;
    if (result < 0)
        return 2;

    action.sa_handler = handle_signal;
    if (sigemptyset(&action.sa_mask) < 0 ||
        sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0) {
        perror("Could not install signal handlers");
        return EXIT_FAILURE;
    }

    avdevice_register_all();

    result = open_input_device(&recorder, &config);
    if (result < 0)
        goto cleanup;
    result = initialize_output(&recorder, &config);
    if (result < 0)
        goto finalize;
    result = allocate_working_objects(&recorder, &config);
    if (result < 0) {
        print_ffmpeg_error("Could not allocate recording buffers", result);
        goto finalize;
    }

    fprintf(stderr, "Recording %dx%d at %d fps from %s to %s for 180 seconds\n",
            config.width, config.height, config.frame_rate,
            config.device_path, config.output_path);
    result = record_frames(&recorder, &config);
    {
        int flush_result = flush_encoder(&recorder);
        if (result >= 0 && flush_result < 0)
            result = flush_result;
    }
    if (result >= 0)
        exit_code = stop_requested ? 130 : EXIT_SUCCESS;

finalize:
    if (recorder.header_written) {
        int trailer_result = av_write_trailer(recorder.output_format);
        if (trailer_result < 0) {
            print_ffmpeg_error("Could not finalize MP4 output", trailer_result);
            exit_code = EXIT_FAILURE;
        }
    }
cleanup:
    if (cleanup_resources(&recorder) < 0)
        exit_code = EXIT_FAILURE;
    return exit_code;
}

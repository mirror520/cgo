#include <dvr.h>

#define ICARCH_LINUX64

extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libavutil/imgutils.h>
    #include <libavutil/opt.h>
    #include <libavutil/time.h>
    #include <libswscale/swscale.h>
}

#include <PeerSDK.h>
using namespace PeerSDK;

#include <stdio.h>
#include <pthread.h>
#include <iostream>
using namespace std;

typedef struct {
    AVCodec              *codec;
    AVCodecContext       *codec_ctx;
    AVCodecParserContext *parser_ctx;
    AVFrame              *frame, *key_frame;
    AVPacket             *packet;
    int64                 pts;
} FFmpegInput;

typedef struct {
    AVCodec         *codec;
    AVCodecContext  *codec_ctx;
    AVFormatContext *format_ctx;
    AVOutputFormat  *format;
    AVPacket        *packet;
    AVStream        *stream;
} FFmpegOutput;

typedef struct {
    FFmpegInput  *in;
    FFmpegOutput *out;
    bool          is_pub;
} FFmpegIO;

typedef struct {
    int        id;
    int        frame_num;
    int        width;
    int        height;
    bool       startup;
    AVCodecID  codec;
    int        bit_rate;
    char      *rtmp_url;
} DVRChannel;

typedef struct {
    DVRChannel **channels;
    Peer        *peer;
    PeerStream  *stream;
    int          channel_nums;
} DVRInfo;

DVRInfo *dvr;
FFmpegIO **ffmpeg_ios;

int *frame_nums;
pthread_t *threads;

int Finalize(int code) {
    delete dvr->peer;

    Peer::Cleanup();

    return code;
}

int ffmpegInit(DVRChannel *channel) {
    int i = channel->id;
    FFmpegInput  *in  = ffmpeg_ios[i]->in;
    FFmpegOutput *out = ffmpeg_ios[i]->out;

    in->packet = av_packet_alloc();
    if (!in->packet)
        exit(-1);
    
    in->codec = avcodec_find_decoder(channel->codec);
    if (!in->codec) {
        fprintf(stderr, "Codec not found\n");
        exit(-1);
    }

    in->parser_ctx = av_parser_init(in->codec->id);
    if (!in->parser_ctx) {
        fprintf(stderr, "Parser not found\n");
        exit(-1);
    }

    in->codec_ctx = avcodec_alloc_context3(in->codec);
    if (!in->codec_ctx) {
        fprintf(stderr, "Could not allocate video codec context\n");
        exit(-1);
    }

    if (avcodec_open2(in->codec_ctx, in->codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        exit(-1);
    }

    in->frame = av_frame_alloc();
    if (!in->frame) {
        fprintf(stderr, "Could not allocate video frame\n");
        exit(-1);
    }

    in->key_frame = av_frame_alloc();
    if (!in->key_frame) {
        fprintf(stderr, "Could not allocate video frame\n");
        exit(-1);
    }

    return 0;
}

int encode(FFmpegInput *in, FFmpegOutput *out) {
    int ret;

    ret = avcodec_send_frame(out->codec_ctx, in->frame);
    if (ret < 0) {
        fprintf(stderr, "Error sending a frame for encoding\n");
        exit(-1);
    }

    while (ret >= 0) {
        ret = avcodec_receive_packet(out->codec_ctx, out->packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        else if (ret < 0) {
            fprintf(stderr, "Error during encoding\n");
            return Finalize(-1);
        }

        if (out->packet->pts != AV_NOPTS_VALUE ) {
            out->packet->pts = av_rescale_q(in->frame->pts, AV_TIME_BASE_Q, out->codec_ctx->time_base);
            out->packet->dts = av_rescale_q(in->frame->pts, AV_TIME_BASE_Q, out->codec_ctx->time_base);
        }

        av_interleaved_write_frame(out->format_ctx, out->packet);
    }

    return 0;
}

void *decode(void *args) {
    FFmpegIO     *io  = (FFmpegIO *) args;
    FFmpegInput  *in  = io->in;
    FFmpegOutput *out = io->out;
    int ret;

    ret = avcodec_send_packet(in->codec_ctx, in->packet);
    if (ret < 0) {
        fprintf(stderr, "Error sending a packet for decoding\n");
        exit(-1);
    }

    while (ret >= 0) {
        ret = avcodec_receive_frame(in->codec_ctx, in->frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        else if (ret < 0) {
            fprintf(stderr, "Error during decoding\n");
            exit(-1);
        }

        in->frame->pts = in->pts;

        if (io->is_pub) {
            encode(in, out);
        }

        if (in->frame->key_frame) {
            av_frame_unref(in->key_frame);
            av_frame_move_ref(in->key_frame, in->frame);
        }
    }
}

int publish(FFmpegOutput *out, DVRChannel *channel) {
    char *url;
    int len;

    out->packet = av_packet_alloc();
    if (!out->packet) {
        exit(-1);
    }

    out->codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!out->codec) {
        fprintf(stderr, "Codec not found\n");
        exit(-1);
    }

    out->codec_ctx = avcodec_alloc_context3(out->codec);
    if (!out->codec_ctx) {
        fprintf(stderr, "Could not allocate video codec context\n");
        exit(-1);
    }
    out->codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    out->codec_ctx->codec_id = out->codec->id;
    out->codec_ctx->thread_count = 2;

    out->codec_ctx->bit_rate = channel->bit_rate;
    out->codec_ctx->width = channel->width;
    out->codec_ctx->height = channel->height;
    out->codec_ctx->time_base = (AVRational) { 1, 1000 };
    out->codec_ctx->framerate = (AVRational) { 30, 1 };

    out->codec_ctx->gop_size = 30;
    out->codec_ctx->max_b_frames = 0;
    out->codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;

    if (out->codec->id == AV_CODEC_ID_H264) {
        av_opt_set(out->codec_ctx->priv_data, "preset", "ultrafast", 0);
        av_opt_set(out->codec_ctx->priv_data, "tune", "zerolatency", 0);
        av_opt_set(out->codec_ctx->priv_data, "crf", "28", 0);
    }

    if (avcodec_open2(out->codec_ctx, out->codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        exit(-1);
    }

    out->format_ctx = avformat_alloc_context();
    if (!out->format_ctx) {
        fprintf(stderr, "Could not allocate video formate context\n");
        exit(-1);
    }

    out->format = av_guess_format("flv", NULL, NULL);
    if (!out->format) {
        fprintf(stderr, "Could not guess output format\n");
        exit(-1);
    }
    out->format_ctx->oformat = out->format;

    out->stream = avformat_new_stream(out->format_ctx, NULL);
    if (!out->stream) {
        fprintf(stderr, "Cloud not new stream\n");
        exit(-1);
    }
    out->stream->codecpar->codec_tag = 0;
    avcodec_parameters_from_context(out->stream->codecpar, out->codec_ctx);

    if (avio_open(&out->format_ctx->pb, channel->rtmp_url, AVIO_FLAG_WRITE) < 0) {
        exit(-1);
    }

    if (avformat_write_header(out->format_ctx, NULL) < 0) {
        exit(-1);
    }
}

int unpublish(FFmpegOutput *out) {
    avformat_close_input(&out->format_ctx);
    avcodec_free_context(&out->codec_ctx);
    av_packet_free(&out->packet);
}

int GetChannelNums() {
    return (dvr) ? dvr->channel_nums : 0;
}

int GetFrameNums(int c) {
    if (GetChannelNums() == 0)
        return 0;

    return dvr->channels[c]->frame_num;
}

int Snapshot(int c, AVFrame **frame) {
    if (GetFrameNums(c) == 0)
        return -1;

    FFmpegInput *in = ffmpeg_ios[c]->in;
    *frame = in->key_frame;
    return 0;
}

void SetPublish(int c, bool is_pub) {
    if (GetFrameNums(c) > 0) {
        FFmpegIO *io = ffmpeg_ios[c];

        if (io->is_pub == is_pub)
            return;

        if (is_pub) {   // Publish
            publish(io->out, dvr->channels[c]);
            io->is_pub = true;
        } else {        // Unpublish
            io->is_pub = false;

            usleep(500 * 1000);
            unpublish(io->out);
        }
    }
}

bool GetPublish(int c) {
    if (GetFrameNums(c) == 0)
        return false;

    return ffmpeg_ios[c]->is_pub;
}

void PEERSDK_CALLBACK OnVideoArrived(void *tag, VideoArrivedEventArgs const &e) {
    int c = e.Channel();

    DVRChannel *channel = dvr->channels[c];
    if (!channel->startup) {
        channel->width = e.Width();
        channel->height = (e.Height() == 1088) ? 1080 : e.Height();

        switch (e.Type()) {
            case VideoType_H264_IFrame:
            case VideoType_H264_PFrame:
                channel->codec = AV_CODEC_ID_H264;
                break;

            case VideoType_H265_IFrame:
            case VideoType_H265_PFrame:
                channel->codec = AV_CODEC_ID_HEVC;
                break;
        }

        ffmpegInit(channel);
        channel->startup = true;
    }

    FFmpegInput *in = ffmpeg_ios[c]->in;
    in->pts = e.PTS();

    byte const *data = e.Buffer();
    int data_size = e.BufferLength();
    int ret;
    while (data_size > 0) {
        ret = av_parser_parse2(in->parser_ctx, in->codec_ctx, &in->packet->data, &in->packet->size, 
                                data, data_size, in->pts, in->pts, 0);
        if (ret < 0) {
            fprintf(stderr, "Error while parsing\n");
            exit(-1);
        }

        data += ret;
        data_size -=ret;

        if (in->packet->size) {
            pthread_create(&threads[c], NULL, decode, ffmpeg_ios[c]);
            pthread_join(threads[c], NULL);
        }
    }

    channel->frame_num++;
}

int PeerInit() {
    char *DVR_HOST = getenv("DVR_HOST");
    int   DVR_PORT = atoi(getenv("DVR_PORT"));
    char *DVR_USER = getenv("DVR_USER");
    char *DVR_PASS = getenv("DVR_PASS");

    char *DVR_RTMP_URL      = getenv("DVR_RTMP_URL");
    char *DVR_RTMP_APP      = getenv("DVR_RTMP_APP");
    char *DVR_STREAM_PREFIX = getenv("DVR_STREAM_PREFIX");
    int   DVR_BIT_RATE = atoi(getenv("DVR_BIT_RATE"));          // unit: KB

    dvr = (DVRInfo *) malloc(sizeof(DVRInfo));
    dvr->channel_nums = 0;

    PeerResult r = Peer::Startup();
    if (!r) {
        cout << "*** failed to initialize" << endl;
        return Finalize(-1);
    }

    dvr->peer = new Peer();
    r = dvr->peer->Connect(DVR_HOST, DVR_PORT, DVR_USER, DVR_PASS, "", true);
    if (!r) {
        cout << "*** failed to connect" << endl;
        return Finalize(-1);
    }

    int channels = dvr->peer->Channels().Count();
    dvr->channels = (DVRChannel **) malloc(sizeof(DVRChannel *) * channels);
    ffmpeg_ios = (FFmpegIO **) malloc(sizeof(FFmpegIO *) * channels);
    threads = (pthread_t *) malloc(sizeof(pthread_t) * channels);
    char *rtmp_url;
    int len;
    for (int i=0; i<channels; i++) {
        len = snprintf(NULL, 0, "rtmp://%s/%s/%s_%d", DVR_RTMP_URL, DVR_RTMP_APP, DVR_STREAM_PREFIX, i);
        rtmp_url = (char *) malloc(len + 1);
        snprintf(rtmp_url, len + 1, "rtmp://%s/%s/%s_%d", DVR_RTMP_URL, DVR_RTMP_APP, DVR_STREAM_PREFIX, i);

        dvr->channels[i] = (DVRChannel *) malloc(sizeof(DVRChannel));
        dvr->channels[i]->id = i;
        dvr->channels[i]->frame_num = 0;
        dvr->channels[i]->startup = false;
        dvr->channels[i]->bit_rate = DVR_BIT_RATE * 1024;
        dvr->channels[i]->rtmp_url = rtmp_url;

        ffmpeg_ios[i] = (FFmpegIO *) malloc(sizeof(FFmpegIO));
        ffmpeg_ios[i]->in  = (FFmpegInput *) malloc(sizeof(FFmpegInput));
        ffmpeg_ios[i]->out = (FFmpegOutput *) malloc(sizeof(FFmpegOutput));
        ffmpeg_ios[i]->is_pub = false;
    }
    dvr->channel_nums = channels;

    r = dvr->peer->CreateLiveStream(&dvr->stream);
    if (!r) {
        cout << "*** failed to create live stream" << endl;
        return Finalize(-1);
    }

    dvr->stream->VideoArrived().Add(NULL, OnVideoArrived);
    dvr->stream->SetActive(1);    // 0: Sub, 1: Main
    dvr->stream->SetChannelMask(0xFFFF, 0xFFFF);
    dvr->stream->Start();

    while (true) {
    }
    
    return Finalize(0);
}

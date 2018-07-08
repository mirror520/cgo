#include <dvr.h>

#define ICARCH_LINUX64

extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libavutil/opt.h>
    #include <libavutil/time.h>
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
    AVFrame              *frame;
    AVPacket             *packet;
    int64                pts;
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
} FFmpegIO;

Peer *m_peer;
PeerStream *m_stream;
int Finalize(int code) {
    delete m_peer;

    Peer::Cleanup();

    return code;
}

FFmpegIO **ffmpeg_ios;
int ffmpeg_init(int channels) {
    ffmpeg_ios = (FFmpegIO **) malloc(sizeof(FFmpegIO *) * channels);

    FFmpegInput  *in;
    FFmpegOutput *out;

    char *url;
    int len;
    for (int i=0; i<channels; i++) {
        ffmpeg_ios[i] = (FFmpegIO *) malloc(sizeof(FFmpegIO) * channels);
        ffmpeg_ios[i]->in  = (FFmpegInput *) malloc(sizeof(FFmpegInput) *channels);
        ffmpeg_ios[i]->out = (FFmpegOutput *) malloc(sizeof(FFmpegOutput));

        in = ffmpeg_ios[i]->in;
        out = ffmpeg_ios[i]->out;

        in->packet = av_packet_alloc();
        if (!in->packet) return Finalize(-1);

        in->codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
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

        out->codec_ctx->bit_rate = 1 * 1024 * 1024;
        out->codec_ctx->width = 1280;
        out->codec_ctx->height = 720;
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

        len = snprintf(NULL, 0, "rtmp://localhost/live/dvr_%d", i);
        url = (char *) malloc(len + 1);
        snprintf(url, len + 1, "rtmp://localhost/live/dvr_%d", i);
        if (avio_open(&out->format_ctx->pb, url, AVIO_FLAG_WRITE) < 0) {
            exit(-1);
        }

        if (avformat_write_header(out->format_ctx, NULL) < 0) {
            exit(-1);
        }
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
    FFmpegIO *io = (FFmpegIO *) args;
    FFmpegInput *in = io->in;
    FFmpegOutput *out = io->out;
    int ret;

    ret = avcodec_send_packet(in->codec_ctx, in->packet);
    if (ret < 0) {
        fprintf(stderr, "Error sending a packet for decoding\n");
        exit(1);
    }

    while (ret >= 0) {
        ret = avcodec_receive_frame(in->codec_ctx, in->frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        else if (ret < 0) {
            fprintf(stderr, "Error during decoding\n");
            exit(1);
        }

        in->frame->pts = in->pts;

        if (encode(in, out) < 0) {
            continue;
        }
    }
}

void PublishFrame(int channel) {

}

pthread_t *threads;
int *frames;
void PEERSDK_CALLBACK OnVideoArrived(void *tag, VideoArrivedEventArgs const &e) {
    int channel = e.Channel();
    if ((channel != 0) && (channel != 1)) return;
    frames[channel]++;

    FFmpegInput *in = ffmpeg_ios[channel]->in;
    switch (e.Type()) {
        case VideoType_H265_IFrame:
        case VideoType_H265_PFrame:
            byte const *data = e.Buffer();
            int data_size = e.BufferLength();
            in->pts = e.PTS();
            int ret;
            while (data_size > 0) {
                ret = av_parser_parse2(in->parser_ctx, in->codec_ctx, &in->packet->data, &in->packet->size, 
                                       data, data_size, in->pts, in->pts, 0);
                
                if (ret < 0) {
                    fprintf(stderr, "Error while parsing\n");
                    exit(1);
                }

                data += ret;
                data_size -=ret;

                if (in->packet->size) {
                    pthread_create(&threads[channel], NULL, decode, ffmpeg_ios[channel]);
                    pthread_join(threads[channel], NULL);
                }
            }

    }    
}

int GetFrames(int channel) {
    return (frames) ? frames[channel] : 0;
}

int PeerInit() {
    PeerResult r = Peer::Startup();
    if (!r) {
        cout << "*** failed to initialize" << endl;
        return Finalize(-1);
    }

    m_peer = new Peer();
    r = m_peer->Connect("192.168.11.100", 80, "admin", "admin", "", true);
    if (!r) {
        cout << "*** failed to connect" << endl;
        return Finalize(-1);
    }

    int channels = m_peer->Channels().Count();
    printf("Channel Count: %d\n", channels);

    ffmpeg_init(channels);
    threads = (pthread_t *) malloc(sizeof(pthread_t) * channels);
    frames = (int *) malloc(sizeof(int) * channels);
    for (int i=0; i<channels; i++) {
        frames[i] = 0;
    }

    r = m_peer->CreateLiveStream(&m_stream);
    if (!r) {
        cout << "*** failed to create live stream" << endl;
        return Finalize(-1);
    }

    m_stream->VideoArrived().Add(NULL, OnVideoArrived);

    m_stream->SetActive(0);
    m_stream->SetChannelMask(0xFFFF, 0xFFFF);
    m_stream->Start();

    while (true) {
    }
    
    return Finalize(0);
}

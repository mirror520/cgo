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
#include <iostream>
using namespace std;

Peer *m_peer;
PeerStream *m_stream;

AVCodec *inCodec, *outCodec;
AVFormatContext *outFtx;
AVOutputFormat *outFmt;
AVStream *outStream;
AVCodecParserContext *inParser;
AVCodecContext *inCtx, *outCtx;
AVFrame *frame;
AVPacket *inPkt, *outPkt;
void ffmpeg_close() {
    av_parser_close(inParser);

    avcodec_free_context(&inCtx);
    av_packet_free(&inPkt);

    av_frame_free(&frame);

    avcodec_free_context(&outCtx);
    av_packet_free(&outPkt);
}

int Finalize(int code) {
    delete m_peer;

    Peer::Cleanup();
    ffmpeg_close();

    return code;
}

int ffmpeg_init() {
    inPkt = av_packet_alloc();
    if (!inPkt) return Finalize(-1);

    inCodec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    if (!inCodec) {
        fprintf(stderr, "Codec not found\n");
        return Finalize(-1);
    }

    inParser = av_parser_init(inCodec->id);
    if (!inParser) {
        fprintf(stderr, "Parser not found\n");
        return Finalize(-1);
    }

    inCtx = avcodec_alloc_context3(inCodec);
    if (!inCtx) {
        fprintf(stderr, "Could not allocate video codec context\n");
        return Finalize(-1);
    }

    if (avcodec_open2(inCtx, inCodec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        return Finalize(-1);
    }

    frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Could not allocate video frame\n");
        return Finalize(-1);
    }

    outPkt = av_packet_alloc();
    if (!outPkt) return Finalize(-1);

    outCodec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!outCodec) {
        fprintf(stderr, "Codec not found\n");
        return Finalize(-1);
    }

    outCtx = avcodec_alloc_context3(outCodec);
    if (!outCtx) {
        fprintf(stderr, "Could not allocate video codec context\n");
        return Finalize(-1);
    }
    outCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    outCtx->codec_id = outCodec->id;
    outCtx->thread_count = 2;

    outCtx->bit_rate = 1 * 1024 * 1024;
    outCtx->width = 1280;
    outCtx->height = 720;
    outCtx->time_base = (AVRational) { 1, 1000 };
    outCtx->framerate = (AVRational) { 30, 1 };

    outCtx->gop_size = 30;
    outCtx->max_b_frames = 0;
    outCtx->pix_fmt = AV_PIX_FMT_YUV420P;

    if (outCodec->id == AV_CODEC_ID_H264) {
        av_opt_set(outCtx->priv_data, "preset", "ultrafast", 0);
        av_opt_set(outCtx->priv_data, "tune", "zerolatency", 0);
        av_opt_set(outCtx->priv_data, "crf", "28", 0);
    }

    if (avcodec_open2(outCtx, outCodec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        return Finalize(-1);
    }
    outFtx = avformat_alloc_context();
    if (!outFtx) {
        fprintf(stderr, "Could not allocate video formate context\n");
        return Finalize(-1);
    }

    outFmt = av_guess_format("flv", NULL, NULL);
    if (!outFmt) {
        fprintf(stderr, "Could not guess output format\n");
        return Finalize(-1);
    }

    outStream = avformat_new_stream(outFtx, NULL);
    if (!outStream) {
        fprintf(stderr, "Cloud not new stream\n");
    }
    outStream->codecpar->codec_tag = 0;
    avcodec_parameters_from_context(outStream->codecpar, outCtx);

    outFtx->oformat = outFmt;
    if (avio_open(&outFtx->pb, "rtmp://localhost/live/livestream", AVIO_FLAG_WRITE) < 0) {
        return Finalize(-1);
    }

    if (avformat_write_header(outFtx, NULL) < 0) {
        return Finalize(-1);
    }

    return 0;
}

int encode(AVCodecContext *enc_dex, AVFrame *frame, AVPacket *pkt) {
    int ret;

    if (frame)
        cout << "Send frame " << frame->pts << endl;
    
    ret = avcodec_send_frame(enc_dex, frame);
    if (ret < 0) {
        fprintf(stderr, "Error sending a frame for encoding\n");
        return Finalize(-1);
    }

    while (ret >= 0) {
        ret = avcodec_receive_packet(enc_dex, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        else if (ret < 0) {
            fprintf(stderr, "Error during encoding\n");
            return Finalize(-1);
        }

        if (pkt->pts != AV_NOPTS_VALUE ) {
            pkt->pts = av_rescale_q(frame->pts, AV_TIME_BASE_Q, outCtx->time_base);
            pkt->dts = av_rescale_q(frame->pts, AV_TIME_BASE_Q, outCtx->time_base);
        }

        cout << "Write packet " << pkt->pts << " (size=" << pkt->size << ")" << endl;
        av_interleaved_write_frame(outFtx, outPkt);
    }

    return 0;
}

int decode(AVCodecContext *dec_ctx, AVFrame *frame, AVPacket *pkt, int64 pts) {
    int ret;

    ret = avcodec_send_packet(dec_ctx, pkt);
    if (ret < 0) {
        fprintf(stderr, "Error sending a packet for decoding\n");
        exit(1);
    }

    while (ret >= 0) {
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        else if (ret < 0) {
            fprintf(stderr, "Error during decoding\n");
            exit(1);
        }

        frame->pts = pts;

        printf("Load frame - %5d\n", dec_ctx->frame_number);

        if (encode(outCtx, frame, outPkt) < 0) {
            continue;
        }

        fflush(stdout);
    }
}

void PublishFrame(int channel) {

}

int frames = 0;
void PEERSDK_CALLBACK OnVideoArrived(void *tag, VideoArrivedEventArgs const &e) {
    if (e.Channel() != 0) return;
    
    switch (e.Type()) {
        case VideoType_H265_IFrame:
        case VideoType_H265_PFrame:
            cout << "Frame " << e.Width() << "x" << e.Height() << endl;

            byte const *data = e.Buffer();
            int data_size = e.BufferLength();
            int64 pts = e.PTS();
            int ret;
            while (data_size > 0) {
                ret = av_parser_parse2(inParser, inCtx, &inPkt->data, &inPkt->size, 
                                       data, data_size, pts, pts, 0);
                
                if (ret < 0) {
                    fprintf(stderr, "Error while parsing\n");
                    exit(1);
                }

                data += ret;
                data_size -=ret;

                if (inPkt->size) {
                    decode(inCtx, frame, inPkt, pts);
                }
            }

            frames++;
    }    
}

int GetFrames() {
    return frames;
}

int PeerInit() {
    ffmpeg_init();

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
    cout << "Channel Count: " << channels << endl;

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

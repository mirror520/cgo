#include "dvr.h"

#define ICARCH_LINUX64

extern "C" {
    #include "libavcodec/avcodec.h"
    #include "libavutil/imgutils.h"
    #include "libswscale/swscale.h"
}

#include "opencv2/opencv.hpp"
#include "opencv2/highgui.hpp"
using namespace cv;

#include "PeerSDK.h"
using namespace PeerSDK;

#include "stdio.h"
#include <iostream>
using namespace std;

Peer *m_peer = NULL;
PeerStream *m_stream = NULL;

AVCodec *codec;
AVCodecParserContext *parser;
AVCodecContext *c = NULL;
AVFrame *frame, *frameRGB;
AVPacket *pkt;
void video_decode_init() {
    pkt = av_packet_alloc();
    if (!pkt)
        exit(1);

    codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    if (!codec) {
        fprintf(stderr, "Codec not found\n");
        exit(1);
    }

    parser = av_parser_init(codec->id);
    if (!parser) {
        fprintf(stderr, "parser not found\n");
        exit(1);
    }

    c = avcodec_alloc_context3(codec);
    if (!c) {
        fprintf(stderr, "Could not allocate video codec context\n");
        exit(1);
    }

    if (avcodec_open2(c, codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        exit(1);
    }

    frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Could not allocate video frame\n");
        exit(1);
    }

    frameRGB = av_frame_alloc();
    if (!frameRGB) {
        fprintf(stderr, "Could not allocate video frame\n");
        exit(1);
    }
}

void video_decode_close() {
    av_parser_close(parser);
    avcodec_free_context(&c);
    av_frame_free(&frame);
    av_packet_free(&pkt);
}

int Finalize(int code) {
    delete m_peer;

    Peer::Cleanup();
    video_decode_close();

    return code;
}

Mat grabbedFrame;
void decode(AVCodecContext *dec_ctx, AVFrame *frame, AVFrame *frameRGB, AVPacket *pkt) {
    int ret;
    struct SwsContext *sws_ctx;
    int src_w, src_h;
    int dst_w, dst_h;
    AVPixelFormat src_pix_fmt = AV_PIX_FMT_YUV420P;
    AVPixelFormat dst_pix_fmt = AV_PIX_FMT_BGR24;

    ret = avcodec_send_packet(dec_ctx, pkt);
    if (ret < 0) {
        fprintf(stderr, "Error sending a packet for decoding\n");
        exit(1);
    }

    while (ret >= 0) {
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return;
        else if (ret < 0) {
            fprintf(stderr, "Error during decoding\n");
            exit(1);
        }

        printf("saving frame %3d\n", dec_ctx->frame_number);
        fflush(stdout);

        src_w = dec_ctx->width;
        src_h = dec_ctx->height;

        dst_w = src_w;
        dst_h = src_h;

        sws_ctx = sws_getContext(src_w, src_h, src_pix_fmt, 
                                 dst_w, dst_h, dst_pix_fmt, 
                                 SWS_BILINEAR, NULL, NULL, NULL);

        if (sws_ctx == NULL) {
            fprintf(stderr, "Cannot initialize the conversion context!\n");
            exit(1);
        }

        static int numbytes = av_image_get_buffer_size(AV_PIX_FMT_BGR24, dst_w, dst_h, 1);
        static uint8_t *buffer = (uint8_t*) av_malloc(numbytes * sizeof(uint8_t));
        av_image_fill_arrays(frameRGB->data, frameRGB->linesize, buffer, AV_PIX_FMT_BGR24, 
                             dst_w, dst_h, 1);
        
        sws_scale(sws_ctx, 
                  frame->data, frame->linesize, 0, src_h, 
                  frameRGB->data, frameRGB->linesize);
        
        Mat mat(dst_h, dst_w, CV_8UC3, frameRGB->data[0], frameRGB->linesize[0]);
        grabbedFrame = mat;
        
        sws_freeContext(sws_ctx);
    }
}

void ShowFrame(int channel) {
    if (grabbedFrame.empty()) return;

    imshow("frame", grabbedFrame);
    waitKey(10);
}

int frames = 0;
void PEERSDK_CALLBACK OnVideoArrived(void *tag, VideoArrivedEventArgs const &e) {
    if (e.Channel() != 0) return;

    switch (e.Type()) {
        case VideoType_H265_IFrame:
        case VideoType_H265_PFrame:
            cout << "P-frame " << e.Width() << "x" << e.Height() << endl;

            byte const *data = e.Buffer();
            int data_size = e.BufferLength();
            int pts = e.PTS();
            int ret;
            while (data_size > 0) {
                ret = av_parser_parse2(parser, c , &pkt->data, &pkt->size, 
                                       data, data_size, pts, pts, 0);
                
                if (ret < 0) {
                    fprintf(stderr, "Error while parsing\n");
                    exit(1);
                }

                data += ret;
                data_size -=ret;

                if (pkt->size) {
                    decode(c, frame, frameRGB, pkt);
                }
            }

            frames++;
    }    
}

int GetFrames() {
    return frames;
}

int PeerInit() {
    video_decode_init();

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

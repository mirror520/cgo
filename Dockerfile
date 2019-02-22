FROM ubuntu:16.04

RUN apt update && apt upgrade -y \
 && apt install -y g++ \
                   gcc \
                   git \
                   libc6-dev \
                   make \
                   pkg-config \
                   wget \
                   software-properties-common \
 && add-apt-repository -y ppa:jonathonf/ffmpeg-4 \
 && apt update && apt upgrade -y \
 && apt install -y libavutil-dev \
                   libswscale-dev \
                   libswresample-dev \
                   libavcodec-dev \
                   libavformat-dev \
                   libavdevice-dev \
                   libavfilter-dev

ENV GOLANG_VERSION 1.11.5
ENV goRelArch linux-amd64

RUN wget https://golang.org/dl/go${GOLANG_VERSION}.${goRelArch}.tar.gz \
 && tar -C /usr/local -xzf go${GOLANG_VERSION}.${goRelArch}.tar.gz \
 && rm go${GOLANG_VERSION}.${goRelArch}.tar.gz

ENV GOPATH /go
ENV PATH $GOPATH/bin:/usr/local/go/bin:$PATH

WORKDIR $GOPATH

COPY ./PeerSDK /native/PeerSDK

RUN cp -R /native/PeerSDK/include /usr/local/ \
 && cp /native/PeerSDK/release/linux64/libpeersdk.so /usr/local/lib/libpeersdk.so \
 && ldconfig

RUN go get github.com/eclipse/paho.mqtt.golang \
 && go get github.com/gorilla/mux \
 && go get github.com/mirror520/cgo \
 && go get github.com/prometheus/client_golang/prometheus \
 && go get github.com/prometheus/client_golang/prometheus/promhttp \
 && go install github.com/mirror520/cgo

EXPOSE 8022

CMD cgo

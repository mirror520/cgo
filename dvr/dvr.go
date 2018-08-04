package dvr

/*
#cgo LDFLAGS: -lpeersdk
#cgo LDFLAGS: -lavcodec -lavformat -lavutil -lswscale
#cgo LDFLAGS: -lpthread
#cgo CXXFLAGS: -g -std=c++11
#include "dvr.h"
*/
import "C"

import (
	"fmt"
	"image"
	"unsafe"
)

// Init ...
func Init() int {
	return int(C.PeerInit())
}

// Finalize ...
func Finalize() int {
	return int(C.Finalize(0))
}

// ChannelNums ...
func ChannelNums() int {
	return int(C.GetChannelNums())
}

// FrameNums ...
func FrameNums(channel int) int {
	return int(C.GetFrameNums(C.int(channel)))
}

// SetPublish ...
func SetPublish(channel int, isPub bool) {
	C.SetPublish(C.int(channel), C.bool(isPub))
}

// Publish ...
func Publish(channel int) bool {
	return bool(C.GetPublish(C.int(channel)))
}

// Snapshot ...
func Snapshot(channel int) (image.Image, error) {
	var frame *C.AVFrame

	err := C.Snapshot(C.int(channel), &frame)
	if err != 0 {
		return nil, fmt.Errorf("C.Snapshot failed")
	}

	if frame.width < 0 || frame.height < 0 {
		return nil, fmt.Errorf("Frame size < 0")
	}

	y := C.GoBytes(unsafe.Pointer(frame.data[0]), frame.linesize[0]*frame.height)
	u := C.GoBytes(unsafe.Pointer(frame.data[1]), frame.linesize[1]*frame.height/2)
	v := C.GoBytes(unsafe.Pointer(frame.data[2]), frame.linesize[2]*frame.height/2)

	return &image.YCbCr{
		Y:              y,
		Cb:             u,
		Cr:             v,
		YStride:        int(frame.linesize[0]),
		CStride:        int(frame.linesize[1]),
		SubsampleRatio: image.YCbCrSubsampleRatio420,
		Rect: image.Rectangle{
			Min: image.Point{
				X: 0,
				Y: 0,
			},
			Max: image.Point{
				X: int(frame.width),
				Y: int(frame.height),
			},
		},
	}, nil
}

package dvr

/*
#cgo LDFLAGS: -lpeersdk
#cgo LDFLAGS: -lavcodec -lavformat -lavutil
#cgo LDFLAGS: -lpthread
#cgo CXXFLAGS: -g -std=c++11
#include "dvr.h"
*/
import "C"

// Init ...
func Init() int {
	return int(C.PeerInit())
}

// Finalize ...
func Finalize() int {
	return int(C.Finalize(0))
}

// Frames ...
func Frames(channel int) int {
	return int(C.GetFrames(C.int(channel)))
}

// Publish ...
func Publish(channel int) {
	C.PublishFrame(C.int(channel))
}

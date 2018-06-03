package dvr

/*
#cgo LDFLAGS: -L /home/ar0660/native/PeerSDK/release/linux64/ -lpeersdk
#cgo LDFLAGS: -lrt
#cgo LDFLAGS: -lpthread
#cgo LDFLAGS: -lavcodec -lavutil -lswscale
#cgo LDFLAGS: -lopencv_core -lopencv_highgui
#cgo CXXFLAGS: -g -std=c++11
#include "dvr.h"
*/
import "C"

func Init() int {
	return int(C.PeerInit())
}

func Finalize() int {
	return int(C.Finalize(0))
}

func Frames() int {
	return int(C.GetFrames())
}

func Show(channel int) {
	C.ShowFrame(C.int(channel))
}

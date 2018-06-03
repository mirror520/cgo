package main

import (
	"fmt"
	"os"
	"time"

	"github.com/mirror520/cgo/dvr"
)

func main() {
	go func() {
		for {
			dvr.Show(0)
			time.Sleep(33 * time.Millisecond)
		}
	}()

	frames := make(chan int)
	go func() {
		for {
			frames <- dvr.Frames()
			time.Sleep(1 * time.Second)
		}
	}()

	abort := make(chan bool)
	go func() {
		for {
			os.Stdin.Read(make([]byte, 1))
			abort <- true
		}
	}()

	go func() {
		for {
			select {
			case x := <-frames:
				fmt.Println(x)

			case <-abort:
				dvr.Finalize()
				return
			}
		}
	}()

	dvr.Init()
}

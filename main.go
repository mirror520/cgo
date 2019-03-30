package main

import (
	"bytes"
	"fmt"
	"image/jpeg"
	"io"
	"log"
	"net/http"
	"os"
	"regexp"
	"strconv"
	"time"

	MQTT "github.com/eclipse/paho.mqtt.golang"
	"github.com/gorilla/mux"
	"github.com/mirror520/cgo/dvr"
	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/promhttp"
)

// Exporter ...
type Exporter struct {
	fpsGauge prometheus.GaugeVec
}

// NewExporter ...
func NewExporter() *Exporter {
	gauge := *prometheus.NewGaugeVec(prometheus.GaugeOpts{
		Namespace: "dvr",
		Name:      "frame_per_seconds",
		Help:      "Frame per seconds for channel output from DVR system"},
		[]string{"dvr", "channel"},
	)

	return &Exporter{fpsGauge: gauge}
}

// Collect (Exporter.Collect) ...
func (e *Exporter) Collect(ch chan<- prometheus.Metric) {
	e.fpsGauge.Collect(ch)
}

// Describe (Exporter.Describe)
func (e *Exporter) Describe(ch chan<- *prometheus.Desc) {
	e.fpsGauge.Describe(ch)
}

func snapshot(channel int, out io.Writer) {
	image, err := dvr.Snapshot(channel)
	if err != nil {
		fmt.Println(err)
	}

	if image != nil {
		err = jpeg.Encode(out, image, &jpeg.Options{100})
		if err != nil {
			fmt.Println(err)
		}
	}
}

func snapshotHandler(w http.ResponseWriter, r *http.Request) {
	vars := mux.Vars(r)
	channel, _ := strconv.Atoi(vars["channel"])
	if channel > -1 && channel < 16 {
		snapshot(channel, w)
	}
}

var channelSwitchHandler MQTT.MessageHandler = func(client MQTT.Client, msg MQTT.Message) {
	re := regexp.MustCompile(`^dvr/channels/(?P<channel_id>[0-9]{1,})/publish$`)
	topic := msg.Topic()
	isPub := string(msg.Payload()) == "true"

	fmt.Printf("TOPIC: %s\n", msg.Topic())
	fmt.Printf("MSG: %v\n", isPub)

	if re.MatchString(topic) {
		channel, _ := strconv.Atoi(re.ReplaceAllString(topic, `${channel_id}`))
		channelNums := dvr.ChannelNums()
		if channel > -1 && channel < channelNums {
			dvr.SetPublish(channel, isPub)
		}

		if channel == 999 { // All channel
			for i := 0; i < channelNums; i++ {
				dvr.SetPublish(i, isPub)
			}
		}
	}
}

func receiveChannelNums(out chan<- int) {
	c := 0
	for {
		c = dvr.ChannelNums()
		if c > 0 {
			fmt.Printf("Channel Count: %d\n", c)

			out <- c
			defer close(out)
			return
		}
		time.Sleep(1 * time.Second)
	}
}

func receiveChannelStatus(out chan<- string, in <-chan int) {
	c := <-in

	dvrName := os.Getenv("DVR_RTMP_APP")
	exporter := NewExporter()
	prometheus.MustRegister(exporter)

	fps := 0
	frameNums := 0
	previousFrameNums := make([]int, c)
	var buf bytes.Buffer
	for {
		buf.Reset()
		for i := 0; i < c; i++ {
			frameNums = dvr.FrameNums(i)
			fps = frameNums - previousFrameNums[i]
			buf.WriteString(fmt.Sprintf("Frame number: %5d, Publish: %5v, fps: %2d (Channel %d)\n", frameNums, dvr.Publish(i), fps, i))
			exporter.fpsGauge.WithLabelValues(dvrName, strconv.Itoa(i)).Set(float64(fps))
			previousFrameNums[i] = frameNums
		}

		out <- buf.String()
		time.Sleep(1 * time.Second)
	}
}

func main() {
	go func() {
		opts := MQTT.NewClientOptions().AddBroker(os.Getenv("MQTT_HOST"))
		opts.SetClientID(fmt.Sprintf("client-%s-%d", os.Getenv("MQTT_USERNAME"), time.Now().Unix()))
		opts.SetUsername(os.Getenv("MQTT_USERNAME"))
		opts.SetPassword(os.Getenv("MQTT_PASSWORD"))

		client := MQTT.NewClient(opts)
		if token := client.Connect(); token.Wait() && token.Error() != nil {
			panic(token.Error())
		}

		time.Sleep(10 * time.Second)

		if token := client.Subscribe("dvr/channels/+/publish", 0, channelSwitchHandler); token.Wait() && token.Error() != nil {
			fmt.Println(token.Error())
			os.Exit(-1)
		}
	}()

	go func() {
		router := mux.NewRouter()
		router.HandleFunc("/dvr/{channel}", snapshotHandler).Methods("GET")
		router.Handle("/metrics", promhttp.Handler()).Methods("GET")
		log.Fatal(http.ListenAndServe(":8022", router))
	}()

	channelNums := make(chan int)
	channelStatus := make(chan string)
	go receiveChannelNums(channelNums)
	go receiveChannelStatus(channelStatus, channelNums)

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
			case x := <-channelStatus:
				fmt.Println(x)

			case <-abort:
				dvr.Finalize()
				return
			}
		}
	}()

	dvr.Init()
}

package main

import (
	"flag"
	"net"
	"strconv"
	"strings"
	"time"

	rsd "github.com/holoplot/ravenna-fpga-drivers/go/stream-device"
	"github.com/mattn/go-colorable"
	"github.com/rs/zerolog"
	"github.com/rs/zerolog/log"
)

func main() {
	deviceFileFlag := flag.String("dev", "/dev/ravenna-stream-device", "Device file to use")
	channelsFlag := flag.Int("channels", 2, "Number of channels to receive")
	primaryIpFlag := flag.String("pri-ip", "", "Primary destination IP")
	secondaryIpFlag := flag.String("sec-ip", "", "Secondary destination IP")
	primaryPortFlag := flag.Int("pri-port", 5004, "Primary UDP port")
	secondaryPortFlag := flag.Int("sec-port", 5004, "Secondary UDP port")
	jitterBufferMarginFlag := flag.Int("jitter-buffer-margin", 500, "Jitter buffer margin")
	rtpOffsetFlag := flag.Int("rtp-offset", 500, "RTP offset")
	rtpSsrcFlag := flag.Int("rtp-ssrc", 0, "RTP Sync Source Identifier")
	vlanTagFlag := flag.Int("vlan", -1, "VLAN tag")
	rtpPayloadTypeFlag := flag.Int("rtp-payload-type", 98, "RTP payload type")
	synchronousFlag := flag.Bool("synchronous", false, "Use synchronous instead of syntonous")
	syncSourceFlag := flag.Bool("sync-source", false, "Use stream as sync source")
	rtpFilterFlag := flag.Bool("rtp-filter", false, "Use RTP filter")
	hitlessFlag := flag.Bool("hitless", false, "Hitless protection")
	trackMapFlag := flag.String("track-map", "", "Comma separated list of tracks to map. Defaults to 1:1 mapping to channels.")
	flag.Parse()

	consoleWriter := zerolog.ConsoleWriter{
		Out: colorable.NewColorableStdout(),
	}

	log.Logger = log.Output(consoleWriter)

	if *primaryIpFlag == "" && *secondaryIpFlag == "" {
		log.Fatal().Msg("-pri-ip or -sec-ip must be passed")
	}

	sd, err := rsd.Open(*deviceFileFlag)
	if err != nil {
		log.Fatal().
			Err(err).
			Str("path", *deviceFileFlag).
			Msg("Unable to open device file")
	}

	rxDesc := rsd.RxStreamDescription{
		SyncSource:        *syncSourceFlag,
		HitlessProtection: *hitlessFlag,

		Synchronous: *synchronousFlag,
		RtpFilter:   *rtpFilterFlag,

		CodecType:      rsd.StreamCodecL24,
		RtpPayloadType: uint8(*rtpPayloadTypeFlag),

		JitterBufferMargin: uint16(*jitterBufferMarginFlag),

		RtpOffset: uint32(*rtpOffsetFlag),
		RtpSsrc:   uint32(*rtpSsrcFlag),

		NumChannels: uint16(*channelsFlag),
	}

	listenMulticast := func(ifiName string, addr net.UDPAddr) {
		if ifi, err := net.InterfaceByName(ifiName); err == nil {
			if _, err := net.ListenMulticastUDP("udp4", ifi, &addr); err == nil {
				log.Info().
					Str("interface", ifiName).
					IPAddr("ip", addr.IP).
					Int("port", addr.Port).
					Msg("Listening")
			} else {
				log.Error().
					Err(err).
					Str("interface", ifiName).
					IPAddr("ip", addr.IP).
					Int("port", addr.Port).
					Msg("Cannot listen")
			}
		} else {
			log.Error().
				Err(err).
				Str("interface", ifiName).
				Msg("Unable to lookup network interface")
		}
	}

	if len(*primaryIpFlag) > 0 {
		rxDesc.PrimaryDestination = net.UDPAddr{
			IP:   net.ParseIP(*primaryIpFlag),
			Port: *primaryPortFlag,
		}

		listenMulticast("ra0", rxDesc.PrimaryDestination)
	}

	if len(*secondaryIpFlag) > 0 {
		rxDesc.SecondaryDestination = net.UDPAddr{
			IP:   net.ParseIP(*secondaryIpFlag),
			Port: *secondaryPortFlag,
		}

		listenMulticast("ra1", rxDesc.SecondaryDestination)
	}

	if *vlanTagFlag >= 0 {
		rxDesc.VlanTag = uint16(*vlanTagFlag)
		rxDesc.VlanTagged = true
	}

	if *trackMapFlag == "" {
		for i := uint16(0); i < rxDesc.NumChannels; i++ {
			rxDesc.Tracks[i] = int16(i)
		}
	} else {
		for i := uint16(0); i < rxDesc.NumChannels; i++ {
			rxDesc.Tracks[i] = rsd.TrackNull
		}

		for i, t := range strings.Split(*trackMapFlag, ",") {
			if i >= int(rxDesc.NumChannels) {
				break
			}

			if n, err := strconv.Atoi(t); err == nil && n < rsd.MaxTracks {
				rxDesc.Tracks[i] = int16(n)
			}
		}
	}

	_, err = sd.AddRxStream(rxDesc)
	if err != nil {
		log.Fatal().
			Err(err).
			Msg("Unable to add RX stream")
	}

	log.Info().
		Int("channels", int(rxDesc.NumChannels)).
		IPAddr("primary-ip", rxDesc.PrimaryDestination.IP).
		Int("primary-port", int(rxDesc.PrimaryDestination.Port)).
		IPAddr("secondary-ip", rxDesc.SecondaryDestination.IP).
		Int("secondary-port", int(rxDesc.SecondaryDestination.Port)).
		Int("jitter-buffer-margin", int(rxDesc.JitterBufferMargin)).
		Int("rtp-offset", int(rxDesc.RtpOffset)).
		Int("rtp-ssrc", int(rxDesc.RtpSsrc)).
		Int("rtp-payload-type", int(rxDesc.RtpPayloadType)).
		Bool("rtp-filter", rxDesc.RtpFilter).
		Bool("synchronous", rxDesc.Synchronous).
		Bool("sync-source", rxDesc.SyncSource).
		Bool("hitless", rxDesc.HitlessProtection).
		Ints16("rx-tracks", rxDesc.Tracks[:rxDesc.NumChannels]).
		Msg("RX stream added")

	log.Info().Msg("Hit ^C to exit.")

	for {
		time.Sleep(time.Minute)
	}
}
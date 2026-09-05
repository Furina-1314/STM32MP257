#!/bin/sh
# rov_camstream.sh - RTP/H264 video pipeline (the "CamStream" role, U-00).
# Pipeline mirrors the vendor /usr/local/bin/stream_libcamera.sh, with the
# shore target parametrized from /etc/rov_gateway.ini [camstream].
# Kept as a separate systemd unit: video has its own lifecycle and the
# gateway never touches UDP 5000.
#
# Two hard-won constraints of this image, do not regress:
#   1. The DCMIPP media pipeline MUST be configured via media-ctl before
#      gst-launch; otherwise the ISP stats node fails buffer queueing and
#      libcamera aborts (assertion in streamOff).
#   2. gst-launch here cannot parse spaces inside a single argv element and
#      backslash continuations also trip it: keep every element property as
#      its own shell word on one flat exec line.

CONF=/etc/rov_gateway.ini
get() { # get KEY DEFAULT - crude INI reader for the [camstream] section
    key="$1"; def="$2"
    val="$(sed -n '/^\[camstream\]/,/^\[/p' "$CONF" 2>/dev/null \
        | grep -E "^[[:space:]]*${key}[[:space:]]*=" | tail -1 \
        | cut -d= -f2 | tr -d ' \t')"
    [ -n "$val" ] && echo "$val" || echo "$def"
}

HOST="$(get host 192.168.1.100)"
PORT="$(get port 5000)"
BITRATE="$(get bitrate 2000000)"
KEYFRAME="$(get keyframe_interval 30)"
WIDTH="$(get width 1920)"
HEIGHT="$(get height 1080)"
FPS="$(get framerate 30)"

echo "rov_camstream: -> ${HOST}:${PORT} ${WIDTH}x${HEIGHT}@${FPS} ${BITRATE}bps"

# ---- clean slate + DCMIPP pipeline configuration (vendor recipe) ----------
MC="media-ctl -d platform:48030000.dcmipp"
killall -9 gst-launch-1.0 2>/dev/null

$MC -r
$MC -l '"48020000.csi":1->"dcmipp_input":0[1]'
$MC -l '"dcmipp_input":2->"dcmipp_main_isp":0[1]'
$MC -l '"dcmipp_main_isp":1->"dcmipp_main_postproc":0[1]'
$MC -l '"dcmipp_main_postproc":1->"dcmipp_main_capture":0[1]'

$MC --set-v4l2 "\"imx335 1-001a\":0[fmt:SRGGB12_1X12/${WIDTH}x${HEIGHT}]"
$MC --set-v4l2 "\"48020000.csi\":0[fmt:SRGGB12_1X12/${WIDTH}x${HEIGHT}]"
$MC --set-v4l2 "\"48020000.csi\":1[fmt:SRGGB12_1X12/${WIDTH}x${HEIGHT}]"
$MC --set-v4l2 "\"dcmipp_input\":0[fmt:SRGGB12_1X12/${WIDTH}x${HEIGHT}]"
$MC --set-v4l2 "\"dcmipp_input\":2[fmt:SRGGB12_1X12/${WIDTH}x${HEIGHT}]"
$MC --set-v4l2 "\"dcmipp_main_isp\":0[fmt:SRGGB12_1X12/${WIDTH}x${HEIGHT} crop:(0,0)/${WIDTH}x${HEIGHT} compose:(0,0)/${WIDTH}x${HEIGHT}]"
$MC --set-v4l2 "\"dcmipp_main_isp\":1[fmt:RGB888_1X24/${WIDTH}x${HEIGHT}]"
$MC --set-v4l2 "\"dcmipp_main_postproc\":0[fmt:RGB888_1X24/${WIDTH}x${HEIGHT} crop:(0,0)/${WIDTH}x${HEIGHT} compose:(0,0)/${WIDTH}x${HEIGHT}]"
$MC --set-v4l2 "\"dcmipp_main_postproc\":1[fmt:YUV420_2X24/${WIDTH}x${HEIGHT}]" 2>/dev/null \
    || $MC --set-v4l2 "\"dcmipp_main_postproc\":1[fmt:RGB888_1X24/${WIDTH}x${HEIGHT}]"

export LIBCAMERA_LOG_LEVELS=WARNING

exec gst-launch-1.0 libcamerasrc ! video/x-raw,format=NV12,width=${WIDTH},height=${HEIGHT},framerate=${FPS}/1 ! queue max-size-buffers=2 leaky=downstream ! v4l2slh264enc bitrate=${BITRATE} keyframe-interval=${KEYFRAME} ! h264parse config-interval=1 ! rtph264pay mtu=1400 config-interval=1 pt=96 aggregate-mode=zero-latency ! udpsink host=${HOST} port=${PORT} sync=false async=false

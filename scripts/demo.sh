#!/usr/bin/env bash
#
# demo.sh: drive the QNX AQ Sensor-Framework demo on the CM4 over qconn/WiFi.
#
# Network (set QNX_IP / LAPTOP_IP to match yours; the IPs below are examples):
#   Pi (QNX):   192.168.1.10  qconn TCP 8000
#   Laptop:     192.168.1.11  HTTP file server on :8099
#
# Subcommands:
#   ./demo.sh httpd            # start the laptop HTTP file server (serves ./stage)
#   ./demo.sh deploy           # curl the .so + both configs onto the Pi
#   ./demo.sh sim              # (re)start the sensor service with the SIM bus, detached
#   ./demo.sh i2c              # (re)start the sensor service with the I2C bus (Teensy), detached
#   ./demo.sh observe [N]      # run sensor_example on unit 1, print N buffers (default 8)
#   ./demo.sh sim-pollute      # (re)start the service with SIM bus + sim_pollute=1 (forces filter ALERT)
#   ./demo.sh sim-co2          # (re)start the service with SIM bus + sim_co2=1 (forces CO2 ALERT)
#   ./demo.sh publisher [bus]  # (re)start the SSE publisher on the Pi (:8090), bus label sim|i2c (default sim)
#   ./demo.sh sse              # curl the Pi SSE stream to the terminal (sanity check)
#   ./demo.sh ivi              # serve the browser IVI on http://localhost:8080  (open it in a browser)
#   ./demo.sh slog             # dump the running service's slog2 (errors + rates)
#   ./demo.sh stop             # slay the sensor service + publisher on the Pi
#   ./demo.sh teensy           # open the Teensy serial monitor (p=pollute g=co2 c=clear s=state)
#
# Typical flows:
#   Full SIM demo:  httpd; deploy; sim; publisher sim; ivi    (open browser; use IVI Demo button too)
#   Force ALERT:    sim-pollute; publisher sim                (IVI banner fires)
#   I2C demo:  (wire Teensy<->Pi) httpd; deploy; i2c; publisher i2c; ivi  # + ./demo.sh teensy to pollute
#
set -u
PI_IP="${QNX_IP:-192.168.1.10}"
LAP_IP="${LAPTOP_IP:-192.168.1.11}"
PORT=8000
HTTP_PORT=8099
HERE="$(cd "$(dirname "$0")" && pwd)"
STAGE="$HERE/stage"
# SF_ROOT: your unzipped Sensor Framework example package (override if the SDP is not at ~/qnx800)
SF_ROOT="${SF_ROOT:-$HOME/qnx800/source/source_package_sf_sensor}"
SO="$SF_ROOT/lib/sensor_drivers/external_sensors/aq/nto/aarch64/so.le/libaq_external_sensor.so"
PUB="$SF_ROOT/apps/sensor/aq_publisher/nto/aarch64/o.le/aq_publisher"
TARGET_DIR=/data/var/tmp/aq
TEENSY=/dev/ttyACM0

qsh() { bash "$HERE/qsh.sh" "$1" "${2:-4}"; }

case "${1:-}" in
  httpd)
    [ -f "$SO" ]  || { echo "ERROR: driver not built at $SO" >&2; echo "Build it first (see SETUP.md), or set SF_ROOT to your SDP source package." >&2; exit 1; }
    [ -f "$PUB" ] || { echo "ERROR: publisher not built at $PUB" >&2; echo "Build it first (see SETUP.md), or set SF_ROOT to your SDP source package." >&2; exit 1; }
    mkdir -p "$STAGE"
    cp "$SO" "$STAGE/libaq_external_sensor.so"
    cp "$PUB" "$STAGE/aq_publisher"
    cp "$HERE/../configs/"*.conf "$STAGE/"
    echo "serving $STAGE on 0.0.0.0:$HTTP_PORT (Ctrl-C to stop)"
    cd "$STAGE" && exec python3 -m http.server "$HTTP_PORT" --bind 0.0.0.0
    ;;
  deploy)
    qsh "mkdir -p $TARGET_DIR/roll; cd $TARGET_DIR; \
         for f in libaq_external_sensor.so aq_publisher aq.conf aq_i2c.conf aq_sim_pollute.conf aq_sim_co2.conf; do \
           /system/bin/curl -s -o \$f http://$LAP_IP:$HTTP_PORT/\$f; done; \
         chmod +x aq_publisher; ls -l $TARGET_DIR" 6
    ;;
  sim|i2c|sim-pollute|sim-co2)
    CONF="aq.conf"
    MUX=""
    [ "$1" = i2c ] && CONF="aq_i2c.conf" &&
      # The QNX rpi image never muxes the 40-pin header I2C pins, so /dev/i2c1
      # (BSC1 @0xfe804000 = GPIO2/3, header pins 3/5) is dead until we do it.
      MUX="gpio-bcm2711 set 2 a0 pu; gpio-bcm2711 set 3 a0 pu;"
    [ "$1" = sim-pollute ] && CONF="aq_sim_pollute.conf"
    [ "$1" = sim-co2 ] && CONF="aq_sim_co2.conf"
    qsh "$MUX slay -f sensor 2>/dev/null; sleep 0.3; \
         nohup /system/bin/sensor -U 1000 -r $TARGET_DIR/roll -c $TARGET_DIR/$CONF -vvvv \
           >$TARGET_DIR/sensor.log 2>&1 & sleep 1.5; echo started with $CONF; ls /dev/sensor" 6
    ;;
  publisher)
    BUS="${2:-sim}"
    qsh "slay -f aq_publisher 2>/dev/null; sleep 0.3; \
         nohup $TARGET_DIR/aq_publisher 1 8090 $BUS >$TARGET_DIR/pub.log 2>&1 & sleep 2; cat $TARGET_DIR/pub.log" 6
    ;;
  sse)
    timeout "${2:-6}" curl -s -N "http://$PI_IP:8090/stream" | head -12
    ;;
  ivi)
    echo "open http://localhost:8080  (set Pi = $PI_IP in the top bar if needed)"
    cd "$HERE/../ivi" && exec python3 -m http.server 8080 --bind 127.0.0.1
    ;;
  observe)
    N="${2:-8}"
    { printf 'service launcher\n'; sleep 0.5;
      printf 'start/flags 0 /system/bin/sensor_example sensor_example -e 1 -u 1 -d -s -b %s -t 8000\n' "$N";
      sleep $((N*2+6));
    } | timeout $((N*2+12)) nc "$PI_IP" "$PORT" 2>&1 | tr -d '\000' | sed 's/QCONN//' \
      | grep -viE 'screen|drm|wfd|usb_otg|dma_lib|io_spi|^OK'
    ;;
  slog)
    qsh "slog2info -b sensor_service 2>/dev/null | tail -30" 5
    ;;
  stop)
    qsh "slay -f aq_publisher 2>/dev/null; slay -f sensor 2>/dev/null; echo stopped" 3
    ;;
  teensy)
    stty -F "$TEENSY" 115200 raw -echo 2>/dev/null
    echo "Teensy serial ($TEENSY): type p=pollute g=co2 c=clear s=state ?=help, Ctrl-C to exit"
    cat "$TEENSY" &
    CATPID=$!
    trap "kill $CATPID 2>/dev/null" EXIT
    while read -rn1 k; do printf '%s' "$k" > "$TEENSY"; done
    ;;
  *)
    grep -E '^#( |$)' "$0" | sed 's/^# \{0,1\}//'
    ;;
esac

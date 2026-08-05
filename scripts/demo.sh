#!/usr/bin/env bash
#
# demo.sh — drive the QNX AQ Sensor-Framework demo on the CM4 over qconn/WiFi.
#
# Network (set QNX_IP / LAPTOP_IP to match yours; the addresses below are examples):
#   Pi (QNX):   192.168.1.10  qconn TCP 8000
#   Laptop:     192.168.1.11  HTTP file server on :8099
#
# Subcommands:
#   ./demo.sh httpd            # start the laptop HTTP file server (serves ./stage)
#   ./demo.sh deploy           # curl the .so + both configs onto the Pi
#   ./demo.sh sim              # (re)start the sensor service with the SIM bus, detached
#   ./demo.sh i2c              # (re)start the sensor service with the I2C bus (Teensy), detached
#   ./demo.sh observe [N]      # run sensor_example on unit 1, print N buffers (default 8)
#   ./demo.sh sim-pollute      # (re)start the service with SIM bus + sim_pollute=1 (forces ALERT)
#   ./demo.sh publisher [bus]  # (re)start the SSE publisher on the Pi (:8090), bus label sim|i2c (default sim)
#   ./demo.sh sse              # curl the Pi SSE stream to the terminal (sanity check)
#   ./demo.sh ivi              # serve the browser IVI on http://localhost:8080  (open it in a browser)
#   ./demo.sh gpiosweep        # print the header-GPIO meter plan (I2C-blocker diagnostic)
#   ./demo.sh gpiodrive N [hi|lo] # drive header GPIO N high/low + hold, so you can meter the pin
#   ./demo.sh slog             # dump the running service's slog2 (errors + rates)
#   ./demo.sh stop             # slay the sensor service + publisher on the Pi
#   ./demo.sh teensy           # open the Teensy serial monitor (p=pollute c=clear s=state)
#
# Typical flows:
#   Full SIM demo:  httpd; deploy; sim; publisher sim; ivi    (open browser; use IVI Demo button too)
#   Force ALERT:    sim-pollute; publisher sim                (IVI banner fires)
#   I2C demo:  (wire Teensy<->Pi) httpd; deploy; i2c; publisher i2c; ivi  # + ./demo.sh teensy to pollute
#
set -u
# Machine-specific addresses live in scripts/env.local.sh (gitignored). The
# defaults below are placeholders, not anyone's real network.
[ -f "$(dirname "$0")/env.local.sh" ] && . "$(dirname "$0")/env.local.sh"
PI_IP="${QNX_IP:-192.168.1.10}"
LAP_IP="${LAPTOP_IP:-192.168.1.11}"
PORT=8000
HTTP_PORT=8099
HERE="$(cd "$(dirname "$0")" && pwd)"
STAGE="$HERE/stage"
SO=~/qnx800/source/source_package_sf_sensor/lib/sensor_drivers/external_sensors/aq/nto/aarch64/so.le/libaq_external_sensor.so
PUB=~/qnx800/source/source_package_sf_sensor/apps/sensor/aq_publisher/nto/aarch64/o.le/aq_publisher
TARGET_DIR=/data/var/tmp/aq
TEENSY=/dev/ttyACM0

qsh() { bash "$HERE/qsh.sh" "$1" "${2:-4}"; }

case "${1:-}" in
  httpd)
    mkdir -p "$STAGE"
    cp "$SO" "$STAGE/libaq_external_sensor.so" 2>/dev/null
    cp "$PUB" "$STAGE/aq_publisher" 2>/dev/null
    cp "$HERE/../configs/"*.conf "$STAGE/" 2>/dev/null
    echo "serving $STAGE on 0.0.0.0:$HTTP_PORT (Ctrl-C to stop)"
    cd "$STAGE" && exec python3 -m http.server "$HTTP_PORT" --bind 0.0.0.0
    ;;
  deploy)
    qsh "mkdir -p $TARGET_DIR/roll; cd $TARGET_DIR; \
         for f in libaq_external_sensor.so aq_publisher aq.conf aq_i2c.conf aq_sim_pollute.conf; do \
           /system/bin/curl -s -o \$f http://$LAP_IP:$HTTP_PORT/\$f; done; \
         chmod +x aq_publisher; ls -l $TARGET_DIR" 6
    ;;
  sim|i2c|sim-pollute)
    CONF="aq.conf"
    MUX=""
    [ "$1" = i2c ] && CONF="aq_i2c.conf" &&
      # The QNX rpi image never muxes the 40-pin header I2C pins, so /dev/i2c1
      # (BSC1 @0xfe804000 = GPIO2/3, header pins 3/5) is dead until we do it.
      MUX="gpio-bcm2711 set 2 a0 pu; gpio-bcm2711 set 3 a0 pu;"
    [ "$1" = sim-pollute ] && CONF="aq_sim_pollute.conf"
    qsh "$MUX slay -f sensor 2>/dev/null; sleep 0.3; \
         nohup /system/bin/sensor -U 1000 -r $TARGET_DIR/roll -c $TARGET_DIR/$CONF -vvvv \
           >$TARGET_DIR/sensor.log 2>&1 & sleep 1.5; echo started with $CONF; ls /dev/sensor" 6
    ;;
  scan)
    qsh "gpio-bcm2711 set 2 a0 pu; gpio-bcm2711 set 3 a0 pu; $TARGET_DIR/i2cscan" 5
    ;;

  # ---- signal-service (QPP / COVESA VSS) path -------------------------------
  mux)
    # Mux the header I2C pins and nothing else. The QNX rpi image leaves GPIO2/3
    # unmuxed, so /dev/i2c1 is dead until this runs. Split out from `i2c` so the
    # direct publisher can use it without starting the sensor service.
    qsh "gpio-bcm2711 set 2 a0 pu; gpio-bcm2711 set 3 a0 pu; echo 'I2C header pins muxed'" 4
    ;;
  vss-stage)
    mkdir -p "$STAGE"
    [ -n "$QPP_BIN" ] || { echo "ERROR: set QPP_BIN to your built aarch64le qpp binary." >&2
                           echo "Build it from github.com/qnx/qnx-posix-publish-subscribe with the SDP." >&2; exit 1; }
    [ -f "$QPP_BIN" ] || { echo "ERROR: no qpp binary at $QPP_BIN" >&2; exit 1; }
    cp "$QPP_BIN" "$STAGE/qpp"
    cp "$VSS_DIR/catalog/$CATALOG" "$STAGE/"
    for b in aq_signal_publisher aq_signal_connector; do
      [ -f "$VSS_DIR/connector/$b" ] && cp "$VSS_DIR/connector/$b" "$STAGE/"
    done
    [ -f "$STAGE/aq_signal_publisher" ] || echo "NOTE: aq_signal_publisher not built (cd ../vss/connector && make publisher)"
    echo "staged in $STAGE:"; ls -l "$STAGE" | grep -E 'qpp|aq_signal|catalog'
    echo ">> now run './demo.sh httpd' in another shell, then './demo.sh vss-deploy'"
    ;;
  vss-deploy)
    qsh "mkdir -p $VSS_TARGET; cd $VSS_TARGET; \
         for f in qpp $CATALOG aq_signal_publisher aq_signal_connector; do \
           /system/bin/curl -sf -o \$f http://$LAP_IP:$HTTP_PORT/\$f || rm -f \$f; done; \
         chmod +x qpp aq_signal_publisher aq_signal_connector 2>/dev/null; ls -l $VSS_TARGET" 8
    ;;
  qpp)
    qsh "slay -f qpp 2>/dev/null; sleep 0.5; \
         on -d $VSS_TARGET/qpp -s -c $VSS_TARGET/$CATALOG -m /dev/qpp -l INF; \
         sleep 2; echo -n 'signal files: '; find /dev/qpp -type f | wc -l" 8
    ;;
  vss-publish)
    # Direct path: I2C -> signal service. No sensor service involved.
    BUS="${2:-1}"; ADDR="${3:-0x28}"     # defaults match configs/aq_i2c.conf
    qsh "slay -f aq_signal_publisher 2>/dev/null; sleep 0.3; \
         on -d $VSS_TARGET/aq_signal_publisher -b $BUS -a $ADDR -m /dev/qpp -f 1 -v \
           > $VSS_TARGET/pub.log 2>&1; sleep 4; cat $VSS_TARGET/pub.log" 10
    ;;
  vss-connect)
    # Bridged path: Sensor Framework unit -> signal service. Start the sensor
    # service first (./demo.sh i2c or sim), then qpp, then this.
    qsh "slay -f aq_signal_connector 2>/dev/null; sleep 0.3; \
         on -d $VSS_TARGET/aq_signal_connector -m /dev/qpp -u ${2:-1}; \
         sleep 3; pidin ar | grep aq_signal_connector" 8
    ;;
  vss-read)
    SIG="${2:-Vehicle/Cabin/AirQuality/PM25}"
    qsh "echo -n '$SIG = '; cat '/dev/qpp/$SIG?text'" 5
    ;;
  vss-stop)
    qsh "slay -f aq_signal_publisher 2>/dev/null; slay -f aq_signal_connector 2>/dev/null; \
         slay -f qpp 2>/dev/null; sleep 0.5; ls /dev/qpp 2>&1; echo vss-stopped" 6
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
  gpiodrive)
    # gpiodrive N [hi|lo|up|dn|in] — set header GPIO N to a state, read back, then HOLD
    # (gpio-bcm2711 writes the pad register and exits, so the state persists — meter now).
    #   hi = OUTPUT drive high   lo = OUTPUT drive low
    #   up = INPUT + pull-up     dn = INPUT + pull-down    in = INPUT, no pull
    # 'up' is the decisive link test: CM4 GPIO2/3 have 1.8k pull-ups to VREF (3.3V) on the
    # module, so `gpiodrive 2 up` should read ~3.3V at header pin 3 IF the CM4 pad reaches
    # the header. 0V there = broken CM4<->header link (reseat the CM4). (2026-07-05 blocker.)
    G="${2:?usage: demo.sh gpiodrive <gpio#> [hi|lo|up|dn|in]}"; LV="${3:-hi}"
    case "$LV" in
      hi) SET="op dh";; lo) SET="op dl";;
      up) SET="ip pu";; dn) SET="ip pd";; in) SET="ip no";;
      *) echo "bad mode '$LV' (hi|lo|up|dn|in)"; exit 1;;
    esac
    qsh "gpio-bcm2711 set $G $SET; echo -n 'get $G => '; gpio-bcm2711 get $G" 4
    echo ">> meter header pin for GPIO$G vs a GND pin (6/9/14/20/25/30/34/39)."
    echo "   hi -> expect 3.3V | up -> expect 3.3V if CM4 pad reaches header | lo/dn -> 0V"
    ;;
  gpiosweep)
    # Print the meter plan: drive a spread of pure-GPIO header pins high, one at a time,
    # and meter each. If NONE follow -> header not driven at all (buffer/OE or mapping).
    # If SOME follow -> only part of the header is routed/buffered -> mapping/partial-buffer.
    cat <<'PLAN'
GPIO drive-test sweep — run each, meter its header pin (vs a GND pin), note V:
  gpio  phys-pin  board-silk   cmd
   2      3       SDA          ./demo.sh gpiodrive 2      (the I2C SDA — known 0V)
   4      7       GPIO4/D4     ./demo.sh gpiodrive 4
   17     11      D17          ./demo.sh gpiodrive 17
   27     13      D27          ./demo.sh gpiodrive 27
   22     15      D22          ./demo.sh gpiodrive 22
   26     37      D26          ./demo.sh gpiodrive 26
   16     36      D16          ./demo.sh gpiodrive 16
   21     40      D21          ./demo.sh gpiodrive 21
GND pins to meter against: 6, 9, 14, 20, 25, 30, 34, 39.
Reset a pin when done: ./demo.sh gpiodrive <N> lo
Read: ALL 0V -> header dead/buffered or offset mapping (leads #1/#3).
      SOME 3.3V -> partial routing/buffer -> compare which follow vs the schematic.
LED variant (lead #4, real drive proof): LED anode->header pin, cathode->330R->GND,
      then ./demo.sh gpiodrive <N> hi  (LED lights only if the pad truly drives).
PLAN
    ;;
  slog)
    qsh "slog2info -b sensor_service 2>/dev/null | tail -30" 5
    ;;
  stop)
    qsh "slay -f aq_publisher 2>/dev/null; slay -f sensor 2>/dev/null; echo stopped" 3
    ;;
  teensy)
    stty -F "$TEENSY" 115200 raw -echo 2>/dev/null
    echo "Teensy serial ($TEENSY) — type p=pollute c=clear s=state ?=help, Ctrl-C to exit"
    cat "$TEENSY" &
    CATPID=$!
    trap "kill $CATPID 2>/dev/null" EXIT
    while read -rn1 k; do printf '%s' "$k" > "$TEENSY"; done
    ;;
  *)
    grep -E '^#( |$)' "$0" | sed 's/^# \{0,1\}//'
    ;;
esac

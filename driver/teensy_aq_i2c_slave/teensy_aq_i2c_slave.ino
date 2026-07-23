/*
 * teensy_aq_i2c_slave.ino
 *
 * Teensy 4.0 as an I2C *slave* that emulates an air-quality sensor, so the
 * QNX AQ external_sensor driver's aq_bus_read_i2c() path can be exercised on
 * real I2C hardware using a stand-in sensor. Proves the plumbing and the QNX
 * i2c-bcm2711 bus, with a generic block layout (not any specific vendor protocol).
 *
 * Wiring (Teensy 4.0 3.3V logic, matches Pi I2C directly; Pi has pull-ups):
 *   Teensy pin 18 (SDA) <-> Pi GPIO2 / pin 3  (SDA1)
 *   Teensy pin 19 (SCL) <-> Pi GPIO3 / pin 5  (SCL1)
 *   Teensy GND          <-> Pi GND   / pin 6
 * Do NOT connect the Teensy 3.3V to the Pi. Power the Teensy over its own USB.
 *
 * I2C protocol (master just issues a READ of 18 bytes; no register write):
 *   little-endian, offsets:
 *     0  uint16 pm1_0      (ug/m^3)
 *     2  uint16 pm2_5      (ug/m^3)
 *     4  uint16 pm10       (ug/m^3)
 *     6  uint16 voc_index  (1..500)
 *     8  int16  temp_c_x10 (deg C * 10)
 *     10 uint16 rh_x10     (%RH * 10)
 *     12 uint16 no2_ppb    (ppb)
 *     14 uint16 co2_ppm    (ppm)
 *     16 uint8  seq        (increments each build; liveness)
 *     17 uint8  checksum   (8-bit sum of bytes 0..16)
 *
 * USB-serial demo commands (115200 baud):
 *   p = pollute   (ramp PM2.5 up to trip the QNX cabin-filter ALERT)
 *   g = co2 build (ramp CO2 up to trip the cabin CO2-buildup ALERT)
 *   c = clear     (decay both back to clean cabin baseline)
 *   s = print current state
 *   ? = help
 */
#include <Wire.h>

static const uint8_t  I2C_SLAVE_ADDR = 0x28;
static const uint16_t PM25_BASELINE  = 8;    // clean cabin
static const uint16_t PM25_MAX       = 150;
static const uint16_t PM25_STEP      = 6;
static const uint16_t CO2_BASELINE   = 500;  // clean cabin (~outdoor + a little)
static const uint16_t CO2_MAX        = 3000;
static const uint16_t CO2_STEP       = 70;

static volatile uint8_t g_block[18];   // latest measurement, sent on I2C read
static uint16_t         g_pm25 = PM25_BASELINE;
static uint16_t         g_co2  = CO2_BASELINE;
static uint8_t          g_seq  = 0;
static bool             g_pollute  = false;   // PM2.5 -> filter alert
static bool             g_co2build = false;   // CO2   -> CO2-buildup alert
static uint32_t         g_lastUpdate = 0;

// Small drifting offsets so a clean cabin still "breathes" like a real sensor
// (a live tile that never moves reads as frozen). Bounded random walks; the PM
// cap (+3) stays well under the cabin-filter ALERT threshold.
static int16_t          g_pmJit = 0;    // ug/m^3
static int16_t          g_tJit  = 0;    // deg C * 10
static int16_t          g_rhJit = 0;    // %RH * 10

static void putU16(volatile uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }

// Nudge a value by -1/0/+1 each tick, clamped to [lo, hi]: a gentle wander.
static int16_t walk(int16_t v, int16_t lo, int16_t hi)
{
    v += (int16_t)random(-1, 2);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

// Rebuild g_block from the current simulated PM2.5. Called from loop() only.
static void buildBlock()
{
    int16_t  rep  = (int16_t)g_pm25 + g_pmJit;  // reported PM2.5 with live jitter
    if (rep < 0) rep = 0;
    uint16_t pm25 = (uint16_t)rep;
    uint16_t pm1  = (uint16_t)(pm25 * 3 / 4);
    uint16_t pm10 = (uint16_t)(pm25 + 4);
    uint16_t voc  = (uint16_t)(100 + pm25 * 2);
    int16_t  tC10 = 215 + g_tJit;              // ~21.5 C, wanders ~+/-0.3
    uint16_t rh10 = (uint16_t)(400 + g_rhJit); // ~40 %RH, wanders ~+/-1.5
    uint16_t no2  = 0;
    uint16_t co2  = g_co2;

    uint8_t b[18];
    putU16(b + 0,  pm1);
    putU16(b + 2,  pm25);
    putU16(b + 4,  pm10);
    putU16(b + 6,  voc);
    putU16(b + 8,  (uint16_t)tC10);
    putU16(b + 10, rh10);
    putU16(b + 12, no2);
    putU16(b + 14, co2);
    b[16] = g_seq++;
    uint8_t sum = 0;
    for (int i = 0; i < 17; i++) sum += b[i];
    b[17] = sum;

    noInterrupts();
    for (int i = 0; i < 18; i++) g_block[i] = b[i];
    interrupts();
}

// I2C master requested a read: hand back the latest 18-byte block.
static void onRequest()
{
    uint8_t snap[18];
    for (int i = 0; i < 18; i++) snap[i] = g_block[i];
    Wire.write(snap, 18);
}

// Master may write a byte (reserved for a future register/command protocol);
// consume it so the bus doesn't stall.
static void onReceive(int n)
{
    while (Wire.available()) (void)Wire.read();
}

static void printState()
{
    Serial.print("PM2.5=");   Serial.print(g_pm25);
    Serial.print(" ug/m3  CO2="); Serial.print(g_co2);
    Serial.print(" ppm  pm_mode=");
    Serial.print(g_pollute ? "POLLUTE" : "clean");
    Serial.print("  co2_mode=");
    Serial.print(g_co2build ? "BUILD" : "clean");
    Serial.print("  seq=");   Serial.println(g_seq);
}

void setup()
{
    Serial.begin(115200);
    randomSeed(analogRead(A0) ^ micros());   // seed the jitter walk from noise
    Wire.begin(I2C_SLAVE_ADDR);     // slave on pins 18(SDA)/19(SCL)
    Wire.onRequest(onRequest);
    Wire.onReceive(onReceive);
    buildBlock();
    Serial.println("Teensy AQ I2C slave @ 0x28 ready. Commands: p=pollute g=co2 c=clear s=state ?=help");
}

void loop()
{
    if (Serial.available()) {
        int ch = Serial.read();
        switch (ch) {
        case 'p': g_pollute = true;  Serial.println("-> POLLUTE"); break;
        case 'g': g_co2build = true; Serial.println("-> CO2 BUILD"); break;
        case 'c': g_pollute = false; g_co2build = false; Serial.println("-> clear"); break;
        case 's': printState(); break;
        case '?': Serial.println("p=pollute g=co2 c=clear s=state ?=help"); break;
        default: break;
        }
    }

    // Update the simulated air roughly 4x/second.
    uint32_t now = millis();
    if (now - g_lastUpdate >= 250) {
        g_lastUpdate = now;
        if (g_pollute) {
            if (g_pm25 < PM25_MAX) g_pm25 += PM25_STEP;
        } else {
            if (g_pm25 > PM25_BASELINE) {
                g_pm25 = (g_pm25 > PM25_BASELINE + 3) ? g_pm25 - 3 : PM25_BASELINE;
            }
        }
        // CO2 buildup / decay (the 2nd alert).
        if (g_co2build) {
            if (g_co2 < CO2_MAX) g_co2 += CO2_STEP;
        } else {
            if (g_co2 > CO2_BASELINE) {
                g_co2 = (g_co2 > CO2_BASELINE + 30) ? g_co2 - 30 : CO2_BASELINE;
            }
        }
        // Gentle live drift so a steady cabin doesn't look frozen.
        g_pmJit = walk(g_pmJit, -2, 3);
        g_tJit  = walk(g_tJit,  -3, 3);
        g_rhJit = walk(g_rhJit, -15, 15);
        buildBlock();
    }
}

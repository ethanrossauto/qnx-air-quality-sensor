// Continuity beacon: hold pin 18 (SDA) LOW forever, pin 19 released.
// Read the Pi: GPIO2 low => SDA wired correctly; GPIO3 low => SDA/SCL swapped;
// neither => SDA wire not making contact.
void setup(){ pinMode(19, INPUT); pinMode(18, OUTPUT); digitalWrite(18, LOW); }
void loop(){}

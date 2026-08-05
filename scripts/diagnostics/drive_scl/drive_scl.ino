// Hold pin 19 (SCL) LOW forever, pin 18 released.
void setup(){ pinMode(18, INPUT); pinMode(19, OUTPUT); digitalWrite(19, LOW); }
void loop(){}

// Continuity test: drive Teensy I2C pins low on serial command so the Pi can
// see which wire is actually connected (and whether SDA/SCL are swapped).
//   '2' = drive pin 18 (SDA) LOW, release 19
//   '3' = drive pin 19 (SCL) LOW, release 18
//   '0' = release both (idle, high-Z)
const int SDA_PIN = 18, SCL_PIN = 19;
void release(){ pinMode(SDA_PIN, INPUT); pinMode(SCL_PIN, INPUT); }
void setup(){ Serial.begin(115200); release(); Serial.println("continuity: 2=SDA-low 3=SCL-low 0=release"); }
void loop(){
  if(Serial.available()){
    int c = Serial.read();
    if(c=='2'){ pinMode(SCL_PIN,INPUT); pinMode(SDA_PIN,OUTPUT); digitalWrite(SDA_PIN,LOW); Serial.println("driving pin18(SDA) LOW"); }
    else if(c=='3'){ pinMode(SDA_PIN,INPUT); pinMode(SCL_PIN,OUTPUT); digitalWrite(SCL_PIN,LOW); Serial.println("driving pin19(SCL) LOW"); }
    else if(c=='0'){ release(); Serial.println("released"); }
  }
}

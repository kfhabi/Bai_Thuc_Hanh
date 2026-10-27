#include <Arduino.h>
#include <DHT.h>
#include <Wire.h>
#include <esp_system.h>
#include <hd44780.h>
#include <hd44780ioClass/hd44780_I2Cexp.h>
#include "sampling_schedule.h"
#include "th2_config.h"
#include "th2_core.h"

using namespace th2config;
namespace {
const char *MODE = sampling::name();
struct Sample {
  uint32_t seq=0, tMs=0, dtMs=0;
  float temperature=NAN, humidity=NAN, temperatureMa5=NAN, humidityMa5=NAN;
  uint16_t adcRaw=0;
  float adcPercent=0;
  uint32_t adcMv=0, readMs=0;
  bool valid=false;
  const char *error="";
};
DHT dht(DHT_PIN, DHT11);
hd44780_I2Cexp lcd;
th2::MovingAverage5 temperatureFilter, humidityFilter;
th2::DebouncedButton button(BUTTON_DEBOUNCE_MS);
Sample lastSample;
uint32_t sampleCount=0, nextEventId=1, validCount=0, errorCount=0;
uint32_t buttonPressCount=0, lastMeasurementMs=0;
bool hasMeasurement=false, recording=true, lcdDetected=false;
bool lcdFilteredPage=false, lcdDirty=true;
char bootId[12]={}, sessionId[32]={};
int lcdStatus=-1;
uint8_t lcdAddress=0, i2cAddresses[16]={}, i2cAddressCount=0;
uint32_t i2cScanMs=0;
const char *sampleHeader=
  "boot_id,session_id,mode,seq,t_ms,dt_ms,temp_c,rh_pct,valid,error,"
  "adc_raw,adc_percent,adc_mv,temp_ma5,rh_ma5,read_ms";

String csvNumber(float value) { return isfinite(value)?String(value,2):String(); }
String csvQuoted(const char *value) {
  String out="\"";
  while(*value) { if(*value=='"') out+='"'; out+=*value++; }
  return out+'"';
}
void addEvent(const char *type,const String &detail) {
  Serial.print("#EVENT,");
  Serial.print(String(bootId)+','+sessionId+','+MODE+','+String(nextEventId++)+','+String(millis())+',');
  Serial.println(csvQuoted(type)+','+csvQuoted(detail.c_str()));
}
void startNewSession() {
  snprintf(sessionId,sizeof(sessionId),"%s-1",bootId);
  Serial.println(sampleHeader);
  addEvent("session","TH02 individual option A; mode="+String(MODE));
}
void scanI2c() {
  i2cAddressCount=0; const uint32_t started=millis();
  for(uint8_t address=1;address<127;++address) {
    Wire.beginTransmission(address);
    if(Wire.endTransmission()==0 && i2cAddressCount<sizeof(i2cAddresses)) {
      i2cAddresses[i2cAddressCount++]=address;
      Serial.printf("#I2C,0x%02X,ACK\n",address);
    }
  }
  i2cScanMs=millis()-started;
  String detail="SDA="+String(SDA_PIN)+" SCL="+String(SCL_PIN)+" addresses=";
  if(!i2cAddressCount) detail+="none";
  for(uint8_t i=0;i<i2cAddressCount;++i) {
    char address[8]; snprintf(address,sizeof(address),"0x%02X",i2cAddresses[i]);
    if(i) detail+=' '; detail+=address;
  }
  addEvent("i2c_scan",detail+" scan_ms="+String(i2cScanMs));
}
void initializeLcd() {
  lcdDetected=false; lcdAddress=0;
  if(!LCD_ENABLED) { lcdStatus=-2; addEvent("lcd","disabled in th2_config.h"); return; }
  if(!i2cAddressCount) { lcdStatus=-1; addEvent("lcd","no I2C device; Serial logging continues"); return; }
  lcdStatus=lcd.begin(LCD_COLS,LCD_ROWS); lcdDetected=lcdStatus==0;
  if(lcdDetected) {
    lcdAddress=lcd.getProp(hd44780_I2Cexp::Prop_addr); lcd.backlight();
    addEvent("lcd","initialized address="+String(lcdAddress)+" geometry="+
      String(LCD_COLS)+"x"+String(LCD_ROWS));
  } else addEvent("lcd","initialization failed status="+String(lcdStatus));
  lcdDirty=true;
}
void printLcdLine(uint8_t row,const String &text) {
  if(!lcdDetected || row>=LCD_ROWS) return;
  lcd.setCursor(0,row);
  for(uint8_t col=0;col<LCD_COLS;++col) lcd.write(col<text.length()?text[col]:' ');
}
void updateLcd() {
  if(!lcdDirty || !lcdDetected) return;
  lcdDirty=false;
  if(!sampleCount) {
    printLcdLine(0,"TH02 "+String(MODE)); printLcdLine(1,"Waiting DHT11...");
  } else {
    const Sample &s=lastSample;
    if(!s.valid) {
      printLcdLine(0,"DHT11 READ ERROR");
      printLcdLine(1,"ADC:"+String(s.adcRaw)+" #"+String(s.seq));
    } else if(!lcdFilteredPage) {
      printLcdLine(0,"T:"+String(s.temperature,1)+"C H:"+String(s.humidity,0)+"%");
      printLcdLine(1,"ADC:"+String(s.adcRaw)+" #"+String(s.seq));
    } else {
      printLcdLine(0,"MA5 T:"+(isfinite(s.temperatureMa5)?String(s.temperatureMa5,1)+"C":String("waiting")));
      printLcdLine(1,"H:"+(isfinite(s.humidityMa5)?String(s.humidityMa5,1)+"%":String("waiting"))+
        " A:"+String(s.adcPercent,0)+"%");
    }
  }
  if(LCD_ROWS>2) printLcdLine(2,String(MODE)+" "+(recording?"REC":"STOP"));
  if(LCD_ROWS>3) printLcdLine(3,"Button: "+String(buttonPressCount));
}
void toggleLcd(const char *source) {
  lcdFilteredPage=!lcdFilteredPage; lcdDirty=true;
  addEvent(source,String("lcd_page=")+(lcdFilteredPage?"filtered":"raw"));
}
String sampleCsv(const Sample &s) {
  String line; line.reserve(200);
  line=String(bootId)+','+sessionId+','+MODE+','+String(s.seq)+','+String(s.tMs)+','+String(s.dtMs);
  line+=','+csvNumber(s.temperature)+','+csvNumber(s.humidity);
  line+=s.valid?",1,":",0,"; line+=s.error;
  line+=','+String(s.adcRaw)+','+String(s.adcPercent,2)+','+String(s.adcMv);
  line+=','+csvNumber(s.temperatureMa5)+','+csvNumber(s.humidityMa5)+','+String(s.readMs);
  return line;
}
void takeSample() {
  if(!recording || sampleCount>=MAX_SAMPLES) return;
  const uint32_t started=millis();
  // No backfilled samples after a slow request, pause, or new session.
  if(hasMeasurement && !th2::sampleDue(started,lastMeasurementMs,SAMPLE_PERIOD_MS)) return;
  lastMeasurementMs=started; hasMeasurement=true;
  Sample s; s.seq=sampleCount+1; s.tMs=started;
  s.dtMs=sampleCount?uint32_t(started-lastSample.tMs):0;
  s.adcRaw=analogRead(ADC_PIN); s.adcPercent=s.adcRaw*100.0f/4095.0f;
  // Calibrated voltage is a second ADC conversion, not the same raw sample.
  s.adcMv=analogReadMilliVolts(ADC_PIN);
  const float humidity=dht.readHumidity(), temperature=dht.readTemperature();
  if(!isfinite(temperature)||!isfinite(humidity)) s.error="dht_read_failed";
  else if(humidity<0||humidity>100||temperature< -40||temperature>80) s.error="out_of_range";
  else {
    s.valid=true; s.temperature=temperature; s.humidity=humidity;
    temperatureFilter.add(temperature); humidityFilter.add(humidity);
    s.temperatureMa5=temperatureFilter.value(); s.humidityMa5=humidityFilter.value();
  }
  s.readMs=millis()-started; lastSample=s; ++sampleCount;
  if(s.valid) ++validCount;
  else { ++errorCount; addEvent("sensor_error","seq="+String(s.seq)+" "+s.error); }
  Serial.println(sampleCsv(s)); lcdDirty=true;
  if(sampleCount==MAX_SAMPLES) {
    recording=false; addEvent("finished","samples="+String(sampleCount)+" valid="+String(validCount)+" errors="+String(errorCount));
  }
}

} // namespace

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN,INPUT_PULLUP);
  pinMode(ADC_PIN,INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(ADC_PIN,ADC_11db);
  dht.begin();
  snprintf(bootId,sizeof(bootId),"%08lx",(unsigned long)esp_random());
  startNewSession();
  Wire.begin(SDA_PIN,SCL_PIN);
  Wire.setClock(100000);
  Wire.setTimeOut(20);
  scanI2c();
  initializeLcd();
  sampling::reset(millis());
  updateLcd();
  Serial.printf("#READY,TH02,%s,period_ms=%lu,max_samples=%u\n",MODE,
    (unsigned long)SAMPLE_PERIOD_MS,(unsigned)MAX_SAMPLES);
}

void loop() {
  if(button.update(digitalRead(BUTTON_PIN)==LOW,millis())) {
    ++buttonPressCount;
    toggleLcd("button");
  }
  if(recording && sampling::ready(millis())) takeSample();
  updateLcd();
  delay(1); // Yield one RTOS tick; this is not the sampling interval.
}

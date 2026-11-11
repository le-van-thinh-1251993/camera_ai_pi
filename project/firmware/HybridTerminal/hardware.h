#pragma once
#include <Arduino.h>
#include <atomic>
#include <driver/gpio.h>
#include "config.h"
#include <Preferences.h>
#if ENABLE_CAMERA
#include "esp_camera.h"
#endif
#if ENABLE_I2S_AUDIO
#include <ESP_I2S.h>
#endif
#if TFT_KIND
#include <LovyanGFX.hpp>
class TerminalLCD : public lgfx::LGFX_Device {
  lgfx::Bus_SPI bus;
#if TFT_KIND == 1
  lgfx::Panel_ILI9488 panel;
#elif TFT_KIND == 2
  lgfx::Panel_ST7796 panel;
#elif TFT_KIND == 3
  lgfx::Panel_ILI9341 panel;
#else
#error Unsupported TFT_KIND. Add a real panel adapter before enabling it.
#endif
public:
  // Doi toc do ghi SPI truoc lcd.init() (gia tri luu trong NVS, xem /cam?spi=).
  void setWriteHz(uint32_t hz) { auto b = bus.config(); b.freq_write = hz; bus.config(b); }
  TerminalLCD() {
    auto b=bus.config(); b.spi_host=SPI2_HOST; b.spi_mode=0;
    b.freq_write=TFT_SPI_HZ; b.freq_read=8000000; b.pin_sclk=TFT_SCLK;
    b.pin_mosi=TFT_MOSI; b.pin_miso=TFT_MISO; b.pin_dc=TFT_DC;
    b.spi_3wire=TFT_MISO<0; b.use_lock=true; b.dma_channel=SPI_DMA_CH_AUTO;
    bus.config(b); panel.setBus(&bus);
    auto p=panel.config(); p.pin_cs=TFT_CS; p.pin_rst=TFT_RST; p.pin_busy=-1;
    p.panel_width=TFT_NATIVE_WIDTH; p.panel_height=TFT_NATIVE_HEIGHT;
    p.memory_width=TFT_NATIVE_WIDTH; p.memory_height=TFT_NATIVE_HEIGHT;
    p.invert=TFT_INVERT; p.rgb_order=TFT_RGB_ORDER; panel.config(p); setPanel(&panel);
  }
};
static TerminalLCD lcd;
#endif
static bool lcdReady=false, cameraReady=false;
static std::atomic<int> currentVolumePercent{AUDIO_GAIN_PERCENT};

static void drawDisplayDemo() {
#if TFT_KIND
  if (!lcdReady) return;
  lcd.fillScreen(0x0000);
  lcd.setTextSize(2);
  lcd.setTextColor(0x07FF);
  lcd.setCursor(55, 18);
  lcd.print("GESTURE TO SPEECH");
  lcd.setTextSize(1);
  lcd.setTextColor(0xFFFF);
  lcd.setCursor(55, 55);
  lcd.print("CAMERA / RESULT DISPLAY");
  lcd.drawRect(18, 82, lcd.width() - 36, 118, 0x07E0);
  lcd.setCursor(35, 118);
  lcd.print("CAMERA FRAME");
  lcd.setCursor(35, 138);
  lcd.print("Large preview area");
  lcd.setCursor(35, 158);
  lcd.print("Ready for hand gesture");
  lcd.fillRect(18, 220, lcd.width() - 36, 42, 0x2348);
  lcd.setTextColor(0xFFFF);
  lcd.setCursor(35, 237);
  lcd.print("Press NEXT to recognize");
  lcd.setCursor(35, lcd.height() - 42);
  lcd.setTextColor(0xFFE0);
  lcd.print("VOL+  VOL-  MANUAL  POWER");
#endif
}

static void drawGreetingScreen() {
#if TFT_KIND
  if (!lcdReady) return;
  lcd.fillScreen(0x0000);
  lcd.setTextColor(0x07FF); lcd.setTextSize(2); lcd.setCursor(42, 22);
  lcd.print("XIN CHAO THAY");
  lcd.setTextSize(1); lcd.setTextColor(0xFFFF);
  lcd.setCursor(28, 72); lcd.print("Xin chao thay va cac ban");
  lcd.setCursor(28, 94); lcd.print("den voi buoi Demo");
  lcd.setCursor(28, 116); lcd.print("bai tap lon.");
  lcd.setCursor(28, lcd.height() - 30); lcd.print("Nhan NEXT de bat dau");
#endif
}

static void drawCameraView(float held, bool automatic) {
#if TFT_KIND
  if (!lcdReady) return;
  lcd.fillScreen(0x0000);
  lcd.setTextColor(0x07E0); lcd.setTextSize(2); lcd.setCursor(18, 12);
  lcd.print(automatic ? "AUTO - GIU TAY" : "CAMERA");
  lcd.setTextSize(1); lcd.setTextColor(0xFFFF);
  lcd.setCursor(18, 43); lcd.printf("Hold %.1f / 2.0 sec", held);
  lcd.drawRect(10, 62, lcd.width() - 20, lcd.height() - 112, 0xFFFF);
  lcd.setCursor(30, lcd.height() - 38); lcd.print("NEXT: chup  MANUAL: dashboard");
#endif
}

static void drawResultView(const String &gesture, const String &meaning) {
#if TFT_KIND
  if (!lcdReady) return;
  lcd.fillScreen(0x0000);
  lcd.setTextColor(0x07FF); lcd.setTextSize(2); lcd.setCursor(42, 14);
  lcd.print("KET QUA");
  lcd.setTextColor(0xFFE0); lcd.setTextSize(3); lcd.setCursor(35, 70);
  lcd.print(gesture);
  lcd.setTextColor(0xFFFF); lcd.setTextSize(1); lcd.setCursor(25, 145);
  lcd.print("Y NGHIA / LOA DOC:");
  lcd.setCursor(25, 170); lcd.print(meaning);
  lcd.setTextColor(0x07E0); lcd.setCursor(25, lcd.height() - 38);
  lcd.print("NEXT: nhan dien tiep  MANUAL: dashboard");
#endif
}

static void drawManualView() {
#if TFT_KIND
  if (!lcdReady) return;
  lcd.fillScreen(0x0000);
  lcd.setTextColor(0x07FF); lcd.setTextSize(2); lcd.setCursor(45, 28);
  lcd.print("MANUAL MODE");
  lcd.setTextColor(0xFFFF); lcd.setTextSize(1); lcd.setCursor(32, 90);
  lcd.print("Thao tac chup anh tren Dashboard");
  lcd.setCursor(32, 115); lcd.print("Ket qua va y nghia se hien o day");
  lcd.setCursor(32, 140); lcd.print("va phat qua loa dong bo.");
  lcd.setCursor(32, lcd.height() - 38); lcd.print("NEXT: quay lai nhan dien");
#endif
}

static void drawStandbyView() {
#if TFT_KIND
  if (!lcdReady) return;
  lcd.fillScreen(0x0000); lcd.setTextColor(0x07FF); lcd.setTextSize(2);
  lcd.setCursor(70, 120); lcd.print("STANDBY");
  lcd.setTextSize(1); lcd.setTextColor(0xFFFF); lcd.setCursor(62, 165);
  lcd.print("Nhan POWER de bat lai");
#endif
}

static void drawError(const String &message) {
#if TFT_KIND
  if (!lcdReady) return;
  lcd.fillScreen(0x0000);
  lcd.setTextColor(0xF800); lcd.setTextSize(2); lcd.setCursor(24, 24);
  lcd.print("LOI");
  lcd.setTextColor(0xFFFF); lcd.setTextSize(1); lcd.setCursor(24, 80);
  lcd.print(message);
  lcd.setTextColor(0x07E0); lcd.setCursor(24, lcd.height() - 38);
  lcd.print("NEXT: thu lai   MANUAL: Dashboard");
#endif
}

// No pin is guessed; even an enabled driver refuses incomplete/duplicate mappings.
static bool validatePins() {
  int used[64]; size_t count=0; bool ok=true;
  auto add=[&](int pin, bool required, bool output) {
    if(pin<0) {if(required) ok=false;return;}
    if(!GPIO_IS_VALID_GPIO(pin) || (output && !GPIO_IS_VALID_OUTPUT_GPIO(pin)) ||
       (pin>=26 && pin<=32) || (pin>=35 && pin<=37)) {
      ok=false;return;
    }
    if (pin==0 || pin==3 || pin==45 || pin==46 || pin==19 || pin==20) {
      Serial.printf("WARNING: GPIO %d is a strapping/USB pin; verify board boot and USB use.\n", pin);
    }
#if ENABLE_CAMERA && TFT_KIND && SHARED_CAM_TFT_BUS
    // PCB hien tai: TFT co y dung chung Y2..Y6, live_camera.h chia thoi gian.
    if(pin==TFT_MOSI||pin==TFT_CS||pin==TFT_RST||pin==TFT_DC||pin==TFT_SCLK) {
      bool camData=false;
      for(int c:{Y2_GPIO_NUM,Y3_GPIO_NUM,Y4_GPIO_NUM,Y5_GPIO_NUM,Y6_GPIO_NUM}) if(c==pin) camData=true;
      if(camData) {if(count<64) used[count++]=pin; return;}
    }
#endif
    for(size_t i=0;i<count;i++) if(used[i]==pin) ok=false;
    if(count<64) used[count++]=pin; else ok=false;
  };
#if ENABLE_CAMERA
  for(int pin:{Y2_GPIO_NUM,Y3_GPIO_NUM,Y4_GPIO_NUM,Y5_GPIO_NUM,Y6_GPIO_NUM,Y7_GPIO_NUM,Y8_GPIO_NUM,Y9_GPIO_NUM,
               PCLK_GPIO_NUM,VSYNC_GPIO_NUM,HREF_GPIO_NUM}) add(pin,true,false);
  for(int pin:{XCLK_GPIO_NUM,SIOD_GPIO_NUM,SIOC_GPIO_NUM}) add(pin,true,true);
  add(PWDN_GPIO_NUM,false,true); add(RESET_GPIO_NUM,false,true);
#endif
#if TFT_KIND
  for(int pin:{TFT_SCLK,TFT_MOSI,TFT_DC}) add(pin,true,true);
  add(TFT_CS,false,true); // -1 khi CS noi GND (bo da sua)
  add(TFT_MISO,false,false);add(TFT_RST,false,true);add(TFT_BL,false,true);
  if(TFT_NATIVE_WIDTH<=0 || TFT_NATIVE_HEIGHT<=0) ok=false;
#endif
#if ENABLE_I2S_AUDIO
  for(int pin:{AUDIO_BCLK,AUDIO_LRCLK,AUDIO_DOUT}) add(pin,true,true);
  add(AUDIO_MCLK,false,true);
#endif
  for(int pin:{BTN_POWER,BTN_NEXT,BTN_VOLUME_UP,BTN_VOLUME_DOWN,BTN_MANUAL}) add(pin,false,false);
  return ok;
}
static bool beginHardware() {
  if(!validatePins()) {Serial.println("CONFIG ERROR: missing/duplicate/invalid GPIO or panel dimensions; hardware not started.");return false;}
#if TFT_KIND
  {  // Toc do SPI: gia tri thu qua /cam?spi=<MHz> (NVS "tft"/"spi_mhz"), 0 = TFT_SPI_HZ
    Preferences p;
    p.begin("tft", true);
    uint32_t mhz = p.getUInt("spi_mhz", 0);
    p.end();
    uint32_t hz = mhz ? mhz * 1000000UL : (uint32_t)TFT_SPI_HZ;
    lcd.setWriteHz(hz);
    Serial.printf("[TFT] SPI %lu MHz%s\n", (unsigned long)(hz / 1000000), mhz ? " (dat qua /cam?spi=)" : "");
  }
  lcd.init();
  if(TFT_RST<0){ // RESET noi EN: reset mem (0x01) phong khi ESP32 khoi dong lai bang phan mem
    lcd.startWrite();lcd.writeCommand(0x01);lcd.endWrite();delay(150);lcd.init();
  }
  lcd.setRotation(TFT_ROTATION);lcd.fillScreen(0);lcdReady=true;
  if(TFT_BL>=0){pinMode(TFT_BL,OUTPUT);digitalWrite(TFT_BL,TFT_BL_ACTIVE_HIGH?HIGH:LOW);}
#endif
  // Camera khoi tao trong live_camera.h (liveCameraBegin); PCB goc can goi
  // truoc lcd.init() de nhuong bus dung chung dung thu tu.
  return true;
}
static int screenWidth(){
#if TFT_KIND
  if(lcdReady)return lcd.width();
#endif
  return 480; // Logical Serial/simulator image only; not a detected panel dimension
}
static int screenHeight(){
#if TFT_KIND
  if(lcdReady)return lcd.height();
#endif
  return 320;
}
static void drawCamera(const uint8_t* jpg,size_t len,float held){
#if TFT_KIND
  if(!lcdReady)return;
  float scale=fminf((lcd.width()-20)/640.f,(lcd.height()-112)/480.f);
  int x=(lcd.width()-int(640*scale))/2,y=62+(lcd.height()-112-int(480*scale))/2;
  lcd.drawJpg(jpg,len,x,y,int(640*scale),int(480*scale),0,0,scale);
  lcd.fillRect(0,0,lcd.width(),52,0);lcd.setTextColor(0x07E0);lcd.setTextSize(2);
  lcd.setCursor(18,12);lcd.print("CAMERA");
  lcd.setTextSize(1);lcd.setTextColor(0xFFFF);lcd.setCursor(18,38);lcd.printf("Hold %.1f / 2.0 sec",held);
  lcd.fillRect(0,lcd.height()-42,lcd.width(),42,0);lcd.setCursor(18,lcd.height()-27);
  lcd.print("NEXT: chup   MANUAL: dashboard");
#endif
}
static void drawScreen(const uint8_t* jpg,size_t len){
#if TFT_KIND
  if(lcdReady)lcd.drawJpg(jpg,len,0,0);
#endif
}
static uint16_t read16(const uint8_t* p){return uint16_t(p[0])|(uint16_t(p[1])<<8);}
static uint32_t read32(const uint8_t* p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
static bool playWav(uint8_t* wav,size_t length,std::atomic<bool>& cancel){
  if(length<44 || memcmp(wav,"RIFF",4)||memcmp(wav+8,"WAVE",4))return false;
  uint32_t rate=0;uint16_t format=0,channels=0,bits=0;size_t data=0,bytes=0;
  for(size_t p=12;p+8<=length;){uint32_t n=read32(wav+p+4);size_t start=p+8;if(n>length-start)return false;
    if(!memcmp(wav+p,"fmt ",4)&&n>=16){format=read16(wav+start);channels=read16(wav+start+2);rate=read32(wav+start+4);bits=read16(wav+start+14);}
    if(!memcmp(wav+p,"data",4)){data=start;bytes=n;}
    p=start+n+(n&1);
  }
  if(format!=1||channels!=1||bits!=16||rate<8000||rate>48000||!data||bytes%2)return false;
#if ENABLE_I2S_AUDIO
  I2SClass output;output.setPins(AUDIO_BCLK,AUDIO_LRCLK,AUDIO_DOUT,-1,AUDIO_MCLK);
  if(!output.begin(I2S_MODE_STD,rate,I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_MONO))return false;
  bool ok=true;
  for(size_t offset=0;offset<bytes&&!cancel;){
    size_t n=fmin(size_t(1024),bytes-offset);
    for(size_t i=0;i<n;i+=2){int16_t value=int16_t(read16(wav+data+offset+i));value=int32_t(value)*constrain(currentVolumePercent.load(),0,100)/100;
      wav[data+offset+i]=value&255;wav[data+offset+i+1]=(value>>8)&255;}
    if(output.write(wav+data+offset,n)!=n){ok=false;break;}offset+=n;
  }
  output.end();return ok&&!cancel;
#else
  Serial.println("Audio disabled: configure a verified I2S amplifier and pins.");return false;
#endif
}

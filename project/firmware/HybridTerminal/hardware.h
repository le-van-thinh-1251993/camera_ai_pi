#pragma once
#include <Arduino.h>
#include <driver/gpio.h>
#include "config.h"
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
static int currentVolumePercent = AUDIO_GAIN_PERCENT;

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

// No pin is guessed; even an enabled driver refuses incomplete/duplicate mappings.
static bool validatePins() {
  int used[64]; size_t count=0; bool ok=true;
  auto add=[&](int pin, bool required, bool output) {
    if(pin<0) {if(required) ok=false;return;}
    if(!GPIO_IS_VALID_GPIO(pin) || (output && !GPIO_IS_VALID_OUTPUT_GPIO(pin))) {ok=false;return;}
    for(size_t i=0;i<count;i++) if(used[i]==pin) ok=false;
    if(count<64) used[count++]=pin; else ok=false;
  };
#if ENABLE_CAMERA
  for(int pin:{CAM_D0,CAM_D1,CAM_D2,CAM_D3,CAM_D4,CAM_D5,CAM_D6,CAM_D7,CAM_PCLK,CAM_VSYNC,CAM_HREF}) add(pin,true,false);
  for(int pin:{CAM_XCLK,CAM_SDA,CAM_SCL}) add(pin,true,true);
  add(CAM_PWDN,false,true); add(CAM_RESET,false,true);
#endif
#if TFT_KIND
  for(int pin:{TFT_SCLK,TFT_MOSI,TFT_CS,TFT_DC}) add(pin,true,true);
  add(TFT_MISO,false,false);add(TFT_RST,false,true);add(TFT_BL,false,true);
  if(TFT_NATIVE_WIDTH<=0 || TFT_NATIVE_HEIGHT<=0) ok=false;
#endif
#if ENABLE_I2S_AUDIO
  for(int pin:{AUDIO_BCLK,AUDIO_LRCLK,AUDIO_DOUT}) add(pin,true,true);
  add(AUDIO_MCLK,false,true);
#endif
  for(int pin:{BTN_HOME,BTN_UP,BTN_DOWN,BTN_SELECT}) add(pin,false,false);
  return ok;
}
static bool beginHardware() {
  if(!validatePins()) {Serial.println("CONFIG ERROR: missing/duplicate/invalid GPIO or panel dimensions; hardware not started.");return false;}
#if TFT_KIND
  lcd.init();lcd.setRotation(TFT_ROTATION);lcd.fillScreen(0);lcdReady=true;
  if(TFT_BL>=0){pinMode(TFT_BL,OUTPUT);digitalWrite(TFT_BL,TFT_BL_ACTIVE_HIGH?HIGH:LOW);}
#endif
#if ENABLE_CAMERA
  camera_config_t c={};c.ledc_channel=LEDC_CHANNEL_0;c.ledc_timer=LEDC_TIMER_0;
  c.pin_d0=CAM_D0;c.pin_d1=CAM_D1;c.pin_d2=CAM_D2;c.pin_d3=CAM_D3;
  c.pin_d4=CAM_D4;c.pin_d5=CAM_D5;c.pin_d6=CAM_D6;c.pin_d7=CAM_D7;
  c.pin_xclk=CAM_XCLK;c.pin_pclk=CAM_PCLK;c.pin_vsync=CAM_VSYNC;c.pin_href=CAM_HREF;
  c.pin_sccb_sda=CAM_SDA;c.pin_sccb_scl=CAM_SCL;c.pin_pwdn=CAM_PWDN;c.pin_reset=CAM_RESET;
  c.xclk_freq_hz=CAM_XCLK_HZ;c.pixel_format=PIXFORMAT_JPEG;c.frame_size=FRAMESIZE_VGA;
  c.jpeg_quality=10;c.fb_count=psramFound()?2:1;c.grab_mode=CAMERA_GRAB_LATEST;
  c.fb_location=psramFound()?CAMERA_FB_IN_PSRAM:CAMERA_FB_IN_DRAM;
  cameraReady=esp_camera_init(&c)==ESP_OK;
  if(cameraReady){auto sensor=esp_camera_sensor_get();sensor->set_vflip(sensor,CAM_VFLIP);sensor->set_hmirror(sensor,CAM_HMIRROR);}
  else Serial.println("Camera initialization failed; inspect verified camera config.");
#endif
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
static void drawMenu(int selected){
  static const char* names[]={"Auto: hold 2 seconds","Manual: preview","Resume camera","Freeze / stop camera","Recognize frozen frame","Replay result","Play greeting","Toggle count both hands"};
#if TFT_KIND
  if(!lcdReady)return; lcd.fillScreen(0);lcd.setTextSize(1);lcd.setTextColor(0x6FF7);
  lcd.setCursor(12,10);lcd.print("GESTURE TO SPEECH - MENU");
  int step=(lcd.height()-55)/8;
  for(int i=0;i<8;i++){int y=36+i*step;if(i==selected)lcd.fillRect(4,y-3,lcd.width()-8,step,0x2348);
    lcd.setCursor(12,y);lcd.setTextColor(0xFFFF);lcd.print(names[i]);}
  lcd.setCursor(8,lcd.height()-16);lcd.print("UP / DOWN: move   SELECT: choose");
#endif
  Serial.printf("Menu %d: %s\n",selected,names[selected]);
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
    for(size_t i=0;i<n;i+=2){int16_t value=int16_t(read16(wav+data+offset+i));value=int32_t(value)*constrain(currentVolumePercent,0,100)/100;
      wav[data+offset+i]=value&255;wav[data+offset+i+1]=(value>>8)&255;}
    if(output.write(wav+data+offset,n)!=n){ok=false;break;}offset+=n;
  }
  output.end();return ok&&!cancel;
#else
  Serial.println("Audio disabled: configure a verified I2S amplifier and pins.");return false;
#endif
}

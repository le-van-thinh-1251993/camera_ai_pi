// =====================================================================
// THU MAN ILI9488 3.5" TREN BO DA SUA - khong bat camera, WiFi, loa.
//
// Dung de tach loi phan cung (day noi / cat duong chua dut) khoi loi firmware.
// Nap sketch nay, mo Serial Monitor 115200.
//
//   Khoi dong: init man, to lan luot DO -> XANH LA -> XANH DUONG -> chu + khung.
//     Thay mau dung        -> day noi OK, loi nam o firmware chinh -> gui log Serial.
//     Van trang            -> loi phan cung: chay lenh 'p' de do tung chan.
//
// Lenh Serial:
//   t = chay lai bai thu mau      s = doi toc do SPI (10 -> 13 -> 16 -> 20 -> 4 MHz)
//   p = do tung chan: dat 1 chan HIGH 4 giay roi LOW 4 giay, lan luot SCK, MOSI, DC.
//       Do bang dong ho van nang tai CHAN SOCKET TFT (khong do tai ESP32):
//       HIGH ~3.3V, LOW ~0V. Khong theo -> day dut / con dinh voi duong cu GPIO 8-12.
// =====================================================================
#include <LovyanGFX.hpp>

// Giong config_35inch_tft.h (bo da sua)
static constexpr int PIN_SCK = 1;   // socket chan 7
static constexpr int PIN_MOSI = 2;  // socket chan 6
static constexpr int PIN_DC = 48;   // socket chan 5
// Thu bang day dupont (bo chi co 1 GND + 1 3V3): CS -> GPIO39, RESET -> GPIO40, code dieu khien.
// Tren mach in (CS noi GND, RESET noi EN): doi ca hai thanh -1.
static constexpr int PIN_CS = 39;
static constexpr int PIN_RST = 40;

// Bat dau 4 MHz (cham nhat, chac nhat); 's' tang dan.
static const uint32_t SPEEDS[] = {4000000, 8000000, 10000000, 16000000, 20000000};
static int speedIdx = 0;

class TestLCD : public lgfx::LGFX_Device {
 public:
  lgfx::Bus_SPI bus;
  lgfx::Panel_ILI9488 panel;
  void setup(uint32_t hz) {
    auto b = bus.config();
    b.spi_host = SPI2_HOST; b.spi_mode = 0;
    b.freq_write = hz; b.freq_read = 4000000;
    b.pin_sclk = PIN_SCK; b.pin_mosi = PIN_MOSI; b.pin_miso = -1; b.pin_dc = PIN_DC;
    b.spi_3wire = true; b.use_lock = true; b.dma_channel = SPI_DMA_CH_AUTO;
    bus.config(b);
    panel.setBus(&bus);
    auto p = panel.config();
    p.pin_cs = PIN_CS; p.pin_rst = PIN_RST; p.pin_busy = -1;
    p.panel_width = 320; p.panel_height = 480;
    p.memory_width = 320; p.memory_height = 480;
    p.invert = false; p.rgb_order = false;
    panel.config(p);
    setPanel(&panel);
  }
};
static TestLCD lcd;
static bool started = false;

static void colorTest() {
  uint32_t hz = SPEEDS[speedIdx];
  Serial.printf("[TFT] SCK=%d MOSI=%d DC=%d CS=%d RST=%d  SPI=%lu Hz\n", PIN_SCK, PIN_MOSI, PIN_DC,
                PIN_CS, PIN_RST, (unsigned long)hz);
  if (!started) { lcd.setup(hz); started = true; }
  else { lcd.bus.release(); auto b = lcd.bus.config(); b.freq_write = hz; lcd.bus.config(b); }
  bool ok = lcd.init();
  // RESET noi EN: reset mem them 1 lan nhu firmware chinh
  lcd.startWrite(); lcd.writeCommand(0x01); lcd.endWrite(); delay(150);
  ok = lcd.init() && ok;
  Serial.printf("[TFT] init %s\n", ok ? "OK" : "LOI");
  lcd.setRotation(3);
  const uint32_t colors[] = {0xFF0000, 0x00FF00, 0x0000FF};
  const char* names[] = {"DO", "XANH LA", "XANH DUONG"};
  for (int i = 0; i < 3; i++) {
    Serial.printf("[TFT] To mau %s\n", names[i]);
    lcd.fillScreen(lcd.color888(colors[i] >> 16, (colors[i] >> 8) & 0xFF, colors[i] & 0xFF));
    delay(900);
  }
  lcd.fillScreen(TFT_BLACK);
  lcd.drawRect(0, 0, lcd.width(), lcd.height(), TFT_WHITE);
  lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  lcd.setTextSize(2);
  lcd.setCursor(16, 20);
  lcd.printf("TFT OK  %dx%d", (int)lcd.width(), (int)lcd.height());
  lcd.setCursor(16, 50);
  lcd.printf("SPI %lu MHz", (unsigned long)(hz / 1000000));
  lcd.fillRect(16, 90, 100, 60, TFT_RED);
  lcd.fillRect(126, 90, 100, 60, TFT_GREEN);
  lcd.fillRect(236, 90, 100, 60, TFT_BLUE);
  Serial.println("[TFT] Xong. Man phai hien khung trang, chu va 3 o mau.");
}

static void pinProbe() {
  if (started) { lcd.bus.release(); }
  const int pins[] = {PIN_SCK, PIN_MOSI, PIN_DC};
  const char* names[] = {"SCK (socket chan 7)", "MOSI (socket chan 6)", "DC (socket chan 5)"};
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) { pinMode(pins[j], OUTPUT); digitalWrite(pins[j], LOW); }
    Serial.printf("[PIN] %s = GPIO%d -> HIGH 4 giay (do ~3.3V)\n", names[i], pins[i]);
    digitalWrite(pins[i], HIGH);
    delay(4000);
    Serial.printf("[PIN] %s = GPIO%d -> LOW 4 giay (do ~0V)\n", names[i], pins[i]);
    digitalWrite(pins[i], LOW);
    delay(4000);
  }
  Serial.println("[PIN] Xong. Go 't' de thu lai man hinh.");
  started = false;  // bus da tha: init lai tu dau
}

// CS noi GND: xung nhieu tren SCK luc khoi dong lam man dem lech nhip -> trang.
// Chen them 1 xung SCK (MOSI/DC thap = bit 0, vo hai) de dich canh byte 1 bit.
static void shiftOneClock() {
  if (started) { lcd.bus.release(); started = false; }
  pinMode(PIN_SCK, OUTPUT);
  digitalWrite(PIN_SCK, LOW); delayMicroseconds(5);
  digitalWrite(PIN_SCK, HIGH); delayMicroseconds(5);
  digitalWrite(PIN_SCK, LOW); delayMicroseconds(5);
}

// 'a': thu ca 8 vi tri lech nhip. Moi lan to 1 mau + so to giua man, giu 4 giay.
// Thay so nao hien len -> lech nhip la nguyen nhan (day noi tot).
// Ca 8 lan deu trang -> loi day / nguon, khong phai lech nhip.
static void sweepShifts() {
  const uint16_t cols[] = {TFT_RED, TFT_GREEN, TFT_BLUE, TFT_YELLOW, TFT_CYAN, TFT_MAGENTA, TFT_ORANGE, TFT_WHITE};
  for (int k = 1; k <= 8; k++) {
    shiftOneClock();
    uint32_t hz = SPEEDS[speedIdx];
    lcd.setup(hz); started = true;
    lcd.init();
    lcd.startWrite(); lcd.writeCommand(0x00); lcd.writeCommand(0x01); lcd.endWrite(); delay(150);
    lcd.init();
    lcd.setRotation(3);
    lcd.fillScreen(cols[k - 1]);
    lcd.setTextColor(TFT_BLACK, cols[k - 1]);
    lcd.setTextSize(10);
    lcd.setCursor(lcd.width() / 2 - 30, lcd.height() / 2 - 40);
    lcd.printf("%d", k);
    Serial.printf("[SHIFT] Lan %d/8 - neu man hien SO %d thi bao lai\n", k, k);
    delay(4000);
  }
  Serial.println("[SHIFT] Xong 8 lan.");
}

void setup() {
  // Giu SCK thap cang som cang tot (giam xung nhieu lam lech nhip).
  pinMode(PIN_SCK, OUTPUT); digitalWrite(PIN_SCK, LOW);
  if (PIN_CS >= 0) { pinMode(PIN_CS, OUTPUT); digitalWrite(PIN_CS, HIGH); }
  Serial.begin(115200);
  delay(400);
  Serial.println("\n=== THU MAN ILI9488 (bo da sua) ===");
  Serial.println("Lenh: t = thu mau, s = doi toc do SPI, p = do tung chan, a = do lech nhip 8 lan");
  colorTest();
}

void loop() {
  if (!Serial.available()) { delay(20); return; }
  char c = Serial.read();
  if (c == 't') colorTest();
  else if (c == 's') { speedIdx = (speedIdx + 1) % 5; colorTest(); }
  else if (c == 'p') pinProbe();
  else if (c == 'a') sweepShifts();
}

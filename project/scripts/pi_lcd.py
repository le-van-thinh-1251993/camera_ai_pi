"""Man ST7789 (SPI) gan tren Raspberry Pi: bang trang thai he thong AI Camera.

Hien: ten/IP Pi, WiFi, web + Supabase, ESP32 (online, AUTO/MANUAL), ket qua gan nhat,
dia chi dashboard. Doc du lieu tu server local (http://127.0.0.1:8000), khong goi Internet.

Chay thu:   ~/ai-camera/.venv/bin/python scripts/pi_lcd.py --test
Chay that:  dich vu systemd ai-camera-lcd.service (tu chay khi bat Pi).

Cau hinh trong .env (deu co mac dinh):
  LCD_WIDTH=240 LCD_HEIGHT=240   kich thuoc man (240x240, 240x280, 240x320, 172x320, 135x240)
  LCD_ROTATION=0                 0/1/2/3 = xoay 0/90/180/270 do
  LCD_INVERT=1                   man IPS thuong can dao mau (mau sai/am ban -> doi 0)
  LCD_SPI_MODE=0                 man 7 chan KHONG co CS -> dat 3
  LCD_SPI_HZ=40000000            nhieu/soc -> giam 20000000
  LCD_DC=25 LCD_RST=27 LCD_BL=24 so GPIO (BCM); LCD_BL=-1 neu BLK noi thang 3.3V
  LCD_X_OFFSET / LCD_Y_OFFSET    chi dat khi hinh bi lech (mac dinh tu tinh theo kich thuoc)
  LCD_BGR=0                      do/xanh duong bi dao -> doi 1
"""

import argparse
import json
import os
import socket
import subprocess
import sys
import time
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
FONT_BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"

# Mau giao dien (giong dashboard "Dem xanh")
BG = (11, 22, 40)
PANEL = (21, 36, 62)
FG = (232, 240, 248)
MUTE = (138, 156, 182)
ACC = (52, 211, 196)
OK = (74, 222, 128)
BAD = (248, 113, 113)
WARN = (251, 191, 36)


def load_font(path, size):
    """DejaVu tren Pi; may khong co font (Windows/CI) thi dung font mac dinh cua Pillow."""
    from PIL import ImageFont

    try:
        return ImageFont.truetype(path, size)
    except OSError:
        return ImageFont.load_default(size)


def env(name, default):
    value = os.getenv(name)
    return default if value in (None, "") else value


def load_env():
    """Doc .env cua server (khong ghi de bien da co)."""
    for name in (".env", ".env.cloud"):
        path = ROOT / name
        if not path.is_file():
            continue
        for line in path.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            os.environ.setdefault(key.strip(), value.strip().strip('"').strip("'"))


# ---------------------------------------------------------------- driver ST7789
# Offset cua vung hien thi trong bo nho 240x320 cua chip, theo kich thuoc + huong xoay.
OFFSETS = {
    (240, 240): {0: (0, 0), 1: (0, 0), 2: (0, 80), 3: (80, 0)},
    (240, 280): {0: (0, 20), 1: (20, 0), 2: (0, 20), 3: (20, 0)},
    (240, 320): {0: (0, 0), 1: (0, 0), 2: (0, 0), 3: (0, 0)},
    (172, 320): {0: (34, 0), 1: (0, 34), 2: (34, 0), 3: (0, 34)},
    (135, 240): {0: (52, 40), 1: (40, 53), 2: (53, 40), 3: (40, 52)},
}
MADCTL = {0: 0x00, 1: 0x60, 2: 0xC0, 3: 0xA0}


class ST7789:
    def __init__(self):
        import lgpio
        import spidev

        self.lgpio = lgpio
        w, h = int(env("LCD_WIDTH", 240)), int(env("LCD_HEIGHT", 240))
        self.rotation = int(env("LCD_ROTATION", 0)) % 4
        # Kich thuoc ve (sau khi xoay 90/270 thi doi chieu)
        self.width, self.height = (h, w) if self.rotation % 2 else (w, h)
        ox, oy = OFFSETS.get((w, h), {}).get(self.rotation, (0, 0))
        self.ox = int(env("LCD_X_OFFSET", ox))
        self.oy = int(env("LCD_Y_OFFSET", oy))
        self.dc = int(env("LCD_DC", 25))
        self.rst = int(env("LCD_RST", 27))
        self.bl = int(env("LCD_BL", 24))

        # Pi 5: chan header la gpiochip0 (kernel moi) hoac gpiochip4 (kernel cu).
        self.chip = None
        for index in (0, 4):
            try:
                handle = lgpio.gpiochip_open(index)
                lgpio.gpio_claim_output(handle, self.dc, 0)
                self.chip = handle
                break
            except lgpio.error:
                try:
                    lgpio.gpiochip_close(handle)
                except Exception:
                    pass
        if self.chip is None:
            raise RuntimeError(f"Khong mo duoc GPIO{self.dc} (gpiochip0/4)")
        if self.rst >= 0:
            lgpio.gpio_claim_output(self.chip, self.rst, 1)
        if self.bl >= 0:
            lgpio.gpio_claim_output(self.chip, self.bl, 0)

        self.spi = spidev.SpiDev()
        self.spi.open(int(env("LCD_SPI_BUS", 0)), int(env("LCD_SPI_CS", 0)))
        self.spi.mode = int(env("LCD_SPI_MODE", 0))
        self.spi.max_speed_hz = int(env("LCD_SPI_HZ", 40_000_000))
        self.init()

    def _write(self, data, command):
        self.lgpio.gpio_write(self.chip, self.dc, 0 if command else 1)
        self.spi.writebytes2(bytes(data))  # tu chia khoi theo bufsiz cua spidev

    def cmd(self, c, *data):
        self._write([c], True)
        if data:
            self._write(data, False)

    def init(self):
        if self.rst >= 0:  # reset cung
            for level, wait in ((1, 0.05), (0, 0.05), (1, 0.15)):
                self.lgpio.gpio_write(self.chip, self.rst, level)
                time.sleep(wait)
        self.cmd(0x01)  # SWRESET
        time.sleep(0.15)
        self.cmd(0x11)  # SLPOUT
        time.sleep(0.12)
        self.cmd(0x3A, 0x55)  # 16 bit/diem anh (RGB565)
        mad = MADCTL[self.rotation] | (0x08 if env("LCD_BGR", "0") == "1" else 0)
        self.cmd(0x36, mad)
        self.cmd(0x21 if env("LCD_INVERT", "1") == "1" else 0x20)
        self.cmd(0x13)  # NORON
        self.cmd(0x29)  # DISPON
        time.sleep(0.02)
        self.backlight(True)

    def backlight(self, on):
        if self.bl >= 0:
            self.lgpio.gpio_write(self.chip, self.bl, 1 if on else 0)

    def show(self, image):
        import numpy as np

        image = image.convert("RGB")
        if image.size != (self.width, self.height):
            image = image.resize((self.width, self.height))
        a = np.asarray(image, dtype=np.uint16)
        rgb565 = ((a[..., 0] & 0xF8) << 8) | ((a[..., 1] & 0xFC) << 3) | (a[..., 2] >> 3)
        x0, y0 = self.ox, self.oy
        x1, y1 = x0 + self.width - 1, y0 + self.height - 1
        self.cmd(0x2A, x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF)
        self.cmd(0x2B, y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF)
        self.cmd(0x2C)
        self._write(rgb565.astype(">u2").tobytes(), False)

    def close(self):
        try:
            self.backlight(False)
            self.spi.close()
            self.lgpio.gpiochip_close(self.chip)
        except Exception:
            pass


# ---------------------------------------------------------------- du lieu he thong
def http_json(path, key=None, timeout=2):
    req = urllib.request.Request("http://127.0.0.1:" + env("PORT", "8000") + path)
    if key:
        req.add_header("X-API-Key", key)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def local_ip():
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("8.8.8.8", 80))  # khong gui goi tin, chi chon card mang
            return s.getsockname()[0]
    except OSError:
        return "chưa có mạng"


def wifi_ssid():
    try:
        out = subprocess.run(
            ["nmcli", "-t", "-f", "ACTIVE,SSID", "dev", "wifi"],
            capture_output=True,
            text=True,
            timeout=3,
        ).stdout
        for line in out.splitlines():
            if line.startswith("yes:"):
                return line[4:] or "?"
    except (OSError, subprocess.SubprocessError):
        pass
    return "không có WiFi"


def gesture_text(result):
    if not result:
        return "Chưa có", MUTE
    count = result.get("finger_count")
    if count:
        return f"{count} ngón", ACC
    g = result.get("gesture") or ""
    return ("Không thấy tay" if g == "No_hand" else "Chưa rõ"), WARN


def ago(iso):
    try:
        t = datetime.fromisoformat(iso.replace("Z", "+00:00"))
        s = int((datetime.now(timezone.utc) - t).total_seconds())
    except (ValueError, AttributeError):
        return ""
    if s < 60:
        return f"{max(s, 0)} giây trước"
    if s < 3600:
        return f"{s // 60} phút trước"
    if s < 86400:
        return f"{s // 3600} giờ trước"
    return t.astimezone().strftime("%d/%m %H:%M")


def collect():
    info = {"ip": local_ip(), "wifi": wifi_ssid(), "host": socket.gethostname()}
    try:
        h = http_json("/health")
        info["web"] = bool(h.get("ready"))
        info["cloud"] = bool(h.get("cloud_ready"))
    except Exception:
        info["web"] = info["cloud"] = False
    try:
        device = env("CLOUD_DEVICE_ID", "esp32-01")
        info["state"] = http_json(f"/api/cloud/device/{device}/state", env("API_KEY", ""))
    except Exception:
        info["state"] = None
    return info


# ---------------------------------------------------------------- giao dien
def render(info, size):
    from PIL import Image, ImageDraw

    w, h = size
    k = min(w, h) / 240  # ti le chu theo kich thuoc man
    f = lambda px, bold=False: load_font(FONT_BOLD if bold else FONT, max(9, int(px * k)))
    img = Image.new("RGB", size, BG)
    d = ImageDraw.Draw(img)
    pad = int(8 * k)

    # Tieu de
    d.text((pad, pad), "AI CAMERA", font=f(18, True), fill=ACC)
    d.text(
        (w - pad, pad + 2 * k),
        datetime.now().strftime("%H:%M"),
        font=f(16, True),
        fill=FG,
        anchor="ra",
    )
    y = pad + int(28 * k)

    def row(label, value, color=FG):
        nonlocal y
        d.text((pad, y), label, font=f(13), fill=MUTE)
        d.text((w - pad, y), value, font=f(13, True), fill=color, anchor="ra")
        y += int(21 * k)

    def dot(ok):
        return ("● Chạy" if ok else "● Lỗi"), (OK if ok else BAD)

    row("Pi", f"{info['host']}  {info['ip']}")
    row("WiFi", info["wifi"], FG if info["wifi"] != "không có WiFi" else BAD)
    row("Web / máy chủ", *dot(info["web"]))
    cloud = ("● Đã nối" if info["cloud"] else "● Mất kết nối"), (OK if info["cloud"] else WARN)
    row("Supabase", *cloud)
    st = info.get("state") or {}
    if st.get("online"):
        row("ESP32", "● " + str(st.get("mode", "?")).upper(), OK)
    else:
        row("ESP32", "● Chưa kết nối", BAD)

    # The ket qua gan nhat
    y += int(4 * k)
    box_h = h - y - int(26 * k)
    if box_h > 40 * k:
        d.rounded_rectangle((pad, y, w - pad, y + box_h), radius=int(8 * k), fill=PANEL)
        d.text((pad * 2, y + int(6 * k)), "KẾT QUẢ GẦN NHẤT", font=f(11, True), fill=MUTE)
        text, color = gesture_text(st.get("result"))
        d.text((pad * 2, y + int(22 * k)), text, font=f(28, True), fill=color)
        when = ago((st.get("result") or {}).get("time", ""))
        if when:
            d.text((w - pad * 2, y + box_h - int(8 * k)), when, font=f(11), fill=MUTE, anchor="rd")

    # Dia chi dashboard
    url = f"{info['host']}.local:{env('PORT', '8000')}"
    d.text((w // 2, h - int(6 * k)), url, font=f(12, True), fill=ACC, anchor="md")
    return img


def test_pattern(size):
    from PIL import Image, ImageDraw

    w, h = size
    img = Image.new("RGB", size, (0, 0, 0))
    d = ImageDraw.Draw(img)
    bars = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255)]
    for i, color in enumerate(bars):
        d.rectangle((i * w // 4, 0, (i + 1) * w // 4, h // 2), fill=color)
    font = load_font(FONT_BOLD, max(12, w // 12))
    d.text((w // 2, h * 3 // 4), "ĐỎ  LÁ  LAM  TRẮNG", font=font, fill=(255, 255, 255), anchor="mm")
    d.rectangle((0, 0, w - 1, h - 1), outline=(255, 255, 0))
    return img


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--test", action="store_true", help="ve 4 mau + khung vang roi thoat")
    ap.add_argument("--once", action="store_true", help="ve bang trang thai 1 lan roi thoat")
    ap.add_argument("--png", help="khong can man: luu bang trang thai ra file PNG")
    args = ap.parse_args()
    load_env()

    if args.png:
        w, h = int(env("LCD_WIDTH", 240)), int(env("LCD_HEIGHT", 240))
        size = (h, w) if int(env("LCD_ROTATION", 0)) % 2 else (w, h)
        render(collect(), size).save(args.png)
        print("Da luu", args.png, size)
        return

    lcd = None
    while lcd is None:
        try:
            lcd = ST7789()
        except Exception as exc:  # SPI chua bat / chua han man: cho, khong spam log
            print("[LCD] Chua san sang:", exc, "- thu lai sau 30 s", flush=True)
            if args.test or args.once:
                sys.exit(1)
            time.sleep(30)
    print(
        f"[LCD] ST7789 {lcd.width}x{lcd.height} xoay {lcd.rotation} offset ({lcd.ox},{lcd.oy})",
        flush=True,
    )
    try:
        if args.test:
            lcd.show(test_pattern((lcd.width, lcd.height)))
            print("[LCD] Dang hien 4 mau DO-LA-LAM-TRANG + khung vang. Kiem tra xong bam Ctrl+C.")
            while True:
                time.sleep(1)
        while True:
            try:
                lcd.show(render(collect(), (lcd.width, lcd.height)))
            except Exception as exc:
                print("[LCD] Loi ve:", exc, flush=True)
            if args.once:
                break
            time.sleep(float(env("LCD_REFRESH", 2)))
    except KeyboardInterrupt:
        pass
    finally:
        if not args.once:
            lcd.close()


if __name__ == "__main__":
    main()

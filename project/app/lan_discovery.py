"""Trả lời gói dò tìm máy chủ từ ESP32 trong mạng LAN.

ESP32 phát broadcast UDP "AICAM?" tới cổng LAN_DISCOVERY_PORT (mặc định 8002);
máy chủ trả "AICAM <cổng HTTP>" về đúng địa chỉ gửi. Nhờ vậy ESP32 tìm được máy
tính Windows (không có mDNS ai-camera.local như Pi) mà không cần nhập IP.
Tắt bằng LAN_DISCOVERY=0.
"""

import asyncio
import logging
import os

log = logging.getLogger(__name__)
MAGIC = b"AICAM?"


class _Responder(asyncio.DatagramProtocol):
    def __init__(self, reply: bytes, on_seen=None):
        self.reply = reply
        self.on_seen = on_seen
        self.transport = None

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, addr):
        if data.strip() == MAGIC and self.transport:
            self.transport.sendto(self.reply, addr)
            if self.on_seen:
                self.on_seen(addr[0])


async def start_discovery(http_port: int, on_seen=None):
    """Mở cổng UDP trả lời dò tìm; trả về transport (để đóng) hoặc None."""
    if os.getenv("LAN_DISCOVERY", "1") == "0":
        return None
    port = int(os.getenv("LAN_DISCOVERY_PORT", "8002"))
    loop = asyncio.get_running_loop()
    try:
        transport, _ = await loop.create_datagram_endpoint(
            lambda: _Responder(f"AICAM {http_port}".encode(), on_seen),
            local_addr=("0.0.0.0", port),
            allow_broadcast=True,
        )
    except OSError as exc:
        log.warning("Không mở được cổng dò tìm UDP %s: %s", port, exc)
        return None
    log.info("Dò tìm LAN: trả lời ESP32 trên UDP %s", port)
    return transport

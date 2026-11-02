"""Show the existing dashboard key only when explicitly requested. Never rotates keys."""

import argparse
from pathlib import Path
from dotenv import dotenv_values

parser = argparse.ArgumentParser(description="Xem API_KEY dashboard hiện có trên Pi")
parser.add_argument(
    "--show", action="store_true", help="In khóa hiện tại ra terminal riêng của bạn"
)
args = parser.parse_args()
if args.show:
    value = dotenv_values(Path(__file__).resolve().parents[1] / ".env").get("API_KEY")
    print(value or "Chưa cấu hình API_KEY")
else:
    parser.print_help()

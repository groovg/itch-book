import datetime as dt
import os
import re
from zoneinfo import ZoneInfo

_PATTERNS = (
    (re.compile(r"^(\d{2})(\d{2})(\d{4})\.NASDAQ_ITCH50"), lambda m: (int(m[3]), int(m[1]), int(m[2]))),
    (re.compile(r"^S(\d{2})(\d{2})(\d{2})-v50"), lambda m: (2000 + int(m[3]), int(m[1]), int(m[2]))),
    (re.compile(r"^(\d{4})(\d{2})(\d{2})\.(?:BX|PSX)_ITCH_50"), lambda m: (int(m[1]), int(m[2]), int(m[3]))),
)


def session_date(path: str) -> dt.date | None:
    name = os.path.basename(path)
    for pattern, pick in _PATTERNS:
        m = pattern.match(name)
        if m:
            y, mo, d = pick(m)
            return dt.date(y, mo, d)
    return None


def midnight_ns(day: dt.date) -> int:
    midnight = dt.datetime(day.year, day.month, day.day, tzinfo=ZoneInfo("America/New_York"))
    return int(midnight.timestamp()) * 1_000_000_000

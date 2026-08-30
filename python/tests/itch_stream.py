import struct


def _msg(kind: bytes, locate: int, ts: int, payload: bytes) -> bytes:
    body = kind + struct.pack(">HH", locate, 0) + ts.to_bytes(6, "big") + payload
    return struct.pack(">H", len(body)) + body


def system_event(ts: int, event: bytes) -> bytes:
    return _msg(b"S", 0, ts, event)


def stock_directory(locate: int, ts: int, stock: str, round_lot: int = 100) -> bytes:
    payload = struct.pack(
        ">8scc I c c 2s c c c c c I c",
        stock.ljust(8).encode(), b"Q", b" ", round_lot, b"N", b"C", b"  ", b"P", b"N", b"N",
        b"1", b"N", 0, b"N",
    )
    return _msg(b"R", locate, ts, payload)


def add_order(locate: int, ts: int, ref: int, side: str, shares: int, stock: str, price: int) -> bytes:
    payload = struct.pack(">Q c I 8s I", ref, side.encode(), shares, stock.ljust(8).encode(), price)
    return _msg(b"A", locate, ts, payload)


def add_order_mpid(locate: int, ts: int, ref: int, side: str, shares: int, stock: str, price: int,
                   mpid: str) -> bytes:
    payload = struct.pack(">Q c I 8s I 4s", ref, side.encode(), shares, stock.ljust(8).encode(), price,
                          mpid.ljust(4).encode())
    return _msg(b"F", locate, ts, payload)


def order_executed(locate: int, ts: int, ref: int, shares: int, match: int) -> bytes:
    return _msg(b"E", locate, ts, struct.pack(">Q I Q", ref, shares, match))


def order_executed_price(locate: int, ts: int, ref: int, shares: int, match: int, printable: bool,
                         price: int) -> bytes:
    payload = struct.pack(">Q I Q c I", ref, shares, match, b"Y" if printable else b"N", price)
    return _msg(b"C", locate, ts, payload)


def order_cancel(locate: int, ts: int, ref: int, shares: int) -> bytes:
    return _msg(b"X", locate, ts, struct.pack(">Q I", ref, shares))


def order_delete(locate: int, ts: int, ref: int) -> bytes:
    return _msg(b"D", locate, ts, struct.pack(">Q", ref))


def order_replace(locate: int, ts: int, old_ref: int, new_ref: int, shares: int, price: int) -> bytes:
    return _msg(b"U", locate, ts, struct.pack(">Q Q I I", old_ref, new_ref, shares, price))


def trade(locate: int, ts: int, ref: int, side: str, shares: int, stock: str, price: int, match: int) -> bytes:
    payload = struct.pack(">Q c I 8s I Q", ref, side.encode(), shares, stock.ljust(8).encode(), price, match)
    return _msg(b"P", locate, ts, payload)


def cross_trade(locate: int, ts: int, shares: int, stock: str, price: int, match: int, cross_type: str) -> bytes:
    payload = struct.pack(">Q 8s I Q c", shares, stock.ljust(8).encode(), price, match, cross_type.encode())
    return _msg(b"Q", locate, ts, payload)


def broken_trade(locate: int, ts: int, match: int) -> bytes:
    return _msg(b"B", locate, ts, struct.pack(">Q", match))


def unknown_message(ts: int) -> bytes:
    return _msg(b"Z", 0, ts, b"\x00" * 5)


def end_of_session() -> bytes:
    return b"\x00\x00"

"""Interface control documents: load, check, and a runtime codec.

The YAML files in icd/ are the single source of every interface. This module reads them, checks they are
well formed, and encodes and decodes messages by interpreting them. The C++ codecs are generated from the
same files by iqk.icdgen; the two implementations are compared byte for byte in tests/unit/test_codec_parity.py.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from functools import cache
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parent.parent
ICD_DIR = ROOT / "icd"

SIZES = {"u8": 1, "i8": 1, "u16": 2, "i16": 2, "u32": 4, "i32": 4, "u48": 6, "u64": 8, "i64": 8}
SIGNED = {"i8", "i16", "i32", "i64"}


@dataclass(frozen=True)
class Field:
    name: str
    type: str
    scale: float | None = None
    unit: str = ""
    range: tuple[float, float] | None = None
    bits: dict[str, int] = field(default_factory=dict)
    description: str = ""
    count: str | None = None  # arrays: the count type
    max: int = 0
    of: tuple[Field, ...] = ()

    @property
    def size(self) -> int:
        return SIZES[self.type]

    def count_max(self) -> int:
        """Largest element count an array's count field can carry."""
        return (1 << (SIZES[self.count] * 8)) - 1 if self.count else 0

    def raw_bounds(self) -> tuple[int, int]:
        n = SIZES[self.type] * 8
        return (-(1 << (n - 1)), (1 << (n - 1)) - 1) if self.type in SIGNED else (0, (1 << n) - 1)


@dataclass(frozen=True)
class Message:
    name: str
    id: int
    direction: str
    fields: tuple[Field, ...]
    header: tuple[Field, ...] = ()
    rate_hz: float | None = None
    per_node: bool = False
    description: str = ""

    @property
    def array(self) -> Field | None:
        return next((f for f in self.fields if f.type == "array"), None)

    @property
    def fixed_size(self) -> int:
        """Bytes before any array (header included), plus the array's count byte."""
        n = sum(f.size for f in self.header)
        for f in self.fields:
            n += SIZES[f.count] if f.type == "array" else f.size  # type: ignore[index]
        return n

    @property
    def record_size(self) -> int:
        a = self.array
        return sum(f.size for f in a.of) if a else 0

    @property
    def max_size(self) -> int:
        a = self.array
        return self.fixed_size + (a.max * self.record_size if a else 0)


@dataclass(frozen=True)
class Icd:
    key: str  # file stem
    icd: str
    title: str
    version: str
    byte_order: str
    transport: dict
    messages: tuple[Message, ...]
    extra: dict

    def message(self, name: str) -> Message:
        for m in self.messages:
            if m.name == name:
                return m
        raise KeyError(name)


def _field(d: dict) -> Field:
    rng = d.get("range")
    return Field(
        name=d["name"],
        type=d["type"],
        scale=float(d["scale"]) if "scale" in d else None,
        unit=d.get("unit", ""),
        range=(float(rng[0]), float(rng[1])) if rng else None,
        bits=dict(d.get("bits", {})),
        description=d.get("description", ""),
        count=d.get("count"),
        max=int(d.get("max", 0)),
        of=tuple(_field(x) for x in d.get("of", [])),
    )


@cache
def load(key: str) -> Icd:
    raw = yaml.safe_load((ICD_DIR / f"{key}.yaml").read_text())
    header = tuple(_field(x) for x in raw.get("header", []))
    msgs = tuple(
        Message(
            name=m["name"],
            id=int(m["id"]),
            direction=m["direction"],
            fields=tuple(_field(x) for x in m["fields"]),
            header=header,
            rate_hz=m.get("rate_hz"),
            per_node=bool(m.get("per_node", False)),
            description=m.get("description", ""),
        )
        for m in raw["messages"]
    )
    known = {"icd", "title", "version", "byte_order", "transport", "messages", "header"}
    icd = Icd(
        key=key,
        icd=raw["icd"],
        title=raw["title"],
        version=raw["version"],
        byte_order=raw["byte_order"],
        transport=raw["transport"],
        messages=msgs,
        extra={k: v for k, v in raw.items() if k not in known},
    )
    check(icd)
    return icd


def keys() -> list[str]:
    return sorted(p.stem for p in ICD_DIR.glob("*.yaml"))


def check(icd: Icd) -> None:
    """Structural rules every ICD must satisfy. A violation is a document error, not a runtime one."""
    names = [m.name for m in icd.messages]
    if len(set(names)) != len(names):
        raise ValueError(f"{icd.icd}: duplicate message names")
    if icd.byte_order not in ("little", "big"):
        raise ValueError(f"{icd.icd}: byte_order must be little or big")
    for m in icd.messages:
        seen = set()
        for f in (*m.header, *m.fields):
            if f.name in seen:
                raise ValueError(f"{icd.icd}.{m.name}: duplicate field {f.name}")
            seen.add(f.name)
            if f.type == "array":
                if f.count not in SIZES or not f.of or f.max <= 0:
                    raise ValueError(f"{icd.icd}.{m.name}.{f.name}: array needs count, of and max")
                if f.max > f.count_max():
                    raise ValueError(f"{icd.icd}.{m.name}.{f.name}: max exceeds count type")
                continue
            if f.type not in SIZES:
                raise ValueError(f"{icd.icd}.{m.name}.{f.name}: unknown type {f.type}")
            for b, pos in f.bits.items():
                if not 0 <= pos < f.size * 8:
                    raise ValueError(f"{icd.icd}.{m.name}.{f.name}: bit {b} out of field")
            if f.range is not None:
                lo, hi = f.range
                if lo > hi:
                    raise ValueError(f"{icd.icd}.{m.name}.{f.name}: empty range")
                rlo, rhi = f.raw_bounds()
                s = f.scale or 1.0
                if round(lo / s) < rlo or round(hi / s) > rhi:
                    raise ValueError(f"{icd.icd}.{m.name}.{f.name}: range does not fit the raw type")
        if sum(1 for f in m.fields if f.type == "array") > 1:
            raise ValueError(f"{icd.icd}.{m.name}: at most one array")
        if m.array is not None and m.fields[-1].type != "array":
            raise ValueError(f"{icd.icd}.{m.name}: the array must be the last field")
        if icd.transport.get("kind") == "can" and m.max_size > 8:
            raise ValueError(f"{icd.icd}.{m.name}: {m.max_size} bytes does not fit a classic CAN frame")


# --- runtime codec -------------------------------------------------------------------------------------------

_FMT = {"u8": "B", "i8": "b", "u16": "H", "i16": "h", "u32": "I", "i32": "i", "u64": "Q", "i64": "q"}


def _put(f: Field, v: int, order: str) -> bytes:
    lo, hi = f.raw_bounds()
    if not lo <= v <= hi:
        raise ValueError(f"{f.name}={v} does not fit {f.type}")
    if f.type == "u48":
        return v.to_bytes(6, order)
    return struct.pack(("<" if order == "little" else ">") + _FMT[f.type], v)


def _get(f: Field, b: bytes, off: int, order: str) -> int:
    if f.type == "u48":
        return int.from_bytes(b[off : off + 6], order)
    return struct.unpack_from(("<" if order == "little" else ">") + _FMT[f.type], b, off)[0]


def encode(icd: Icd, msg: Message, values: dict) -> bytes:
    """Encode a message payload (no framing). Missing scalar fields are an error."""
    out = bytearray()
    for f in (*msg.header, *msg.fields):
        if f.type == "array":
            items = values.get(f.name, [])
            if len(items) > f.max:
                raise ValueError(f"{f.name}: {len(items)} > max {f.max}")
            out += _put(Field("count", f.count or "u8"), len(items), icd.byte_order)
            for it in items:
                for g in f.of:
                    out += _put(g, int(it[g.name]), icd.byte_order)
        else:
            out += _put(f, int(values[f.name]), icd.byte_order)
    return bytes(out)


def decode(icd: Icd, msg: Message, data: bytes) -> dict | None:
    """Decode a payload. Returns None unless the length is exactly what the ICD defines."""
    if len(data) < msg.fixed_size:
        return None
    out: dict = {}
    off = 0
    for f in (*msg.header, *msg.fields):
        if f.type == "array":
            cnt_field = Field("count", f.count or "u8")
            n = _get(cnt_field, data, off, icd.byte_order)
            off += cnt_field.size
            if n > f.max or len(data) != off + n * msg.record_size:
                return None
            items = []
            for _ in range(n):
                it = {}
                for g in f.of:
                    it[g.name] = _get(g, data, off, icd.byte_order)
                    off += g.size
                items.append(it)
            out[f.name] = items
        else:
            out[f.name] = _get(f, data, off, icd.byte_order)
            off += f.size
    if off != len(data):
        return None
    return out


def in_range(msg: Message, values: dict) -> list[str]:
    """Fields whose engineering value is outside the ICD range (empty when all are in range)."""
    bad = []
    for f in (*msg.header, *msg.fields):
        if f.range is None or f.type == "array":
            continue
        eng = values[f.name] * (f.scale or 1.0)
        lo, hi = f.range
        if not lo - 1e-12 <= eng <= hi + 1e-12:
            bad.append(f.name)
    return bad

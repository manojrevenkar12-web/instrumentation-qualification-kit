"""The ICDs are well formed, the committed generated code matches them, and the C++ codecs agree with an
independent Python reading of the same documents."""

import subprocess

import pytest
from hypothesis import HealthCheck, given, settings
from hypothesis import strategies as st

from iqk import icd as I
from iqk import icdgen


def test_every_icd_loads_and_is_consistent():
    for k in I.keys():
        d = I.load(k)
        assert d.messages, k


def test_a_can_message_must_fit_eight_bytes(tmp_path, monkeypatch):
    bad = (I.ICD_DIR / "aam.yaml").read_text().replace("- {name: ch_c, type: i16}", "- {name: ch_c, type: i32}")
    (tmp_path / "aam.yaml").write_text(bad)
    monkeypatch.setattr(I, "ICD_DIR", tmp_path)
    I.load.cache_clear()
    try:
        with pytest.raises(ValueError, match="does not fit a classic CAN frame"):
            I.load("aam")
    finally:
        I.load.cache_clear()


@pytest.mark.verifies("SRS-001")
def test_generated_files_are_current():
    assert icdgen.drift() == [], "run `iqk icd generate` and commit the result"


def _values(msg: I.Message, draw) -> dict:
    out = {}
    for f in (*msg.header, *msg.fields):
        if f.type == "array":
            n = draw(st.integers(0, min(f.max, 6)))
            out[f.name] = [{g.name: draw(st.integers(*g.raw_bounds())) for g in f.of} for _ in range(n)]
        else:
            out[f.name] = draw(st.integers(*f.raw_bounds()))
    return out


def _kv(values: dict) -> str:
    lines = []
    for k, v in values.items():
        if isinstance(v, list):
            for i, it in enumerate(v):
                lines += [f"{k}.{i}.{g}={x}" for g, x in it.items()]
        else:
            lines.append(f"{k}={v}")
    return "\n".join(lines) + "\n"


MESSAGES = [(k, m.name) for k in I.keys() for m in I.load(k).messages]


@pytest.mark.verifies("SRS-001", "SRS-002")
@pytest.mark.parametrize(("key", "name"), MESSAGES)
@settings(max_examples=12, deadline=None, suppress_health_check=[HealthCheck.function_scoped_fixture])
@given(data=st.data())
def test_cpp_and_python_codecs_agree(build, key, name, data):
    d = I.load(key)
    msg = d.message(name)
    values = _values(msg, data.draw)
    payload = I.encode(d, msg, values)
    # C++ decodes what Python encoded ...
    out = subprocess.run(
        [str(build / "iqkctl"), "codec", "decode", key, name, payload.hex()], capture_output=True, text=True
    )
    assert out.returncode == 0, out.stdout
    assert out.stdout == _kv(values)
    # ... and encodes the same bytes from the same values.
    enc = subprocess.run(
        [str(build / "iqkctl"), "codec", "encode", key, name], input=_kv(values), capture_output=True, text=True
    )
    assert enc.stdout.strip() == payload.hex()
    # A byte more or less is rejected by both.
    assert I.decode(d, msg, payload + b"\x00") is None
    bad = subprocess.run(
        [str(build / "iqkctl"), "codec", "decode", key, name, (payload + b"\x00").hex()], capture_output=True, text=True
    )
    assert bad.returncode != 0

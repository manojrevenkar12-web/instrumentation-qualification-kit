import os
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
BUILD = Path(os.environ.get("IQK_BUILD", ROOT / "build"))


def pytest_configure(config):
    config.addinivalue_line("markers", "verifies(*ids): requirements this test verifies (recorded in the JUnit report)")
    config.addinivalue_line("markers", "system: runs the real daemon against the emulators (needs a build)")


@pytest.fixture(autouse=True)
def _record_requirements(request, record_property):
    m = request.node.get_closest_marker("verifies")
    if m:
        record_property("verifies", ",".join(m.args))


@pytest.fixture(scope="session")
def build():
    if not (BUILD / "daud").exists() or not (BUILD / "iqkctl").exists():
        pytest.skip(f"no build in {BUILD}; run cmake and ninja first")
    return BUILD

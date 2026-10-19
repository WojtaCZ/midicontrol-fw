"""Sdilena hardwarova session: USB-CDC link k base (s automatickym keepalive) a ST-Link pomocnici."""

import pytest


def pytest_collection_modifyitems(items):
    for item in items:
        if not item.fspath.basename.endswith("_offline.py"):
            item.add_marker(pytest.mark.hw)


@pytest.fixture(scope="session")
def link():
    import usblink
    L = usblink.Link(keepalive=True)
    yield L
    assert L.stray == b"", f"bytes outside frames on CDC: {L.stray!r}"
    L.close()


@pytest.fixture(scope="session")
def base():
    import dbg
    t = dbg.Target("base")
    yield t
    t.stop()


@pytest.fixture(scope="session")
def remote():
    import dbg
    t = dbg.Target("remote")
    yield t
    t.stop()

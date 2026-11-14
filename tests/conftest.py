import sys

import pytest

# Under Pyodide (WebAssembly), Python can start neither threads nor
# processes: the tests that need them are skipped.
EMSCRIPTEN = sys.platform == "emscripten"


def pytest_configure(config):
    config.addinivalue_line("markers", "requires_threads: starts Python threads")
    config.addinivalue_line("markers", "requires_processes: starts processes (fork, subprocess)")


def pytest_collection_modifyitems(config, items):
    if not EMSCRIPTEN:
        return
    for item in items:
        for name in ("requires_threads", "requires_processes"):
            if item.get_closest_marker(name):
                item.add_marker(pytest.mark.skip(reason=f"{name}: not available under Pyodide"))

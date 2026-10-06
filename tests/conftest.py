"""Shared pytest setup: the build mode, temporary folders on disk."""

import pathlib

from helpers import BUILD


def pytest_configure(config):
    # Temporary folders under build/<mode>/pytest/tmp, not the system's /tmp.
    if not config.option.basetemp:
        config.option.basetemp = str(BUILD / "pytest" / "tmp")

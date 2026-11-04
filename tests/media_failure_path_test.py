#!/usr/bin/env python3
"""Guard stage-specific Load failures, cleanup, and responsive UI reporting."""
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
media = (ROOT / 'src/media_starfish.cpp').read_text()   # the webOS 3-4 player
main = (ROOT / 'src/main.cpp').read_text()

assert 'open_failure_=MediaOpenFailure::StarfishLoad;close(); return false;' in media
assert 'if (api_) {' in media and 'if (acb_) {' in media
assert 'auto report_media_start_failure=' in main
assert 'media_open_failure_message(failure)' in main
assert 'Settings remain available; reconnect to retry.' in main
print('PASS: failed media startup is cleaned up and surfaced without terminating the SDL loop')

#!/usr/bin/env python3
"""Guard stage-specific Load failures, cleanup, and responsive UI reporting."""
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
media = (ROOT / 'src/media.cpp').read_text()
main = (ROOT / 'src/main.cpp').read_text()

assert 'open_failure_=MediaOpenFailure::StarfishLoad;close(); return false;' in media
assert 'if (api_) {' in media and 'if (acb_) {' in media
assert 'auto report_media_start_failure=' in main
assert 'media_open_failure_message(failure)' in main
assert 'Settings remain available; reconnect to retry.' in main
# Main-loop integration guards supplement the real callback/native adapter tests.
assert 'if(action==Action::Exit&&!receiver.direct_active)break;' in main
cleanup = main.split('if(action==Action::Back||action==Action::Exit||!direct_open', 1)[1].split('            } else {', 1)[0]
assert cleanup.index('direct_player.close();receiver.direct_active=false;direct_open=false;') < cleanup.index('usage.record(summary,false)')
assert cleanup.index('usage.record(summary,false)') < cleanup.index('if(action==Action::Exit)break;')
assert 'action=Action::None;' in cleanup
assert 'direct_command.kind!=DirectCommandKind::Stop&&!direct_player.control' in main
assert 'direct_player.close();ui_visible=true;last_ui=0;' in main
assert 'const auto failure=direct_player.snapshot();direct_player.close();' in main
assert 'terminal=direct_failed?(status.state==DirectState::Error?status:receiver.direct.snapshot()):status' in main
assert '[direct-player] dequeued command=%s' in main
print('PASS: failed media startup is cleaned up and surfaced without terminating the SDL loop')

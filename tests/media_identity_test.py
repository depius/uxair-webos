#!/usr/bin/env python3
"""Guard consistency of launcher, ACB and Starfish application identities."""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
app = json.loads((ROOT / 'packaging/appinfo.json').read_text())
identity = (ROOT / 'src/identity.h').read_text()
main = (ROOT / 'src/main.cpp').read_text()
media = (ROOT / 'src/media_starfish.cpp').read_text()   # the webOS 3-4 player
package = (ROOT / 'tools/package.py').read_text()

assert f'#define UXAIR_APP_ID "{app["id"]}"' in identity
assert f'#define UXAIR_APP_ID "{app["id"]}.dev"' in identity
assert "const char *app_id=runtime_app_id&&*runtime_app_id?runtime_app_id:UXAIR_APP_ID;" in main
assert "media.open(app_id," in main
assert 'AcbAPI_initialize(acb_, PLAYER_TYPE_MSE, app_id, acb_event)' in media
assert 'starfish_load_payload(app_id,' in media
assert "app['id'] += '.dev'" in package
assert "'com.github.depius.uxair'" == repr(app['id'])
print('PASS: packaged, compiled fallback, ACB and Starfish identities share one app ID')

"""Real IPK writer -> permission normalization, with payload/identity preservation."""
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest

ROOT=Path(__file__).resolve().parent.parent
spec=importlib.util.spec_from_file_location('ipk_permissions',ROOT/'tools/ipk_permissions.py')
p=importlib.util.module_from_spec(spec);spec.loader.exec_module(p)


def inventory(data):
    result={}
    for archive,_,blob in p.ar_members(data):
        if archive=='debian-binary':continue
        with tarfile.open(fileobj=io.BytesIO(blob),mode='r:gz') as t:
            for m in t:
                result[(archive,m.name.removeprefix('./'))]=(m.mode,t.extractfile(m).read() if m.isfile() else None)
    return result


class IpkPermissionsTest(unittest.TestCase):
    def test_actual_ipk_preserves_payload_and_managed_root(self):
        for app_id in ['com.github.depius.uxair','com.github.depius.uxair.dev']:
            with tempfile.TemporaryDirectory() as tmp:
                tmp=Path(tmp);stage=tmp/'app';(stage/'lib').mkdir(parents=True);(stage/'licenses').mkdir()
                (stage/'appinfo.json').write_text(json.dumps(dict(id=app_id,version='1.1.0',title='UxAir')))
                (stage/'uxair').write_bytes(b'ELF receiver fixture')
                (stage/'lib/libstdc++.so.6').write_bytes(b'ELF runtime fixture')
                (stage/'licenses/LICENSE').write_text('license fixture')
                # Construct an actual package, then emulate ares' permissive modes.
                output=tmp/'test.ipk'
                subprocess.run([sys.executable,str(ROOT/'tools/make-ipk.py'),str(stage),str(output)],check=True,capture_output=True)
                original=output.read_bytes(); permissive=bytearray(b'!<arch>\n')
                for archive,header,blob in p.ar_members(original):
                    if archive!='debian-binary':
                        buf=io.BytesIO()
                        with tarfile.open(fileobj=io.BytesIO(blob),mode='r:gz') as src,tarfile.open(fileobj=buf,mode='w:gz') as dst:
                            for m in src:
                                m.mode=0o777 if m.isdir() else 0o666
                                dst.addfile(m,src.extractfile(m) if m.isfile() else None)
                        blob=buf.getvalue()
                    permissive.extend(header[:48]+f'{len(blob):<10}'.encode()+header[58:]+blob+(b'\n' if len(blob)%2 else b''))
                before=inventory(bytes(permissive));fixed=p.process(bytes(permissive),app_id);after=inventory(fixed)
                self.assertEqual({n:v[1] for n,v in before.items()},{n:v[1] for n,v in after.items()})
                app='usr/palm/applications/'+app_id
                self.assertEqual(after[('data.tar.gz',app)][0],0o777)
                self.assertEqual(after[('data.tar.gz',app+'/lib')][0],0o755)
                self.assertEqual(after[('data.tar.gz',app+'/licenses')][0],0o755)
                self.assertEqual(after[('data.tar.gz',app+'/uxair')][0],0o755)
                self.assertEqual(after[('data.tar.gz',app+'/lib/libstdc++.so.6')][0],0o755)
                self.assertEqual(after[('control.tar.gz','control')][0],0o644)
                self.assertEqual(after[('data.tar.gz','usr/palm/packages/'+app_id+'/packageinfo.json')][0],0o644)
                self.assertEqual(p.process(fixed,app_id,True),fixed)
                with self.assertRaises(ValueError):p.process(bytes(permissive),app_id,True)

    def test_unsafe_entries_rejected(self):
        for path in ['/absolute','../escape','./usr/../../escape']:
            with self.assertRaises(ValueError):p.desired_mode(tarfile.TarInfo(path),'data.tar.gz','app')
        for kind in [tarfile.SYMTYPE,tarfile.LNKTYPE,tarfile.FIFOTYPE,tarfile.CHRTYPE]:
            entry=tarfile.TarInfo('usr/palm/applications/app/lib/x');entry.type=kind
            with self.assertRaises(ValueError):p.desired_mode(entry,'data.tar.gz','app')
        with self.assertRaises(ValueError):p.ar_members(b'not ar')


if __name__=='__main__':unittest.main()

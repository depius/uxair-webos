"""Corresponding-source integrity: exact project Git blobs and original inputs."""
import hashlib,importlib.util,io,tarfile,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parent.parent
spec=importlib.util.spec_from_file_location('candidate',ROOT/'tools/prepare-candidate.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
def blob(data):return hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()
class CandidateSourceTest(unittest.TestCase):
    def archive(self,root,entries):
        p=root/'source.tar.gz'
        with tarfile.open(p,'w:gz') as t:
            for name,data in entries:
                info=tarfile.TarInfo(name);info.size=len(data);t.addfile(info,io.BytesIO(data))
        return p
    def test_exact_source_and_failures(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);good=[('uxair-webos/src/test.c',b'project'),('uxair-webos/upstream-uxplay-source.tar',b'upstream')]
            project={'src/test.c':blob(b'project')};deps={'upstream-uxplay-source.tar':hashlib.sha256(b'upstream').hexdigest()}
            self.assertTrue(m.verify_source(self.archive(root,good),project,deps)['git_blobs_match'])
            for entries in [good[:1],good+[good[0]],[(good[0][0],b'changed'),good[1]],
                            [good[0],(good[1][0],b'changed')],good+[('uxair-webos/paired.keys',b'private')],
                            good+[('uxair-webos/../escape',b'bad')],good+[('/absolute',b'bad')]]:
                with self.assertRaises(ValueError):m.verify_source(self.archive(root,entries),project,deps)
if __name__=='__main__':unittest.main()

#!/usr/bin/env python3
"""Stage explicitly authorized, unreleased TEST candidates; never publish a release."""
import argparse
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parent.parent
SOURCE_ROOTS = {'src','tools','tests','assets','packaging','docs','CMakeLists.txt',
                'dependencies.lock.json','README.md','CHANGELOG.md','THIRD_PARTY.md','LICENSE','.github'}
ASSET_NAMES = {'starfish':'UxAir-1.2.0-webOS3-4-Starfish.ipk',
               'ndl':'UxAir-1.2.0-webOS5plus-NDL-Beta.ipk'}


def digest(stream, algorithm='sha256', prefix=b''):
    result=hashlib.new(algorithm);result.update(prefix)
    for block in iter(lambda:stream.read(1024*1024),b''):result.update(block)
    return result.hexdigest()


def git_blobs():
    raw=subprocess.check_output(['git','ls-tree','-rz','HEAD'],cwd=ROOT)
    result={}
    for entry in raw.split(b'\0'):
        if not entry:continue
        fields,name=entry.split(b'\t',1);mode,kind,sha=fields.split();name=name.decode('utf-8')
        if name.split('/')[0] in SOURCE_ROOTS:
            if kind!=b'blob' or mode not in (b'100644',b'100755'):raise ValueError('Unexpected source tree entry')
            result[name]=sha.decode('ascii')
    return result


def verify_source(path, project_blobs, dependency_archives):
    seen=set()
    with tarfile.open(path,'r:gz') as archive:
        for member in archive:
            pure=PurePosixPath(member.name)
            if pure.is_absolute() or '..' in pure.parts or not pure.parts or pure.parts[0]!='uxair-webos':
                raise ValueError('Unsafe source archive path')
            if member.isdir():continue
            if not member.isfile():raise ValueError('Source link/special file rejected')
            name='/'.join(pure.parts[1:])
            if name in seen:raise ValueError('Duplicate source file')
            seen.add(name)
            stream=archive.extractfile(member)
            if name in project_blobs:
                value=digest(stream,'sha1',b'blob '+str(member.size).encode()+b'\0')
                if value!=project_blobs[name]:raise ValueError('Source differs from Git: '+name)
            elif name in dependency_archives:
                if digest(stream)!=dependency_archives[name]:raise ValueError('Dependency source archive mismatch')
            else:raise ValueError('Untracked/unexpected source file: '+name)
    if seen!=set(project_blobs)|set(dependency_archives):raise ValueError('Missing corresponding source')
    return {'project_files':len(project_blobs),'dependency_archives':len(dependency_archives),
            'git_blobs_match':True,'original_dependency_hashes_match':True}


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--backend',choices=ASSET_NAMES,required=True)
    args=parser.parse_args();backend=args.backend
    if subprocess.check_output(['git','status','--porcelain','--untracked-files=no'],cwd=ROOT).strip():
        raise SystemExit('Tracked working tree must be clean')
    commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()
    app=json.loads((ROOT/'packaging/appinfo.json').read_text())
    if app['id']!='com.github.depius.uxair' or app['version']!='1.2.0':raise ValueError('Unexpected candidate identity/version')
    if (ROOT/'build/media-backend.txt').read_text().strip()!=backend:raise ValueError('Wrong backend marker')
    if (ROOT/'build/package-variant.txt').read_text().strip()!='production':raise ValueError('Not the production app identity')
    subprocess.run(['python3',str(ROOT/'tools/validate-ipk.py')],check=True)
    platform='webos3-4' if backend=='starfish' else 'webos5plus'
    package=ROOT/'dist'/f"{app['id']}_{app['version']}_arm-{platform}.ipk"
    from ipk_permissions import ar_members
    members={name:data for name,_,data in ar_members(package.read_bytes())}
    with tarfile.open(fileobj=io.BytesIO(members['data.tar.gz']),mode='r:gz') as t:
        candidates=[m for m in t if m.isfile() and m.name.endswith('/build-info.txt')]
        if len(candidates)!=1:raise ValueError('Ambiguous embedded build metadata')
        if t.extractfile(candidates[0]).read()!=f'version=1.2.0\ncommit={commit}\n'.encode():
            raise ValueError('Embedded source revision mismatch')
    lock=json.loads((ROOT/'dependencies.lock.json').read_text())
    dependencies={'.cache/'+lock[k]['archive']:lock[k]['sha256'] for k in ['openssl','libplist','ffmpeg']}
    pinned=lock['uxplay']['commit']
    proc=subprocess.Popen(['git','-C',str(ROOT/'vendor/uxplay'),'archive','--format=tar','--prefix=uxplay/',pinned],stdout=subprocess.PIPE)
    dependencies['upstream-uxplay-source.tar']=digest(proc.stdout)
    if proc.wait()!=0:raise ValueError('Pinned upstream source archive failed')
    source=ROOT/'dist/uxair-webos-1.2.0-source.tar.gz'
    verification=verify_source(source,git_blobs(),dependencies)
    destination=ROOT/'dist/test-candidates'/backend
    destination.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(package,destination/ASSET_NAMES[backend])
    shutil.copyfile(source,destination/source.name)
    shutil.copyfile(ROOT/'dist/ELF_REPORT.txt',destination/'ELF_REPORT.txt')
    (destination/'BUILD_COMMIT.txt').write_text(commit+'\n')
    (destination/'SOURCE_VERIFICATION.json').write_text(json.dumps(verification,indent=2)+'\n')
    (destination/'TEST-CANDIDATE.txt').write_text(
        'TEST ONLY / UNRELEASED. No GitHub Release has been published.\n'
        'Starfish is the Stable target. The B7 owner accepted candidate 9af36d1. NDL is physically unvalidated Beta/Experimental.\n'
        'The historical browser Malware verdict and documented security findings remain unresolved.\n'
        'Do not bypass browser or antivirus protection. Keep production app identity/state on upgrade.\n')
    manifest={'version':app['version'],'app_id':app['id'],'source_commit':commit,'backend':backend,
              'maturity':'Stable target, held for publication' if backend=='starfish' else 'Public Beta / Experimental, no hardware validation',
              'publication_approved':False,'security_release_hold':True,'files':{}}
    for item in [destination/ASSET_NAMES[backend],destination/source.name]:
        with item.open('rb') as stream:manifest['files'][item.name]={'bytes':item.stat().st_size,'sha256':digest(stream)}
    (destination/'CANDIDATE.json').write_text(json.dumps(manifest,indent=2)+'\n')
    (destination/'SHA256SUMS').write_text(''.join(v['sha256']+'  '+name+'\n' for name,v in manifest['files'].items()))
    print(json.dumps(manifest,indent=2))


if __name__=='__main__':main()

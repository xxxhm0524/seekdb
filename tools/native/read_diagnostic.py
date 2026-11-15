"""Diagnostic-only branch: keep all original assertions, compare both cores."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import tarfile
import tempfile
import sys

repo=Path(__file__).resolve().parents[2]
w=Path(tempfile.mkdtemp(prefix='mvd-read-diag-'));e=w/'evidence';e.mkdir();(w/'cases').mkdir()
env=dict(os.environ,MVDB_TEST_DIR=str(w/'cases'),TMPDIR=str(w/'cases'),PYTHONDONTWRITEBYTECODE='1')
def run(name,cmd,required=True):
    p=subprocess.run(cmd,capture_output=True,text=True,env=env,timeout=900)
    (e/(name+'.log')).write_text(p.stdout+p.stderr)
    print(json.dumps(dict(stage=name,exit_code=p.returncode)),flush=True)
    if required:assert p.returncode==0,name
    return p
run('compiler',['g++','--version']);run('kernel',['uname','-a'])
sources={}
base=w/'baseline';base.mkdir()
with tarfile.open(repo/'tools/native/fixtures/reference-a08.tar') as t:
    for f in t.getmembers():
        p=(base/f.name).resolve();p.relative_to(base.resolve());assert f.isdir() or f.isfile()
        if f.isdir():p.mkdir(parents=True,exist_ok=True)
        else:p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(t.extractfile(f).read())
sources['baseline']=base
candidate=w/'candidate';candidate.mkdir()
for name in ('src','include','tests'):shutil.copytree(repo/name,candidate/name)
shutil.copyfile(repo/'CMakeLists.txt',candidate/'CMakeLists.txt');sources['candidate']=candidate
results={}
for label,source in sources.items():
    # Diagnostics precede the existing assertions; their conditions remain intact.
    files=['unindexed_snapshot_tests.cpp','typed_scan_tests.cpp','cache_admission_tests.cpp']
    for name in files:
        p=source/'tests'/name;s=p.read_text()
        if name=='unindexed_snapshot_tests.cpp':needle='CHECK(failed);CHECK(failed_reads==static_cast<unsigned>(attempt+1));'
        elif name=='typed_scan_tests.cpp':needle='CHECK(failed);CHECK(failed_reads==attempt+1);'
        else:needle='CHECK(rejected&&failed_reads==attempt+1);'
        assert needle in s,name
        var='rejected' if name=='cache_admission_tests.cpp' else 'failed'
        prefix='std::cerr<<"[DIAG] caught="<<'+var+'<<" failed_reads="<<failed_reads<<"\\n";'
        p.write_text(s.replace(needle,prefix+needle))
    # Add only test instrumentation. Database source and production target flags
    # remain unchanged in both copies; all original assertions still execute.
    config=source/'CMakeLists.txt'
    extra='\nforeach(t unindexed_snapshot_tests typed_scan_tests cache_admission_tests)\n'
    extra+=' target_link_options(${t} PRIVATE -Wl,--wrap=__pread_chk)\n'
    extra+=' target_sources(${t} PRIVATE "'+str(repo/'tools/native/fortified_pread_wrap.cpp')+'")\nendforeach()\n'
    config.write_bytes(config.read_bytes()+extra.encode())
    b=w/(label+'-build')
    run(label+'-configure',['cmake','-S',str(source),'-B',str(b),'-G','Unix Makefiles','-DCMAKE_BUILD_TYPE=Release','-DCMAKE_EXE_LINKER_FLAGS=-static'])
    run(label+'-build',['cmake','--build',str(b),'--target','unindexed_snapshot_tests','typed_scan_tests','cache_admission_tests','minivectordb','--parallel','2','--verbose'])
    nm=run(label+'-symbols',['nm','-A',str(b/'libminivectordb_core.a')])
    lines=[line for line in nm.stdout.splitlines() if any(n in line for n in ('pread','pwrite','__read','_chk'))]
    (e/(label+'-relevant-symbols.log')).write_text('\n'.join(lines)+'\n');print(label,lines,flush=True)
    tests=run(label+'-tests',['ctest','--test-dir',str(b),'-R','core_(unindexed_snapshot|typed_scan|cache_admission)$','--output-on-failure','--output-junit',str(e/(label+'-tests.xml'))])
    results[label]=dict(exit_code=tests.returncode,output=tests.stdout+tests.stderr,binary_sha256=hashlib.sha256((b/'minivectordb').read_bytes()).hexdigest())
(e/'results.json').write_text(json.dumps(results,indent=2)+'\n');print(json.dumps(results),flush=True)

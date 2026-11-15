#!/usr/bin/env python3
"""Reproduce this fork's native checks; no Docker, push or platform requests."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time

HERE=Path(__file__).resolve().parent
REPO=HERE.parents[1]


def main():
    p=argparse.ArgumentParser()
    p.add_argument('--existing-build',type=Path)
    p.add_argument('--baseline-bin',type=Path)
    p.add_argument('--work-parent',type=Path,default=Path('/tmp'))
    p.add_argument('--benchmark',action='store_true')
    args=p.parse_args()
    assert sys.platform.startswith('linux') and os.geteuid()==0,'Use the already authorized root Linux environment.'
    assert Path('/data').is_dir(),'An existing /data mount target is required. Only the disposable CI runner creates it; this verifier does not alter host data directories.'
    manifest=json.loads((REPO/'validation/reference-opt-20261115/runtime-inputs.json').read_text())
    for n,h in manifest['sha256_lf'].items():assert hashlib.sha256((REPO/n).read_bytes().replace(b'\r\n',b'\n')).hexdigest()==h,n
    w=Path(tempfile.mkdtemp(prefix='mvd-ref-native-',dir=args.work_parent));w.chmod(0o755)
    for n in ('build','case-tmp','native-evidence','unit-case'):(w/n).mkdir(mode=0o755)
    for f in HERE.glob('*.py'):
        if f.name!='verify.py':shutil.copyfile(f,w/f.name)
    for n in ('native_ab.py','native_wide_bind.py'):shutil.copyfile(REPO/'bench'/n,w/n)
    shutil.copytree(REPO/'bench/vendor/seekdb-round2-f4d4e26',w/'official-kit')
    lock=json.loads((w/'official-kit/upstream-lock.json').read_text())
    for n,v in lock['files'].items():assert hashlib.sha256((w/'official-kit'/n).read_bytes()).hexdigest()==v['sha256'],n
    group=Path('/sys/fs/cgroup')/w.name;group.mkdir()
    for n,v in [('memory.max','2147483648'),('memory.swap.max','0'),('cpu.max','200000 100000'),('pids.max','128')]: (group/n).write_text(v)
    (w/'native-settings.json').write_text(json.dumps(dict(cgroup=str(group))))
    stages=[];env=dict(os.environ,TMPDIR=str(w/'case-tmp'),MVDB_TEST_DIR=str(w/'unit-case'),PYTHONDONTWRITEBYTECODE='1')
    def run(name,cmd,extra=None):
        start=time.monotonic()
        with (w/'native-evidence'/(name+'.log')).open('wb') as log:r=subprocess.run(cmd,env=dict(env,**(extra or {})),stdout=log,stderr=subprocess.STDOUT,timeout=1800)
        stages.append(dict(name=name,exit_code=r.returncode,seconds=time.monotonic()-start,argv=cmd));print(json.dumps(stages[-1]),flush=True)
        assert r.returncode==0,(name,str(w/'native-evidence'))
    build=args.existing_build
    if build is None:
        build=w/'candidate-build'
        run('configure',['cmake','-S',str(REPO),'-B',str(build),'-G','Unix Makefiles','-DCMAKE_BUILD_TYPE=Release','-DCMAKE_EXE_LINKER_FLAGS=-static'])
        run('build',['cmake','--build',str(build),'--parallel','2'])
    run('ctest',['ctest','--test-dir',str(build),'--output-on-failure','--parallel','2','--output-junit',str(w/'native-evidence/ctest.xml')])
    shutil.copyfile(build/'minivectordb',w/'build/candidate');(w/'build/candidate').chmod(0o755)
    if args.baseline_bin:shutil.copyfile(args.baseline_bin,w/'build/baseline')
    else:
        source=w/'baseline-source';source.mkdir()
        fixture=HERE/'fixtures/reference-a08.tar'
        assert hashlib.sha256(fixture.read_bytes()).hexdigest()=='36dfa7546462d8958a9a02276f763e08c87ccf34edc3459758f375c869288cf5'
        with tarfile.open(fixture) as t:
            for f in t.getmembers():
                dest=(source/f.name).resolve();dest.relative_to(source.resolve());assert f.isfile() or f.isdir()
                if f.isdir():dest.mkdir(parents=True,exist_ok=True)
                else:dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(t.extractfile(f).read())
        run('baseline-configure',['cmake','-S',str(source),'-B',str(w/'baseline-build'),'-G','Unix Makefiles','-DCMAKE_BUILD_TYPE=Release','-DCMAKE_EXE_LINKER_FLAGS=-static'])
        run('baseline-build',['cmake','--build',str(w/'baseline-build'),'--target','minivectordb','--parallel','2'])
        shutil.copyfile(w/'baseline-build/minivectordb',w/'build/baseline')
    (w/'build/baseline').chmod(0o755)
    run('public',[sys.executable,str(w/'official-kit/protocol_check/run.py'),'--command',json.dumps([sys.executable,str(w/'native_launcher_hardened.py'),'{data_dir}']),'--output',str(w/'native-evidence/public')],dict(MVD_NATIVE_BINARY=str(w/'build/candidate')))
    run('parity',[sys.executable,str(w/'native_parity_audit.py'),'--output',str(w/'native-evidence/parity'),'--candidate',str(w/'build/candidate'),'--reference',str(w/'build/baseline')])
    run('wire',[sys.executable,str(w/'native_wire_boundaries.py'),'--output',str(w/'native-evidence/wire'),'--candidate',str(w/'build/candidate'),'--reference',str(w/'build/baseline')])
    run('default-contract',[sys.executable,str(w/'default_contract.py'),'default-contract',str(w/'build/candidate')])
    if args.benchmark:
        shutil.copyfile(REPO/'bench/jsonl_oracle.cpp',w/'benchmark-oracle.cpp')
        run('oracle-build',['g++','-O2','-ffp-contract=off',str(w/'benchmark-oracle.cpp'),'-o',str(w/'build/benchmark-oracle')])
        run('benchmark',[sys.executable,str(w/'native_ab.py'),'--output',str(w/'native-evidence/benchmark'),'--baseline',str(w/'build/baseline'),'--candidate',str(w/'build/candidate'),'--rounds','6'])
    report=dict(passed=True,runtime_core=manifest['runtime_core'],workspace=str(w),stages=stages,
                binary_sha256=hashlib.sha256((w/'build/candidate').read_bytes()).hexdigest(),
                existing_build_reused=bool(args.existing_build),benchmark=args.benchmark,docker_used=False,formal_submitted=False)
    (w/'native-evidence/ci.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(dict(passed=True,evidence=str(w/'native-evidence'))),flush=True)


if __name__=='__main__':main()

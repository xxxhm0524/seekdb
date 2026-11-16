"""Native correctness and resource checks. Never pushes or requests formal evaluation."""
import hashlib
import io
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import traceback
import argparse

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
BASELINE = 'e86045306f5940613bfcf4aa84b0ee4bf74c20f1'


def extract(data, destination):
    destination.mkdir()
    with tarfile.open(fileobj=io.BytesIO(data)) as archive:
        for item in archive.getmembers():
            path = (destination / item.name).resolve(); path.relative_to(destination.resolve())
            assert item.isdir() or item.isfile()
            if item.isdir(): path.mkdir(parents=True, exist_ok=True)
            else: path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(archive.extractfile(item).read())


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--benchmark',action='store_true');args=parser.parse_args()
    assert sys.platform.startswith('linux') and os.geteuid() == 0, 'Use an authorized root Linux host.'
    assert Path('/data').is_dir(), 'An existing mount target is required; host contents are never used.'
    w = Path(tempfile.mkdtemp(prefix='mvd-e860-native-')); w.chmod(0o755)
    for name in ('build', 'case-tmp', 'native-evidence', 'artifact'): (w / name).mkdir(mode=0o755)
    out = w / 'native-evidence'; stages = []
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', TMPDIR=str(w / 'case-tmp'))
    for key in ('CXXFLAGS', 'CPPFLAGS', 'MVD_DOCKER_IMAGE', 'MVD_NATIVE_CGROUP'): env.pop(key, None)
    def run(name, command, cwd=REPO, extra=None):
        start = time.monotonic()
        with (out / (name + '.log')).open('wb') as log:
            result = subprocess.run(command, cwd=cwd, env=dict(env, **(extra or {})), stdout=log, stderr=subprocess.STDOUT, timeout=1800)
        row = dict(name=name, returncode=result.returncode, seconds=time.monotonic()-start, command=command)
        stages.append(row); print(json.dumps(row), flush=True)
        assert result.returncode == 0, (name, str(out))
    report = dict(passed=False, baseline=BASELINE, workspace=str(w), docker_used=False, formal_submitted=False, stages=stages)
    try:
        for source in HERE.glob('*.py'):
            if source.name != 'verify.py': shutil.copyfile(source, w / source.name)
        official = json.loads((REPO / 'validation/official-kit-sha256.json').read_text())
        assert official['upstream_commit'] == 'f4d4e26653af823a7601668802f80e374ad6643c' and len(official['sha256_lf']) == 49
        for name, digest in official['sha256_lf'].items():
            assert hashlib.sha256((REPO / name).read_bytes().replace(b'\r\n', b'\n')).hexdigest() == digest, name
        (w / 'official-kit').mkdir()
        for name in ('protocol_check', 'mysqltest'): shutil.copytree(REPO / name, w / 'official-kit' / name)
        group = Path('/sys/fs/cgroup') / w.name; group.mkdir()
        for name, value in [('memory.max', str(2 << 30)), ('memory.swap.max', '0'), ('cpu.max', '200000 100000'), ('pids.max', '128')]: (group / name).write_text(value)
        controllers = set((group / 'cgroup.controllers').read_text().split()); assert {'cpu', 'memory', 'pids'} <= controllers
        (group / 'cgroup.subtree_control').write_text(' '.join('+'+c for c in sorted(controllers & {'cpu','memory','pids','io'})))
        (group / 'shared').mkdir()
        for name, value in [('memory.max', str(2 << 30)), ('memory.swap.max', '0'), ('cpu.max', '200000 100000'), ('pids.max', '128')]: (group / 'shared' / name).write_text(value)
        (w / 'native-settings.json').write_text(json.dumps(dict(cgroup=str(group))))
        run('build-static', ['make', '-B', '-j2', 'CXX=g++', 'LDFLAGS=-static'])
        shutil.copyfile(REPO / 'minivectordb', w / 'build/candidate'); (w / 'build/candidate').chmod(0o755)
        dry_output = subprocess.check_output(['make','-Bn','CXX=g++','LDFLAGS=-static'], cwd=REPO, env=env, text=True)
        dry = next(line for line in reversed(dry_output.splitlines()) if line.lstrip().startswith('g++ '))
        command = shlex.split(dry); source_start = next(i for i, x in enumerate(command) if x.endswith('.cpp'))
        flags = command[1:source_start]
        run('build-dynamic', ['make', '-B', '-j2', 'CXX=g++', 'LDFLAGS='])
        shutil.copyfile(REPO / 'minivectordb', w / 'build/candidate-dynamic'); (w / 'build/candidate-dynamic').chmod(0o755)
        if subprocess.run(['git','cat-file','-e',BASELINE+'^{commit}'], cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
            run('fetch-baseline', ['git','fetch','--depth=1','--no-tags','origin',BASELINE])
        archive = subprocess.check_output(['git','-c','core.autocrlf=false','archive',BASELINE,'src','third_party','Makefile'], cwd=REPO)
        extract(archive, w / 'baseline-source')
        run('baseline-build', ['make','-B','-j2','CXX=g++','LDFLAGS=-static'], cwd=w / 'baseline-source')
        shutil.copyfile(w / 'baseline-source/minivectordb', w / 'build/baseline'); (w / 'build/baseline').chmod(0o755)
        fixture = HERE / 'fixtures/reference-a08.tar'
        assert hashlib.sha256(fixture.read_bytes()).hexdigest() == '36dfa7546462d8958a9a02276f763e08c87ccf34edc3459758f375c869288cf5'
        extract(fixture.read_bytes(), w / 'reference-source')
        run('reference-configure', ['cmake','-S',str(w/'reference-source'),'-B',str(w/'reference-build'),'-DCMAKE_BUILD_TYPE=Release','-DCMAKE_EXE_LINKER_FLAGS=-static'])
        run('reference-build', ['cmake','--build',str(w/'reference-build'),'--target','minivectordb','--parallel','2'])
        shutil.copyfile(w/'reference-build/minivectordb',w/'build/reference-candidate'); (w/'build/reference-candidate').chmod(0o755)
        run('regression', [sys.executable,str(w/'run_native_e860_checks.py'),'--source',str(REPO),'--binary','build/candidate-dynamic','--output','native-evidence/regression'], cwd=w)
        run('public', [sys.executable,str(w/'run_native_public.py'),'--binary','build/candidate','--output','native-evidence/public'], cwd=w)
        run('baseline-parity', [sys.executable,str(w/'native_parity_audit.py'),'--candidate',str(w/'build/candidate'),'--reference',str(w/'build/baseline'),'--reference-commit',BASELINE,'--output',str(out/'baseline-parity')], cwd=w)
        run('wire', [sys.executable,str(w/'native_wire_boundaries.py'),'--candidate',str(w/'build/candidate'),'--reference',str(w/'build/baseline'),'--output',str(out/'wire')], cwd=w)
        run('default-contract', [sys.executable,str(w/'default_contract.py'),'default-contract',str(w/'build/candidate')], cwd=w)
        runtime = ['vector_index','storage','record_reader','model','btree','compaction','vector_literal']
        probes = [('read_mapping_probe', ['storage','record_reader','model','btree','compaction','vector_literal'], True),
            ('mapped_source_probe',runtime,True),('scalar_snapshot_probe',runtime,True),('certified_screen_probe',runtime,True),('error_successor_probe',[],False),
            ('json_request_probe',['request','model','vector_literal'],False),('json_string_output_probe',['model','vector_literal'],False),('decimal_conversion_probe',['model','vector_literal'],False),('vector_literal_probe',['model','vector_literal'],False),('exact_batch_probe',[],False)]
        for name, dependencies, data_argument in probes:
            options = [x for x in flags if x != '-DNDEBUG'] + ['-UNDEBUG'] if name == 'read_mapping_probe' else flags
            extra = ['-Wl,--wrap=mmap,--wrap=munmap'] if name == 'read_mapping_probe' else []
            if name == 'decimal_conversion_probe': extra += ['tests/fixtures/legacy_vector_literal.cpp']
            run(name+'-build', ['g++']+options+['tests/'+name+'.cpp']+['src/'+n+'.cpp' for n in dependencies]+extra+['-o',str(w/'build'/name)])
            cmd = [str(w/'build'/name)] + ([str(out/(name+'-data'))] if data_argument else [])
            run(name, cmd)
        run('reclaim-lifecycle', [sys.executable,str(REPO/'tests/reclaim_pages.py'),'--binary',str(w/'build/candidate-dynamic'),'--reference-bin',str(w/'build/baseline'),'--output',str(out/'reclaim-lifecycle')])
        run('capacity', [sys.executable,str(w/'capacity.py'),'--binary','build/candidate','--output','native-evidence/capacity'], cwd=w)
        cpp = sorted(str(x.relative_to(REPO)) for x in (REPO/'src').glob('*.cpp'))
        sanitized = ['g++','-Isrc','-Ithird_party','-std=c++20','-O1','-g','-fno-omit-frame-pointer','-fno-pie','-no-pie','-fsanitize=address,undefined','-ffp-contract=off']
        run('sanitizer-build', sanitized+cpp+['-o',str(w/'build/candidate-asan')])
        for name in ('invariants','column_layouts','cache_column_sets','cache_ranges','sql_literals','protocol_complete_input','protocol_ids','protocol_nested_ids'):
            run('sanitizer-'+name, [sys.executable,str(REPO/'tests'/(name+'.py')),'--output',str(out/('sanitizer-'+name))], extra=dict(MVD_BIN=str(w/'build/candidate-asan'),ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1'))
        run('sanitizer-json-string-build', sanitized+['tests/json_string_output_probe.cpp','src/model.cpp','src/vector_literal.cpp','-o',str(w/'build/json-string-asan')])
        run('sanitizer-json-string', [str(w/'build/json-string-asan')], extra=dict(ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1'))
        run('sanitizer-decimal-build', sanitized+['tests/decimal_conversion_probe.cpp','tests/fixtures/legacy_vector_literal.cpp','src/model.cpp','src/vector_literal.cpp','-o',str(w/'build/decimal-asan')])
        run('sanitizer-decimal', [str(w/'build/decimal-asan')], extra=dict(ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1'))
        if args.benchmark:
            run('performance',[sys.executable,str(w/'benchmark.py'),str(REPO)],cwd=w)
        report.update(passed=True, commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=REPO,text=True).strip(),
            compiler=subprocess.check_output(['g++','--version'],text=True).splitlines()[0], default_build_command=dry,
            binary_sha256=hashlib.sha256((w/'build/candidate').read_bytes()).hexdigest(),
            dynamic_binary_sha256=hashlib.sha256((w/'build/candidate-dynamic').read_bytes()).hexdigest())
    except Exception:
        report['error'] = traceback.format_exc(); raise
    finally:
        (out/'ci.json').write_text(json.dumps(report,indent=2)+'\n')
        for source in out.rglob('*'):
            if source.is_file() and source.suffix in ('.json','.jsonl','.log','.txt','.xml'):
                target = w/'artifact'/source.relative_to(out); target.parent.mkdir(parents=True,exist_ok=True); shutil.copyfile(source,target)
        print(json.dumps(dict(passed=report['passed'], artifact=str(w/'artifact'))),flush=True)


if __name__ == '__main__': main()

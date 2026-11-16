"""Matched durable-write and query measurements; no official E calculation."""
import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = Path(sys.argv[1]).resolve(strict=True)
BASE = '6a69bf19d1a33c641be6a567fed90bf729ef5873'
FLOOR = '999f3a9dac509d63aed62aea2f0a5027cbf93f67'
out = ROOT/'native-evidence/performance'
out.mkdir()
env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1')
env.pop('MVD_NATIVE_CGROUP', None)

def run(name, command, cwd=ROOT):
    with (out/(name+'.log')).open('wb') as log:
        result = subprocess.run(command, cwd=cwd, env=env, stdout=log,
                                stderr=subprocess.STDOUT, timeout=1800)
    print(json.dumps(dict(performance_stage=name, returncode=result.returncode)), flush=True)
    result.check_returncode()

def build_control(name, commit):
    if subprocess.run(['git','cat-file','-e',commit+'^{commit}'], cwd=REPO,
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
        run('fetch-'+name, ['git','fetch','--no-tags','--depth=1','origin',commit], REPO)
    source = ROOT/('performance-'+name+'-source')
    source.mkdir()
    raw = subprocess.check_output(['git','-c','core.autocrlf=false','archive',commit,'src','third_party','Makefile'], cwd=REPO)
    with tarfile.open(fileobj=io.BytesIO(raw)) as archive:
        for item in archive.getmembers():
            target = (source/item.name).resolve()
            target.relative_to(source.resolve())
            assert item.isfile() or item.isdir()
            if item.isdir(): target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(archive.extractfile(item).read())
    run(name+'-build', ['make','-B','-j2','CXX=g++','LDFLAGS=-static'], source)
    target = ROOT/'build'/('performance-'+name)
    shutil.copyfile(source/'minivectordb', target)
    target.chmod(0o755)

for name in ('native_ab_e860.py','native_query_pairs.py','native_write_pairs.py','native_resource_pairs.py','write_focus.py','query_focus.py','benchmark-oracle.cpp'):
    shutil.copyfile(REPO/'bench/e5000'/name, ROOT/name)
build_control('baseline', BASE)
build_control('floor', FLOOR)
run('oracle-build', ['g++','-O2','-ffp-contract=off','benchmark-oracle.cpp','-o','build/benchmark-oracle'])
cpus = sorted(os.sched_getaffinity(0))
assert len(cpus) >= 2
driver, server = cpus[:2]
cpu_args = ['--driver-cpu',str(driver),'--server-cpu',str(server)]
focus_args = ['--shape','wide','--rows','1000','--rounds','6','--gate-slack','0.01'] + cpu_args
for name, extra in [('query-calibration-id',[]),('query-calibration-projection',['--with-body','--kind','text','--indexed','yes'])]:
    run(name,[sys.executable,'query_focus.py','--output','native-evidence/performance/'+name,
              '--baseline','build/performance-baseline','--candidate','build/performance-baseline',
              '--queries','1000','--rounds','12'] + cpu_args + extra)
for name, candidate, extra in [('write-calibration','build/performance-baseline',[]),
                               ('write-focus','build/candidate',[]),
                               ('write-whole','build/candidate',['--whole'])]:
    run(name, [sys.executable,'write_focus.py','--output','native-evidence/performance/'+name,
               '--baseline','build/performance-baseline','--candidate',candidate] + focus_args + extra)
run('resources', [sys.executable,'native_resource_pairs.py','--output','native-evidence/performance/resources',
                  '--rounds','3','--program','baseline=build/performance-baseline',
                  '--program','floor=build/performance-floor','--program','candidate=build/candidate','--ordered-keys'] + cpu_args)
for shape, extra in [('id',[]),('projection',['--with-body'])]:
    run('queries-'+shape, [sys.executable,'native_query_pairs.py','--output','native-evidence/performance/queries-'+shape,
                          '--baseline','build/performance-baseline','--candidate','build/candidate','--rounds','12'] + cpu_args + extra)
    run('queries-floor-'+shape, [sys.executable,'native_query_pairs.py','--output','native-evidence/performance/queries-floor-'+shape,
                          '--baseline','build/performance-floor','--candidate','build/candidate','--rounds','12'] + cpu_args + extra)
for name, baseline in [('writes','build/performance-baseline'),('writes-floor','build/performance-floor')]:
    run(name, [sys.executable,'native_write_pairs.py','--output','native-evidence/performance/'+name,
               '--baseline',baseline,'--candidate','build/candidate','--rounds','6'] + cpu_args)
result = dict(passed=True, baseline_commit=BASE, prior_baseline_commit=FLOOR,
              driver_cpu=driver, server_cpu=server, results={},
              scope='Same native compiler, exact file bytes and reopened f32 bits, fixed CPUs not reserved. Cgroup memory includes page cache. No formal E inferred.')
for name in ('query-calibration-id','query-calibration-projection','write-calibration','write-focus','write-whole','resources','queries-id','queries-projection','queries-floor-id','queries-floor-projection','writes','writes-floor'):
    data = json.loads((out/name/'report.json').read_text())
    assert data['passed']
    result['results'][name] = {k:v for k,v in data.items() if k not in ('records','samples')}
    if (out/name/'performance-gate.json').exists():
        result['results'][name]['performance_gate'] = json.loads((out/name/'performance-gate.json').read_text())
calibration = abs(result['results']['write-calibration']['performance_gate']['median_regression_pct']) <= 1
result['calibration_within_one_percent'] = calibration
result['focused_regression_remaining'] = calibration and not result['results']['write-focus']['performance_gate']['performance_gate_passed']
query_calibration = all(max(abs(v) for v in result['results'][name]['performance_gate']['regression_percent'].values()) <= 1 for name in ('query-calibration-id','query-calibration-projection'))
worst_query_regression = max(100*(1/value['median_paired_speedup']-1) for name in ('queries-id','queries-projection') for value in result['results'][name]['summary'].values())
result['query_calibration_within_one_percent'] = query_calibration
result['worst_query_regression_percent'] = worst_query_regression
(out/'report.json').write_text(json.dumps(result,indent=2)+'\n')
assert not result['focused_regression_remaining'], 'Calibrated wide-vector durable write regression remains'
assert not (query_calibration and worst_query_regression>1.5), 'Calibrated query regression exceeds 1.5 percent'
print(json.dumps(dict(performance_passed=True, calibration_within_one_percent=calibration)), flush=True)

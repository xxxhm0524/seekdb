"""Freeze two GCC13 builds and replay query regression controls; no submission."""
from pathlib import Path
import hashlib, json, os, shutil, subprocess, sys, tempfile, time, zipfile
from verify import extract

REPO = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
VERSIONS = {'old': '999f3a9dac509d63aed62aea2f0a5027cbf93f67',
            'current': '6a69bf19d1a33c641be6a567fed90bf729ef5873',
            'candidate': '6a69bf19d1a33c641be6a567fed90bf729ef5873'}

def main():
    assert sys.platform.startswith('linux') and os.geteuid() == 0
    assert Path('/data').is_dir()
    w = Path(tempfile.mkdtemp(prefix='mvd-e860-query-'))
    w.chmod(0o755)
    for name in ('build', 'case-tmp', 'native-evidence', 'artifact'):
        (w/name).mkdir(mode=0o755)
    out = w/'native-evidence'
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', TMPDIR=str(w/'case-tmp'))
    for key in ('CXXFLAGS','CPPFLAGS','MVD_DOCKER_IMAGE','MVD_NATIVE_CGROUP'):
        env.pop(key, None)
    stages = []
    report = dict(passed=False, commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=REPO,text=True).strip(),
                  compiler=subprocess.check_output(['g++','--version'],text=True).splitlines()[0],
                  versions=VERSIONS, stages=stages, formal_submitted=False)
    def run(name, command, cwd=w, allowed=(0,)):
        start = time.monotonic()
        with (out/(name+'.log')).open('wb') as log:
            result = subprocess.run(command,cwd=cwd,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=1200)
        stages.append(dict(name=name,returncode=result.returncode,seconds=time.monotonic()-start))
        print(json.dumps(stages[-1]),flush=True)
        assert result.returncode in allowed, name
    try:
        for name in ('native_test_support.py','native_launcher_hardened.py','run_native_public.py'):
            shutil.copyfile(HERE/name,w/name)
        for name in ('native_ab_e860.py','native_query_pairs.py','native_write_pairs.py','native_resource_pairs.py','query_focus.py','benchmark-oracle.cpp'):
            shutil.copyfile(REPO/'bench/e5000'/name,w/name)
        (w/'official-kit').mkdir()
        for name in ('protocol_check','mysqltest'):
            shutil.copytree(REPO/name,w/'official-kit'/name)
        group = Path('/sys/fs/cgroup')/w.name
        group.mkdir()
        limits = [('memory.max',str(2<<30)),('memory.swap.max','0'),('cpu.max','200000 100000'),('pids.max','128')]
        for name,value in limits: (group/name).write_text(value)
        controllers = set((group/'cgroup.controllers').read_text().split())
        (group/'cgroup.subtree_control').write_text(' '.join('+'+c for c in sorted(controllers & {'cpu','memory','io','pids'})))
        (group/'shared').mkdir()
        for name,value in limits: (group/'shared'/name).write_text(value)
        (w/'native-settings.json').write_text(json.dumps(dict(cgroup=str(group))))
        for name,commit in VERSIONS.items():
            if subprocess.run(['git','cat-file','-e',commit+'^{commit}'],cwd=REPO,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode:
                run('fetch-'+name,['git','fetch','--depth=1','--no-tags','origin',commit],REPO)
            raw = subprocess.check_output(['git','-c','core.autocrlf=false','archive',commit,'src','third_party','Makefile'],cwd=REPO)
            source = w/(name+'-source')
            extract(raw,source)
            if name == 'candidate':
                prototype = REPO/'bench/prototypes/single_pass_vector_literal.cpp'
                shutil.copyfile(prototype, source/'src/vector_literal.cpp')
                report['prototype_sha256'] = hashlib.sha256(prototype.read_bytes()).hexdigest()
            run('build-'+name,['make','-B','CXX=g++','LDFLAGS=-static'],source)
            shutil.copyfile(source/'minivectordb',w/'build'/name)
            (w/'build'/name).chmod(0o755)
            shutil.copyfile(REPO/'bench/e5000/parse_cost_probe.cpp',source/'parse_cost_probe.cpp')
            run('parse-build-'+name,['g++','-Isrc','-Ithird_party','-std=c++20','-O3','-DNDEBUG','-ffp-contract=off','parse_cost_probe.cpp','src/model.cpp','src/vector_literal.cpp','-static','-o',str(w/'build'/('parse-'+name))],source)
            if name == 'candidate':
                (source/'tests/fixtures').mkdir(parents=True)
                shutil.copyfile(REPO/'bench/prototypes/single_pass_decimal_probe.cpp',source/'tests/decimal_conversion_probe.cpp')
                shutil.copyfile(REPO/'tests/fixtures/legacy_vector_literal.cpp',source/'tests/fixtures/legacy_vector_literal.cpp')
                run('candidate-conformance-build',['g++','-Isrc','-Ithird_party','-std=c++20','-O3','-DNDEBUG','-ffp-contract=off','tests/decimal_conversion_probe.cpp','tests/fixtures/legacy_vector_literal.cpp','src/model.cpp','src/vector_literal.cpp','-o',str(w/'build/candidate-conformance')],source)
        reference = w/'scored-reference-source'
        reference.mkdir()
        with zipfile.ZipFile(REPO/'bench/references/freaybru-3dfd062.zip') as archive:
            identity = json.loads(archive.read('IDENTITY.json'))
            hashes = json.loads(archive.read('SOURCE_HASHES.json'))
            assert identity['commit'] == '3dfd062502aed05de32b269c64bd32995b0ffb5e'
            for name,digest in hashes['files'].items():
                raw = archive.read(name)
                assert hashlib.sha256(raw).hexdigest() == digest
                target = (reference/name).resolve()
                target.relative_to(reference.resolve())
                target.parent.mkdir(parents=True,exist_ok=True)
                target.write_bytes(raw)
            report['scored_reference'] = identity
        run('build-scored-reference',['g++','-std=c++20','-O3','-DNDEBUG','-ffp-contract=off','-static',
            'extra/main.cpp','extra/db.cpp','extra/sql.cpp','extra/log.cpp','-o',str(w/'build/scored-reference')],reference)
        run('oracle-build',['g++','-O2','-ffp-contract=off','benchmark-oracle.cpp','-o','build/benchmark-oracle'])
        cpuinfo = Path('/proc/cpuinfo').read_text().splitlines()
        report['cpu_model'] = next((x.split(':',1)[1].strip() for x in cpuinfo if x.startswith('model name')), 'unknown')
        flags = set(next((x.split(':',1)[1].split() for x in cpuinfo if x.startswith('flags')), []))
        report['cpu_features'] = sorted(flags & {'avx2','avx512f','avx512_vnni'})
        cpus = sorted(os.sched_getaffinity(0))
        assert len(cpus)>=2
        driver,server=cpus[:2]
        cpu_args=['--driver-cpu',str(driver),'--server-cpu',str(server)]
        run('candidate-conformance',[str(w/'build/candidate-conformance')])
        run('scored-reference-public',[sys.executable,'run_native_public.py','--binary','build/scored-reference','--output',str(out/'scored-reference-public')])
        for name,baseline,candidate,extra in [('aa','current','current',[]),('repro','old','current',[]),
                                             ('reverse','current','old',[]),('whole','old','current',['--whole']),
                                             ('projection','old','current',['--with-body','--kind','text','--indexed','yes'])]:
            run(name,[sys.executable,'query_focus.py','--output',str(out/name),'--baseline','build/'+baseline,
                      '--candidate','build/'+candidate,'--rounds','12','--queries','1000','--gate']+cpu_args+extra,allowed=(0,2))
        for shape,extra in [('id',[]),('projection',['--with-body'])]:
            run('candidate-'+shape,[sys.executable,'native_query_pairs.py','--output',str(out/('candidate-'+shape)),
                '--baseline','build/current','--candidate','build/candidate','--rounds','6']+cpu_args+extra)
        run('candidate-writes',[sys.executable,'native_write_pairs.py','--output',str(out/'candidate-writes'),
            '--baseline','build/current','--candidate','build/candidate','--rounds','6']+cpu_args)
        for version in ('current','candidate'):
            for shape,extra in [('id',[]),('projection',['--with-body'])]:
                name = 'reference-'+version+'-'+shape
                run(name,[sys.executable,'native_query_pairs.py','--output',str(out/name),
                    '--baseline','build/scored-reference','--candidate','build/'+version,'--rounds','6']+cpu_args+extra)
        run('reference-resources',[sys.executable,'native_resource_pairs.py','--output',str(out/'reference-resources'),
            '--rounds','3','--program','reference=build/scored-reference','--program','current=build/current',
            '--program','candidate=build/candidate','--ordered-keys']+cpu_args)
        for name in VERSIONS:
            run('parse-'+name,['taskset','-c',str(server),str(w/'build'/('parse-'+name))])
        old=[json.loads(x) for x in (out/'parse-old.log').read_text().splitlines()]
        for name in ('current','candidate'):
            current=[json.loads(x) for x in (out/('parse-'+name+'.log')).read_text().splitlines()]
            assert [x['checksum'] for x in old]==[x['checksum'] for x in current]
        report.update(passed=True,driver_cpu=driver,server_cpu=server,
                      binaries={name:hashlib.sha256((w/'build'/name).read_bytes()).hexdigest() for name in ('old','current','candidate','scored-reference','parse-old','parse-current','parse-candidate')},
                      results={name:json.loads((out/name/'performance-gate.json').read_text()) for name in ('aa','repro','reverse','whole','projection')})
    finally:
        (out/'ci.json').write_text(json.dumps(report,indent=2)+'\n')
        for source in out.rglob('*'):
            if source.is_file() and source.suffix in ('.json','.jsonl','.txt','.log'):
                target=w/'artifact'/source.relative_to(out)
                target.parent.mkdir(parents=True,exist_ok=True)
                shutil.copyfile(source,target)
        (w/'artifact/binaries').mkdir()
        for source in (w/'build').iterdir():
            if source.name in ('old','current','candidate','scored-reference','parse-old','parse-current','parse-candidate'):
                shutil.copyfile(source,w/'artifact/binaries'/source.name)
        print(json.dumps(dict(passed=report['passed'],artifact=str(w/'artifact'))),flush=True)

if __name__=='__main__': main()

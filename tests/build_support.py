"""Use the product source manifest for fault probes and sanitizer builds."""
import hashlib,json,os,subprocess
from pathlib import Path

def library_sources(root):
    root=Path(root);manifest=root/'src/sources.mk'
    if not manifest.exists():return [root/'src/engine.cpp',root/'src/sql.cpp']
    names=manifest.read_text(encoding='utf-8').split('=',1)[1].split()
    assert names and all(name.startswith('src/') and name.endswith('.cpp') and '..' not in Path(name).parts for name in names)
    actual={path.relative_to(root).as_posix() for path in (root/'src').rglob('*.cpp') if path.name!='main.cpp'}
    assert len(names)==len(set(names)) and set(names)==actual,'Product source manifest is incomplete or duplicated'
    return [root/name for name in names]

def engine_objects(root,flags):
    root=Path(root);sources=library_sources(root)
    compiler=subprocess.check_output(['g++','-dumpfullversion'],text=True).strip()
    digest=hashlib.sha256(json.dumps([compiler,flags],sort_keys=True).encode())
    for path in sorted((root/'src').rglob('*')):
        if path.is_file() and path.suffix in ('.h','.cpp','.mk'):
            digest.update(path.relative_to(root).as_posix().encode());digest.update(path.read_bytes())
    folder=root/'.mvd-test-build'/digest.hexdigest();folder.mkdir(parents=True,exist_ok=True)
    result=[]
    for index,source in enumerate(sources):
        output=folder/('%03d-%s.o'%(index,source.stem))
        if not output.exists():
            temporary=folder/('%03d-%s-%d.pending.o'%(index,source.stem,os.getpid()))
            subprocess.run(['g++',*flags,'-c',str(source),'-o',str(temporary)],check=True,timeout=180)
            temporary.replace(output)
        result.append(str(output))
    return result

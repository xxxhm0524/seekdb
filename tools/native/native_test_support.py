import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile

ROOT=Path(__file__).resolve().parent
tempfile.tempdir=str(ROOT/'case-tmp')
spec=importlib.util.spec_from_file_location('official_transport',ROOT/'official-kit/protocol_check/_transport.py')
transport=importlib.util.module_from_spec(spec);spec.loader.exec_module(transport)
CONFIG=json.loads((ROOT/'official-kit/protocol_check/config.json').read_text())


class NativeSession(transport.Session):
    def __init__(self,out,name,binary=None):
        self.native_binary=str(binary or os.environ.get('MVD_TEST_BINARY') or ROOT/'build/minivectordb')
        super().__init__(CONFIG,Path(out)/(name+'.stderr.log'),command=[sys.executable,str(ROOT/'native_launcher_hardened.py'),'{data_dir}'])
        self.start()
    def start(self,ready=True):
        previous=os.environ.get('MVD_NATIVE_BINARY');os.environ['MVD_NATIVE_BINARY']=self.native_binary
        try:return super().start(ready)
        finally:
            if previous is None:os.environ.pop('MVD_NATIVE_BINARY',None)
            else:os.environ['MVD_NATIVE_BINARY']=previous
    def execute(self,sql,ok=True,mode='exact'):
        result=self.request(sql,mode)
        if ok is not None:assert result['ok'] is ok,(sql[:150],result)
        return result
    def restart(self,crash=False):
        self.crash() if crash else self.shutdown()
        self.start()


def save(path,value):Path(path).write_text(json.dumps(value,ensure_ascii=False,indent=2)+'\n')

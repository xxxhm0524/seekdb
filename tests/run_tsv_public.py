#!/usr/bin/env python3
"""Run public v4 cases against the tsv + __MVD_READY__ protocol."""
import json, math, os, shutil, subprocess, sys, tempfile
HERE=os.path.dirname(os.path.abspath(__file__)); ROOT=os.path.dirname(HERE)
BIN=os.environ.get('MVD_BIN') or os.path.join(ROOT,'minivectordb.exe' if os.path.exists(os.path.join(ROOT,'minivectordb.exe')) else 'minivectordb')
MARK='__MVD_READY__'
class C:
    def __init__(self,d):
        self.d=d; self.p=None; self.start()
    def start(self):
        self.p=subprocess.Popen([BIN,'--data-dir='+self.d,'--output=tsv','--ready-marker='+MARK],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,encoding='utf-8',bufsize=1)
    def stop(self):
        if not self.p:return
        try:self.p.stdin.close()
        except:pass
        try:self.p.wait(timeout=10)
        except: self.p.kill(); self.p.wait()
        self.p=None
    def run(self,sql):
        self.p.stdin.write(sql.rstrip(';')+';\n');self.p.stdin.flush();out=[]
        while True:
            line=self.p.stdout.readline()
            if line=='': raise RuntimeError('exited: '+self.p.stderr.read())
            line=line.rstrip('\r\n')
            if line==MARK:return out
            out.append(line)
def cast(raw,want):
    if isinstance(want,bool):return raw.lower()=='true'
    if isinstance(want,int):return int(raw)
    if isinstance(want,float):return float(raw)
    return raw
def cmp(actual,exp):
    if 'error' in exp: return actual and actual[0].startswith('ERROR '+exp['error']+':')
    if exp=={}: return actual and actual[0].startswith('OK')
    if 'affected_rows' in exp: return actual==['OK %d'%exp['affected_rows']]
    cols=exp.get('columns',[]);rows=exp.get('rows',[]);got=[]
    for line in actual:
        f=line.split('\t')
        if len(f)!=len(cols):return False
        got.append(f)
    if len(got)!=len(rows):return False
    for gr,er in zip(got,rows):
        for raw,w in zip(gr,er):
            try:g=cast(raw,w)
            except ValueError:return False
            if isinstance(w,float):
                if not math.isclose(g,w,rel_tol=1e-5,abs_tol=1e-5):return False
            elif g!=w:return False
    if exp.get('unordered'):return sorted(map(str,got))==sorted(map(str,rows))
    return True
def main():
    manifest=json.load(open(os.path.join(HERE,'manifest_new.json'),encoding='utf-8'))
    bad=0
    for case in manifest['cases']:
        base=tempfile.mkdtemp(prefix='mvdtsv_'); d=os.path.join(base,'data');os.makedirs(d)
        c=C(d); exp=json.load(open(os.path.join(HERE,'newcases',os.path.basename(case['result'])),encoding='utf-8')); idx=0; ok=True
        try:
            for line in open(os.path.join(HERE,'newcases',os.path.basename(case['test'])),encoding='utf-8'):
                s=line.strip()
                if not s:continue
                if s=='--restart': c.stop();c.start();continue
                if s.startswith('--mode '): c.run('SET vector_mode = '+s.split()[-1]);continue
                if s.startswith('--'):continue
                actual=c.run(s)
                if idx>=len(exp) or not cmp(actual,exp[idx]):
                    print('FAIL',case['id'],'stmt',idx+1,s,'expected',exp[idx] if idx<len(exp) else None,'actual',actual);ok=False;bad+=1;break
                idx+=1
        finally:c.stop();shutil.rmtree(base,ignore_errors=True)
        print(('PASS ' if ok else 'FAIL ')+case['id'])
    print(f'{len(manifest["cases"])-bad}/{len(manifest["cases"])} passed')
    return 1 if bad else 0
if __name__=='__main__':sys.exit(main())

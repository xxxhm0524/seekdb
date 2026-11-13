"""600-digit Decimal oracle, independent of the fixed-word implementation."""
import argparse,decimal,json,math,os,random,struct,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parent.parent
p=argparse.ArgumentParser();p.add_argument('--sanitize',choices=['undefined','address']);p.add_argument('--output',type=Path);a=p.parse_args()
decimal.getcontext().prec=600;rng=random.Random(20261113)
def f(bits):return struct.unpack('f',struct.pack('I',bits))[0]
def bits(x):return struct.unpack('Q',struct.pack('d',x))[0]
def sample():
    while True:
        v=rng.getrandbits(32)
        if ((v>>23)&255)!=255:return v
cases=[([],[])]
for n in [1,2,3,8,9,16,33,65,128,256]:
    for _ in range(30):
        x=[sample() for i in range(n)];y=[sample() for i in range(n)]
        cases.append((x,y));cases.append((x[::-1],y[::-1]));cases.append((y,x))
for _ in range(4):cases.append(([sample() for i in range(16384)],[sample() for i in range(16384)]))
cases += [([i],[j]) for i in [0,0x80000000,1,0x7f7fffff,0xff7fffff,0x3f800000] for j in [0,1,0x80000000,0x7f7fffff,0xff7fffff]]
def fb(value):return struct.unpack('I',struct.pack('f',value))[0]
for power in [-40,-20,0,20,80]:
    scale=2.0**power
    for values in [[scale,scale*2.0**-27,scale*2.0**-27],
                   [scale,scale*2.0**-26,scale*2.0**-27,scale*2.0**-27]]:
        x=[fb(value) for value in values]
        for sign in [-1,0,1]:
            y=[0]*len(x);y[1]=fb(sign*scale*2.0**-100)
            cases.append((x,y))
expected=[]
for x,y in cases:
    square=sum((decimal.Decimal.from_float(f(i))-decimal.Decimal.from_float(f(j)))**2 for i,j in zip(x,y))
    value=float(square);expected.append((bits(value),bits(math.sqrt(value))))
flags=['-std=c++20','-O2','-Wall','-Wextra','-Werror']
if a.sanitize:flags+=['-g','-fno-omit-frame-pointer','-fno-sanitize-recover=all','-fsanitize='+('address,undefined' if a.sanitize=='address' else 'undefined')]
with tempfile.TemporaryDirectory(prefix='mvd-exact-norm-model-') as directory:
    binary=Path(directory)/('model.exe' if os.name=='nt' else 'model')
    subprocess.run(['g++',*flags,str(ROOT/'tests/exact_norm_model.cpp'),'-o',str(binary)],check=True,timeout=60)
    text='\n'.join(' '.join(map(str,[len(x),*x,*y])) for x,y in cases)+'\n'
    run=subprocess.run([str(binary)],input=text,text=True,capture_output=True,timeout=60)
    assert run.returncode==0,run.stderr
    actual=[tuple(int(v,16) for v in row.split()) for row in run.stdout.splitlines()]
    assert len(actual)==len(expected),(len(actual),len(expected))
    for i,(x,y) in enumerate(zip(actual,expected)):assert x==y,(i,cases[i],x,y)
report=dict(passed=True,cases=len(cases),coordinates=sum(len(x) for x,y in cases),oracle_decimal_digits=600,
            squared_and_returned_distance_bits_identical=True,sanitizer=a.sanitize)
if a.output:
    a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print(json.dumps(report))

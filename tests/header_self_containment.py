"""Compile module interfaces independently to reject hidden include ordering."""
import argparse,json,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parent.parent
p=argparse.ArgumentParser();p.add_argument('--output',type=Path);a=p.parse_args()
results=[]
for header in sorted((ROOT/'src').rglob('*.h')):
    unit='#include "'+header.as_posix()+'"\nint main(){}\n'
    run=subprocess.run(['g++','-std=c++20','-fopenmp','-fsyntax-only','-x','c++','-'],input=unit,capture_output=True,text=True,timeout=45)
    results.append(dict(header=header.relative_to(ROOT).as_posix(),exit_code=run.returncode,stderr=run.stderr if run.returncode else ''))
report=dict(passed=all(row['exit_code']==0 for row in results),headers=len(results),results=results)
if a.output:a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print(json.dumps(report));raise SystemExit(0 if report['passed'] else 1)

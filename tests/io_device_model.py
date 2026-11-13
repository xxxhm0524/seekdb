"""Data-filesystem selection, including the exact multi-device CI snapshot."""
import argparse,json,subprocess,unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
import docker_score_compare as score
p=argparse.ArgumentParser();p.add_argument('--output',type=Path);a=p.parse_args()
class Session:
    name='fixture'
    def __init__(self,counters,peak=123456):self.counters=counters;self.peak=peak
    def docker(self,*args):
        rows=[str(self.peak)]+[device+' '+ ' '.join(k+'='+str(v) for k,v in values.items()) for device,values in self.counters.items()]
        return SimpleNamespace(stdout=('\n'.join(rows)+'\n').encode())
def measure(counters,mount='252:0',parent=None,explicit=None,peak=123456):
    with patch.object(score,'data_mount_device',return_value=mount,create=True),patch.object(score,'partition_parent_device',return_value=parent,create=True):
        return score.resources(Session(counters,peak),explicit)
class Model(unittest.TestCase):
    def test_real_multi_device_snapshot(self):
        counters={'259:5':dict(rbytes=3067904,wbytes=18587648),'259:0':dict(rbytes=3100672,wbytes=0),'252:0':dict(rbytes=6168576,wbytes=18579456)}
        result=measure(counters);self.assertEqual(result['io_device'],'252:0');self.assertEqual(result['io_bytes'],24748032)
        self.assertEqual(result['io_stat'],counters);self.assertEqual(result['data_mount_device'],'252:0')
    def test_data_device_is_not_largest_counter(self):
        result=measure({'8:0':dict(rbytes=1,wbytes=2),'8:16':dict(rbytes=1000000,wbytes=1000000)},mount='8:0')
        self.assertEqual(result['io_bytes'],3)
    def test_partition_parent(self):
        result=measure({'8:0':dict(rbytes=1,wbytes=2),'8:16':dict(rbytes=9,wbytes=9)},mount='8:1',parent='8:0')
        self.assertEqual(result['io_device'],'8:0');self.assertEqual(result['io_device_selection'],'partition_parent')
    def test_explicit_override_is_recorded(self):
        result=measure({'8:0':dict(rbytes=1,wbytes=2),'8:16':dict(rbytes=9,wbytes=9)},mount='8:1',explicit='8:16')
        self.assertEqual(result['io_device'],'8:16');self.assertEqual(result['io_device_selection'],'explicit')
    def test_unidentified_single_device_is_not_guessed(self):
        with self.assertRaises(AssertionError):measure({'8:0':dict(rbytes=1,wbytes=2)})
    def test_missing_measurements_are_rejected(self):
        cases=[{}, {'252:0':dict(rbytes=0,wbytes=0)}, {'252:0':dict(rbytes=2,wbytes=0)}, {'252:0':dict(wbytes=2)}, {'252:0':dict(rbytes=2)}, {'252:0':dict(rbytes=-1,wbytes=2)}, {'252:0':dict(rbytes=1,wbytes=-2)}]
        for counters in cases:
            with self.subTest(counters=counters),self.assertRaises(AssertionError):measure(counters)
    def test_missing_explicit_device(self):
        with self.assertRaises(AssertionError):measure({'252:0':dict(rbytes=2,wbytes=2)},explicit='8:16')
    def test_zero_memory_measurement(self):
        with self.assertRaises(AssertionError):measure({'252:0':dict(rbytes=2,wbytes=2)},peak=0)
result=unittest.TextTestRunner(verbosity=1).run(unittest.defaultTestLoader.loadTestsFromTestCase(Model))
report=dict(passed=result.wasSuccessful(),test_groups=result.testsRun,failures=len(result.failures),errors=len(result.errors),scope='Pure cgroup device-selection model; no official performance score.')
if a.output:a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print(json.dumps(report));raise SystemExit(0 if report['passed'] else 1)

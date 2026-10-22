#!/usr/bin/env python3
import importlib.util
import tempfile
import sys
from pathlib import Path

sys.dont_write_bytecode = True

spec=importlib.util.spec_from_file_location('control',Path(__file__).with_name('analyze.py'))
gate=importlib.util.module_from_spec(spec); spec.loader.exec_module(gate)
with tempfile.TemporaryDirectory() as d:
    path=Path(d)/'stat'; fields=['S','1']+['0']*18
    fields[11:15]=['3','4','5','6']; fields[19]='123'
    path.write_text('2 (cake-adapt) '+' '.join(fields)+'\n')
    rows=gate.stats(path)
    assert rows[0]['own']==7 and rows[0]['waited']==11 and rows[0]['start']==123
    path.write_text('2 (other) '+' '.join(fields)+'\n')
    try: gate.stats(path)
    except AssertionError: pass
    else: raise AssertionError('wrong parent accepted')

def case(cpu):
    return {'phases':{phase:{'added_rtt_p50_ms':1,'added_rtt_p95_ms':2,'added_rtt_p99_ms':3,
                            'over15_pct':0,'over30_pct':0,'lost':0,'ul_received_mbit_s':5}
                     for phase in gate.PHASES},'cpu':{'one_core_pct':cpu},
            'health_observed':True,'health':{name:1 for name in gate.HEALTH}}
a=case(2); assert gate.compare({'fping':a,'native':case(1)})['native']['passed']
for metric,value in [('added_rtt_p95_ms',2.1),('ul_received_mbit_s',4.9)]:
    b=case(1); b['phases']['upload-steady'][metric]=value
    assert not gate.compare({'fping':a,'native':b})['native']['passed']
b=case(1); b['health_observed']=False
assert not gate.compare({'fping':a,'native':b})['native']['passed']
b=case(1); b['health']['loss_one_detect_s']=2.1
assert not gate.compare({'fping':a,'native':b})['native']['passed']
assert not gate.compare({'fping':a,'native':case(2)})['native']['passed']
print('scorer checks: stat fields, strict performance gate and health timing pass')

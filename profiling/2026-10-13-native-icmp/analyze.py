#!/usr/bin/env python3
"""Score complete control cases at recorded epoch boundaries, preserving failures."""
import argparse
import gzip
import json
import re
import statistics
from pathlib import Path

PHASES = ('upload-steady', 'upload-step', 'download-steady', 'bidirectional')
HEALTH = {'loss_one_detect_s':1.0, 'loss_all_stall_check_s':0.5,
          'loss_all_global_timeout_s':0.5, 'first_data_after_recovery_s':0.5}
PROBE = re.compile(r'^\[(\d+\.\d+)\] \S+\s+: \[\d+\], (?:\d+ bytes, ([\d.]+) ms|timed out)')

def text(path):
    if path.exists(): return path.read_text()
    with gzip.open(str(path)+'.gz', 'rt') as stream: return stream.read()

def percentile(values, fraction):
    values=sorted(values); assert values, 'empty phase'
    return values[min(len(values)-1,int(fraction*len(values)))]

def stats(path):
    rows=[]
    for line in text(path).splitlines():
        pid,rest=line.split(' (',1); name,fields=rest.rsplit(') ',1); f=fields.split()
        rows.append(dict(pid=int(pid),name=name,own=int(f[11])+int(f[12]),waited=int(f[13])+int(f[14]),start=int(f[19])))
    assert rows and rows[0]['name']=='cake-adapt', 'wrong parent'
    assert all(r['name']=='fping' for r in rows[1:]), 'unexpected child'
    return rows

def analyze(run, smoke=False):
    marks=[(float(t),n) for t,n in (line.split() for line in text(run/'phases-epoch').splitlines())]
    assert marks[-1][1]=='health-end', 'incomplete case'
    assert all(a[0]<=b[0] for a,b in zip(marks,marks[1:])), 'clock stepped'
    mark={n:t for t,n in marks if n!='idle'}
    assert text(run/'log-inode-before')==text(run/'log-inode-after'), 'log replaced'
    samples=[]
    for line in text(run/'probe').splitlines():
        m=PROBE.match(line)
        if m: samples.append((float(m[1]),float(m[2]) if m[2] else None))
    baseline=statistics.median(r for t,r in samples if mark['start']<=t<mark['upload-steady'] and r is not None)
    phases={}
    for i,(begin,name) in enumerate(marks):
        if name not in PHASES: continue
        end=next(t for t,n in marks[i+1:] if n in ('idle','end'))
        values=[r for t,r in samples if begin<=t<end]
        added=[round(r-baseline,6) for r in values if r is not None]
        assert len(added)>=(5 if smoke else 300), f'few samples: {name}'
        phases[name]={'samples':len(values),'lost':sum(r is None for r in values),
                      **{f'added_rtt_p{p}_ms':percentile(added,p/100) for p in (50,95,99)},
                      **{f'over{p}_pct':100*sum(r>p for r in added)/len(added) for p in (15,30)}}
    files={'upload-steady':[('upload-steady.json','ul')],'upload-step':[('upload-step.json','ul')],
           'download-steady':[('download-steady.json','dl')],
           'bidirectional':[('bidir-upload.json','ul'),('bidir-download.json','dl')]}
    for phase,pairs in files.items():
        for file,direction in pairs:
            data=json.loads(text(run/file)); assert 'error' not in data, file
            rate=data['end']['sum_received']['bits_per_second']/1e6; assert rate>0, file
            phases[phase][direction+'_received_mbit_s']=rate
    before,after=stats(run/'cpu-before.stat'),stats(run/'cpu-after.stat')
    assert (before[0]['pid'],before[0]['start'])==(after[0]['pid'],after[0]['start']), 'daemon changed'
    ticks=sum(r['own']+r['waited'] for r in after)-sum(r['own']+r['waited'] for r in before)
    seconds=float(text(run/'cpu-after.time'))-float(text(run/'cpu-before.time'))
    assert ticks>=0 and seconds>0
    expected=1 if run.name.startswith('native') else 2
    assert len(before)==len(after)==expected, 'wrong pinger child count'
    events=[]; data_times=[]; idle_owd=[]
    for line in text(run/'cake-adapt.log').splitlines():
        fields=[v.strip() for v in line.split(';')]
        if len(fields)<3: continue
        try: t=float(fields[2])
        except ValueError: continue
        if fields[0]=='DATA':
            data_times.append(t)
            if mark['start']<=t<mark['upload-steady']: idle_owd.append((int(fields[12])+int(fields[17]))/1000)
        events.append((t,line))
    def event(phrase,begin,end):
        return next((t-mark[begin] for t,line in events if mark[begin]<=t<mark[end] and phrase in line),None)
    health={'loss_one_detect_s':event('no ping response from reflector: 10.99.0.11','loss-one','recovery-one'),
            'loss_all_stall_check_s':event('no reflector response within:','loss-all','recovery-all'),
            'loss_all_global_timeout_s':event('global ping response timeout:','loss-all','recovery-all'),
            'first_data_after_recovery_s':next((t-mark['recovery-all'] for t in data_times if mark['recovery-all']<=t<mark['health-end']),None)}
    for file in ('loss-one-filter','loss-all-filter'):
        packets=re.findall(r'Sent \d+ bytes (\d+) pkt',text(run/file))
        assert packets and any(int(n)>0 for n in packets), f'fault not applied: {file}'
    return {'idle_rtt_ms':baseline,'idle_owd_sum_median_ms':statistics.median(idle_owd), 'phases':phases,
            'cpu':{'daemon_own_ticks':after[0]['own']-before[0]['own'],'daemon_plus_pinger_ticks':ticks,
                   'seconds':seconds,'one_core_pct':ticks/seconds,'before':before,'after':after},
            'health':health,'health_observed':all(v is not None for v in health.values())}

def compare(results):
    comparisons={}
    for ref,native in (('fping','native'),('fping-ts','native-ts')):
        if ref not in results or native not in results: continue
        a,b=results[ref],results[native]; regressions=[]
        for phase in PHASES:
            for metric in ('added_rtt_p50_ms','added_rtt_p95_ms','added_rtt_p99_ms','over15_pct','over30_pct','lost'):
                if b['phases'][phase][metric]>a['phases'][phase][metric]: regressions.append(f'{phase}: {metric}')
            for direction in ('ul','dl'):
                metric=direction+'_received_mbit_s'
                if metric in a['phases'][phase] and b['phases'][phase][metric]<a['phases'][phase][metric]: regressions.append(f'{phase}: {metric}')
        cpu=b['cpu']['one_core_pct']<a['cpu']['one_core_pct']
        observed=a['health_observed'] and b['health_observed']
        slower=[n for n,tolerance in HEALTH.items() if observed and b['health'][n]>a['health'][n]+tolerance]
        matched=observed and not slower
        comparisons[native]={'reference':ref,'reported_regressions':regressions,'cpu_lower':cpu,
                             'health_matched':matched,'slower_health':slower,'passed':not regressions and cpu and matched}
    return comparisons

if __name__=='__main__':
    p=argparse.ArgumentParser(); p.add_argument('root',type=Path); p.add_argument('--smoke',action='store_true'); p.add_argument('--pair',action='store_true'); p.add_argument('--output',type=Path)
    args=p.parse_args(); results={d.name:analyze(d,args.smoke) for d in sorted(args.root.iterdir()) if d.is_dir() and (d/'phases-epoch').exists()}
    assert results, 'no cases'
    if not args.smoke:
        required={'fping-ts','native-ts'} if args.pair else {'fping','native','fping-ts','native-ts'}
        assert set(results)==required, 'incomplete acceptance set'
    summary={'cases':results,'comparisons':compare(results)}; output=json.dumps(summary,indent=2)+'\n'
    if args.output: args.output.write_text(output)
    print(output)
    if not args.smoke and not all(c['passed'] for c in summary['comparisons'].values()): raise SystemExit(1)

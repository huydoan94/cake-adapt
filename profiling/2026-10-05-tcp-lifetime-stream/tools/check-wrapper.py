#!/usr/bin/env python3
"""Check wrapper cleanup with fake network commands and an isolated log fixture."""
from pathlib import Path
import os, subprocess, tempfile, shutil
repo=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='cake-lifetime-wrapper-') as temp:
    base=Path(temp); root=base/'bundle'; tool=root/'tools/tcp-lifetime'; tool.mkdir(parents=True)
    (root/'src').symlink_to(repo/'src',target_is_directory=True)
    for name in ('runner.c','log-shim.c','Makefile'):
        shutil.copyfile(repo/'tools/tcp-lifetime'/name,tool/name)
    release=base/'openwrt_release';release.write_text('host cleanup fixture\n')
    log=base/'test.log'; log.write_text('preserve this inode\n'); original_inode=log.stat().st_ino
    script=(repo/'tools/tcp-lifetime/run.sh').read_text().replace('/etc/openwrt_release',str(release)).replace('test_log=/tmp/sqm-mon-test.log', 'test_log='+str(log)).replace('/tmp/cake-adapt-tcp-lifetime-log.XXXXXX',str(base/'log.XXXXXX'))
    wrapper=tool/'run.sh';wrapper.write_text(script);wrapper.chmod(0o755)
    obj=base/'object.o';obj.write_text('host cleanup fixture, never loaded\n')
    runner=root/'build/tcp-lifetime/runner';runner.parent.mkdir(parents=True);runner.write_text('''#!/bin/sh
if [ "$1" = "--file-identity" ]; then
	python3 -c 'import os,sys; s=os.stat(sys.argv[1]); print(f"{s.st_dev}:{s.st_ino}")' "$2"
	exit $?
fi
exit 0
''');runner.chmod(0o755)
    mock=base/'mock';mock.mkdir()
    ip=mock/'ip';ip.write_text('''#!/bin/sh
case "$*" in
 "netns list") [ ! -s "$STATE" ] || cat "$STATE"; exit 0;;
 "netns add "*) printf '%s\\n' "$3" > "$STATE"; exit 0;;
 "netns del "*) : > "$STATE"; exit 0;;
 "netns exec "*) shift 3; exec "$@";;
 "-n "*" link add "*) [ "$FAIL_SETUP" != 1 ]; exit $?;;
esac
exit 0
''');ip.chmod(0o755)
    for name in ('tc','pidof','sysctl','apk'):
        p=mock/name;p.write_text('#!/bin/sh\nexit 0\n');p.chmod(0o755)
    state=base/'namespace';state.write_text('')
    env=dict(os.environ,PATH=str(mock)+':'+os.environ['PATH'],STATE=str(state),FAIL_SETUP='0')
    def run(name,fail=False):
        env['FAIL_SETUP']='1' if fail else '0'
        result=base/name
        completed=subprocess.run(['sh',str(wrapper),'--preflight',str(obj),str(result)],env=env,capture_output=True,text=True,timeout=20)
        assert log.stat().st_ino==original_inode
        assert not state.read_text(),completed.stderr
        assert not list(base.glob('log.*')),completed.stderr
        return completed,result
    alias=base/'alias';os.link(log,alias)
    with alias.open('a'):
        completed,result=run('writer')
        assert completed.returncode!=0 and 'writer' in completed.stderr,(completed.returncode,completed.stderr)
        assert log.read_text()=='preserve this inode\n'
        assert (result/'after.qdisc').exists()
        print('PASS hardlink writer refused before truncation; failure snapshots retained')
    completed,result=run('setup-failure',fail=True)
    assert completed.returncode!=0,(completed.returncode,completed.stderr)
    assert (result/'after.namespaces').exists()
    print('PASS failed veth setup removes owned namespace/log link; inode and snapshots retained')
    with log.open('r'):
        completed,result=run('reader')
        assert completed.returncode==0,(completed.returncode,completed.stderr,completed.stdout)
        assert (result/'after.process').exists()
        print('PASS read-only log holder allowed; successful cleanup preserves inode')
    runner.write_text('''#!/bin/sh
if [ "$1" = "--file-identity" ]; then
	python3 -c 'import os,sys; s=os.stat(sys.argv[1]); print(f"{s.st_dev}:{s.st_ino}")' "$2"
	exit $?
fi
rm -f "$FIXTURE_LOG"
exit 0
''')
    env['FIXTURE_LOG']=str(log)
    completed=subprocess.run(['sh',str(wrapper),'--preflight',str(obj),str(base/'missing-log')],env=env,capture_output=True,text=True,timeout=20)
    assert completed.returncode!=0 and 'disappeared' in completed.stderr,(completed.returncode,completed.stderr)
    assert not state.read_text()
    assert not list(base.glob('log.*'))
    print('PASS missing recorded log fails cleanup; owned namespace/link removed')

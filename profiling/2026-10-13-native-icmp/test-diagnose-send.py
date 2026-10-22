#!/usr/bin/env python3
"""Validate the diagnostic analyzer with synthetic captures and saved case data."""
import gzip
import importlib.util
from pathlib import Path
import shutil
import sys
import tempfile

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('diagnostic', Path(__file__).with_name('diagnose-send.py'))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

source = Path(sys.argv[1])
with tempfile.TemporaryDirectory(prefix='native-send-check-') as directory:
    case = Path(directory) / 'native-ts'
    shutil.copytree(source, case)
    with gzip.open(case / 'cake-adapt.log.gz', 'rt') as stream:
        original = stream.read()
    marks = [(float(t), name) for t, name in
             (line.split() for line in (case / 'phases-epoch').read_text().splitlines())]
    start_us = int(marks[0][0] * 1e6)
    end_us = int(next(t for t, name in marks if name == 'end') * 1e6)
    rows = []
    for serial, realtime_us in enumerate(range(start_us, end_us, 50000)):
        callback_us = 1000000 + serial * 50000
        rows.append(dict(serial=serial, target=serial % 6, deadline_us=callback_us-100,
                         callback_us=callback_us, realtime_us=realtime_us,
                         sent_us=callback_us+20, returned_us=callback_us+30,
                         originate_ms=realtime_us // 1000 % 86400000, failed=0))

    def write(base, captures):
        markers = ['NATIVE_SEND ' + ' '.join(f'{key}={value}' for key, value in row.items())
                   for row in captures]
        (case / 'cake-adapt.log').write_text(base + '\n' + '\n'.join(markers) + '\n')

    def rejects(phrase):
        try:
            module.diagnose(case)
        except AssertionError as error:
            assert phrase in str(error), str(error)
        else:
            raise AssertionError('invalid evidence accepted')

    write(original, rows)
    result = module.diagnose(case)
    for phase in result['send_diagnostic'].values():
        assert phase['callback_to_completed_send_upper_bound_age']['max_ms'] == 0.03
        assert phase['pre_send_to_return_interval']['max_ms'] == 0.01
    assert module.describe([0.5, 2.5, 8])['over_2.4_ms_pct'] == 200 / 3
    bad = [dict(row) for row in rows]
    bad[-1]['realtime_us'] = start_us - 500000
    bad[-1]['originate_ms'] = bad[-1]['realtime_us'] // 1000 % 86400000
    write(original, bad)
    rejects('capture clocks')
    lines = original.splitlines()
    index = next(i for i, line in enumerate(lines) if line.startswith('DATA;'))
    fields = lines[index].split(';')
    fields[2] = str(float(fields[8].strip().strip('[]')) - 1)
    lines[index] = ';'.join(fields)
    write('\n'.join(lines), rows)
    rejects('negative receive-to-DATA')
    write(original, rows[:-1] + [dict(rows[-1], returned_us=rows[-1]['sent_us']-1)])
    rejects('capture ordering')

print('send diagnostic checks pass: bounds/units, backwards clock outside window, negative delay, invalid return')

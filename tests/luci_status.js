'use strict';
/* Unit checks of the Status view's pure helpers; stock node only. */
const assert = require('assert');
const fs = require('fs');
const src = fs.readFileSync(process.argv[2], 'utf8');
const L = { hasViewPermission: () => true };
const fmt = (v) => String(v);
String.prototype.format = function(...a) {
	if (this.includes('%s'))
		return this.replace(/%s/g, () => String(a.shift()));
	return this.replace('%1000.2mbit/s', (a[0] / 1e6).toFixed(2) + ' Mbit/s').replace('%.1f', a[0].toFixed(1));
};
/* fs.exec answers with the next reply; ui keeps the notifications. */
const execs = [], notes = [];
let execReply = Promise.resolve({ code: 0 });
const fs_ = { exec: (cmd, args) => { execs.push([ cmd, ...args ]); return execReply; } };
const ui = { addNotification: (title, node) => notes.push(node) };
const E = (tag, attrs, text) => ({ tag, text: typeof attrs == 'string' ? attrs : text });
const v = new Function('view', 'rpc', 'L', '_', 'E', 'fs', 'ui', src)(
	{ extend: (o) => o }, { declare: () => () => {} }, L, fmt, E, fs_, ui);

const down = { available: true, shaper_bps: 90e6, delay_us: -500, load_e6: 500000, bufferbloat: false };
assert.strictEqual(v.cell(down, 'shaper_bps', (x) => v.rate(x)), '90.00 Mbit/s');
assert.strictEqual(v.cell(down, 'delay_us', (x) => v.delay(x)), '-0.5 ms');
assert.strictEqual(v.cell(down, 'tcp_queue_us', (x) => v.delay(x)), '–');
assert.strictEqual(v.cell({ available: false }, 'shaper_bps', (x) => v.rate(x)), '–');
assert.strictEqual(v.cell(undefined, 'load_e6', fmt), '–');
assert.strictEqual(v.sample(down, 'delay_us'), 0);
assert.strictEqual(v.sample({ available: false }, 'shaper_bps'), 0);
assert.strictEqual(v.sample(down, 'shaper_bps'), 90e6);
const s = [];
for (let i = 0; i < 5; i++)
	v.push(s, i, 3);
assert.deepStrictEqual(s, [ 2, 3, 4 ]);
assert.ok(v.scale(0) >= 1 && Number.isFinite(v.scale(0)));
assert.ok(v.scale(90e6) >= 90e6);
assert.ok(!/NaN/.test(v.points([ 0, 0 ], v.scale(0), 100, 298, 5)));
/* Newest at the right edge, a short series starts part-way across, no baseline closing. */
assert.strictEqual(v.points([ 1 ], 2, 100, 100, 5), '100,50');
assert.strictEqual(v.points([ 0, 1, 2 ], 2, 100, 100, 5), '90,100 95,50 100,0');
assert.strictEqual(v.points([], 2, 100, 100, 5), '');

/* updateChart on the real structure: frame -> svg -> elements by id. */
const els = {};
[ 'rx', 'tx', 'label_25', 'label_50', 'label_75' ].forEach((id) => {
	els[id] = { attrs: {}, style: {}, firstChild: { data: ' ' }, setAttribute(k, x) { this.attrs[k] = x; } };
});
const chart = { firstElementChild: { getElementById: (id) => els[id] } };
global.document = { querySelector: () => ({ offsetWidth: 302 }) };
v.updateChart(chart, [ [ 0, 1e6 ], [ 0, 0 ] ], (x) => v.rate(x));
/* Two samples of 150 at width 300: x = 300 - 300/149 and 300. */
assert.strictEqual(els.rx.attrs.points.split(' ').length, 2);
assert.ok(els.rx.attrs.points.endsWith(' 300,' + (298 - Math.floor(1e6 * 298 / v.scale(1e6)))));
assert.strictEqual(els.rx.attrs.points.split(',')[0], String(300 - 300 / 149));
assert.ok(els.tx.attrs.points.endsWith(' 300,298'));
assert.strictEqual(els.rx.style.fill, 'none');
assert.strictEqual(els.tx.style.fill, 'none');
assert.strictEqual(els.rx.style.strokeDasharray, 'none');
assert.ok(/\d/.test(els.tx.style.strokeDasharray));
assert.ok(els.label_50.firstChild.data.endsWith('Mbit/s'));
assert.ok(!/NaN/.test(els.rx.attrs.points));
assert.ok(!/undefined|Invalid/.test(v.stateText({})));

/* A zero start time is unavailable, not 1970. */
assert.ok(!/1970/.test(v.stateText({ started: 0, state: 'x' })));
assert.ok(v.stateText({ started: 0 }).includes('–'));
assert.ok(v.stateText({ started: -5 }).includes('–'));

/* Each instance's state comes from procd's record, not from the daemon's answers. */
const procd = (instances) => ({ 'cake-adapt': { instances } });
const inst = (reply) => v.instances(reply);
const running = { main: { running: true, pid: 4242 }, guest: { running: false } };
const status = { state: 'idle', started: 1700000000, version: '0.3.16', download: {}, upload: {} };
const state = (reply, name, enabled, st) => v.instanceState(inst(reply), name, enabled, st);

assert.strictEqual(v.instances(null), null);
assert.deepStrictEqual(v.instances({}), {}, 'procd lists no cake-adapt service');
assert.strictEqual(state(procd(running), 'main', true, status), 'running');
assert.strictEqual(state(procd(running), 'main', false, status), 'running', 'running although disabled since');
assert.strictEqual(state(procd(running), 'main', true, null), 'unanswered');
assert.strictEqual(state(procd(running), 'guest', true, null), 'stopped');
assert.strictEqual(state(procd(running), 'guest', false, null), 'disabled');
assert.strictEqual(state(procd(running), 'other', true, null), 'stopped');
assert.strictEqual(state({}, 'main', true, status), 'stopped', 'procd says no instance, whatever the daemon says');
assert.strictEqual(state(null, 'main', true, null), 'unknown');
assert.strictEqual(state(null, 'main', false, status), 'unknown');

/* The theme's alert colours: green, amber, red, grey; every state has a title and a class. */
assert.strictEqual(v.stateClass('running'), 'alert-message success');
assert.strictEqual(v.stateClass('unanswered'), 'alert-message warning');
assert.strictEqual(v.stateClass('stopped'), 'alert-message danger');
assert.strictEqual(v.stateClass('disabled'), 'alert-message notice');
assert.strictEqual(v.stateClass('unknown'), 'alert-message notice');
for (const st of [ 'running', 'unanswered', 'stopped', 'disabled', 'unknown' ])
	assert.ok(typeof v.stateTitle(st) == 'string' && v.stateTitle(st).length, st);
assert.ok(v.stateTitle('unanswered').includes('live status unavailable'));

/* The daemon's activity as a theme label. */
assert.strictEqual(v.activityClass('running'), 'label success');
assert.strictEqual(v.activityClass('idle'), 'label');
assert.strictEqual(v.activityClass('stall'), 'label warning');
assert.strictEqual(v.activityClass('something-new'), 'label');

/* The line under the state. */
const text = (st, s2) => v.instanceText(inst(procd(running)), 'main', st, s2);
assert.ok(text('running', status).startsWith('pid 4242. Started '));
assert.ok(text('running', status).includes('version 0.3.16'));
assert.ok(text('unanswered', null).startsWith('pid 4242;'));
assert.strictEqual(text('stopped', null), 'Not running.');
assert.ok(text('disabled', null).includes('disabled in the configuration'));
assert.ok(text('unknown', null).includes('procd'));

/* Buttons: Start only when it can start, Stop only when it runs; read-only offers none. */
const offered = (st, enabled, readonly) => v.actionsOffered(st, enabled, readonly || null);
assert.deepStrictEqual(offered('running', true), { start: false, stop: true, restart: true });
assert.deepStrictEqual(offered('unanswered', true), { start: false, stop: true, restart: true });
assert.deepStrictEqual(offered('running', false), { start: false, stop: true, restart: true }, 'a running instance disabled since can be stopped');
assert.deepStrictEqual(offered('stopped', true), { start: true, stop: false, restart: true });
assert.deepStrictEqual(offered('disabled', false), { start: false, stop: false, restart: false });
assert.deepStrictEqual(offered('unknown', true), { start: true, stop: true, restart: true });
assert.deepStrictEqual(offered('unknown', false), { start: false, stop: true, restart: false });
for (const st of [ 'running', 'unanswered', 'stopped', 'disabled', 'unknown' ])
	for (const enabled of [ true, false ])
		assert.deepStrictEqual(offered(st, enabled, true), { start: false, stop: false, restart: false }, `read-only ${st}`);

/* After a start or restart that exited 0, procd's record tells what did not happen. */
const up = (pid) => ({ running: true, pid });
assert.strictEqual(v.actionProblem('main', 'restart', up(1), up(2)), null, 'a new pid: restarted');
assert.ok(v.actionProblem('main', 'restart', up(1), up(1)).includes('keeps running its previous configuration'));
assert.ok(v.actionProblem('main', 'restart', up(1), up(1)).includes('logread -e cake-adapt'));
assert.ok(v.actionProblem('main', 'restart', up(1), undefined).includes('did not start'));
assert.ok(v.actionProblem('main', 'restart', up(1), { running: false }).includes('did not start'));
assert.strictEqual(v.actionProblem('main', 'restart', undefined, up(3)), null, 'restart of a stopped instance started it');
assert.strictEqual(v.actionProblem('main', 'restart', null, up(3)), null, 'procd was silent before');
assert.strictEqual(v.actionProblem('main', 'start', undefined, up(3)), null);
assert.ok(v.actionProblem('main', 'start', undefined, undefined).includes('Instance main did not start'));
assert.ok(v.actionProblem('main', 'start', undefined, { running: false }).includes('did not start'));
assert.strictEqual(v.actionProblem('main', 'stop', up(1), undefined), null);
assert.strictEqual(v.actionProblem('main', 'stop', up(1), up(1)), null, 'stop is judged by the state shown');

/* An action runs the init script for that instance only and reports a failure. */
const actions = v.handleAction('main', 'restart').then((done) => {
	assert.strictEqual(done, true, 'exit 0');
	assert.deepStrictEqual(execs.pop(), [ '/etc/init.d/cake-adapt', 'restart', 'main' ]);
	assert.strictEqual(notes.length, 0);
	execReply = Promise.resolve({ code: 1, stderr: 'refused\n' });
	return v.handleAction('guest', 'start');
}).then((done) => {
	assert.strictEqual(done, false, 'a failed exit');
	assert.deepStrictEqual(execs.pop(), [ '/etc/init.d/cake-adapt', 'start', 'guest' ]);
	assert.ok(notes.pop().text.includes('refused'));
	execReply = Promise.reject(new Error('Permission denied'));
	return v.handleAction('guest', 'stop');
}).then((done) => {
	assert.strictEqual(done, false, 'a refused exec');
	assert.ok(notes.pop().text.includes('Permission denied'));
});

/* A section is enabled as the daemon reads the flag. */
for (const value of [ '1', 'true', 'TRUE', 'yes', 'on', 'On', 'enabled' ])
	assert.strictEqual(v.isEnabled(value), true, value);
for (const value of [ '0', 'false', 'no', 'off', '', undefined, null ])
	assert.strictEqual(v.isEnabled(value), false, String(value));
assert.strictEqual(v.serviceRunning, undefined, 'no state is derived from ubus status answers');
assert.strictEqual(v.anyRunning, undefined, 'no service-wide state or row');
actions.then(() => console.log('luci_status.js passed')).catch((e) => { console.error(e); process.exit(1); });

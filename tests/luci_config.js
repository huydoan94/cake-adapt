'use strict';
/*
 * Checks of the Configuration view's rules; stock node only. Each rule of the
 * daemon's configuration checks (src/config/config.c load-time checks and
 * src/config/validate.c) has a passing and a failing case.
 */
const assert = require('assert');
const fs = require('fs');
/*
 * argv[3] is the installed package's features file ('' for cake-adapt, 'mqtt'
 * for cake-adapt-mqtt, 'mqtt tls' for cake-adapt-mqtt-ssl); argv[4] 'stored'
 * stores an instance with MQTT on.
 */
const featureText = process.argv[3] ?? '';
const mqttStored = process.argv[4] === 'stored';
const src = fs.readFileSync(process.argv[2], 'utf8');

String.prototype.format = function(...a) {
	return this.replace(/%[sd]/g, () => String(a.shift()));
};

/* Just enough of form.* to build the view and see which options it declares. */
class Option {
	constructor(section, option, title, description) {
		Object.assign(this, { section, option, title, description, checks: 0, loaded: {}, parsed: [] });
	}
	triggerValidation() { this.checks++; }
	cfgvalue(sid, value) { this.loaded[sid] = value; }
	parse(sid) {
		this.parsed.push(sid);
		if (this.persisted) {
			const deps = typeof this.dependencies?.[0] === 'string' ?
				{ [this.dependencies[0]]: this.dependencies[1] } : (this.dependencies?.[0] ?? {});
			const active = Object.entries(deps).every(([ name, value ]) =>
				(screen[name] ?? this.persisted[name]) === value);
			/* Stock form.js AbstractValue and Flag remove inactive values unless retain is set. */
			if (active)
				this.persisted[this.option] = screen[this.option] ?? this.persisted[this.option];
			else if (!this.retain)
				delete this.persisted[this.option];
		}
		return Promise.resolve();
	}
	depends(...values) { this.dependencies = values; }
}
class Value extends Option {}
class Flag extends Value { constructor(...a) { super(...a); this.enabled = '1'; } }
class DynamicList extends Value {}
class DummyValue extends Option {}
/* Every section the view creates, the outer one first; what is on screen, by option name. */
const sections = [];
let screen = {}, nodeHandlers;
class Section {
	constructor(...args) { this.args = args; this.children = []; this.tabs = []; sections.push(this); }
	static extend(props) {
		const extended = class extends this {};
		Object.assign(extended.prototype, props);
		return extended;
	}
	tab(name) { this.tabs.push(name); }
	taboption(tab, cls, name, ...rest) {
		const option = new cls(this, name, ...rest);
		option.tab = tab;
		this.children.push(option);
		return option;
	}
	/* As LuCI: each section answers for its own options only. */
	formvalue(sid, name) {
		if (name !== undefined)
			return this.children.some((o) => o.option == name) ? screen[name] : null;
		return Object.fromEntries(this.children.map((o) => [ o.option, screen[o.option] ]));
	}
	cfgsections() { return [ 'main', 'guest' ]; }
	loadChildren(sid) { return Promise.resolve(this.children.map((o) => `${sid}:${o.option}`)); }
	renderUCISection(sid) { return Promise.resolve([ `tabs of ${sid}` ]); }
}
class SectionValue extends Option {
	constructor(section, option, cls, ...args) {
		super(section, option);
		this.subsection = new cls(null, ...args);
		this.subsection.parentoption = this;
	}
}
class Map {
	section(cls, ...args) { return new cls(...args); }
	render() {
		nodeHandlers = {};
		return Promise.resolve({ addEventListener: (type, fn) => { nodeHandlers[type] = fn; } });
	}
}
/* As form.js: a NamedSection lists only its own (fixed) section, null when none was given. */
class NamedSection extends Section {
	constructor(map, sid, ...args) { super(map, sid, ...args); this.section = sid; }
	cfgsections() { return [ this.section ]; }
}
const form = { Map, TypedSection: Section, NamedSection, SectionValue, Value, Flag, DynamicList, DummyValue };
global.E = (tag, attrs, nodes) => ({ tag, attrs, nodes });
/* Nothing is kept between two calls here, as when a moment passes between them. */
const realSetTimeout = global.setTimeout;
global.setTimeout = (fn) => fn();
const stored = {};
const uciSections = [ { '.name': 'main' }, { '.name': 'guest', mqtt_enabled: mqttStored ? '1' : '0' } ];
const files = { '/usr/share/cake-adapt/features': featureText };
const fsStub = {
	read: (path) => (path in files) ? Promise.resolve(files[path]) : Promise.reject(new Error('NotFoundError'))
};
const uciStub = {
	load: (config) => Promise.resolve(config),
	get: (config, sid, name) => stored[name],
	sections: (config, type) => (config == 'cake-adapt' && type == 'cake_adapt') ? uciSections : []
};
global.L = { resolveDefault: (promise, fallback) => promise.catch(() => fallback) };
const view = new Function('view', 'form', 'fs', 'uci', '_', src)(
	{ extend: (o) => o }, form, fsStub, uciStub, (s) => s);
const { instanceErrors: errorsWith, reflectorError, reflectorTooLong, latencyChecked, defaults, scales, parseFeatures } = view;
const features = parseFeatures(featureText);
const instanceErrors = (screen) => errorsWith(screen, features);

/* The features file: its words, and nothing when it is missing or empty. */
assert.deepStrictEqual(parseFeatures(''), { mqtt: false, tls: false });
assert.deepStrictEqual(parseFeatures(undefined), { mqtt: false, tls: false });
assert.deepStrictEqual(parseFeatures('mqtt\n'), { mqtt: true, tls: false });
assert.deepStrictEqual(parseFeatures('mqtt\ntls\n'), { mqtt: true, tls: true });
const loaded = view.load().then((data) => {
	assert.deepStrictEqual(parseFeatures(data[1]), features);
	delete files['/usr/share/cake-adapt/features'];
	return view.load();
}).then((data) => {
	assert.strictEqual(data[1], '', 'a missing features file reads as nothing');
	files['/usr/share/cake-adapt/features'] = featureText;
});

/* A valid, running configuration; values are in the form's units. */
const good = {
	enabled: '1', interface: 'eth1', adjust_dl_shaper_rate: '1', adjust_ul_shaper_rate: '1',
	min_dl_shaper_rate_kbps: '10000', base_dl_shaper_rate_kbps: '20000', max_dl_shaper_rate_kbps: '50000',
	min_ul_shaper_rate_kbps: '10000', base_ul_shaper_rate_kbps: '20000', max_ul_shaper_rate_kbps: '50000',
	reflectors: [ '1.1.1.1', '1.0.0.1', '8.8.8.8', '8.8.4.4', '9.9.9.9', '9.9.9.10' ]
};
const errors = (changes) => {
	const values = Object.assign({}, good, changes);
	return instanceErrors((name) => values[name]);
};
const ok = (changes, what) => assert.deepStrictEqual(errors(changes), [], what);
/* The option shows the error, with this text. */
const fails = (changes, option, text) => {
	const found = errors(changes).filter((e) => e.option == option);
	assert.ok(found.length, `${option} should fail for ${JSON.stringify(changes)}`);
	assert.ok(found[0].message.includes(text), `${option}: "${found[0].message}" lacks "${text}"`);
};

ok({ mqtt_enabled: '0', mqtt_host: '', mqtt_topic: '#', mqtt_port: '0' });
ok({ mqtt_enabled: '0', mqtt_tls: '1', mqtt_ca_file: 'relative.pem' });
if (!features.mqtt) {
	/* cake-adapt only warns about MQTT (the daemon runs), checks no broker setting, and refuses TLS. */
	ok({ mqtt_enabled: '1', mqtt_host: 'broker.example' });
	ok({ mqtt_enabled: '1', mqtt_host: 'mqtt://broker', mqtt_topic: '#' });
	fails({ mqtt_enabled: '1', mqtt_tls: '1' }, 'mqtt_tls', 'cake-adapt-mqtt-ssl package');
	fails({ mqtt_enabled: '1', mqtt_tls: '1' }, 'mqtt_enabled', 'cake-adapt-mqtt-ssl package');
} else {
	fails({ mqtt_enabled: '1' }, 'mqtt_host', 'hostname or IP');
	ok({ mqtt_enabled: '1', mqtt_host: 'broker.example' });
	ok({ mqtt_enabled: '1', mqtt_host: '2001:db8::1', mqtt_port: '65535' });
	fails({ mqtt_enabled: '1', mqtt_host: 'mqtt://broker' }, 'mqtt_host', 'hostname or IP');
	fails({ mqtt_enabled: '1', mqtt_port: '0' }, 'mqtt_port', 'between 1 and 65535');
	fails({ mqtt_enabled: '1', mqtt_port: '65536' }, 'mqtt_port', 'between 1 and 65535');
	fails({ mqtt_enabled: '1', mqtt_port: '1.5' }, 'mqtt_port', 'more precision');
	fails({ mqtt_enabled: '1', mqtt_topic: 'logs/+' }, 'mqtt_topic', 'no + or #');
	fails({ mqtt_enabled: '1', mqtt_topic: 'logs/#' }, 'mqtt_topic', 'no + or #');
	fails({ mqtt_enabled: '1', mqtt_password: 'test' }, 'mqtt_password', 'needs mqtt_username');
	ok({ mqtt_enabled: '1', mqtt_host: 'broker.example', mqtt_username: 'user', mqtt_password: 'test' });
	ok({ mqtt_enabled: '1', mqtt_host: 'broker.example', mqtt_tls: '0', mqtt_ca_file: 'relative.pem' });
}
fails({ mqtt_username: 'x'.repeat(512) }, 'mqtt_username', 'too long');
if (features.tls) {
	ok({ mqtt_enabled: '1', mqtt_host: 'broker.example', mqtt_tls: '1' });
	ok({ mqtt_enabled: '1', mqtt_host: 'broker.example', mqtt_tls: '1', mqtt_ca_file: '/etc/ssl/private-ca.pem' });
	fails({ mqtt_enabled: '1', mqtt_tls: '1', mqtt_ca_file: 'relative.pem' }, 'mqtt_ca_file', 'absolute path');
	/* Empty form values use the daemon's CA bundle default. */
	ok({ mqtt_enabled: '1', mqtt_host: 'broker.example', mqtt_tls: '1', mqtt_ca_file: '' });
} else if (features.mqtt) {
	fails({ mqtt_enabled: '1', mqtt_tls: '1' }, 'mqtt_tls', 'cake-adapt-mqtt-ssl package');
	fails({ mqtt_enabled: '1', mqtt_tls: '1' }, 'mqtt_enabled', 'cake-adapt-mqtt-ssl package');
}

ok({}, 'the reference configuration is clean');

/* config.c: text too long (a size of IF_NAMESIZE or more). */
ok({ interface: 'a'.repeat(15) });
fails({ interface: 'a'.repeat(16) }, 'interface', 'is too long');
fails({ ul_if: 'a'.repeat(16), dl_if: 'b' }, 'ul_if', 'is too long');
ok({ ping_prefix_string: 'x'.repeat(511) });
fails({ ping_prefix_string: 'x'.repeat(512) }, 'ping_prefix_string', 'is too long');
fails({ config_file: 'é'.repeat(256) }, 'config_file', 'is too long');

/* config.c parse_scaled_decimal: a non-negative decimal, enough precision, not too large. */
for (const bad of [ 'abc', '5.', '.5', '-1', '1e3', ' 5', '5 ', '1,5' ])
	fails({ if_up_check_interval_s: bad }, 'if_up_check_interval_s', 'is not a non-negative decimal');
ok({ if_up_check_interval_s: '0.000001' });
fails({ if_up_check_interval_s: '0.0000001' }, 'if_up_check_interval_s', 'more precision');
ok({ if_up_check_interval_s: '1.0000000' });
fails({ log_file_max_size_KB: '1.5' }, 'log_file_max_size_KB', 'more precision');
ok({ log_file_max_size_KB: '1.0' });
fails({ log_file_max_time_mins: '0.00000001' }, 'log_file_max_time_mins', 'more precision');
ok({ log_file_max_time_mins: '0.5' });
fails({ no_pingers: '6.5' }, 'no_pingers', 'more precision');
fails({ log_file_buffer_timeout_ms: '18446744073709552' }, 'log_file_buffer_timeout_ms', 'is too large');
assert.ok(!errors({ log_file_buffer_timeout_ms: '18446744073709551' }).some((e) => e.message.includes('is too large')));
fails({ log_file_max_size_KB: '99999999999999999999999' }, 'log_file_max_size_KB', 'is too large');

/* config.c: reflector count and size. */
fails({ reflectors: Array.from({ length: 65 }, (_, i) => `h${i}.example`) }, 'reflectors', 'more than 64 entries');
ok({ reflectors: Array.from({ length: 64 }, (_, i) => `h${i}.example`), no_pingers: '6' });
fails({ reflectors: [ 'a'.repeat(256) ] , no_pingers: '1' }, 'reflectors', 'is too long');

/* config.c resolve_interfaces: a pair is both or neither. */
fails({ interface: '', ul_if: 'eth1' }, 'dl_if', 'configured together');
fails({ interface: '', dl_if: 'ifb4eth1' }, 'ul_if', 'configured together');
ok({ interface: '', ul_if: 'eth1', dl_if: 'ifb4eth1' }, 'a complete pair replaces interface');
ok({ interface: 'eth1', ul_if: 'eth2', dl_if: 'ifb4eth2' });

/* validate.c config_validate: an interface when enabled or adjusting. */
fails({ interface: '' }, 'interface', "set either option 'interface'");
fails({ interface: '', enabled: '0' }, 'interface', "set either option 'interface'");
fails({ interface: '', enabled: '0', adjust_dl_shaper_rate: '0' }, 'interface', "set either option 'interface'");
ok({ interface: '', enabled: '0', adjust_dl_shaper_rate: '0', adjust_ul_shaper_rate: '0' });

/* validate.c validate_rate_range, with the defaults for what is empty. */
for (const [ d, label ] of [ [ 'dl', 'download' ], [ 'ul', 'upload' ] ]) {
	const min = `min_${d}_shaper_rate_kbps`, base = `base_${d}_shaper_rate_kbps`, max = `max_${d}_shaper_rate_kbps`;

	fails({ [min]: '30000' }, min, `${label} rates must satisfy minimum <= base <= maximum`);
	fails({ [min]: '30000' }, max, 'minimum <= base <= maximum');
	fails({ [max]: '15000' }, base, 'minimum <= base <= maximum');
	ok({ [min]: '20000', [base]: '20000', [max]: '20000' }, 'equal rates are allowed');
	fails({ [min]: '0' }, min, `${label} min/base/max rates are required together`);
	fails({ [base]: '0' }, max, 'required together');
	fails({ [min]: '10.001', [base]: '10.001', [max]: '10.001' }, base, 'whole bytes/s (multiples of 8 bit/s)');
	ok({ enable_sleep_function: '0', [min]: '10.008', [base]: '10.008', [max]: '10.008' });
}
/* With adjustment off the rates may be left all zero; any other case is checked. */
ok({ enable_sleep_function: '0', adjust_dl_shaper_rate: '0', min_dl_shaper_rate_kbps: '0', base_dl_shaper_rate_kbps: '0', max_dl_shaper_rate_kbps: '0' });
fails({ adjust_dl_shaper_rate: '1', min_dl_shaper_rate_kbps: '0', base_dl_shaper_rate_kbps: '0', max_dl_shaper_rate_kbps: '0' },
	'min_dl_shaper_rate_kbps', 'required together');
fails({ adjust_dl_shaper_rate: '0', min_dl_shaper_rate_kbps: '0', base_dl_shaper_rate_kbps: '1000', max_dl_shaper_rate_kbps: '0' },
	'min_dl_shaper_rate_kbps', 'required together');
/* Empty rates are the daemon defaults: 5/20/80 down, 5/20/35 up. */
ok({ min_dl_shaper_rate_kbps: '', base_dl_shaper_rate_kbps: '', max_dl_shaper_rate_kbps: '', min_ul_shaper_rate_kbps: '', base_ul_shaper_rate_kbps: '', max_ul_shaper_rate_kbps: '' });
fails({ min_ul_shaper_rate_kbps: '', base_ul_shaper_rate_kbps: '', max_ul_shaper_rate_kbps: '15000' }, 'base_ul_shaper_rate_kbps', 'minimum <= base <= maximum');

/* validate.c validate_latency_config: the latency checks run only for an enabled or adjusting instance. */
const idle = { enabled: '0', adjust_dl_shaper_rate: '0', adjust_ul_shaper_rate: '0' };
ok(Object.assign({ pinger_method: 'ping', reflectors: [], monitor_achieved_rates_interval_ms: '0' }, idle));
ok(Object.assign({ pinger_method: 'ping' }, idle), 'nothing enabled, nothing checked');
for (const [ flag, value ] of [ [ 'enabled', '1' ], [ 'adjust_dl_shaper_rate', '1' ], [ 'adjust_ul_shaper_rate', '1' ] ])
	fails(Object.assign({ pinger_method: 'ping' }, idle, { [flag]: value }), 'pinger_method', "must be 'fping', 'fping-ts' or 'irtt'");

/* validate.c validate_pinger. */
ok({ pinger_method: 'fping-ts' });
ok({ pinger_method: 'irtt', irtt_session_duration_m: '5' });
fails({ pinger_method: 'irtt', irtt_session_duration_m: '0' }, 'irtt_session_duration_m', 'must be positive for irtt');
ok({ pinger_method: 'fping', irtt_session_duration_m: '0' });
fails({ reflectors: [] }, 'reflectors', 'at least one reflector is required');
fails({ reflectors: undefined }, 'reflectors', 'at least one reflector is required');
fails({ no_pingers: '7' }, 'no_pingers', 'between 1 and the reflector count');
fails({ no_pingers: '0' }, 'no_pingers', 'between 1 and the reflector count');
/* An empty no_pingers is the default 6, which three reflectors cannot supply. */
fails({ no_pingers: '', reflectors: [ 'a.example', 'b.example', 'c.example' ] }, 'no_pingers', 'between 1 and the reflector count');
ok({ no_pingers: '', reflectors: good.reflectors.concat('9.9.9.11') });
ok({ no_pingers: '3', reflectors: [ 'a.example', 'b.example', 'c.example' ] });
fails({ reflectors: [ '-bad', '1.1.1.1', '1.0.0.1', '8.8.8.8', '8.8.4.4', '9.9.9.9' ] }, 'reflectors', "invalid reflector '-bad'");
fails({ reflectors: [ 'a b', '1.1.1.1', '1.0.0.1', '8.8.8.8', '8.8.4.4', '9.9.9.9' ] }, 'reflectors', 'invalid reflector');
ok({ reflectors: [ ':1', 'a_b.example', '1.1.1.1', '1.0.0.1', '8.8.8.8', '8.8.4.4' ] });
fails({ reflectors: [ '1.1.1.1', '1.0.0.1', '8.8.8.8', '8.8.4.4', '9.9.9.9', '1.1.1.1' ] }, 'reflectors', "duplicate reflector '1.1.1.1'");
fails({ pinger_method: 'fping-ts', reflectors: [ '1.1.1.1', '1.0.0.1', '8.8.8.8', '8.8.4.4', '9.9.9.9', '2606:4700::1111' ] },
	'reflectors', 'IPv4-only ICMP timestamps; reflector \'2606:4700::1111\' is IPv6');
ok({ pinger_method: 'fping', reflectors: [ '1.1.1.1', '1.0.0.1', '8.8.8.8', '8.8.4.4', '9.9.9.9', '2606:4700::1111' ] });
ok({ tcp_delay_attribution: '1' });
fails({ tcp_delay_attribution: '1', pinger_method: 'irtt' }, 'tcp_delay_attribution', "needs pinger_method 'fping'");
fails({ tcp_delay_attribution: '1', pinger_method: 'fping-ts' }, 'pinger_method', "needs pinger_method 'fping'");
ok({ tcp_delay_attribution: '1', tcp_ts_request: '1' });
fails({ tcp_ts_request: '1' }, 'tcp_ts_request', 'needs tcp_delay_attribution');
ok({ reflector_ping_interval_s: '0.006', no_pingers: '6' });
fails({ reflector_ping_interval_s: '0.005999', no_pingers: '6' }, 'reflector_ping_interval_s', 'at least 1 ms per active reflector');
fails({ reflector_ping_interval_s: '0.005999', no_pingers: '6' }, 'no_pingers', 'at least 1 ms per active reflector');
fails({ reflector_ping_interval_s: '0' }, 'reflector_ping_interval_s', 'at least 1 ms');

/* validate.c validate_detection. */
ok({ monitor_achieved_rates_interval_ms: '1' });
fails({ monitor_achieved_rates_interval_ms: '0' }, 'monitor_achieved_rates_interval_ms', 'positive whole number of milliseconds');
fails({ monitor_achieved_rates_interval_ms: '0.5' }, 'monitor_achieved_rates_interval_ms', 'whole number of milliseconds');
ok({ monitor_achieved_rates_interval_ms: '4294967295' });
fails({ monitor_achieved_rates_interval_ms: '4294967296' }, 'monitor_achieved_rates_interval_ms', 'no greater than 4294967295');
ok({ bufferbloat_detection_window: '1', bufferbloat_detection_thr: '1' });
fails({ bufferbloat_detection_window: '0', bufferbloat_detection_thr: '0' }, 'bufferbloat_detection_window', 'between 1 and 4294967295');
fails({ bufferbloat_detection_window: '4294967296' }, 'bufferbloat_detection_window', 'between 1 and');
fails({ bufferbloat_detection_window: '6', bufferbloat_detection_thr: '7' }, 'bufferbloat_detection_thr', 'cannot be greater than');
/* An empty threshold is 3, an empty window 6. */
fails({ bufferbloat_detection_window: '2' }, 'bufferbloat_detection_thr', 'cannot be greater than');
ok({ ul_congest_ack_share: '1' });
ok({ ul_congest_ack_share: '0.25' });
fails({ ul_congest_ack_share: '1.000001' }, 'ul_congest_ack_share', 'between 0 and 1');
for (const alpha of [ 'alpha_baseline_increase', 'alpha_baseline_decrease', 'alpha_delta_ewma' ]) {
	ok({ [alpha]: '1' });
	fails({ [alpha]: '1.000001' }, alpha, 'alpha options must be between 0 and 1');
}

/* validate.c validate_reflector_policy. */
fails({ reflector_health_check_interval_s: '0' }, 'reflector_health_check_interval_s', 'must be positive');
fails({ reflector_response_deadline_s: '0' }, 'reflector_response_deadline_s', 'must be positive');
ok({ reflector_health_check_interval_s: '0.001' });
fails({ reflector_health_check_interval_s: '0.0005' }, 'reflector_health_check_interval_s', 'whole number of milliseconds');
fails({ reflector_health_check_interval_s: '4294968' }, 'reflector_health_check_interval_s', 'no greater than');
ok({ reflector_misbehaving_detection_window: '5', reflector_misbehaving_detection_thr: '5' });
fails({ reflector_misbehaving_detection_window: '0', reflector_misbehaving_detection_thr: '0' }, 'reflector_misbehaving_detection_window', 'offence threshold');
fails({ reflector_misbehaving_detection_thr: '0' }, 'reflector_misbehaving_detection_thr', 'between 1 and its window');
fails({ reflector_misbehaving_detection_thr: '61' }, 'reflector_misbehaving_detection_thr', 'between 1 and its window');
fails({ reflector_misbehaving_detection_window: '4294967296', reflector_misbehaving_detection_thr: '3' }, 'reflector_misbehaving_detection_window', 'offence threshold');
ok({ stall_detection_thr: '1' });
fails({ stall_detection_thr: '0' }, 'stall_detection_thr', 'positive and representable');
/* 300 ms / 6 = 50 ms; 2^64 / 50000 us is the largest representable count. */
ok({ stall_detection_thr: '368934881474191' });
fails({ stall_detection_thr: '368934881474192' }, 'stall_detection_thr', 'positive and representable');
fails({ global_ping_response_timeout_s: '0' }, 'global_ping_response_timeout_s', 'positive and representable');
ok({ global_ping_response_timeout_s: '0.000001' });
fails({ if_up_check_interval_s: '0' }, 'if_up_check_interval_s', 'positive and representable');

/* validate.c validate_monitoring. */
ok({ monitor_cpu_usage_interval_ms: '0' }, 'CPU interval is not checked without CPU records');
fails({ output_cpu_stats: '1', monitor_cpu_usage_interval_ms: '0' }, 'monitor_cpu_usage_interval_ms', 'CPU monitoring interval');
fails({ output_cpu_raw_stats: '1', monitor_cpu_usage_interval_ms: '0.5' }, 'monitor_cpu_usage_interval_ms', 'CPU monitoring interval');
ok({ output_cpu_stats: '1', monitor_cpu_usage_interval_ms: '2000' });
ok({ log_file_buffer_timeout_ms: '4294967295' });
fails({ log_file_buffer_timeout_ms: '4294967296' }, 'log_file_buffer_timeout_ms', 'intervals are too large');
ok({ reflector_ping_interval_s: '9223372036854', no_pingers: '6' });
fails({ reflector_ping_interval_s: '9223372036855', no_pingers: '6' }, 'reflector_ping_interval_s', 'intervals are too large');
/* Sleeping with an active threshold above either minimum rate. */
ok({ enable_sleep_function: '1', connection_active_thr_kbps: '10000' });
fails({ enable_sleep_function: '1', connection_active_thr_kbps: '10001' }, 'connection_active_thr_kbps', 'cannot exceed either minimum shaper rate');
fails({ enable_sleep_function: '1', connection_active_thr_kbps: '10001' }, 'min_dl_shaper_rate_kbps', 'cannot exceed');
fails({ enable_sleep_function: '1', connection_active_thr_kbps: '5001', min_ul_shaper_rate_kbps: '5000', base_ul_shaper_rate_kbps: '20000' }, 'min_ul_shaper_rate_kbps', 'cannot exceed');
ok({ enable_sleep_function: '0', connection_active_thr_kbps: '99999' });
/* An empty threshold is 2000 kbit/s; the flag empty is its default, on. */
fails({ enable_sleep_function: '', connection_active_thr_kbps: '', min_dl_shaper_rate_kbps: '1000', base_dl_shaper_rate_kbps: '1000', max_dl_shaper_rate_kbps: '1000' }, 'connection_active_thr_kbps', 'cannot exceed');

/* config.c absolute_log_directory: '/' is put before a relative directory; the result must fit 512 bytes. */
ok({ log_file_path_override: 'a'.repeat(510) });
fails({ log_file_path_override: 'a'.repeat(511) }, 'log_file_path_override', 'File name too long');
ok({ log_file_path_override: '/' + 'a'.repeat(510) });
fails({ log_file_path_override: '/' + 'a'.repeat(511) }, 'log_file_path_override', 'is too long');
fails({ log_file_path_override: 'é'.repeat(255) + 'a' }, 'log_file_path_override', 'File name too long');
fails({ log_file_path_override: 'a'.repeat(511), enabled: '0', adjust_dl_shaper_rate: '0', adjust_ul_shaper_rate: '0' }, 'log_file_path_override', 'File name too long');

/* config.c load_section: a legacy latency_target stands for the reflector list (and one pinger). */
{
	const legacy = (changes, target) => instanceErrors((name) => Object.assign({}, good, { reflectors: [], latency_target: target }, changes)[name]);

	assert.deepStrictEqual(legacy({}, '1.1.1.1'), [], 'no_pingers unset: one pinger');
	assert.deepStrictEqual(legacy({ no_pingers: '1' }, '1.1.1.1'), []);
	assert.ok(legacy({ no_pingers: '2' }, '1.1.1.1').some((e) => e.option == 'no_pingers'), 'no_pingers set stays as set');
	assert.ok(legacy({}, '').some((e) => e.message.includes('at least one reflector')), 'no target, no reflectors');
	assert.ok(legacy({}, '-bad').some((e) => e.message.includes("invalid reflector '-bad'")));
	/* reflectors take precedence over the target. */
	assert.deepStrictEqual(instanceErrors((name) => Object.assign({}, good, { latency_target: '-bad' })[name]), []);
}

/* Byte lengths, as strlen: a reflector or text of multibyte characters. */
assert.ok(reflectorTooLong('é'.repeat(128)) !== true, '256 bytes');
assert.strictEqual(reflectorTooLong('é'.repeat(127)), true);
fails({ reflectors: [ 'é'.repeat(128) ], no_pingers: '1' }, 'reflectors', 'is too long');

/* The typed entry: duplicates of the listed ones, and only the loader's size limit when nothing runs. */
assert.ok(reflectorError('1.1.1.1', [ '1.1.1.1' ]).includes('duplicate'));
assert.strictEqual(latencyChecked((name) => ({ enabled: '1' })[name]), true);
assert.strictEqual(latencyChecked((name) => ({})[name]), false);
assert.strictEqual(latencyChecked((name) => ({ adjust_ul_shaper_rate: '1' })[name]), true);

/* A reflector typed on its own (the entry being added). */
assert.strictEqual(reflectorError('9.9.9.11', good.reflectors), true);
assert.ok(reflectorError('1.1.1.1', good.reflectors).includes('duplicate'));
assert.ok(reflectorError('bad host', []).includes('invalid reflector'));
assert.ok(reflectorError('a'.repeat(256), []).includes('too long'));

/* The table of defaults: every numeric option has one, and a default is itself valid. */
for (const name of Object.keys(scales))
	assert.ok(defaults[name] != null, `${name} has a default`);
assert.deepStrictEqual(instanceErrors((name) => ({
	enabled: '1', interface: 'eth1', reflectors: good.reflectors
})[name]), [], 'every default at once is valid');

/* Drift: the view declares every option the rules and the table mention, and builds its widgets. */
const rendered = view.render([ 'cake-adapt', featureText ]);
const [ outer, advanced ] = sections;
const declared = sections.flatMap((sec) => sec.children);
const allOptions = declared.filter((o) => !o.option.startsWith('_'));
const options = allOptions.map((o) => o.option);
const option = (name) => allOptions.find((o) => o.option == name);
/* Each option is declared once, across both sections. */
assert.strictEqual(new Set(options).size, options.length, `duplicates: ${options.filter((n, i) => options.indexOf(n) != i)}`);
/* What the package cannot use is not shown; the MQTT switch only while an instance has it on. */
const unshown = [
	...(features.mqtt || mqttStored ? [] : [ 'mqtt_enabled' ]),
	...(features.mqtt ? [] : [ 'mqtt_host', 'mqtt_port', 'mqtt_topic', 'mqtt_username', 'mqtt_password' ]),
	...(features.tls ? [] : [ 'mqtt_tls', 'mqtt_ca_file' ])
];
const asked = new Set();
instanceErrors((name) => { asked.add(name); return undefined; });
errorsWith((name) => { asked.add(name); return name == 'mqtt_enabled' ? '1' : undefined; }, { mqtt: true, tls: true });
errorsWith((name) => { asked.add(name); return [ 'mqtt_enabled', 'mqtt_tls' ].includes(name) ? '1' : undefined; }, { mqtt: true, tls: true });
/* The legacy single target is read from the stored section; the form has no field for it. */
asked.delete('latency_target');
for (const name of new Set([ ...asked, ...Object.keys(defaults), ...Object.keys(scales) ])) {
	if (unshown.includes(name))
		assert.ok(!options.includes(name), `${name} is not shown by this package`);
	else
		assert.ok(options.includes(name), `${name} is an option of the view`);
}
for (const name of unshown)
	assert.ok(!options.includes(name), `${name} is not shown by this package`);
if (features.tls) {
	assert.deepStrictEqual(option('mqtt_tls').dependencies, [ 'mqtt_enabled', '1' ]);
	assert.deepStrictEqual(option('mqtt_ca_file').dependencies, [ { mqtt_enabled: '1', mqtt_tls: '1' } ]);
}
for (const option of allOptions) {
	assert.strictEqual(typeof option.validate, 'function', `${option.option} is validated`);
	if (!(option instanceof Flag) && defaults[option.option] != null)
		assert.strictEqual(option.placeholder, defaults[option.option], `${option.option} placeholder`);
	assert.ok(!option.datatype, `${option.option} has no datatype of its own`);
}

/*
 * Layout: General, Shaper rates, Reflectors and Advanced; Advanced holds only
 * the nested section on the same instance, whose own tabs hold every other option.
 */
assert.deepStrictEqual(outer.tabs, [ 'general', 'rates', 'reflectors', 'advanced' ]);
const holders = outer.children.filter((o) => o.tab == 'advanced');
assert.strictEqual(holders.length, 1, 'Advanced has only the nested section');
assert.ok(holders[0] instanceof SectionValue);
assert.strictEqual(holders[0].subsection, advanced);
assert.deepStrictEqual(advanced.args.slice(1), [ null, 'cake_adapt' ], 'a NamedSection of the instance type');
assert.strictEqual(sections.length, 2);
assert.strictEqual(advanced.tabs.length, 8);
for (const o of declared)
	assert.ok(o.section.tabs.includes(o.tab), `${o.option} is in a tab of its section`);
for (const tab of advanced.tabs)
	assert.ok(advanced.children.some((o) => o.tab == tab), `${tab} has options`);
assert.ok(!advanced.children.some((o) => o.option.startsWith('_')), 'no heading rows are left');
for (const name of [ 'enabled', 'interface', 'max_ul_shaper_rate_kbps', 'reflectors' ])
	assert.strictEqual(option(name).section, outer, `${name} stays outside Advanced`);
for (const name of [ 'no_pingers', 'tcp_ts_request', 'log_to_file', 'alpha_delta_ewma', 'config_file' ])
	assert.strictEqual(option(name).section, advanced, `${name} is in Advanced`);

/* The nested section works on the instance it is given, never on another one. */
assert.deepStrictEqual(advanced.cfgsections(), [ 'main', 'guest' ], 'checkDepends walks the outer instances');
const nested = advanced.load('guest').then(() => {
	for (const o of advanced.children) {
		assert.strictEqual(o.loaded.guest, `guest:${o.option}`);
		assert.ok(!('main' in o.loaded), `${o.option} loads only the instance asked`);
	}
	return advanced.parse('main');
}).then(async () => {
	for (const o of advanced.children)
		assert.deepStrictEqual(o.parsed, [ 'main' ], `${o.option} parses only the instance asked`);
	if (features.tls) {
		const savedScreen = screen;
		const persisted = { mqtt_enabled: '1', mqtt_tls: '1', mqtt_ca_file: '/etc/ssl/custom-mqtt-ca.pem' };
		const fields = [ option('mqtt_enabled'), option('mqtt_tls'), option('mqtt_ca_file') ];
		fields.forEach((field) => { field.persisted = persisted; });
		/* Save with MQTT disabled: dependent fields are inactive and absent from form values. */
		screen = { mqtt_enabled: '0' };
		await Promise.all(fields.map((field) => field.parse('main')));
		assert.deepStrictEqual(persisted, { mqtt_enabled: '0', mqtt_tls: '1', mqtt_ca_file: '/etc/ssl/custom-mqtt-ca.pem' });
		/* The seam exercises stock removal too; an ordinary inactive option is deleted. */
		const probe = new Value(advanced, 'inactive_probe');
		probe.persisted = { inactive_probe: 'saved', mqtt_enabled: '1' };
		probe.depends('mqtt_enabled', '1');
		await probe.parse('main');
		assert.ok(!('inactive_probe' in probe.persisted));
		/* Reload the saved values, enable MQTT and save: TLS and the custom CA survive. */
		screen = { ...persisted, mqtt_enabled: '1' };
		await Promise.all(fields.map((field) => field.parse('main')));
		assert.deepStrictEqual(persisted, { mqtt_enabled: '1', mqtt_tls: '1', mqtt_ca_file: '/etc/ssl/custom-mqtt-ca.pem' });
		fields.forEach((field) => { delete field.persisted; });
		screen = savedScreen;
	}
	return advanced.render('guest');
}).then((node) => {
	assert.strictEqual(node.attrs['data-section-id'], 'guest');
	assert.ok(node.attrs.class.split(' ').includes('cbi-section-node-tabbed'), 'its tabs are initialised as a tab group');
	assert.deepStrictEqual(node.nodes, [ 'tabs of guest' ]);
});

/* A widget shows the first error naming its option, from the values on screen. */
const show = (values, name, value) => {
	screen = Object.assign({}, good, values);
	return option(name).validate.call(option(name), 'main', value);
};
assert.strictEqual(show({}, 'base_dl_shaper_rate_kbps', '20000'), true);
/* Unshown settings are checked as stored, and the error shows on the MQTT switch. */
if (features.mqtt && !features.tls) {
	stored.mqtt_tls = '1';
	assert.ok(show({ mqtt_enabled: '1', mqtt_host: 'broker.example' }, 'mqtt_enabled', '1').includes('cake-adapt-mqtt-ssl package'));
	delete stored.mqtt_tls;
}
if (!features.mqtt && mqttStored) {
	/* A warning, not an error: saving is not blocked. */
	stored.mqtt_host = 'mqtt://broker';
	assert.strictEqual(show({ mqtt_enabled: '1' }, 'mqtt_enabled', '1'), true, 'MQTT on only warns');
	assert.ok(option('mqtt_enabled').description.startsWith('Warning: this package has no MQTT support'));
	stored.mqtt_tls = '1';
	assert.ok(show({ mqtt_enabled: '1' }, 'mqtt_enabled', '1').includes('cake-adapt-mqtt-ssl package'), 'stored TLS is refused');
	assert.strictEqual(show({ mqtt_enabled: '0' }, 'mqtt_enabled', '0'), true, 'off, the stored settings are not checked');
	delete stored.mqtt_tls;
	delete stored.mqtt_host;
}
if (features.mqtt)
	assert.ok(!option('mqtt_enabled').description.includes('Warning'));
assert.ok(show({ min_dl_shaper_rate_kbps: '30000' }, 'min_dl_shaper_rate_kbps', '30000').includes('minimum <= base'));
assert.ok(show({ min_dl_shaper_rate_kbps: '30000' }, 'max_dl_shaper_rate_kbps', '50000').includes('minimum <= base'));
assert.strictEqual(show({ tcp_ts_request: '1' }, 'tcp_delay_attribution', '0') === true, false, 'the attribution is named too');
/* Rules across the two sections see both: the reflectors outside Advanced, the pingers and thresholds in it. */
assert.ok(show({ no_pingers: '7' }, 'no_pingers', '7').includes('between 1 and the reflector count'));
assert.strictEqual(show({ no_pingers: '6' }, 'no_pingers', '6'), true);
assert.ok(show({ connection_active_thr_kbps: '10001' }, 'min_dl_shaper_rate_kbps', '10000').includes('cannot exceed'));
assert.ok(show({ pinger_method: 'fping-ts', reflectors: [ '2606:4700::1111' ], no_pingers: '1' }, 'reflectors', '').includes('IPv6'));
/* The reflector list when empty is checked as a list; typed text, as an entry. */
assert.ok(show({ reflectors: [] }, 'reflectors', '').includes('at least one reflector'));
assert.strictEqual(show({ reflectors: [] }, 'reflectors', '9.9.9.11'), true);
assert.ok(show({ reflectors: [ '1.1.1.1' ] }, 'reflectors', '1.1.1.1 ').includes('invalid reflector'));
assert.ok(show({ reflectors: [ '1.1.1.1' ] }, 'reflectors', '1.1.1.1').includes('duplicate reflector'));
assert.strictEqual(show({ enabled: '0', adjust_dl_shaper_rate: '0', adjust_ul_shaper_rate: '0', reflectors: [] }, 'reflectors', 'bad host'), true, 'no host rule while nothing runs');
assert.ok(show({ enabled: '0', adjust_dl_shaper_rate: '0', adjust_ul_shaper_rate: '0' }, 'reflectors', 'a'.repeat(256)).includes('too long'));
/* The stored legacy target reaches the checks. */
stored.latency_target = '1.1.1.1';
assert.strictEqual(show({ reflectors: [], no_pingers: '' }, 'reflectors', ''), true);
delete stored.latency_target;

/* A change in one field checks every field of its instance again. */
Promise.all([ rendered, nested, loaded ]).then(() => {
	for (const type of [ 'keyup', 'change', 'widget-change', 'cbi-dynlist-change', 'input' ])
		assert.strictEqual(typeof nodeHandlers[type], 'function', `${type} rechecks`);
	declared.forEach((o) => { o.checks = 0; });
	nodeHandlers.keyup({ target: { closest: () => ({ getAttribute: () => 'main' }) } });
	for (const o of declared)
		assert.strictEqual(o.checks, o.option.startsWith('_') ? 0 : 1, `${o.option} rechecked`);
	nodeHandlers.keyup({ target: { closest: () => null } });

	/* The per-tick cache, with the real timer: a second ask in the same tick reuses the first answer. */
	global.setTimeout = realSetTimeout;
	const base = option('base_dl_shaper_rate_kbps');
	const ask = () => show({ min_dl_shaper_rate_kbps: '30000' }, 'min_dl_shaper_rate_kbps', '30000');

	assert.ok(ask() !== true, 'first ask computes the error');
	screen = Object.assign({}, good);
	assert.ok(option('min_dl_shaper_rate_kbps').validate.call(option('min_dl_shaper_rate_kbps'), 'main', '10000') !== true,
		'same tick: the cached answer');
	return new Promise((resolve) => realSetTimeout(resolve, 5)).then(() => {
		assert.strictEqual(option('min_dl_shaper_rate_kbps').validate.call(option('min_dl_shaper_rate_kbps'), 'main', '10000'), true,
			'next tick: recomputed from the screen');
		assert.ok(base);
		console.log('luci_config.js passed');
	});
}).catch((e) => { console.error(e); process.exit(1); });

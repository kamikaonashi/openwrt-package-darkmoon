'use strict';
'require view';
'require form';
'require rpc';
'require uci';
'require ui';
'require tools.widgets as widgets';
'require darkmoon.common as dm';

var callServiceList = rpc.declare({
	object: 'service',
	method: 'list',
	params: [ 'name' ],
	expect: { '': {} }
});

var callInitList = rpc.declare({
	object: 'luci',
	method: 'getInitList',
	params: [ 'name' ],
	expect: { '': {} }
});

var callInitAction = rpc.declare({
	object: 'luci',
	method: 'setInitAction',
	params: [ 'name', 'action' ],
	expect: { result: false }
});

/* darkmoon 1.x used numeric values */
var LEGACY = {
	cake_diffserv: { '0': 'diffserv3', '1': 'diffserv4', '2': 'diffserv8', '3': 'besteffort', '4': 'precedence' },
	cake_dl_flow_mode: { '0': 'flowblind', '1': 'srchost', '2': 'dsthost', '3': 'hosts', '4': 'flows', '5': 'dual-srchost', '6': 'dual-dsthost', '7': 'triple-isolate' },
	cake_ack_filter: { 'no-ack-filter': '0', 'ack-filter': '1', 'ack-filter-aggressive': '2' },
	ping_type: { '0': 'echo', '1': 'timestamp' }
};
LEGACY.cake_ul_flow_mode = LEGACY.cake_dl_flow_mode;

function legacyCfgvalue(name) {
	return function(sid) {
		var v = uci.get('darkmoon', sid, name);
		if (v == null && (name == 'cake_dl_flow_mode' || name == 'cake_ul_flow_mode'))
			v = uci.get('darkmoon', sid, 'cake_flow_mode');
		if (v == '-1')
			v = uci.get('darkmoon', sid, 'cake_flow_mode');
		return (v != null && LEGACY[name][v] != null) ? LEGACY[name][v] : v;
	};
}

function flowModes(o, recommended) {
	var modes = [
		[ 'dual-dsthost', _('dual-dsthost – fair per LAN host, then per flow') ],
		[ 'dual-srchost', _('dual-srchost – fair per LAN host, then per flow') ],
		[ 'triple-isolate', _('triple-isolate – fair per source and destination host') ],
		[ 'hosts', _('hosts – fair per host pair') ],
		[ 'flows', _('flows – fair per flow only') ],
		[ 'srchost', _('srchost – fair per source host only') ],
		[ 'dsthost', _('dsthost – fair per destination host only') ],
		[ 'flowblind', _('flowblind – no isolation') ]
	];
	modes.forEach(function(m) {
		o.value(m[0], m[0] == recommended ? m[1] + ' ' + _('(recommended)') : m[1]);
	});
}

return view.extend({
	load: function() {
		return Promise.all([
			uci.load('darkmoon'),
			L.resolveDefault(uci.load('network'), null)
		]);
	},

	/* procd only reloads a registered service, so start it if it was stopped */
	afterApply: function() {
		var enabled = uci.sections('darkmoon', 'darkmoon').some(function(s) { return s.enabled == '1'; });
		return Promise.all([
			L.resolveDefault(callServiceList('darkmoon'), {}),
			L.resolveDefault(callInitList('darkmoon'), {})
		]).then(function(res) {
			var registered = !!res[0].darkmoon;
			var boot = !!((res[1].darkmoon || {}).enabled);
			var tasks = [];
			if (enabled && !boot)
				tasks.push(callInitAction('darkmoon', 'enable'));
			if (enabled && !registered)
				tasks.push(callInitAction('darkmoon', 'start'));
			return Promise.all(tasks);
		});
	},

	render: function() {
		var m, s, o;
		var self = this;

		document.addEventListener('uci-applied', function() { self.afterApply(); });

		m = new form.Map('darkmoon', _('Darkmoon – Settings'),
			_('darkmoon adjusts CAKE to the capacity your line actually delivers right now. Set realistic minimum, base and maximum rates for each direction; darkmoon moves between them based on measured latency. Live data is on the <a href="%s">Status</a> page.')
				.format(L.url('admin/services/darkmoon/status')));

		s = m.section(form.TypedSection, 'darkmoon', _('Instances'),
			_('Each instance shapes one WAN connection. Most setups only need the default <em>primary</em> instance.'));
		s.addremove = true;
		s.anonymous = false;
		s.addbtntitle = _('Add instance');

		s.tab('basic', _('General'));
		s.tab('cake', _('CAKE'));
		s.tab('ping', _('Reflectors'));
		s.tab('tuning', _('Algorithm'));
		s.tab('health', _('Reflector health'));
		s.tab('misc', _('Idle, stall & misc'));

		o = s.taboption('basic', form.Flag, 'enabled', _('Enable'));
		o.rmempty = false;

		o = s.taboption('basic', widgets.DeviceSelect, 'ul_if', _('WAN device'),
			_('Upload is shaped on this device\'s egress. Use the device that carries your internet traffic, e.g. <code>wan</code>, <code>eth1</code>, <code>pppoe-wan</code> or <code>wwan0</code>.'));
		o.noaliases = true;
		o.nocreate = true;
		o.rmempty = false;
		o.default = 'wan';

		o = s.taboption('basic', form.Value, 'dl_if', _('Download device'),
			_('Name of the IFB device darkmoon creates to shape incoming WAN traffic (recommended). Alternatively, pick an existing LAN-side device such as <code>br-lan</code> to shape download on its egress instead; traffic to the router itself is then not shaped.'));
		o.value('ifb-wan', 'ifb-wan ' + _('(IFB, recommended)'));
		o.value('br-lan', 'br-lan ' + _('(LAN bridge egress)'));
		o.datatype = 'and(netdevname,maxlength(15))';
		o.rmempty = false;
		o.default = 'ifb-wan';
		o.validate = function(sid, value) {
			var ul = this.section.formvalue(sid, 'ul_if');
			return (value && value == ul) ? _('Download and upload devices must differ') : true;
		};

		var rateOpt = function(name, title, def, descr) {
			var opt = s.taboption('basic', form.Value, name, title, descr);
			opt.datatype = 'and(uinteger,min(64))';
			opt.default = String(def);
			opt.placeholder = String(def);
			opt.rmempty = false;
			opt.validate = function(sid, value) {
				var dir = name.substr(name.indexOf('_') + 1, 2);
				var v = function(k) { return +this.section.formvalue(sid, k + '_' + dir + '_shaper_rate_kbps'); }.bind(this);
				var mn = v('min'), bs = v('base'), mx = v('max');
				if (!(mn <= bs && bs <= mx))
					return _('Rates must satisfy minimum ≤ base ≤ maximum');
				return true;
			};
			return opt;
		};

		rateOpt('min_dl_shaper_rate_kbps', _('Download minimum (kbit/s)'), 5000,
			_('Lowest rate darkmoon will drop to under bufferbloat. Rates are in kbit/s: 100000 = 100 Mbit/s.'));
		rateOpt('base_dl_shaper_rate_kbps', _('Download base (kbit/s)'), 20000,
			_('Rate used when the line is idle or lightly loaded; darkmoon decays back to it. A rate your line reliably achieves.'));
		rateOpt('max_dl_shaper_rate_kbps', _('Download maximum (kbit/s)'), 80000,
			_('Highest rate darkmoon may probe up to, typically your plan speed.'));
		rateOpt('min_ul_shaper_rate_kbps', _('Upload minimum (kbit/s)'), 5000);
		rateOpt('base_ul_shaper_rate_kbps', _('Upload base (kbit/s)'), 20000);
		rateOpt('max_ul_shaper_rate_kbps', _('Upload maximum (kbit/s)'), 35000);

		o = s.taboption('basic', form.Flag, 'adjust_dl_shaper_rate', _('Adapt download rate'),
			_('When disabled, download is shaped at a fixed base rate.'));
		o.default = '1';
		o = s.taboption('basic', form.Flag, 'adjust_ul_shaper_rate', _('Adapt upload rate'),
			_('When disabled, upload is shaped at a fixed base rate.'));
		o.default = '1';

		o = s.taboption('cake', form.ListValue, 'link_layer', _('Link layer'),
			_('Per-packet framing overhead of your access link, so CAKE accounts for the real bytes on the wire. When unsure, <em>Ethernet</em> is a safe choice for fibre, cable, cellular and Starlink.'));
		o.value('ethernet', _('Ethernet / fibre / cellular (overhead 38, mpu 84)'));
		o.value('ethernet-vlan', _('Ethernet with VLAN tag (overhead 42, mpu 84)'));
		o.value('docsis', _('Cable / DOCSIS (overhead 18, mpu 64)'));
		o.value('pppoe-ptm', _('VDSL2 with PPPoE (PTM, overhead 30)'));
		o.value('bridged-ptm', _('VDSL2 without PPPoE (PTM, overhead 22)'));
		o.value('pppoe-llcsnap', _('ADSL, PPPoE LLC/SNAP (ATM, overhead 40)'));
		o.value('pppoe-vcmux', _('ADSL, PPPoE VC-Mux (ATM, overhead 32)'));
		o.value('pppoa-llc', _('ADSL, PPPoA LLC (ATM, overhead 14)'));
		o.value('pppoa-vcmux', _('ADSL, PPPoA VC-Mux (ATM, overhead 10)'));
		o.value('bridged-llcsnap', _('ADSL, bridged LLC/SNAP (ATM, overhead 32)'));
		o.value('bridged-vcmux', _('ADSL, bridged VC-Mux (ATM, overhead 24)'));
		o.value('ipoa-llcsnap', _('ADSL, IPoA LLC/SNAP (ATM, overhead 16)'));
		o.value('ipoa-vcmux', _('ADSL, IPoA VC-Mux (ATM, overhead 8)'));
		o.value('raw', _('Raw – no overhead compensation'));
		o.value('custom', _('Custom…'));
		o.default = 'ethernet';
		o.cfgvalue = function(sid) {
			var v = uci.get('darkmoon', sid, 'link_layer');
			if (v == null && (uci.get('darkmoon', sid, 'cake_overhead') != null || uci.get('darkmoon', sid, 'cake_atm') != null))
				return 'custom';
			return v;
		};

		o = s.taboption('cake', form.Value, 'cake_overhead', _('Overhead (bytes)'));
		o.datatype = 'range(-64,256)';
		o.placeholder = '0';
		o.depends('link_layer', 'custom');
		o = s.taboption('cake', form.Value, 'cake_mpu', _('Minimum packet unit (bytes)'));
		o.datatype = 'range(0,256)';
		o.placeholder = '0';
		o.depends('link_layer', 'custom');
		o = s.taboption('cake', form.ListValue, 'cake_atm', _('Cell framing'));
		o.value('0', _('None'));
		o.value('1', _('ATM (ADSL)'));
		o.value('2', _('PTM (VDSL2)'));
		o.default = '0';
		o.depends('link_layer', 'custom');

		o = s.taboption('cake', form.ListValue, 'cake_diffserv', _('Traffic classes'),
			_('How CAKE prioritises by DSCP marking.'));
		o.value('diffserv4', _('diffserv4 – bulk, best effort, video, voice (recommended)'));
		o.value('diffserv3', _('diffserv3 – bulk, best effort, voice'));
		o.value('diffserv8', _('diffserv8 – eight classes'));
		o.value('besteffort', _('besteffort – single class, no prioritisation'));
		o.value('precedence', _('precedence – legacy IP precedence'));
		o.default = 'diffserv4';
		o.cfgvalue = legacyCfgvalue('cake_diffserv');

		o = s.taboption('cake', form.ListValue, 'cake_dl_flow_mode', _('Download fairness'),
			_('How bandwidth is shared between LAN hosts and flows on download.'));
		flowModes(o, 'dual-dsthost');
		o.default = 'dual-dsthost';
		o.cfgvalue = legacyCfgvalue('cake_dl_flow_mode');

		o = s.taboption('cake', form.ListValue, 'cake_ul_flow_mode', _('Upload fairness'),
			_('How bandwidth is shared between LAN hosts and flows on upload.'));
		flowModes(o, 'dual-srchost');
		o.default = 'dual-srchost';
		o.cfgvalue = legacyCfgvalue('cake_ul_flow_mode');

		o = s.taboption('cake', form.Flag, 'cake_nat', _('NAT awareness'),
			_('Look up the real LAN host behind NAT so per-host fairness works. Recommended on routers doing NAT.'));
		o.default = '1';

		o = s.taboption('cake', form.Flag, 'cake_wash', _('Wash DSCP on upload'),
			_('Clear DSCP markings on traffic leaving to the ISP (after CAKE has used them).'));
		o.default = '1';

		o = s.taboption('cake', form.ListValue, 'cake_ack_filter', _('Upload ACK filter'),
			_('Drop redundant TCP ACKs on upload. Can help on very asymmetric links.'));
		o.value('0', _('Off'));
		o.value('1', _('On'));
		o.value('2', _('Aggressive'));
		o.default = '0';
		o.cfgvalue = legacyCfgvalue('cake_ack_filter');

		o = s.taboption('cake', form.Flag, 'cake_split_gso', _('Split GSO packets'),
			_('Split GSO super-packets so small packets are not stuck behind them. Recommended below ~1 Gbit/s.'));
		o.default = '1';

		o = s.taboption('cake', form.Value, 'cake_rtt_ms', _('CAKE RTT (ms)'),
			_('Expected round-trip time CAKE tunes its AQM for. 0 = CAKE default (100 ms).'));
		o.datatype = 'ufloat';
		o.placeholder = '0';

		o = s.taboption('cake', form.Value, 'cake_memlimit_kb', _('CAKE memory limit (KiB)'),
			_('0 = automatic. Lower it on routers with little RAM.'));
		o.datatype = 'uinteger';
		o.placeholder = '0';

		o = s.taboption('cake', form.Flag, 'cake_mq', _('Use cake_mq on upload'),
			_('Multi-queue CAKE spreads upload shaping across CPU cores. Needs a kernel with <code>cake_mq</code> (Linux 6.19+); darkmoon falls back to normal CAKE automatically.'));
		o.default = '0';

		o = s.taboption('ping', form.ListValue, 'ping_type', _('Measurement'),
			_('<strong>ICMP echo</strong> works with almost any host but only sees round-trip time, so delay in either direction slows both. <strong>ICMP timestamp</strong> measures each direction separately but needs reflectors that answer timestamp requests (most public DNS servers do not); see <a href="https://github.com/tievolu/timestamp-reflectors" target="_blank" rel="noreferrer">timestamp-reflectors</a>.'));
		o.value('echo', _('ICMP echo (round trip / 2)'));
		o.value('timestamp', _('ICMP timestamp (one-way per direction)'));
		o.default = 'echo';
		o.cfgvalue = legacyCfgvalue('ping_type');

		o = s.taboption('ping', form.DynamicList, 'reflectors', _('Reflectors'),
			_('IPv4 hosts to ping. The first <em>Active reflectors</em> are used, the rest are spares that replace reflectors which stop answering or behave worse than the others.'));
		o.datatype = 'ip4addr("nomask")';

		o = s.taboption('ping', form.Value, 'reflectors_file', _('Reflector list file'),
			_('Optional file with one IPv4 address per line. Replaces the list above; with large files a random subset of 128 is used.'));
		o.placeholder = '/etc/darkmoon/reflectors.txt';
		o.datatype = 'file';

		o = s.taboption('ping', form.Value, 'no_pingers', _('Active reflectors'));
		o.datatype = 'range(1,32)';
		o.placeholder = '6';

		o = s.taboption('ping', form.Value, 'reflector_ping_interval_s', _('Ping interval per reflector (s)'),
			_('With 6 reflectors every 0.3 s darkmoon gets a sample every 50 ms.'));
		o.datatype = 'and(ufloat,min(0.01))';
		o.placeholder = '0.3';

		o = s.taboption('ping', form.Flag, 'randomize_reflectors', _('Randomise order'),
			_('Shuffle the reflector list at every start.'));
		o.default = '1';

		o = s.taboption('ping', form.Flag, 'bind_ping_to_ul_if', _('Bind pings to WAN device'),
			_('Send pings through the WAN device even if the default route points elsewhere (multi-WAN setups).'));
		o.default = '0';

		var num = function(tab, name, title, ph, dt, descr) {
			var opt = s.taboption(tab, form.Value, name, title, descr);
			opt.datatype = dt || 'ufloat';
			opt.placeholder = ph;
			return opt;
		};

		num('tuning', 'dl_owd_delta_delay_thr_ms', _('Download delay threshold (ms)'), '30.0', null,
			_('A sample counts as delayed when latency rises this much above baseline.'));
		num('tuning', 'ul_owd_delta_delay_thr_ms', _('Upload delay threshold (ms)'), '30.0');
		num('tuning', 'dl_avg_owd_delta_max_adjust_up_thr_ms', _('Download full-speed increase below (ms)'), '10.0', null,
			_('Under high load the rate increases fastest while the average delta stays below this.'));
		num('tuning', 'ul_avg_owd_delta_max_adjust_up_thr_ms', _('Upload full-speed increase below (ms)'), '10.0');
		num('tuning', 'dl_avg_owd_delta_max_adjust_down_thr_ms', _('Download maximum decrease above (ms)'), '60.0', null,
			_('Rate decreases hardest when the average delta reaches this.'));
		num('tuning', 'ul_avg_owd_delta_max_adjust_down_thr_ms', _('Upload maximum decrease above (ms)'), '60.0');
		num('tuning', 'bufferbloat_detection_window', _('Detection window (samples)'), '6', 'range(1,64)');
		num('tuning', 'bufferbloat_detection_thr', _('Delayed samples for bufferbloat'), '3', 'range(1,64)');
		num('tuning', 'high_load_thr', _('High load threshold'), '0.75', 'range(0.01,1)',
			_('Fraction of the shaper rate above which load counts as high and the rate may increase.'));
		num('tuning', 'connection_active_thr_kbps', _('Activity threshold (kbit/s)'), '2000', 'uinteger',
			_('Below this, a direction counts as idle.'));
		num('tuning', 'shaper_rate_min_adjust_down_bufferbloat', _('Smallest decrease factor'), '0.99', 'range(0.1,1)');
		num('tuning', 'shaper_rate_max_adjust_down_bufferbloat', _('Largest decrease factor'), '0.75', 'range(0.1,1)');
		num('tuning', 'shaper_rate_min_adjust_up_load_high', _('Smallest increase factor'), '1.0', 'range(1,2)');
		num('tuning', 'shaper_rate_max_adjust_up_load_high', _('Largest increase factor'), '1.04', 'range(1,2)');
		num('tuning', 'shaper_rate_adjust_down_load_low', _('Decay factor towards base (from above)'), '0.99', 'range(0.1,1)');
		num('tuning', 'shaper_rate_adjust_up_load_low', _('Recovery factor towards base (from below)'), '1.01', 'range(1,2)');
		num('tuning', 'bufferbloat_refractory_period_ms', _('Bufferbloat refractory period (ms)'), '300', 'uinteger');
		num('tuning', 'decay_refractory_period_ms', _('Decay refractory period (ms)'), '1000', 'uinteger');
		num('tuning', 'alpha_baseline_increase', _('Baseline rise rate (alpha)'), '0.001', 'range(0.000001,1)',
			_('How quickly a reflector\'s baseline follows increases. Keep small.'));
		num('tuning', 'alpha_baseline_decrease', _('Baseline fall rate (alpha)'), '0.9', 'range(0.000001,1)');
		num('tuning', 'alpha_delta_ewma', _('Jitter EWMA alpha'), '0.095', 'range(0.000001,1)');

		num('health', 'reflector_health_check_interval_s', _('Health check interval (s)'), '1.0');
		num('health', 'reflector_response_deadline_s', _('Response deadline (s)'), '1.0', null,
			_('A reflector misses a check if it has not answered within this time.'));
		num('health', 'reflector_misbehaving_detection_window', _('Miss window (checks)'), '60', 'range(1,64)');
		num('health', 'reflector_misbehaving_detection_thr', _('Misses before replacement'), '3', 'range(1,64)');
		num('health', 'reflector_comparison_interval_s', _('Comparison interval (s)'), '60', 'uinteger',
			_('How often reflectors are compared with each other.'));
		num('health', 'reflector_sum_owd_baselines_delta_thr_ms', _('Replace if baseline exceeds best by (ms)'), '20.0');
		num('health', 'reflector_owd_delta_ewma_delta_thr_ms', _('Replace if jitter exceeds best by (ms)'), '10.0');
		num('health', 'reflector_replacement_interval_s', _('Rotate a random reflector every (s)'), '3600', 'and(uinteger,min(60))');
		o = s.taboption('health', form.Flag, 'retain_reflector_stats', _('Remember reflector baselines'),
			_('Keep the learned baseline of a reflector that is rotated out, for when it comes back.'));
		o.default = '1';

		o = s.taboption('misc', form.Flag, 'enable_sleep_function', _('Sleep when idle'),
			_('Pause pinging after a period without traffic; it resumes immediately when traffic starts.'));
		o.default = '1';
		num('misc', 'sustained_idle_sleep_thr_s', _('Idle time before sleeping (s)'), '60.0').depends('enable_sleep_function', '1');
		o = s.taboption('misc', form.Flag, 'min_shaper_rates_enforcement', _('Use minimum rates when idle or unreachable'),
			_('Drop to the minimum rates while sleeping or when no reflector answers.'));
		o.default = '0';
		num('misc', 'stall_detection_thr', _('Stall detection (missed samples)'), '5', 'range(1,1000)');
		num('misc', 'connection_stall_thr_kbps', _('Stall traffic threshold (kbit/s)'), '10', 'uinteger');
		num('misc', 'global_ping_response_timeout_s', _('Ping restart timeout (s)'), '10.0', null,
			_('Restart the pinger when no reflector has answered for this long.'));
		num('misc', 'monitor_achieved_rates_interval_ms', _('Throughput sampling interval (ms)'), '200', 'range(50,10000)');
		num('misc', 'if_up_check_interval_s', _('Interface check interval (s)'), '10.0', null,
			_('Fallback poll for the WAN device appearing or being recreated; link changes are also picked up instantly.'));
		num('misc', 'startup_wait_s', _('Startup delay (s)'), '0');
		o = s.taboption('misc', form.Flag, 'debug', _('Debug logging'),
			_('Log every decision to the system log (<code>logread -e darkmoon</code>).'));
		o.default = '0';

		return m.render().then(function(node) {
			return E([], [ dm.style(), node ]);
		});
	}
});

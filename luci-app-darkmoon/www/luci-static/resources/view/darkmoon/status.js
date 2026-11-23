'use strict';
'require view';
'require dom';
'require poll';
'require rpc';
'require uci';
'require ui';
'require fs';
'require darkmoon.common as dm';

var HISTORY = 180;   /* 1 s samples */

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

var statusCalls = {};
function callStatus(instance) {
	if (!statusCalls[instance])
		statusCalls[instance] = rpc.declare({
			object: 'darkmoon.' + instance,
			method: 'status',
			expect: { '': {} }
		});
	return L.resolveDefault(statusCalls[instance](), null);
}

function svgChart(series, opts) {
	var W = 600, H = opts.height || 110, pad = 2;
	var ymax = opts.ymin || 1;

	series.forEach(function(s) {
		s.values.forEach(function(v) { if (v != null && v > ymax) ymax = v; });
	});
	ymax *= 1.1;

	var x = function(i) { return pad + (W - 2 * pad) * i / (HISTORY - 1); };
	var y = function(v) { return H - pad - (H - 2 * pad) * Math.max(0, Math.min(v, ymax)) / ymax; };

	var out = '<svg viewBox="0 0 ' + W + ' ' + H + '" preserveAspectRatio="none" class="dm-chart" ' +
	          'xmlns="http://www.w3.org/2000/svg">';

	for (var g = 1; g < 4; g++)
		out += '<line x1="0" x2="' + W + '" y1="' + (H * g / 4) + '" y2="' + (H * g / 4) + '" class="dm-grid"/>';

	series.forEach(function(s) {
		var off = HISTORY - s.values.length, pts = [];
		s.values.forEach(function(v, i) {
			if (v != null)
				pts.push(x(off + i).toFixed(1) + ',' + y(v).toFixed(1));
		});
		if (pts.length < 2)
			return;
		if (s.fill)
			out += '<polygon points="' + x(off).toFixed(1) + ',' + H + ' ' + pts.join(' ') + ' ' +
			       x(HISTORY - 1).toFixed(1) + ',' + H + '" fill="' + s.color + '" fill-opacity="0.18"/>';
		out += '<polyline points="' + pts.join(' ') + '" fill="none" stroke="' + s.color + '" stroke-width="' +
		       (s.width || 1.6) + '"' + (s.dash ? ' stroke-dasharray="5 4"' : '') +
		       ' vector-effect="non-scaling-stroke"/>';
	});

	out += '</svg>';

	var box = E('div', { 'class': 'dm-chartbox' });
	box.innerHTML = out;
	return E([], [
		E('div', { 'class': 'dm-chart-head' }, [ E('h4', {}, opts.title), E('span', {}, _('scale %s').format(opts.fmt(ymax))) ]),
		box
	]);
}

function legend(items) {
	return E('div', { 'class': 'dm-legend' }, items.map(function(it) {
		return E('span', {}, [
			E('i', { 'style': 'background:' + it[1] + (it[2] ? ';opacity:.45' : '') }), ' ', it[0]
		]);
	}));
}

function compact(list) {
	return list.filter(function(n) { return n != null; });
}

function rangeBar(d, color) {
	var span = Math.max(1, d.max_kbps - d.min_kbps);
	var pos = function(v) { return Math.max(0, Math.min(100, 100 * (v - d.min_kbps) / span)); };
	var ach = Math.max(0, Math.min(100, 100 * d.achieved_kbps / Math.max(1, d.max_kbps)));

	return E('div', { 'class': 'dm-range', 'title': _('min %s · base %s · max %s')
		.format(dm.rate(d.min_kbps), dm.rate(d.base_kbps), dm.rate(d.max_kbps)) }, [
		E('div', { 'class': 'dm-range-track' }, [
			E('div', { 'class': 'dm-range-ach', 'style': 'width:' + ach + '%;background:' + color }),
			E('div', { 'class': 'dm-range-base', 'style': 'left:' + pos(d.base_kbps) + '%' }),
			E('div', { 'class': 'dm-range-shaper', 'style': 'left:' + pos(d.shaper_kbps) + '%;border-color:' + color })
		]),
		E('div', { 'class': 'dm-range-labels' }, [
			E('span', {}, dm.rate(d.min_kbps)),
			E('span', {}, _('base') + ' ' + dm.rate(d.base_kbps)),
			E('span', {}, dm.rate(d.max_kbps))
		])
	]);
}

function directionCard(title, d, color) {
	var delta = d.avg_owd_delta_us, thr = d.delay_thr_us;
	var lat = delta > thr ? 'bad' : (delta > thr / 2 ? 'warn' : 'ok');
	var load = d.bufferbloat ? [ _('Bufferbloat'), 'bad' ]
	         : d.load == 'high' ? [ _('High load'), 'warn' ]
	         : d.load == 'low' ? [ _('Low load'), 'info' ] : [ _('Idle'), 'muted' ];

	return E('div', { 'class': 'dm-card' }, [
		E('div', { 'class': 'dm-card-head' }, [
			E('h4', { 'style': 'color:' + color }, [ title, ' ', E('small', {}, d.interface) ]),
			E('span', { 'class': 'dm-pill dm-' + load[1] }, load[0])
		]),
		E('div', { 'class': 'dm-big' }, dm.rate(d.shaper_kbps)),
		E('div', { 'class': 'dm-sub' }, [
			_('shaper rate'), d.adjust ? '' : ' (' + _('fixed') + ')',
			' · ', _('achieved'), ' ', E('strong', {}, dm.rate(d.achieved_kbps)),
			' (', d.load_percent, '%)'
		]),
		rangeBar(d, color),
		E('div', { 'class': 'dm-kv' }, [
			E('span', {}, _('Latency increase')),
			E('strong', { 'class': 'dm-t-' + lat }, dm.ms(delta, true)),
			E('span', {}, _('threshold') + ' ' + dm.ms(thr))
		]),
		E('div', { 'class': 'dm-kv' }, [
			E('span', {}, _('Bufferbloat events')),
			E('strong', {}, String(d.bufferbloat_events)),
			E('span', {}, d.qdisc)
		])
	]);
}

function reflectorTable(st) {
	var echo = st.ping_type == 'echo';
	var rows = st.reflectors.map(function(r) {
		var loss = r.sent > 1 ? Math.max(0, Math.round(100 * (1 - r.received / (r.sent - 1)))) : 0;
		var fresh = r.last_response_ms < 1000;
		return E('tr', { 'class': 'tr' }, compact([
			E('td', { 'class': 'td' }, [
				E('span', { 'class': 'dm-dot dm-' + (fresh ? 'ok' : (r.received ? 'warn' : 'bad')) }), ' ', r.address
			]),
			E('td', { 'class': 'td' }, r.valid ? dm.ms(echo ? r.dl_baseline_us * 2 : r.dl_baseline_us + r.ul_baseline_us) : '–'),
			E('td', { 'class': 'td' }, r.valid ? dm.ms(r.dl_delta_us, true) : '–'),
			echo ? null : E('td', { 'class': 'td' }, r.valid ? dm.ms(r.ul_delta_us, true) : '–'),
			E('td', { 'class': 'td' }, r.valid ? dm.ms(r.dl_delta_ewma_us) + (echo ? '' : ' / ' + dm.ms(r.ul_delta_ewma_us)) : '–'),
			E('td', { 'class': 'td' }, r.received ? dm.age(r.last_response_ms) : _('never')),
			E('td', { 'class': 'td' }, r.received + ' / ' + r.sent + (loss > 0 ? ' (' + loss + '%)' : '')),
			E('td', { 'class': 'td' }, String(r.offences))
		]));
	});

	return E('table', { 'class': 'table dm-refl' }, [
		E('tr', { 'class': 'tr table-titles' }, compact([
			E('th', { 'class': 'th' }, _('Reflector')),
			E('th', { 'class': 'th' }, echo ? _('Base RTT') : _('Base RTT (sum)')),
			E('th', { 'class': 'th' }, echo ? _('Delta') : _('DL delta')),
			echo ? null : E('th', { 'class': 'th' }, _('UL delta')),
			E('th', { 'class': 'th' }, _('Jitter (EWMA)')),
			E('th', { 'class': 'th' }, _('Last reply')),
			E('th', { 'class': 'th' }, _('Replies')),
			E('th', { 'class': 'th' }, _('Misses'))
		]))
	].concat(rows));
}

return view.extend({
	history: {},

	load: function() {
		return Promise.all([
			uci.load('darkmoon'),
			L.resolveDefault(uci.load('firewall'), null),
			L.resolveDefault(uci.load('sqm'), null),
			L.resolveDefault(fs.stat('/sys/module/sch_cake'), null),
			L.resolveDefault(fs.stat('/sys/module/ifb'), null)
		]);
	},

	fetch: function() {
		var instances = uci.sections('darkmoon', 'darkmoon');
		return Promise.all([
			L.resolveDefault(callServiceList('darkmoon'), {}),
			L.resolveDefault(callInitList('darkmoon'), {})
		]).then(function(res) {
			var svc = (res[0].darkmoon || {}).instances || {};
			var boot = !!((res[1].darkmoon || {}).enabled);
			return Promise.all(instances.map(function(s) {
				var name = s['.name'];
				return (svc[name] && svc[name].running) ? callStatus(name) : Promise.resolve(null);
			})).then(function(stats) {
				return { svc: svc, boot: boot, instances: instances, stats: stats };
			});
		});
	},

	record: function(name, st) {
		var h = this.history[name] || (this.history[name] = {
			dl_s: [], dl_a: [], ul_s: [], ul_a: [], dl_d: [], ul_d: [], dl_t: [], ul_t: []
		});
		var push = function(arr, v) { arr.push(v); if (arr.length > HISTORY) arr.shift(); };
		push(h.dl_s, st.download.shaper_kbps);
		push(h.dl_a, st.download.achieved_kbps);
		push(h.ul_s, st.upload.shaper_kbps);
		push(h.ul_a, st.upload.achieved_kbps);
		push(h.dl_d, st.state == 'running' ? st.download.avg_owd_delta_us / 1000 : null);
		push(h.ul_d, st.state == 'running' ? st.upload.avg_owd_delta_us / 1000 : null);
		push(h.dl_t, st.download.delay_thr_us / 1000);
		push(h.ul_t, st.upload.delay_thr_us / 1000);
		return h;
	},

	serviceAction: function(action, ev) {
		var btn = ev.currentTarget;
		btn.disabled = true;
		return callInitAction('darkmoon', action).then(function(ok) {
			if (!ok)
				ui.addNotification(null, E('p', _('Service action "%s" failed.').format(action)), 'error');
		}).finally(function() {
			btn.disabled = false;
			window.setTimeout(function() { poll.start(); }, 500);
		});
	},

	renderWarnings: function(data, fsinfo) {
		var warn = [];
		var anyEnabled = data.instances.some(function(s) { return s.enabled == '1'; });

		if (!fsinfo[0])
			warn.push([ 'bad', _('The CAKE kernel module is not loaded. Install <code>kmod-sched-cake</code>.') ]);
		if (!fsinfo[1])
			warn.push([ 'warn', _('The IFB kernel module is not loaded. Install <code>kmod-ifb</code> for download shaping.') ]);

		var fw = uci.sections('firewall', 'defaults')[0] || {};
		if (fw.flow_offloading == '1' && fw.flow_offloading_hw == '1')
			warn.push([ 'bad', _('<strong>Hardware flow offloading is enabled.</strong> Offloaded connections bypass the CPU and therefore CAKE, so darkmoon cannot shape them. Disable hardware offloading in <a href="%s">Network → Firewall</a> (software offloading is fine).').format(L.url('admin/network/firewall')) ]);

		data.instances.forEach(function(s) {
			if (s.enabled != '1')
				return;
			uci.sections('sqm', 'queue').forEach(function(q) {
				if (q.enabled == '1' && q.interface == s.ul_if)
					warn.push([ 'bad', _('SQM is also shaping <code>%s</code>. Disable that SQM queue to avoid both fighting over the same qdisc.').format(q.interface) ]);
			});
		});

		if (anyEnabled && !data.boot)
			warn.push([ 'warn', _('The darkmoon service is not enabled at boot.') ]);

		return warn.map(function(w) {
			var p = E('p');
			p.innerHTML = w[1];
			return E('div', { 'class': 'alert-message ' + (w[0] == 'bad' ? 'danger' : 'warning') }, p);
		});
	},

	renderInstance: function(s, st, running) {
		var name = s['.name'];
		var head, body = [];

		if (s.enabled != '1') {
			return E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, name),
				E('p', {}, [ _('This instance is disabled.'), ' ',
					E('a', { 'href': L.url('admin/services/darkmoon/config') }, _('Configure…')) ])
			]);
		}

		if (!st) {
			return E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, name),
				E('div', { 'class': 'alert-message warning' },
					running ? _('Waiting for status…')
					        : _('Enabled but not running. Check the system log (<code>logread -e darkmoon</code>) or press Start.'))
			]);
		}

		var h = this.record(name, st);
		var stateClass = { running: 'ok', idle: 'muted', stall: 'warn', waiting: 'bad' }[st.state] || 'muted';
		var stateText = {
			running: _('Running'), idle: _('Idle – pingers asleep'),
			stall: _('Stall – no reflector replies'), waiting: _('Waiting')
		}[st.state] || st.state;

		head = E('div', { 'class': 'dm-head' }, [
			E('h3', {}, name),
			E('span', { 'class': 'dm-pill dm-' + stateClass }, stateText),
			E('span', { 'class': 'dm-sub' }, [
				_('up %s').format(dm.duration(st.uptime_s)), ' · ',
				st.ping_type == 'timestamp' ? _('ICMP timestamp') : _('ICMP echo'), ' · ',
				_('%d rate changes').format(st.rate_changes), ' · ',
				_('%d reflector swaps').format(st.reflector_replacements)
			])
		]);

		if (st.error)
			body.push(E('div', { 'class': 'alert-message ' + (st.state == 'waiting' ? 'danger' : 'warning') }, st.error));

		body.push(E('div', { 'class': 'dm-cards' }, [
			directionCard(_('Download'), st.download, dm.DL),
			directionCard(_('Upload'), st.upload, dm.UL)
		]));

		var echo = st.ping_type == 'echo';
		var fmtRate = function(v) { return dm.rate(v); };
		var fmtMs = function(v) { return v.toFixed(1) + ' ms'; };

		body.push(E('div', { 'class': 'dm-cards' }, [
			E('div', { 'class': 'dm-card' }, [
				svgChart([
					{ values: h.dl_a, color: dm.DL, fill: true, width: 1 },
					{ values: h.dl_s, color: dm.DL, width: 2 }
				], { title: _('Download throughput'), fmt: fmtRate, ymin: st.download.base_kbps }),
				legend([ [ _('shaper'), dm.DL ], [ _('achieved'), dm.DL, true ] ])
			]),
			E('div', { 'class': 'dm-card' }, [
				svgChart([
					{ values: h.ul_a, color: dm.UL, fill: true, width: 1 },
					{ values: h.ul_s, color: dm.UL, width: 2 }
				], { title: _('Upload throughput'), fmt: fmtRate, ymin: st.upload.base_kbps }),
				legend([ [ _('shaper'), dm.UL ], [ _('achieved'), dm.UL, true ] ])
			]),
			E('div', { 'class': 'dm-card' }, [
				svgChart([
					{ values: h.dl_t, color: dm.BAD, dash: true, width: 1 },
					{ values: h.dl_d, color: dm.DL },
					echo ? null : { values: h.ul_d, color: dm.UL }
				].filter(Boolean), { title: _('Latency increase over baseline'), fmt: fmtMs, ymin: 5 }),
				legend(echo
					? [ [ _('round trip / 2 (both directions)'), dm.DL ], [ _('bufferbloat threshold'), dm.BAD ] ]
					: [ [ _('download'), dm.DL ], [ _('upload'), dm.UL ], [ _('bufferbloat threshold'), dm.BAD ] ])
			])
		]));

		if (st.state == 'idle')
			body.push(E('p', { 'class': 'dm-sub' }, _('No significant traffic for a while, so reflector pings are paused. They resume as soon as traffic exceeds the activity threshold.')));
		else
			body.push(E('div', { 'class': 'dm-card' }, [
				E('h4', {}, _('Reflectors (%d active, %d spare)').format(st.reflectors.length, st.spare_reflectors)),
				reflectorTable(st)
			]));

		return E('div', { 'class': 'cbi-section' }, [ head ].concat(body));
	},

	update: function(container, fsinfo) {
		return this.fetch().then(L.bind(function(data) {
			var running = Object.keys(data.svc).length > 0;
			var nodes = [];

			nodes.push(E('div', { 'class': 'dm-toolbar' }, [
				E('span', { 'class': 'dm-pill dm-' + (running ? 'ok' : 'muted') },
					running ? _('Service running') : _('Service stopped')),
				E('button', { 'class': 'btn cbi-button cbi-button-apply', 'click': ui.createHandlerFn(this, 'serviceAction', 'start') }, _('Start')),
				E('button', { 'class': 'btn cbi-button cbi-button-reload', 'click': ui.createHandlerFn(this, 'serviceAction', 'restart') }, _('Restart')),
				E('button', { 'class': 'btn cbi-button cbi-button-reset', 'click': ui.createHandlerFn(this, 'serviceAction', 'stop') }, _('Stop')),
				E('button', { 'class': 'btn cbi-button', 'click': ui.createHandlerFn(this, 'serviceAction', data.boot ? 'disable' : 'enable') },
					data.boot ? _('Disable autostart') : _('Enable autostart'))
			]));

			nodes = nodes.concat(this.renderWarnings(data, fsinfo));

			if (!data.instances.length || !data.instances.some(function(s) { return s.enabled == '1'; })) {
				nodes.push(E('div', { 'class': 'cbi-section' }, [
					E('h3', {}, _('Get started')),
					E('p', {}, _('darkmoon is installed but no instance is enabled. Set your WAN interface and line speeds, then enable it.')),
					E('a', { 'class': 'btn cbi-button cbi-button-action', 'href': L.url('admin/services/darkmoon/config') }, _('Open settings'))
				]));
			}

			data.instances.forEach(L.bind(function(s, i) {
				var inst = data.svc[s['.name']];
				nodes.push(this.renderInstance(s, data.stats[i], inst && inst.running));
			}, this));

			dom.content(container, nodes);
		}, this));
	},

	render: function(res) {
		var fsinfo = [ res[3], res[4] ];
		var container = E('div', {}, E('p', {}, E('em', { 'class': 'spinning' }, _('Loading…'))));

		poll.add(L.bind(this.update, this, container, fsinfo), 1);

		return E([], [
			dm.style(),
			E('h2', {}, _('Darkmoon')),
			E('div', { 'class': 'cbi-map-descr' }, _('Adaptive CAKE shaping: darkmoon measures latency to a set of reflectors and lowers the shaper rate the moment queues start to build, then probes back up when the line allows it.')),
			container
		]);
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});

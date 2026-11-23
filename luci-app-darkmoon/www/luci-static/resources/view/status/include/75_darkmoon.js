'use strict';
'require baseclass';
'require rpc';
'require uci';
'require darkmoon.common as dm';

/* the overview page re-runs load()/render() itself, so no timers here */

var callServiceList = rpc.declare({
	object: 'service',
	method: 'list',
	params: [ 'name' ],
	expect: { '': {} }
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

return baseclass.extend({
	title: _('Darkmoon'),

	load: function() {
		return Promise.all([
			L.resolveDefault(uci.load('darkmoon'), null),
			L.resolveDefault(callServiceList('darkmoon'), {})
		]).then(function(res) {
			var svc = ((res[1] || {}).darkmoon || {}).instances || {};
			var sections = uci.sections('darkmoon', 'darkmoon').filter(function(s) { return s.enabled == '1'; });
			return Promise.all(sections.map(function(s) {
				var i = svc[s['.name']];
				return (i && i.running) ? callStatus(s['.name']) : Promise.resolve(null);
			})).then(function(stats) {
				return { sections: sections, stats: stats };
			});
		});
	},

	render: function(data) {
		if (!data || !data.sections.length)
			return E('p', { 'class': 'dm-sub' }, [
				_('No darkmoon instance is enabled.'), ' ',
				E('a', { 'href': L.url('admin/services/darkmoon/config') }, _('Configure…'))
			]);

		var rows = data.sections.map(function(s, i) {
			var st = data.stats[i];
			var link = E('a', { 'href': L.url('admin/services/darkmoon/status') }, s['.name']);

			if (!st)
				return E('tr', { 'class': 'tr' }, [
					E('td', { 'class': 'td' }, link),
					E('td', { 'class': 'td', 'colspan': '5' }, E('span', { 'class': 'dm-t-warn' }, _('not running')))
				]);

			var cls = { running: 'ok', idle: 'muted', stall: 'warn', waiting: 'bad' }[st.state] || 'muted';
			var dir = function(d) {
				var lat = d.avg_owd_delta_us > d.delay_thr_us ? 'bad' : 'ok';
				return E('td', { 'class': 'td' }, [
					E('strong', {}, dm.rate(d.shaper_kbps)), ' ',
					E('span', { 'class': 'dm-sub' }, '(' + dm.rate(d.achieved_kbps) + ')'), ' ',
					st.state == 'running' ? E('span', { 'class': 'dm-t-' + lat }, dm.ms(d.avg_owd_delta_us, true)) : '',
					d.bufferbloat ? E('span', { 'class': 'dm-t-bad' }, ' ⚠') : ''
				]);
			};

			return E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td' }, link),
				E('td', { 'class': 'td' }, [ E('span', { 'class': 'dm-pill dm-' + cls }, st.state) ]),
				dir(st.download),
				dir(st.upload),
				E('td', { 'class': 'td' }, st.error ? E('span', { 'class': 'dm-t-bad' }, st.error) : String(st.reflectors.length)),
				E('td', { 'class': 'td' }, dm.duration(st.uptime_s))
			]);
		});

		return E([], [
			dm.style(),
			E('table', { 'class': 'table dm-ov' }, [
				E('tr', { 'class': 'tr table-titles' }, [
					E('th', { 'class': 'th' }, _('Instance')),
					E('th', { 'class': 'th' }, _('State')),
					E('th', { 'class': 'th' }, _('Download shaper (achieved) Δlatency')),
					E('th', { 'class': 'th' }, _('Upload shaper (achieved) Δlatency')),
					E('th', { 'class': 'th' }, _('Reflectors')),
					E('th', { 'class': 'th' }, _('Uptime'))
				])
			].concat(rows))
		]);
	}
});

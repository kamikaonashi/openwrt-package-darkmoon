'use strict';
'require baseclass';

var CSS = [
	'.dm-toolbar{display:flex;flex-wrap:wrap;gap:.5em;align-items:center;margin:0 0 1em}',
	'.dm-toolbar .dm-pill{margin-right:.5em}',
	'.dm-head{display:flex;flex-wrap:wrap;gap:.4em .8em;align-items:baseline;margin-bottom:.6em}',
	'.dm-head h3{margin:0;padding:0;border:0}',
	'.dm-cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:1em;margin:0 0 1em}',
	'.dm-card{border:1px solid rgba(128,128,128,.28);border-radius:8px;padding:.8em 1em;min-width:0}',
	'.dm-card h4{margin:0 0 .5em;font-size:.85em;font-weight:600;text-transform:uppercase;letter-spacing:.04em;opacity:.85}',
	'.dm-card h4 small{text-transform:none;font-weight:400;opacity:.7;letter-spacing:0}',
	'.dm-card-head{display:flex;justify-content:space-between;align-items:flex-start;gap:.5em}',
	'.dm-big{font-size:2em;font-weight:600;line-height:1.15}',
	'.dm-sub{opacity:.72;font-size:.9em}',
	'.dm-pill{display:inline-block;padding:.12em .7em;border-radius:1em;font-size:.8em;font-weight:600;white-space:nowrap;color:#fff}',
	'.dm-pill.dm-ok{background:#15803d}.dm-pill.dm-warn{background:#b45309}.dm-pill.dm-bad{background:#b91c1c}',
	'.dm-pill.dm-info{background:#1d4ed8}.dm-pill.dm-muted{background:#6b7280}',
	'.dm-t-ok{color:#16a34a}.dm-t-warn{color:#d97706}.dm-t-bad{color:#dc2626}',
	'.dm-dot{display:inline-block;width:.6em;height:.6em;border-radius:50%;vertical-align:middle}',
	'.dm-dot.dm-ok{background:#16a34a}.dm-dot.dm-warn{background:#d97706}.dm-dot.dm-bad{background:#dc2626}',
	'.dm-range{margin:.8em 0 .6em}',
	'.dm-range-track{position:relative;height:10px;border-radius:5px;background:rgba(128,128,128,.2)}',
	'.dm-range-ach{position:absolute;left:0;top:0;bottom:0;border-radius:5px;opacity:.35;transition:width .4s}',
	'.dm-range-base{position:absolute;top:-3px;bottom:-3px;width:2px;margin-left:-1px;background:rgba(128,128,128,.8)}',
	'.dm-range-shaper{position:absolute;top:-4px;width:14px;height:14px;margin-left:-7px;border-radius:50%;border:3px solid;background:#fff;box-sizing:border-box;transition:left .4s}',
	'.dm-range-labels{display:flex;justify-content:space-between;font-size:.75em;opacity:.65;margin-top:.3em}',
	'.dm-kv{display:grid;grid-template-columns:1fr auto auto;gap:.8em;align-items:baseline;font-size:.92em;padding:.25em 0;border-top:1px solid rgba(128,128,128,.15)}',
	'.dm-kv span:last-child{opacity:.6;font-size:.85em;min-width:6em;text-align:right}',
	'.dm-chartbox{position:relative}',
	'.dm-chart{width:100%;height:110px;display:block}',
	'.dm-chart .dm-grid{stroke:rgba(128,128,128,.18);stroke-width:1;vector-effect:non-scaling-stroke}',
	'.dm-chart-head{display:flex;justify-content:space-between;align-items:baseline;gap:.5em}',
	'.dm-chart-head span{font-size:.75em;opacity:.6}',
	'.alert-message a{color:inherit;text-decoration:underline}',
	'.dm-legend{display:flex;flex-wrap:wrap;gap:1em;font-size:.78em;opacity:.8;margin-top:.3em}',
	'.dm-legend i{display:inline-block;width:.9em;height:.55em;border-radius:2px;vertical-align:middle}',
	'.dm-refl{font-size:.9em}',
	'.dm-ov td,.dm-ov th{white-space:nowrap}'
].join('\n');

return baseclass.extend({
	DL: '#2f7fe0',
	UL: '#a24ee0',
	BAD: '#dc2626',

	style: function() {
		return E('style', {}, CSS);
	},

	rate: function(kbps) {
		if (kbps == null || isNaN(kbps))
			return '–';
		if (kbps >= 1000000)
			return (kbps / 1000000).toFixed(2) + ' Gbit/s';
		if (kbps >= 10000)
			return (kbps / 1000).toFixed(1) + ' Mbit/s';
		if (kbps >= 1000)
			return (kbps / 1000).toFixed(2) + ' Mbit/s';
		return kbps + ' kbit/s';
	},

	ms: function(us, signed) {
		if (us == null || isNaN(us))
			return '–';
		var v = us / 1000;
		return (signed && v > 0 ? '+' : '') + v.toFixed(v < 10 && v > -10 ? 1 : 0) + ' ms';
	},

	age: function(ms) {
		if (ms < 1000)
			return ms + ' ms';
		return this.duration(Math.floor(ms / 1000)) + ' ' + _('ago');
	},

	duration: function(s) {
		if (s < 60)
			return s + 's';
		if (s < 3600)
			return Math.floor(s / 60) + 'm ' + (s % 60) + 's';
		if (s < 86400)
			return Math.floor(s / 3600) + 'h ' + Math.floor((s % 3600) / 60) + 'm';
		return Math.floor(s / 86400) + 'd ' + Math.floor((s % 86400) / 3600) + 'h';
	}
});

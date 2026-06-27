
const $ = id => document.getElementById(id);

function fmtBps(b) {
  if (b < 1024)       return [b.toFixed(0), 'B/s'];
  if (b < 1048576)    return [(b/1024).toFixed(1), 'KB/s'];
  if (b < 1073741824) return [(b/1048576).toFixed(2), 'MB/s'];
  return [(b/1073741824).toFixed(2), 'GB/s'];
}
function fmtSz(n) {
  if (n < 1073741824) return (n/1048576).toFixed(0)+' MB';
  return (n/1073741824).toFixed(1)+' GB';
}
function fmtUp(s) {
  const d=Math.floor(s/86400), h=Math.floor(s%86400/3600),
        m=Math.floor(s%3600/60), sc=s%60;
  if (d) return `${d}d ${h}h ${m}m`;
  if (h) return `${h}h ${m}m ${sc}s`;
  return `${m}m ${sc}s`;
}
function barClr(p) {
  return p<60 ? 'var(--green)' : p<80 ? 'var(--amber)' : 'var(--red)';
}
function setBar(id, pct) {
  const el = $(id);
  el.style.width = pct+'%';
  el.style.background = barClr(pct);
}
function setTrend(id, curr, prev, badHigh=true) {
  const el = $(id); if (!el) return;
  if (prev==null) { el.textContent='—'; el.className='sc-trend t-flat'; return; }
  const d=curr-prev, thr=Math.max(Math.abs(prev)*0.04,0.5);
  if (Math.abs(d)<thr) { el.textContent='—'; el.className='sc-trend t-flat'; return; }
  el.textContent = d>0 ? '▲' : '▼';
  el.className = 'sc-trend '+(d>0 ? (badHigh?'t-up':'t-down') : (badHigh?'t-down':'t-up'));
}

let prev={}, uptimeBase=null, uptimeTick=null, sysData=null, activeK8sNode='';

async function loadWeather() {
  try {
    const res = await fetch('/api/weather');
    const data = await res.json();
    if (!data.configured) {
      $('weather-section').style.display = 'none';
      return;
    }
    $('weather-section').style.display = '';
    $('weather-label').textContent = data.label || 'Outdoor weather';
    if (data.error) {
      $('weather-temp').textContent = '—';
      $('weather-summary').textContent = 'Weather fetch failed.';
      $('weather-feels').textContent = '—'; $('weather-humidity').textContent = '—'; $('weather-wind').textContent = '—';
      $('weather-note').textContent = data.error;
      return;
    }
    const toF = c => c != null ? Math.round(c * 9/5 + 32) : null;
    const toMph = k => k != null ? (k * 0.621371).toFixed(1) : null;
    $('weather-temp').textContent = toF(data.temperature_c) ?? '—';
    $('weather-unit').textContent = '°F';
    $('weather-summary').textContent = data.summary
      ? `${data.summary}${data.is_day === 0 ? ' · night' : ''}`
      : 'Current outdoor conditions';
    $('weather-feels').textContent   = toF(data.apparent_c)   != null ? `${toF(data.apparent_c)}°F` : '—';
    $('weather-humidity').textContent= data.humidity_pct != null ? `${data.humidity_pct}%` : '—';
    $('weather-wind').textContent    = toMph(data.wind_kph) != null ? `${toMph(data.wind_kph)} mph` : '—';
    $('weather-note').textContent = `Source: ${data.source || 'weather service'} · cached server-side`;
  } catch {
    $('weather-section').style.display = 'none';
  }
}

function tempClr(t) {
  return t < 60 ? 'var(--green)' : t < 75 ? 'var(--amber)' : 'var(--red)';
}

function barRow(label, pct, clr, extra) {
  return `<div class="node-bar-label"${extra||''}><span>${label}</span><span>${pct}%</span></div>
    <div class="node-bar"><div class="node-bar-fill" style="width:${pct}%;background:${clr}"></div></div>`;
}
function tempBarRow(temp) {
  if (temp == null) return '';
  const clr = tempClr(temp);
  const pct = Math.min(Math.round(temp), 100);
  return `<div class="node-bar-label" style="margin-top:0.15rem"><span>TEMP</span><span style="color:${clr}">${temp}°C</span></div>
    <div class="node-bar"><div class="node-bar-fill" style="width:${pct}%;background:${clr}"></div></div>`;
}

function buildK3sNodes(nodes, pveNodes) {
  const grid = $('k3s-nodes-grid');
  if (!nodes || !nodes.length) { grid.innerHTML = ''; return; }
  const pveMap = {};
  (pveNodes || []).forEach(p => { pveMap[p.name] = p; });

  grid.innerHTML = nodes.map(n => {
    const isActive = activeK8sNode && n.name === activeK8sNode;
    const hostTemp = n.pve_host_temp != null
      ? `<span style="color:${tempClr(n.pve_host_temp)}">${n.pve_host_temp}°C</span>`
      : '<span style="color:var(--muted)">—</span>';
    const hostLine = n.pve_host
      ? `<span style="color:var(--text)">${n.pve_host}</span> · ${hostTemp}`
      : '<span style="color:var(--muted)">host unknown</span>';

    let metricsHtml = '';
    if (isActive && sysData) {
      const s = sysData;
      const hostTempVal = n.pve_host_temp;
      metricsHtml = `
        <div class="node-cpu-num">${s.cpu.pct}<span class="u">%</span></div>
        <div class="node-bar-row">
          ${barRow('CPU', s.cpu.pct, barClr(s.cpu.pct), '')}
          ${barRow('RAM', s.mem.pct, barClr(s.mem.pct), ' style="margin-top:0.15rem"')}
          ${tempBarRow(hostTempVal)}
        </div>`;
    } else if (n.host_metrics) {
      const h = n.host_metrics;
      const hostTempVal = n.pve_host_temp ?? h.temp;
      metricsHtml = `
        <div class="node-cpu-num">${h.cpu_pct}<span class="u">%</span></div>
        <div class="node-bar-row">
          ${barRow('CPU', h.cpu_pct, barClr(h.cpu_pct), '')}
          ${barRow('RAM', h.mem_pct, barClr(h.mem_pct), ' style="margin-top:0.15rem"')}
          ${tempBarRow(hostTempVal)}
        </div>`;
    } else if (n.pve_host && pveMap[n.pve_host]) {
      const p = pveMap[n.pve_host];
      const mem = p.mem_pct ?? 0;
      metricsHtml = `
        <div class="node-cpu-num">${p.cpu}<span class="u">%</span></div>
        <div class="node-bar-row">
          ${barRow('CPU', p.cpu, barClr(p.cpu), '')}
          ${barRow('RAM', mem, barClr(mem), ' style="margin-top:0.15rem"')}
          ${tempBarRow(p.temp)}
        </div>`;
    }

    return `<div class="node-cell${n.ready ? '' : ' offline'}">
      <div class="node-name"><span class="node-dot${n.ready ? '' : ' off'}"></span>${n.name}${isActive ? ' <span style="color:var(--muted);font-size:0.52rem">★</span>' : ''}</div>
      ${metricsHtml}
      <div class="node-temp-text" style="margin-top:auto">${hostLine}</div>
    </div>`;
  }).join('');
}

function buildPveNodes(nodes) {
  const grid = $('pve-grid');
  grid.innerHTML = '';
  nodes.forEach(n => {
    const online = n.status === 'online';
    const mem = n.mem_pct ?? 0;
    const cpuClr = barClr(n.cpu), memClr = barClr(mem);
    const tempPct = n.temp != null ? Math.min(Math.round(n.temp), 100) : 0;
    const tempClrVal = n.temp != null ? tempClr(n.temp) : 'var(--muted)';
    const tempBarHtml = n.temp != null ? `
      <div class="pve-bar-label" style="margin-top:0.18rem"><span>TEMP</span><span style="color:${tempClrVal}">${n.temp}°C</span></div>
      <div class="pve-bar"><div class="pve-bar-fill" style="width:${tempPct}%;background:${tempClrVal}"></div></div>` : '';
    grid.insertAdjacentHTML('beforeend', `
      <div class="pve-node-cell ${online?'':'offline'}">
        <div class="pve-node-name"><span class="pve-dot ${online?'':'off'}"></span>${n.name}</div>
        <div class="pve-cpu-num">${n.cpu}<span class="u">%</span></div>
        <div class="pve-bar-row">
          <div class="pve-bar-label"><span>CPU</span><span>${n.cpu}%</span></div>
          <div class="pve-bar"><div class="pve-bar-fill" style="width:${n.cpu}%;background:${cpuClr}"></div></div>
          <div class="pve-bar-label" style="margin-top:0.18rem"><span>RAM</span><span>${mem}%</span></div>
          <div class="pve-bar"><div class="pve-bar-fill" style="width:${mem}%;background:${memClr}"></div></div>
          ${tempBarHtml}
        </div>
      </div>`);
  });
}

const incidentLabels = {
  node_down: 'K3s node down',
  pve_down: 'Proxmox node offline',
  temp_high: 'High temp',
  db_down: 'Database unreachable',
};

function fmtAgo(ts) {
  const s = Math.max(Math.floor(Date.now() / 1000 - ts), 0);
  if (s < 90) return `${s}s`;
  if (s < 5400) return `${Math.round(s / 60)}m`;
  if (s < 129600) return `${(s / 3600).toFixed(1)}h`;
  return `${(s / 86400).toFixed(1)}d`;
}

function renderIncidentBanner(open) {
  const banner = $('incident-banner');
  if (!open || !open.length) {
    banner.classList.remove('on');
    banner.innerHTML = '';
    return;
  }
  banner.innerHTML = open.map(inc => `
    <span class="inc-item">
      <span class="inc-dot"></span>
      <span class="inc-label">${incidentLabels[inc.type] || inc.type}</span>
      <span>${inc.target}</span>
      <span class="inc-meta">${fmtAgo(inc.started_ts)}</span>
    </span>
  `).join('');
  banner.classList.add('on');
}

function apply(d) {
  sysData = d;
  activeK8sNode = d.k8s_node || '';

  renderIncidentBanner(d.incidents_open);

  $('h-host').textContent = d.k8s_node || d.host || '—';
  $('dot').classList.remove('off');
  $('status').textContent = 'Live';
  $('tick').textContent = 'updated ' + new Date().toLocaleTimeString();

  uptimeBase = { ref: Date.now(), val: d.uptime };
  if (!uptimeTick) uptimeTick = setInterval(() => {
    if (!uptimeBase) return;
    $('uptime').textContent = fmtUp(uptimeBase.val + Math.floor((Date.now() - uptimeBase.ref) / 1000));
  }, 1000);

  $('k3s-nodes').textContent = `${d.cluster.nodes_ready}/${d.cluster.nodes_total}`;
  $('k3s-pods').textContent  = d.cluster.pods;
  $('k3s-req').textContent   = d.requests.toLocaleString();
  buildK3sNodes(d.cluster.nodes || [], (d.pve || {}).nodes || []);

  const pve = d.pve || {}, t = pve.total || {};
  if (pve.nodes && pve.nodes.length) {
    $('pve-total').style.display = '';
    $('pve-empty').style.display = 'none';
    $('pt-nodes').textContent = t.online ?? '—';
    $('pt-cpu').textContent   = t.cpu    ?? '—';
    $('pt-mem').textContent   = t.mem_pct ?? '—';
    if (t.avg_temp != null) {
      $('pt-temp').textContent   = t.avg_temp;
      $('pt-temp-u').textContent = '°C';
    } else {
      $('pt-temp').textContent   = 'N/A';
      $('pt-temp-u').textContent = '';
    }
    buildPveNodes(pve.nodes);
  } else {
    $('pve-total').style.display = 'none';
    $('pve-grid').innerHTML = '';
    $('pve-empty').style.display = '';
    $('pve-empty').textContent = pve.configured
      ? 'Proxmox polling is configured, but no node data is available right now.'
      : 'Set PVE_TOKEN and PVE_IPS in your private deployment config to enable Proxmox polling.';
  }

  prev = {};
}

function connect() {
  const es = new EventSource('/api/stream');
  es.onmessage = e => { try { apply(JSON.parse(e.data)); } catch {} };
  es.onerror   = () => {
    $('dot').classList.add('off');
    $('status').textContent = 'Offline';
    es.close();
    setTimeout(connect, 5000);
  };
}

loadWeather();
setInterval(loadWeather, 600000);
connect();

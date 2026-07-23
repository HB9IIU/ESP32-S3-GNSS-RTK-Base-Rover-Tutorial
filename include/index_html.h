#pragma once
#include <pgmspace.h>

static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width,initial-scale=1" />
  <title>GNSS Sky Plot + Map</title>

  <!-- Bootstrap 5 (CDN). Requires internet on the client device. -->
  <link href="https://cdn.jsdelivr.net/npm/bootstrap@5.3.3/dist/css/bootstrap.min.css" rel="stylesheet"
        integrity="sha384-QWTKZyjpPEjISv5WaRU9OFeRpok6YctnYmDr5pNlyT2bRjXh0JMhjY6hW+ALEwIH" crossorigin="anonymous">

  <!-- Leaflet (CDN). Requires internet on the client device. -->
  <link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css"
        integrity="sha256-p4NxAoJBhIIN+hmNHrzRCf9tD/miZyoHS5obTRR9BMY=" crossorigin="">
  <script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"
          integrity="sha256-20nQCchB9co0qIjJZRGuk2/Z9VM+kNiyxNV1lvTlZBo=" crossorigin=""></script>

  <!-- Highcharts (CDN). Requires internet on the client device. -->
  <script src="https://code.highcharts.com/highcharts.js"></script>

  <style>
    /* --- Bootstrap-friendly base --- */
    body{
      font-family: system-ui, -apple-system, Segoe UI, Roboto, sans-serif;
      margin: 0;
      background: #f6f7fb;
      color: #111;
    }

    .card { border: 1px solid rgba(0,0,0,.08); border-radius: 14px; padding: 14px 16px; background:#fff; box-shadow: 0 6px 18px rgba(0,0,0,.06); }
    .muted { color: #6c757d; font-size: 0.95rem; }
    .good { color: #198754; font-weight: 700; }
    .bad  { color: #dc3545; font-weight: 700; }

    canvas { border: 1px solid rgba(0,0,0,.08); border-radius: 14px; background: #fff; }

    .kv { display: grid; grid-template-columns: 140px 1fr; gap: 6px 10px; font-size: 0.98rem; }
    .k { color: #6c757d; }
    .v { color: #111; font-weight: 700; }
    .v-muted { color: #6c757d; font-weight: 500; }

    .legend  { display: grid; grid-template-columns: auto auto; gap: 6px 12px; font-size: 0.95rem; }
    .legend3 { display: grid; grid-template-columns: 1fr 1fr 1fr; gap: 6px 12px; font-size: 0.95rem; }
    .dot { display:inline-block; width:10px; height:10px; border-radius:50%; margin-right:6px; border:1px solid rgba(0,0,0,.25); vertical-align:middle; }
    .legendItem { white-space: nowrap; }

    /* ---------- GRID LAYOUT ---------- */
    .grid {
      display: grid;
      gap: 16px;
      align-items: start;

      /* allow right columns to shrink to avoid overlap */
      grid-template-columns: 380px minmax(0, 520px) minmax(0, 520px);
      grid-template-rows: auto auto;
    }

    .leftPane { grid-column: 1; grid-row: 1 / span 2; }
    .skyCard  { grid-column: 2; grid-row: 1; }
    .mapCard  { grid-column: 3; grid-row: 1; }
    .histCard { grid-column: 2 / span 2; grid-row: 2; }

    /* Prevent children forcing overflow */
    .skyCard, .mapCard { overflow: hidden; display: flex; flex-direction: column; align-items: stretch; }

    /* Aspect-ratio wrapper for square cards */
    .aspect-ratio-1x1 {
      position: relative;
      width: 100%;
      padding-top: 100%; /* 1:1 Aspect Ratio */
    }
    .aspect-ratio-1x1 > * {
      position: absolute;
      top: 0; left: 0; right: 0; bottom: 0;
      width: 100%;
      height: 100%;
    }

 /* --- Make sky + map the same height (square, responsive) --- */
#sky {
  width: 100%;
  height: auto;
  aspect-ratio: 1 / 1;
  display: block;
}

#map {
  width: 100%;
  height: auto;              /* remove fixed height */
  aspect-ratio: 1 / 1;       /* match the canvas */
  min-height: 320px;         /* safety for very narrow screens */
  border-radius: 14px;
}
    /* Histogram chart container height */
    #histChart { width: 100%; height: 245px; }

    @media (max-width: 1500px) {
      .grid { grid-template-columns: 380px minmax(0, 1fr) minmax(0, 1fr); }
      #map { width: 100%; height: 520px; }
      #sky { width: 100%; height: auto; }
    }

    @media (max-width: 1100px) {
      .grid { grid-template-columns: 1fr; grid-template-rows: auto; }
      .leftPane, .skyCard, .mapCard, .histCard { grid-column: 1 !important; grid-row: auto !important; }
      #map { width: 100%; height: 420px; }
    }

    .pageTitle{
      font-weight: 800;
      letter-spacing: .2px;
      margin: 0;
      font-size: 1.25rem;
    }
    .subtitle{
      color: #6c757d;
      margin: 0;
      font-size: .95rem;
    }
    .badgeSoft{
      background: rgba(13,110,253,.12);
      color: #0d6efd;
      border: 1px solid rgba(13,110,253,.18);
      font-weight: 700;
      padding: .35rem .55rem;
      border-radius: 999px;
      font-size: .8rem;
    }
    .hrSoft{
      border: 0;
      height: 1px;
      background: rgba(0,0,0,.08);
      margin: .75rem 0;
    }
    code {
      background: rgba(0,0,0,.06);
      padding: .12rem .35rem;
      border-radius: .4rem;
    }

    /* -------- Night theme (UI) -------- */
    body.night {
      background: #0f1115;
      color: #e9ecef;
    }

    /* Make sure EVERYTHING text-like flips */
    body.night, body.night * { color-scheme: dark; }

    body.night .pageTitle { color: rgba(233,236,239,.95) !important; }
    body.night .subtitle,
    body.night .muted,
    body.night .k,
    body.night .v-muted {
      color: rgba(233,236,239,.72) !important;
    }
    body.night .v { color: rgba(233,236,239,.95) !important; }
    body.night .card {
      background: #161a22;
      border-color: rgba(255,255,255,.10);
      box-shadow: 0 10px 28px rgba(0,0,0,.35);
    }
    body.night canvas {
      background: #0f1115;
      border-color: rgba(255,255,255,.10);
    }
    body.night code {
      background: rgba(255,255,255,.08);
      color: rgba(233,236,239,.92) !important;
    }
    body.night .dot { border-color: rgba(255,255,255,.25); }
    /* Ensure legend dots are visible in night mode */
    body.night .legend .dot[style*="background:#fff"] {
      background: #222 !important;
      border-color: #eee !important;
    }
    body.night .legend .dot[style*="background:#bbb"] {
      background: #888 !important;
      border-color: #eee !important;
    }
    body.night .legend .dot[style*="background:#2b7"] {
      background: #4fd964 !important;
    }
    body.night .hrSoft { background: rgba(255,255,255,.10); }
    /* Fix legend label text color in night mode */
    body.night .legend,
    body.night .legend3,
    body.night .legendItem {
      color: rgba(233,236,239,.92) !important;
    }

    /* Leaflet controls readable in night mode */
    body.night .leaflet-control-container .leaflet-control {
      background: rgba(22,26,34,.92);
      color: #e9ecef !important;
      border-color: rgba(255,255,255,.12);
      box-shadow: 0 8px 20px rgba(0,0,0,.35);
    }
    body.night .leaflet-control a { color: #e9ecef !important; }
    body.night .leaflet-control-layers label { color: rgba(233,236,239,.9) !important; }
  </style>
</head>

<body>
  <div class="container-fluid px-3 px-md-4 py-3">
    <div style="max-width: 1500px; margin: 0 auto;">
      <div class="d-flex flex-wrap align-items-center justify-content-between gap-2">
        <div>
          <h2 class="pageTitle">HB9IIU GNSS Dashboard <span class="badgeSoft ms-1">polling every 1s</span></h2>
        </div>
        <div>
          <a href="/webserial" id="btnConsole" class="btn btn-success btn-sm" style="margin-right:8px;"> Console </a>
          <button id="btnRestartESP" type="button" class="btn btn-danger btn-sm" style="float:right;">Restart ESP</button>
        </div>
      </div>

      <div class="grid" style="margin-top:24px;">

      <!-- LEFT PANE -->
      <div class="card leftPane">

        <div class="d-flex align-items-center justify-content-between mb-2">
          <div class="text-end">
            <div class="muted">Status</div>
            <div id="status" class="bad">Not started</div>
          </div>
          <button id="btnNight" type="button" class="btn btn-sm btn-outline-secondary">
            Night
          </button>
        </div>
        



        <div class="muted">GNSS</div>
        <div class="kv" style="margin-top:10px;">
          <div class="k">Latitude</div>            <div class="v" id="lat">—</div>
          <div class="k">Longitude</div>           <div class="v" id="lon">—</div>
          <div class="k">Altitude</div>            <div class="v" id="alt">—</div>

          <div class="k">Fix quality</div>         <div class="v" id="fixq">—</div>
          <div class="k">Satellites used</div>     <div class="v" id="svused">—</div>

          <div class="k">HDOP</div>                <div class="v" id="hdop">—</div>
          <div class="k">PDOP</div>                <div class="v" id="pdop">—</div>
          <div class="k">VDOP</div>                <div class="v" id="vdop">—</div>

          <div class="k">Geoid height</div>        <div class="v" id="geoid">—</div>

          <div class="k">DGPS age</div>            <div class="v" id="dgpsAge">—</div>
          <div class="k">DGPS station</div>        <div class="v" id="dgpsStation">—</div>

          <div class="k">Speed</div>               <div class="v" id="speed">—</div>
          <div class="k">Course</div>              <div class="v" id="course">—</div>
          <div class="k">Mag variation</div>       <div class="v" id="magvar">—</div>

          <div class="k">UTC</div>                 <div class="v" id="utctime">—</div>
          <div class="k">Date</div>                <div class="v" id="date">—</div>
          <div class="k">Epoch</div>               <div class="v" id="epoch">—</div>
          <div class="k">Time valid</div>          <div class="v" id="tvalid">—</div>

          <div class="k">Maidenhead</div>          <div class="v" id="mh">—</div>

          <div class="k">Horiz error</div>         <div class="v" id="herr">—</div>
          <div class="k">Vert error</div>          <div class="v" id="verr">—</div>
          <div class="k">Pos error</div>           <div class="v" id="perr">—</div>
        </div>

        <hr class="hrSoft">

        <div class="muted" style="margin-top:2px;">Constellations (count)</div>
        <div class="legend3" style="margin-top:10px;">
          <div class="legendItem"><span class="dot" style="background:#2b7;"></span>GPS <span id="cntGPS" class="v-muted">0</span></div>
          <div class="legendItem"><span class="dot" style="background:#27b;"></span>GAL <span id="cntGAL" class="v-muted">0</span></div>
          <div class="legendItem"><span class="dot" style="background:#b72;"></span>GLO <span id="cntGLO" class="v-muted">0</span></div>
          <div class="legendItem"><span class="dot" style="background:#a2b;"></span>BDS <span id="cntBDS" class="v-muted">0</span></div>
          <div class="legendItem"><span class="dot" style="background:#2aa;"></span>QZSS <span id="cntQZSS" class="v-muted">0</span></div>
          <div class="legendItem"><span class="dot" style="background:#999;"></span>SBAS <span id="cntSBAS" class="v-muted">0</span></div>
        </div>

        
      </div>

      <!-- SKY PLOT -->
      <div class="card skyCard">
        <div class="d-flex align-items-center justify-content-between">
          <div class="muted">Sky plot (Azimuth/Elevation)</div>
        </div>
        <div style="margin-top:10px;">
          <div class="aspect-ratio-1x1">
            <canvas id="sky" width="520" height="520"></canvas>
          </div>
        </div>

      </div>

      <!-- MAP -->
      <div class="card mapCard">
        <div class="d-flex align-items-center justify-content-between">
          <div class="muted">Map</div>

          <div class="d-flex align-items-center gap-2">
            <div class="muted small">Leaflet</div>
          </div>
        </div>

        <div style="margin-top:10px;">
          <div class="aspect-ratio-1x1">
            <div id="map"></div>
          </div>
        </div>

      </div>

      <!-- HISTOGRAM (Highcharts) -->
      <div class="card histCard">
        <div class="d-flex align-items-center justify-content-between">
          <div class="muted">Signal strength (SNR) histogram</div>
        </div>
        <div style="margin-top:10px;">
          <div id="histChart"></div>
        </div>

      </div>

    </div>
  </div>

  <script>
    document.addEventListener('DOMContentLoaded', function() {
      var btn = document.getElementById('btnRestartESP');
      if(btn) {
        btn.addEventListener('click', function() {
          showToast('ESP will reboot in a few seconds...');
          fetch('/reset');
        });
      }

      // Toast function with fade in/out
      function showToast(message) {
        var toast = document.createElement('div');
        toast.textContent = message;
        toast.style.position = 'fixed';
        toast.style.top = '20px';
        toast.style.right = '20px';
        toast.style.background = 'rgba(220,53,69,0.97)';
        toast.style.color = '#fff';
        toast.style.padding = '14px 28px';
        toast.style.borderRadius = '10px';
        toast.style.fontWeight = 'bold';
        toast.style.zIndex = 9999;
        toast.style.boxShadow = '0 2px 16px rgba(0,0,0,0.18)';
        toast.style.opacity = 0;
        toast.style.transition = 'opacity 0.5s';
        document.body.appendChild(toast);
        // Fade in
        setTimeout(function() {
          toast.style.opacity = 1;
        }, 10);
        // Fade out after 2.5s
        setTimeout(function() {
          toast.style.opacity = 0;
        }, 2500);
        // Remove after fade out
        setTimeout(function() {
          toast.remove();
        }, 3000);
      }
    });

    const el = (id) => document.getElementById(id);

    function setStatus(ok, msg) {
      const s = el('status');
      s.className = ok ? 'good' : 'bad';
      s.textContent = msg;
    }

    function clamp(v, lo, hi) { return Math.max(lo, Math.min(hi, v)); }
    function pad2(n) { return String(n).padStart(2, '0'); }

    function fmtNum(n, digits) {
      if (typeof n !== 'number' || !Number.isFinite(n)) return '—';
      return n.toFixed(digits);
    }

    function fmtMaybe(n, digits, suffix) {
      const v = fmtNum(n, digits);
      if (v === '—') return v;
      return suffix ? (v + suffix) : v;
    }

    function parseNmeaTimeStr(t) {
      if (typeof t !== 'string') return null;
      const m = t.match(/^(\d{2})(\d{2})(\d{2})(?:\.(\d+))?$/);
      if (!m) return null;
      return { hh: +m[1], mm: +m[2], ss: +m[3] };
    }

    function parseNmeaDateStr(d) {
      if (typeof d !== 'string') return null;
      const m = d.match(/^(\d{2})(\d{2})(\d{2})$/);
      if (!m) return null;
      return { dd: +m[1], mo: +m[2], yy: 2000 + +m[3] };
    }

    function formatUtcFromEpoch(epoch) {
      if (typeof epoch !== 'number' || !Number.isFinite(epoch) || epoch <= 0) return null;
      const d = new Date(epoch * 1000);
      return d.toISOString().replace('T', ' ').replace(/\.\d{3}Z$/, '');
    }

    function formatUtcFromNmea(d, t) {
      const dateObj = parseNmeaDateStr(d);
      const timeObj = parseNmeaTimeStr(t);
      if (!dateObj || !timeObj) return null;
      return `${dateObj.yy}-${pad2(dateObj.mo)}-${pad2(dateObj.dd)} ${pad2(timeObj.hh)}:${pad2(timeObj.mm)}:${pad2(timeObj.ss)} UTC`;
    }

    function formatDateNice(d) {
      const obj = parseNmeaDateStr(d);
      if (!obj) return '—';
      return `${obj.yy}-${pad2(obj.mo)}-${pad2(obj.dd)}`;
    }

    const colorMap = {
      'GPS': '#2b7',
      'GAL': '#27b', 'GALILEO': '#27b',
      'GLO': '#b72', 'GLONASS': '#b72',
      'BDS': '#a2b', 'BEIDOU': '#a2b',
      'QZSS': '#2aa',
      'SBAS': '#999'
    };

    function canonConstellation(name) {
      if (!name) return 'GPS';
      const k = String(name).trim().toUpperCase();
      if (k === 'GALILEO') return 'GAL';
      if (k === 'GLONASS') return 'GLO';
      if (k === 'BEIDOU') return 'BDS';
      if (k in colorMap) return k;
      if (k.startsWith('GAL')) return 'GAL';
      if (k.startsWith('GLO')) return 'GLO';
      if (k.startsWith('BDS') || k.startsWith('BD')) return 'BDS';
      return k;
    }

    function pickColor(name) {
      const c = canonConstellation(name);
      return colorMap[c] || '#2b7';
    }

    function updateConstellationCounts(constellation) {
      const counts = { GPS:0, GAL:0, GLO:0, BDS:0, QZSS:0, SBAS:0 };
      (constellation || []).forEach(s => {
        // Only count satellites that will be plotted (valid az/el)
        const az = Number(s.azimuth);
        const elv = Number(s.elevation);
        if (!Number.isFinite(az) || !Number.isFinite(elv)) return;
        const c = canonConstellation(s.constellation);
        if (counts[c] !== undefined) counts[c]++;
      });
      el('cntGPS').textContent  = counts.GPS;
      el('cntGAL').textContent  = counts.GAL;
      el('cntGLO').textContent  = counts.GLO;
      el('cntBDS').textContent  = counts.BDS;
      el('cntQZSS').textContent = counts.QZSS;
      el('cntSBAS').textContent = counts.SBAS;
    }

    function isNightMode() {
      return document.body.classList.contains('night');
    }

    // ------- SKY PLOT (canvas) with night-aware colors -------
    function drawSkyPlot(constellation) {
      const canvas = el('sky');
      const ctx = canvas.getContext('2d');
      const W = canvas.width, H = canvas.height;
      ctx.clearRect(0, 0, W, H);

      const cx = W / 2, cy = H / 2;
      const R = Math.min(W, H) * 0.46;

      const night = isNightMode();

      const bg       = night ? '#0f1115' : '#ffffff';
      const grid1    = night ? 'rgba(255,255,255,.12)' : '#d8d8d8';
      const grid2    = night ? 'rgba(255,255,255,.10)' : '#cfcfcf';
      const txt      = night ? 'rgba(233,236,239,.92)' : '#222';
      const txtMuted = night ? 'rgba(233,236,239,.65)' : '#777';
      const strokePt = night ? 'rgba(255,255,255,.25)' : '#666';

      ctx.fillStyle = bg;
      ctx.fillRect(0, 0, W, H);

      function ringForElev(elevDeg) { return (90 - elevDeg) / 90 * R; }

      ctx.strokeStyle = grid1;
      ctx.lineWidth = 2;
      [0, 30, 60, 90].forEach(elev => {
        const rr = ringForElev(elev);
        ctx.beginPath();
        ctx.arc(cx, cy, rr, 0, Math.PI * 2);
        ctx.stroke();
        if (elev !== 90) {
          ctx.fillStyle = txtMuted;
          ctx.font = '12px system-ui';
          ctx.textAlign = 'left';
          ctx.textBaseline = 'alphabetic';
          ctx.fillText(elev + '°', cx + 6, cy - rr + 14);
        }
      });

      ctx.strokeStyle = grid2;
      ctx.beginPath();
      ctx.moveTo(cx, cy - R); ctx.lineTo(cx, cy + R);
      ctx.moveTo(cx - R, cy); ctx.lineTo(cx + R, cy);
      ctx.stroke();

      ctx.fillStyle = txt;
      ctx.font = '14px system-ui';
      ctx.textAlign = 'center';
      ctx.textBaseline = 'middle';
      ctx.fillText('N', cx, cy - R - 14);
      ctx.fillText('S', cx, cy + R + 14);
      ctx.fillText('W', cx - R - 14, cy);
      ctx.fillText('E', cx + R + 14, cy);

      ctx.textAlign = 'left';
      ctx.textBaseline = 'alphabetic';

      (constellation || []).forEach(s => {
        const az = Number(s.azimuth);
        const elv = Number(s.elevation);
        const snr = Number(s.snr);
        if (!Number.isFinite(az) || !Number.isFinite(elv)) return;

        const theta = (az - 90) * Math.PI / 180;
        const r = ringForElev(clamp(elv, 0, 90));
        const x = cx + r * Math.cos(theta);
        const y = cy + r * Math.sin(theta);

        const size = clamp((Number.isFinite(snr) ? snr : 0) / 6, 2.5, 9);
        const baseColor = (snr > 0) ? pickColor(s.constellation) : '#bbb';

        ctx.beginPath();
        ctx.fillStyle = baseColor;
        ctx.strokeStyle = strokePt;
        ctx.lineWidth = 1;
        ctx.arc(x, y, size, 0, Math.PI * 2);
        ctx.fill();
        ctx.stroke();

        const label = `${canonConstellation(s.constellation)} ${s.prn ?? ''}`.trim();
        ctx.fillStyle = txt;
        ctx.font = '12px system-ui';
        ctx.fillText(label, x + size + 4, y - size - 2);
      });
    }

    // ------- MAP (Leaflet) -------
    let map = null;
    let marker = null;
    let mapInited = false;

    // follow/center state
    let followCenter = true;
    let lastLat = null, lastLon = null;

    // layers (NO dark layer anymore)
    let layerOSM = null;
    let layerTopo = null;
    let layerEsri = null;

    function initMap(lat, lon) {
      layerOSM = L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
        maxZoom: 19, attribution: '&copy; OpenStreetMap contributors'
      });

      layerTopo = L.tileLayer('https://{s}.tile.opentopomap.org/{z}/{x}/{y}.png', {
        maxZoom: 17,
        attribution: 'Map data: &copy; OpenStreetMap contributors, SRTM | Map style: &copy; OpenTopoMap'
      });

      layerEsri = L.tileLayer(
        'https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}', {
          maxZoom: 19, attribution: 'Tiles &copy; Esri'
        }
      );

      map = L.map('map', { zoomControl: false, layers: [layerEsri] }).setView([lat, lon], 16);
      L.control.layers({ 'OSM': layerOSM, 'Topo': layerTopo, 'Satellite': layerEsri }, null, { collapsed: true }).addTo(map);
      marker = L.marker([lat, lon]).addTo(map);
      mapInited = true;

      setTimeout(() => { try { map.invalidateSize(); } catch(e) {} }, 50);
    }

    function updateMap(lat, lon, shouldRecenter) {
      if (!Number.isFinite(lat) || !Number.isFinite(lon)) return;
      if (!mapInited) { initMap(lat, lon); return; }
      marker.setLatLng([lat, lon]);
      if (shouldRecenter) map.setView([lat, lon], map.getZoom(), { animate: false });
    }



    // ------- HISTOGRAM (Highcharts) -------
    let histChart = null;

    function getChartTheme(isNight) {
      if (!isNight) {
        return {
          background: '#ffffff',
          text: '#111',
          grid: 'rgba(0,0,0,.10)',
          axis: 'rgba(0,0,0,.35)',
          tooltipBg: 'rgba(255,255,255,.96)',
          tooltipText: '#111'
        };
      }
      return {
        background: '#161a22',
        text: 'rgba(233,236,239,.92)',
        grid: 'rgba(255,255,255,.10)',
        axis: 'rgba(255,255,255,.35)',
        tooltipBg: 'rgba(22,26,34,.96)',
        tooltipText: 'rgba(233,236,239,.92)'
      };
    }

    // Simple HTML escape for tooltip safety (for Highcharts < v11)
    function escapeHTML(str) {
      return String(str)
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;')
        .replace(/'/g, '&#39;');
    }

    function ensureHistChart() {
      if (histChart) return;

      const t = getChartTheme(isNightMode());

      histChart = Highcharts.chart('histChart', {
        chart: {
          type: 'column',
          backgroundColor: t.background,
          spacing: [10, 10, 10, 10],
          animation: false,
          scrollablePlotArea: {
            minWidth: 700,
            scrollPositionX: 1
          }
        },
        title: { text: null },
        credits: { enabled: false },
        legend: { enabled: false },
        xAxis: {
          type: 'category',
          labels: {
            style: { color: t.text, fontSize: '11px' }
          },
          lineColor: t.axis,
          tickColor: t.axis
        },
        yAxis: {
          title: { text: null },
          min: 0,
          max: 60,
          tickInterval: 10,
          gridLineColor: t.grid,
          labels: { style: { color: t.text } }
        },
        tooltip: {
          backgroundColor: t.tooltipBg,
          style: { color: t.tooltipText },
          formatter: function () {
            const p = this.point || {};
            const c = p.custom || {};
            return `<b>${escapeHTML(this.key)}</b><br>` +
                   `SNR: <b>${this.y}</b><br>` +
                   `Constellation: <b>${escapeHTML(c.constellation || '')}</b>`;
          }
        },
        plotOptions: {
          series: {
            animation: false,
            borderWidth: 0,
            pointWidth: 10,     // fixed bar width
            groupPadding: 0.02,
            pointPadding: 0.05
          },
          column: {
            states: { inactive: { opacity: 1 } }
          }
        },
        series: [{
          name: 'SNR',
          data: []
        }]
      });
    }

    function updateHistTheme() {
      if (!histChart) return;
      const t = getChartTheme(isNightMode());
      histChart.update({
        chart: { backgroundColor: t.background },
        xAxis: {
          labels: { style: { color: t.text, fontSize: '11px' } },
          lineColor: t.axis,
          tickColor: t.axis
        },
        yAxis: {
          gridLineColor: t.grid,
          labels: { style: { color: t.text } }
        },
        tooltip: {
          backgroundColor: t.tooltipBg,
          style: { color: t.tooltipText }
        }
      }, false);
      histChart.redraw(false);
    }

    function updateHistogramChart(constellation) {
      ensureHistChart();

      const sats = (constellation || [])
        .map(s => ({ c: canonConstellation(s.constellation), prn: (s.prn ?? ''), snr: Number(s.snr) }))
        .filter(s => s.prn !== '' && Number.isFinite(s.snr));

      sats.sort((a, b) => (b.snr - a.snr));

      const data = sats.map(s => {
        const label = `${s.c}${s.prn}`;
        const y = clamp(s.snr, 0, 60);
        const col = (s.snr > 0) ? (colorMap[s.c] || '#2b7') : '#bbb';
        return {
          name: label,
          y,
          color: col,
          custom: { constellation: s.c }
        };
      });

      const maxSNR = Math.max(10, ...sats.map(s => s.snr));
      const yMax = Math.ceil(Math.min(60, Math.max(30, maxSNR)) / 10) * 10;

      histChart.update({ yAxis: { max: yMax } }, false);
      histChart.series[0].setData(data, false);
      histChart.redraw(false);
    }

    // ------- Night Theme Toggle (NO dark map tiles) -------
    function applyNightTheme(isNight) {
      document.body.classList.toggle('night', !!isNight);

      const btn = el('btnNight');
      if (btn) {
        btn.className = isNight ? 'btn btn-sm btn-light' : 'btn btn-sm btn-outline-secondary';
        btn.textContent = isNight ? 'Day' : 'Night';
      }

      updateHistTheme();

      // redraw sky plot in correct colors
      // (hist & map update will occur next poll, but we redraw immediately if we have last data)
      if (window.__lastSats) drawSkyPlot(window.__lastSats);
    }

    // ------- UI Update -------
    function updateUI(obj) {
      const g = obj.gnss || {};

      el('lat').textContent   = fmtNum(g.latitude, 6);
      el('lon').textContent   = fmtNum(g.longitude, 6);
      el('alt').textContent   = fmtMaybe(g.altitude, 1, ' m');

      el('fixq').textContent  = (g.fixQuality ?? '—');
      el('svused').textContent= (g.satellitesUsed ?? '—');

      el('hdop').textContent  = (g.hdop ?? '—');
      el('pdop').textContent  = (g.pdop ?? '—');
      el('vdop').textContent  = (g.vdop ?? '—');

      el('geoid').textContent = fmtMaybe(g.geoidHeight, 1, ' m');

      el('dgpsAge').textContent     = (g.dgpsAge ?? '—');
      el('dgpsStation').textContent = (g.dgpsStationID ?? '—');

      el('speed').textContent  = fmtMaybe(g.speed, 3, ' m/s');
      el('course').textContent = fmtMaybe(g.course, 1, '°');
      el('magvar').textContent = fmtMaybe(g.magneticVariation, 1, '°');

      const utcDate = formatUtcFromEpoch(g.epoch) || formatUtcFromNmea(g.date, g.utcTime);
      const utcTimeOnly = utcDate ? utcDate.split(' ')[1] : '—';
      el('utctime').textContent = utcTimeOnly ?? '—';

      el('date').textContent  = formatDateNice(g.date);
      el('epoch').textContent = (g.epoch ?? '—');

      el('tvalid').textContent = (g.timeValid === true) ? 'true' : (g.timeValid === false ? 'false' : '—');
      el('mh').textContent = (g.maidenhead ?? '—');

      el('herr').textContent = fmtMaybe(g.horizontalError, 2, ' m');
      el('verr').textContent = fmtMaybe(g.verticalError, 2, ' m');
      el('perr').textContent = fmtMaybe(g.positionError, 2, ' m');

      const sats = obj.constellation || [];
      window.__lastSats = sats;

      updateConstellationCounts(sats);
      drawSkyPlot(sats);
      updateHistogramChart(sats);

      updateMap(Number(g.latitude), Number(g.longitude), (followCenter || firstFix));
    }

    async function pollOnce() {
      try {
        const r = await fetch('/data', { cache: 'no-store' });
        if (!r.ok) throw new Error('HTTP ' + r.status);
        const obj = await r.json();
        updateUI(obj);
        setStatus(true, 'OK');
      } catch (e) {
        setStatus(false, 'Error: ' + e.message);
      }
    }

    // ------- Wire buttons + persist night mode -------
    (function initUiExtras(){
      const saved = localStorage.getItem('hb9iiuNight');
      const isNight = (saved === '1');



      const btnNight = el('btnNight');
      if (btnNight) {
        btnNight.addEventListener('click', () => {
          const now = !document.body.classList.contains('night');
          localStorage.setItem('hb9iiuNight', now ? '1' : '0');
          applyNightTheme(now);
        });
      }



      // Apply saved theme
      setTimeout(() => applyNightTheme(isNight), 0);
    })();

    window.addEventListener('resize', () => {
      if (mapInited && map) {
        try { map.invalidateSize(); } catch(e) {}
      }

      // Highcharts reflow on resize
      if (histChart) {
        try { histChart.reflow(); } catch(e) {}
      }
      pollOnce();
    });



    pollOnce();
    setInterval(pollOnce, 1000);
  </script>

  <!-- Bootstrap JS (optional). -->
  <script src="https://cdn.jsdelivr.net/npm/bootstrap@5.3.3/dist/js/bootstrap.bundle.min.js"
          integrity="sha384-YvpcrYf0tY3lHB60NNkmXc5s9fDVZLESaAA55NDzOxhy9GkcIdslK1eN7N6jIeHz"
          crossorigin="anonymous"></script>
</body>
</html>
)rawliteral";
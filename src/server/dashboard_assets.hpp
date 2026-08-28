#pragma once

#include <string_view>

namespace kcd2o::server::dashboard_assets
{
	inline constexpr std::string_view index_html = R"DASHBOARD(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <meta name="color-scheme" content="dark">
  <meta name="description" content="KCD2Online dedicated-server telemetry">
  <title>KCD2Online · Server telemetry</title>
  <link rel="stylesheet" href="/assets/dashboard.css">
  <link rel="stylesheet" href="/assets/resources.css">
  <script src="/assets/dashboard.js" defer></script>
</head>
<body>
  <div class="shell">
    <aside class="sidebar">
      <a class="brand" href="#overview" aria-label="KCD2Online dashboard home">
        <span class="brand-mark">K2</span>
        <span><strong>KCD2</strong><small>ONLINE</small></span>
      </a>
      <nav aria-label="Dashboard sections">
        <a class="active" href="#overview"><span>01</span>Overview</a>
        <a href="#network"><span>02</span>Network</a>
        <a href="#performance"><span>03</span>Performance</a>
        <a href="#resources"><span>04</span>Resources</a>
      </nav>
      <div class="sidebar-foot">
        <span class="shield">READ ONLY</span>
        <p>Operational telemetry<br>without admin controls.</p>
      </div>
    </aside>

    <main>
      <header class="topbar">
        <div><span class="eyebrow">DEDICATED SERVER</span><strong id="server-name">Connecting…</strong></div>
        <div class="top-actions">
          <span id="last-update" class="muted">Awaiting telemetry</span>
          <span id="live-state" class="live"><i></i> LIVE</span>
        </div>
      </header>

      <section id="overview" class="hero">
        <div>
          <span class="eyebrow">COMMAND OVERVIEW</span>
          <h1>Every packet.<br><em>One clear signal.</em></h1>
          <p>Real-time health for the authoritative world, tuned to stay out of the game loop's way.</p>
        </div>
        <div class="hero-meta">
          <div><span>WORLD</span><strong id="level">—</strong></div>
          <div><span>UPTIME</span><strong id="uptime">—</strong></div>
          <div><span>BUILD</span><strong id="version">—</strong></div>
        </div>
      </section>

      <section class="metric-grid" aria-label="Key server metrics">
        <article class="metric accent-green">
          <div class="metric-head"><span>PLAYERS ONLINE</span><b>LIVE</b></div>
          <div class="metric-value"><strong id="players">—</strong><small id="players-max">/ —</small></div>
          <div class="capacity"><i id="player-capacity"></i></div>
          <p id="connection-detail">No connection data yet</p>
        </article>
        <article class="metric accent-gold">
          <div class="metric-head"><span>TICK DELIVERY</span><b>CORE</b></div>
          <div class="metric-value"><strong id="tick-rate">—</strong><small> Hz</small></div>
          <div class="capacity"><i id="tick-capacity"></i></div>
          <p><span id="tick-p95">—</span> ms p95 · target <span id="tick-target">—</span> Hz</p>
        </article>
        <article class="metric accent-cyan">
          <div class="metric-head"><span>AVG. LATENCY</span><b>RTT</b></div>
          <div class="metric-value"><strong id="ping">—</strong><small> ms</small></div>
          <div class="sparkline"><canvas id="ping-chart" aria-label="Latency history"></canvas></div>
          <p id="ping-quality">Waiting for peers</p>
        </article>
        <article class="metric accent-red">
          <div class="metric-head"><span>PACKET LOSS</span><b>LINK</b></div>
          <div class="metric-value"><strong id="loss">—</strong><small> %</small></div>
          <div class="sparkline"><canvas id="loss-chart" aria-label="Packet loss history"></canvas></div>
          <p id="loss-quality">Waiting for peers</p>
        </article>
      </section>

      <section id="network" class="panel-grid">
        <article class="panel traffic-panel">
          <div class="panel-head">
            <div><span class="eyebrow">NETWORK ANALYTICS</span><h2>Traffic flow</h2></div>
            <div class="legend"><span class="rx">Inbound</span><span class="tx">Outbound</span></div>
          </div>
          <div class="chart-wrap"><canvas id="traffic-chart" aria-label="Inbound and outbound throughput history"></canvas></div>
          <div class="chart-stats">
            <div><span>INBOUND</span><strong id="rx-rate">—</strong><small>/s</small></div>
            <div><span>OUTBOUND</span><strong id="tx-rate">—</strong><small>/s</small></div>
            <div><span>TOTAL SESSION</span><strong id="total-traffic">—</strong></div>
          </div>
        </article>

        <article class="panel network-health">
          <div class="panel-head"><div><span class="eyebrow">PRESSURE</span><h2>Network health</h2></div></div>
          <div class="health-score"><strong id="health-score">—</strong><span>/ 100</span><i id="health-ring"></i></div>
          <div class="health-list">
            <div><span>Send queue</span><strong id="queue">—</strong></div>
            <div><span>Congestion drops</span><strong id="drops">—</strong></div>
            <div><span>Malformed input</span><strong id="malformed">—</strong></div>
            <div><span>Reliable failures</span><strong id="failures">—</strong></div>
          </div>
        </article>
      </section>

      <section id="performance" class="panel-grid lower">
        <article class="panel performance-panel">
          <div class="panel-head">
            <div><span class="eyebrow">PERFORMANCE</span><h2>Frame budget</h2></div>
            <span class="budget" id="budget">— ms budget</span>
          </div>
          <div class="chart-wrap compact"><canvas id="frame-chart" aria-label="Server tick processing history"></canvas></div>
          <div class="chart-stats four">
            <div><span>CURRENT</span><strong id="tick-current">—</strong><small> ms</small></div>
            <div><span>P95</span><strong id="tick-frame-p95">—</strong><small> ms</small></div>
            <div><span>MESSAGES IN</span><strong id="messages-in">—</strong><small>/s</small></div>
            <div><span>MESSAGES OUT</span><strong id="messages-out">—</strong><small>/s</small></div>
          </div>
        </article>
        <article class="panel lane-panel">
          <div class="panel-head"><div><span class="eyebrow">TRAFFIC LANES</span><h2>Queue distribution</h2></div></div>
          <div id="lanes" class="lanes"><p class="muted">Awaiting lane data…</p></div>
        </article>
      </section>

      <section id="resources" class="panel-grid resource-grid">
        <article class="panel resource-chart-panel">
          <div class="panel-head">
            <div><span class="eyebrow">HOST RESOURCES</span><h2>CPU utilization</h2></div>
            <div class="legend"><span class="server-cpu">Server</span><span class="host-cpu">Host total</span></div>
          </div>
          <div class="chart-wrap"><canvas id="cpu-chart" aria-label="Server process and total host CPU history"></canvas></div>
          <div class="chart-stats four">
            <div><span>SERVER CPU</span><strong id="process-cpu">—</strong><small> %</small></div>
            <div><span>HOST CPU</span><strong id="system-cpu">—</strong><small> %</small></div>
            <div><span>LOGICAL CORES</span><strong id="logical-cores">—</strong></div>
            <div><span>TICK BUDGET</span><strong id="tick-budget-used">—</strong><small> %</small></div>
          </div>
        </article>
        <article class="panel resource-pressure">
          <div class="panel-head"><div><span class="eyebrow">MEMORY</span><h2>Resource pressure</h2></div></div>
          <div class="resource-meters">
            <div class="resource-meter">
              <div><span>SERVER WORKING SET</span><strong id="process-memory">—</strong></div>
              <div class="capacity"><i id="process-memory-capacity"></i></div>
            </div>
            <div class="resource-meter">
              <div><span>HOST MEMORY USED</span><strong id="system-memory-percent">—</strong></div>
              <div class="capacity"><i id="system-memory-capacity"></i></div>
            </div>
          </div>
          <div class="health-list resource-list">
            <div><span>Private memory</span><strong id="private-memory">—</strong></div>
            <div><span>Peak working set</span><strong id="peak-memory">—</strong></div>
            <div><span>Host available</span><strong id="available-memory">—</strong></div>
            <div><span>Process CPU time</span><strong id="process-cpu-time">—</strong></div>
            <div><span>Tick overruns</span><strong id="tick-overruns">—</strong></div>
            <div><span>PID / handles</span><strong id="process-detail">—</strong></div>
          </div>
        </article>
      </section>

      <footer><span>KCD2Online server telemetry</span><span>Local time <b id="clock">—</b></span></footer>
    </main>
  </div>

  <dialog id="auth-dialog">
    <form method="dialog" id="auth-form">
      <span class="brand-mark">K2</span>
      <span class="eyebrow">SECURE TELEMETRY</span>
      <h2>Dashboard access</h2>
      <p>Enter the token from the server's configured token file. It stays in this browser tab only.</p>
      <label for="token">Access token</label>
      <input id="token" name="token" type="password" autocomplete="off" required autofocus>
      <p id="auth-error" class="error" role="alert"></p>
      <button value="default">Connect securely</button>
    </form>
  </dialog>
</body>
</html>)DASHBOARD";

	inline constexpr std::string_view stylesheet = R"DASHBOARD(:root{--bg:#0b0d0c;--surface:#121513;--surface-2:#171b18;--line:#292e2a;--text:#f3f0e8;--muted:#898f89;--green:#a6ed7b;--gold:#e6bd69;--cyan:#66d6dc;--red:#ef786b;--sidebar:216px;font-family:Inter,"Segoe UI",system-ui,sans-serif;color-scheme:dark}*{box-sizing:border-box}html{scroll-behavior:smooth}body{margin:0;background:var(--bg);color:var(--text);min-width:320px}.shell{min-height:100vh}.sidebar{position:fixed;inset:0 auto 0 0;width:var(--sidebar);padding:28px 22px;border-right:1px solid var(--line);background:#0e110f;display:flex;flex-direction:column;z-index:5}.brand{display:flex;align-items:center;gap:12px;color:var(--text);text-decoration:none;letter-spacing:.04em}.brand-mark{display:grid;place-items:center;width:40px;height:40px;background:var(--green);color:#101510;font:800 14px/1 Georgia,serif;clip-path:polygon(50% 0,92% 20%,92% 72%,50% 100%,8% 72%,8% 20%)}.brand strong,.brand small{display:block}.brand strong{font:700 17px/1 Georgia,serif}.brand small{margin-top:4px;color:var(--green);font:700 8px/1 sans-serif;letter-spacing:.35em}.sidebar nav{display:grid;gap:7px;margin-top:76px}.sidebar nav a{padding:13px 12px;color:#777d78;text-decoration:none;font-size:12px;font-weight:650;border:1px solid transparent;transition:.18s ease}.sidebar nav a span{display:inline-block;width:31px;color:#4b514d;font:10px ui-monospace,monospace}.sidebar nav a:hover,.sidebar nav a.active{color:var(--text);background:var(--surface);border-color:var(--line)}.sidebar nav a.active span{color:var(--green)}.sidebar-foot{margin-top:auto}.shield{display:inline-flex;padding:7px 9px;border:1px solid #38543a;color:var(--green);font:700 9px ui-monospace,monospace;letter-spacing:.13em}.sidebar-foot p{color:#626762;font-size:10px;line-height:1.6}main{margin-left:var(--sidebar);padding:0 38px 28px;max-width:1800px}.topbar{height:76px;display:flex;align-items:center;justify-content:space-between;border-bottom:1px solid var(--line)}.topbar>div:first-child{display:flex;gap:16px;align-items:center}.topbar strong{font-size:13px}.eyebrow{color:#707771;font:700 9px/1.4 ui-monospace,monospace;letter-spacing:.18em}.top-actions{display:flex;align-items:center;gap:18px}.muted{color:var(--muted)}.top-actions .muted{font:10px ui-monospace,monospace}.live{display:flex;gap:7px;align-items:center;color:var(--green);font:700 10px ui-monospace,monospace;letter-spacing:.12em}.live i{width:6px;height:6px;border-radius:50%;background:currentColor;box-shadow:0 0 0 5px #a6ed7b15}.live.offline{color:var(--red)}.hero{min-height:260px;padding:49px 0 38px;display:flex;align-items:flex-end;justify-content:space-between;gap:40px}.hero h1{margin:10px 0 13px;font:500 clamp(38px,5vw,66px)/.96 Georgia,serif;letter-spacing:-.045em}.hero h1 em{color:var(--green);font-weight:400}.hero p{margin:0;color:var(--muted);font-size:12px;max-width:530px;line-height:1.7}.hero-meta{min-width:300px;border-left:1px solid var(--line);padding-left:34px}.hero-meta div{display:flex;justify-content:space-between;gap:30px;padding:11px 0;border-bottom:1px solid var(--line)}.hero-meta span,.metric-head span,.chart-stats span,.health-list span{color:#777e78;font:700 9px ui-monospace,monospace;letter-spacing:.13em}.hero-meta strong{font:600 11px ui-monospace,monospace}.metric-grid{display:grid;grid-template-columns:repeat(4,1fr);border:1px solid var(--line)}.metric{position:relative;min-height:194px;padding:22px;border-right:1px solid var(--line);background:var(--surface);overflow:hidden}.metric:last-child{border:0}.metric:before{content:"";position:absolute;inset:0 auto auto 0;width:100%;height:2px;background:var(--accent)}.accent-green{--accent:var(--green)}.accent-gold{--accent:var(--gold)}.accent-cyan{--accent:var(--cyan)}.accent-red{--accent:var(--red)}.metric-head{display:flex;justify-content:space-between}.metric-head b{color:var(--accent);font:700 8px ui-monospace,monospace}.metric-value{margin-top:24px}.metric-value strong{font:500 42px/1 ui-monospace,monospace;letter-spacing:-.07em}.metric-value small{color:#6d736d;font:13px ui-monospace,monospace}.metric p{position:absolute;bottom:17px;margin:0;color:#757b75;font:9px ui-monospace,monospace}.capacity{height:3px;margin-top:18px;background:#272d28}.capacity i{display:block;width:0;height:100%;background:var(--accent);transition:width .4s}.sparkline{height:38px;margin-top:8px}.sparkline canvas,.chart-wrap canvas{width:100%;height:100%}.panel-grid{display:grid;grid-template-columns:minmax(0,2fr) minmax(280px,.7fr);gap:14px;margin-top:14px}.panel{background:var(--surface);border:1px solid var(--line);padding:24px}.panel-head{display:flex;align-items:center;justify-content:space-between}.panel h2{font:500 20px Georgia,serif;margin:4px 0 0}.legend{display:flex;gap:18px;color:var(--muted);font:9px ui-monospace,monospace}.legend span:before{content:"";display:inline-block;width:7px;height:7px;margin-right:7px;background:var(--cyan)}.legend .tx:before{background:var(--green)}.chart-wrap{height:190px;margin-top:18px}.chart-wrap.compact{height:130px}.chart-stats{display:grid;grid-template-columns:repeat(3,1fr);margin-top:16px;border-top:1px solid var(--line)}.chart-stats.four{grid-template-columns:repeat(4,1fr)}.chart-stats div{padding-top:15px}.chart-stats strong{margin-left:8px;font:500 16px ui-monospace,monospace}.chart-stats small{color:var(--muted);font-size:9px}.network-health{position:relative}.health-score{position:relative;display:grid;place-items:center;width:126px;height:126px;margin:25px auto}.health-score strong{font:500 38px/1 ui-monospace,monospace}.health-score span{margin-top:-38px;color:var(--muted);font:9px ui-monospace,monospace}.health-score i{position:absolute;inset:0;border-radius:50%;background:conic-gradient(var(--green) 0deg,#283029 0);mask:radial-gradient(transparent 59%,#000 60%)}.health-list{display:grid;gap:9px}.health-list div{display:flex;justify-content:space-between;border-top:1px solid var(--line);padding-top:9px}.health-list strong{font:500 10px ui-monospace,monospace}.lower{grid-template-columns:minmax(0,1.55fr) minmax(310px,1fr)}.budget{padding:7px 10px;background:#20251f;color:var(--gold);font:9px ui-monospace,monospace}.lanes{margin-top:19px}.lane{display:grid;grid-template-columns:95px 1fr 58px;gap:10px;align-items:center;margin:14px 0}.lane span,.lane b{font:9px ui-monospace,monospace}.lane span{color:#858c85}.lane b{text-align:right;font-weight:500}.lane-track{height:4px;background:#272d28}.lane-track i{display:block;height:100%;min-width:2px;background:var(--green)}footer{display:flex;justify-content:space-between;padding:25px 2px 0;color:#59605a;font:9px ui-monospace,monospace;letter-spacing:.08em}dialog{border:1px solid #343c35;background:#121613;color:var(--text);padding:0;box-shadow:0 30px 100px #000c}dialog::backdrop{background:#070907e8;backdrop-filter:blur(6px)}dialog form{width:min(420px,calc(100vw - 32px));padding:38px;display:grid;gap:13px}dialog h2{font:500 28px Georgia,serif;margin:4px 0}dialog p{margin:0;color:var(--muted);font-size:12px;line-height:1.6}dialog label{margin-top:8px;color:#a9afa9;font:700 9px ui-monospace,monospace;letter-spacing:.1em}dialog input{width:100%;padding:13px;border:1px solid #3a413b;background:#0b0e0c;color:var(--text);font:13px ui-monospace,monospace;outline:none}dialog input:focus{border-color:var(--green)}dialog button{margin-top:8px;padding:13px;border:0;background:var(--green);color:#101510;font-weight:750;cursor:pointer}.error{min-height:18px!important;color:var(--red)!important}.offline-data main>*:not(.topbar){opacity:.68}@media(max-width:1100px){.metric-grid{grid-template-columns:repeat(2,1fr)}.metric:nth-child(2){border-right:0}.metric:nth-child(-n+2){border-bottom:1px solid var(--line)}.panel-grid,.lower{grid-template-columns:1fr}}@media(max-width:740px){:root{--sidebar:0px}.sidebar{position:static;width:auto;height:68px;padding:13px 18px;flex-direction:row;align-items:center;border-right:0;border-bottom:1px solid var(--line)}.sidebar nav{display:none}.sidebar-foot{display:none}main{margin:0;padding:0 16px 22px}.topbar{height:62px}.topbar .eyebrow,.top-actions .muted{display:none}.hero{display:block;padding:36px 0}.hero-meta{margin-top:28px;padding:0;border-left:0}.metric-grid{grid-template-columns:1fr}.metric{border-right:0;border-bottom:1px solid var(--line)!important}.metric:last-child{border-bottom:0!important}.panel{padding:18px}.chart-stats,.chart-stats.four{grid-template-columns:repeat(2,1fr)}.top-actions{gap:10px}footer{display:none}})DASHBOARD";

	inline constexpr std::string_view resource_stylesheet = R"DASHBOARD(.resource-grid{grid-template-columns:minmax(0,1.55fr) minmax(310px,1fr)}.legend .server-cpu:before{background:var(--green)}.legend .host-cpu:before{background:var(--gold)}.resource-meters{display:grid;gap:23px;margin:27px 0 25px}.resource-meter>div:first-child{display:flex;align-items:center;justify-content:space-between;gap:16px}.resource-meter span{color:#777e78;font:700 9px ui-monospace,monospace;letter-spacing:.13em}.resource-meter strong{font:500 15px ui-monospace,monospace}.resource-meter .capacity{margin-top:10px}.resource-meter:first-child .capacity i{background:var(--cyan)}.resource-meter:nth-child(2) .capacity i{background:var(--gold)}.resource-list{gap:8px}.resource-list div{padding-top:8px}@media(max-width:1100px){.resource-grid{grid-template-columns:1fr}})DASHBOARD";

	inline constexpr std::string_view javascript = R"DASHBOARD((()=>{'use strict';
const $=id=>document.getElementById(id),history={ping:[],loss:[],rx:[],tx:[],tick:[],processCpu:[],systemCpu:[]},limit=90;
let token=sessionStorage.getItem('kcd2o-dashboard-token')||'',timer=null,lastOk=0,refresh=1000;
const auth=$('auth-dialog'),form=$('auth-form'),error=$('auth-error');
const text=(id,value)=>{$(id).textContent=value};
const push=(key,value)=>{history[key].push(Number(value)||0);if(history[key].length>limit)history[key].shift()};
const bytes=value=>{let n=Number(value)||0,u='B';for(const next of ['KiB','MiB','GiB','TiB']){if(n<1024)break;n/=1024;u=next}return `${n>=100?n.toFixed(0):n>=10?n.toFixed(1):n.toFixed(2)} ${u}`};
const duration=value=>{let s=Math.max(0,Number(value)||0),d=Math.floor(s/86400);s%=86400;const h=Math.floor(s/3600),m=Math.floor(s%3600/60);return d?`${d}d ${h}h`:`${h}h ${m}m`};
const nice=value=>Number.isFinite(Number(value))?Number(value).toLocaleString(): '—';
function lineChart(id,series,colors,fill=false,fixedMax=0){const c=$(id),rect=c.getBoundingClientRect(),dpr=Math.min(devicePixelRatio||1,2);if(!rect.width||!rect.height)return;c.width=rect.width*dpr;c.height=rect.height*dpr;const x=c.getContext('2d');x.scale(dpr,dpr);const w=rect.width,h=rect.height;x.clearRect(0,0,w,h);x.strokeStyle='#242a25';x.lineWidth=1;for(let i=1;i<4;i++){x.beginPath();x.moveTo(0,h*i/4);x.lineTo(w,h*i/4);x.stroke()}const max=Math.max(fixedMax,1,...series.flat());series.forEach((values,index)=>{if(values.length<2)return;x.beginPath();values.forEach((v,i)=>{const px=i/(limit-1)*w,py=h-(v/max)*(h-5)-2;i?x.lineTo(px,py):x.moveTo(px,py)});x.strokeStyle=colors[index];x.lineWidth=1.6;x.stroke();if(fill){x.lineTo((values.length-1)/(limit-1)*w,h);x.lineTo(0,h);x.closePath();const g=x.createLinearGradient(0,0,0,h);g.addColorStop(0,colors[index]+'2d');g.addColorStop(1,colors[index]+'00');x.fillStyle=g;x.fill()}})}
function health(n){const loss=n.network.packet_loss_percent||0,ping=n.network.average_ping_ms||0,drops=n.network.congestion_drops||0,fail=n.network.reliable_send_failures||0;return Math.max(0,Math.round(100-Math.min(45,loss*8)-Math.min(25,Math.max(0,ping-55)/5)-Math.min(20,drops)-Math.min(10,fail*2)))}
function render(n){const s=n.server,p=n.performance,w=n.network,r=n.resources||{};refresh=n.refresh_interval_ms||1000;lastOk=Date.now();document.body.classList.remove('offline-data');$('live-state').classList.remove('offline');text('server-name',s.name);text('level',s.level_id);text('uptime',duration(s.uptime_seconds));text('version',s.version);text('players',s.players_connected);text('players-max',`/ ${s.max_players}`);$('player-capacity').style.width=`${Math.min(100,s.players_connected/Math.max(1,s.max_players)*100)}%`;text('connection-detail',`${w.connections} active · ${s.pending_connections} pending`);text('tick-rate',p.ticks_per_second.toFixed(1));text('tick-target',s.tick_rate_target);text('tick-p95',p.tick_ms_p95.toFixed(2));$('tick-capacity').style.width=`${Math.min(100,p.ticks_per_second/Math.max(1,s.tick_rate_target)*100)}%`;text('ping',w.average_ping_ms<0?'—':w.average_ping_ms.toFixed(0));text('loss',w.packet_loss_percent.toFixed(2));text('ping-quality',w.average_ping_ms<0?'Waiting for peers':w.average_ping_ms<80?'Healthy round trip':w.average_ping_ms<150?'Elevated latency':'High latency');text('loss-quality',w.connections===0?'Waiting for peers':w.packet_loss_percent<1?'Clean delivery':w.packet_loss_percent<5?'Degraded delivery':'Critical packet loss');text('rx-rate',bytes(w.rx_bytes_per_second));text('tx-rate',bytes(w.tx_bytes_per_second));text('total-traffic',bytes(w.total_rx_bytes+w.total_tx_bytes));text('queue',bytes(w.send_queue_bytes));text('drops',nice(w.congestion_drops));text('malformed',nice(w.malformed_messages));text('failures',nice(w.reliable_send_failures));text('tick-current',p.tick_ms_current.toFixed(2));text('tick-frame-p95',p.tick_ms_p95.toFixed(2));text('messages-in',p.messages_in_per_second.toFixed(1));text('messages-out',p.messages_out_per_second.toFixed(1));text('budget',(p.tick_budget_ms||1000/Math.max(1,s.tick_rate_target)).toFixed(1)+' ms budget');const score=health(n);text('health-score',score);$('health-ring').style.background=`conic-gradient(${score>85?'var(--green)':score>60?'var(--gold)':'var(--red)'} ${score*3.6}deg,#283029 0)`;const lanes=w.lanes||[],maxQueue=Math.max(1,...lanes.map(l=>l.pending_bytes));$('lanes').innerHTML=lanes.map(l=>`<div class="lane"><span>${l.name}</span><div class="lane-track"><i style="width:${Math.max(1,l.pending_bytes/maxQueue*100)}%"></i></div><b>${bytes(l.pending_bytes)}</b></div>`).join('');const processCpu=Number(r.process_cpu_percent)||0,systemCpu=Number(r.system_cpu_percent)||0,totalMemory=Number(r.system_memory_total_bytes)||0,processMemory=Number(r.process_working_set_bytes)||0,memoryPercent=Number(r.system_memory_used_percent)||0,tickBudgetUsed=Number(p.tick_budget_used_percent)||0;text('process-cpu',processCpu.toFixed(1));text('system-cpu',systemCpu.toFixed(1));text('logical-cores',nice(r.logical_processors));text('tick-budget-used',tickBudgetUsed.toFixed(1));text('process-memory',bytes(processMemory));text('system-memory-percent',memoryPercent.toFixed(1)+' %');text('private-memory',bytes(r.process_private_bytes));text('peak-memory',bytes(r.process_peak_working_set_bytes));text('available-memory',`${bytes(r.system_memory_available_bytes)} / ${bytes(totalMemory)}`);text('process-cpu-time',duration(r.process_cpu_seconds));text('tick-overruns',nice(p.tick_budget_overruns));text('process-detail',`${r.process_id||'—'} / ${nice(r.handle_count)}`);$('process-memory-capacity').style.width=`${Math.min(100,processMemory/Math.max(1,totalMemory)*100)}%`;$('system-memory-capacity').style.width=`${Math.min(100,memoryPercent)}%`;push('ping',Math.max(0,w.average_ping_ms));push('loss',w.packet_loss_percent);push('rx',w.rx_bytes_per_second);push('tx',w.tx_bytes_per_second);push('tick',p.tick_ms_current);push('processCpu',processCpu);push('systemCpu',systemCpu);draw();text('last-update','Updated just now')}
function draw(){lineChart('ping-chart',[history.ping],['#66d6dc']);lineChart('loss-chart',[history.loss],['#ef786b']);lineChart('traffic-chart',[history.rx,history.tx],['#66d6dc','#a6ed7b'],true);lineChart('frame-chart',[history.tick],['#e6bd69'],true);lineChart('cpu-chart',[history.processCpu,history.systemCpu],['#a6ed7b','#e6bd69'],false,100)}
async function load(){try{const headers=token?{Authorization:`Bearer ${token}`}:{},response=await fetch('/api/v1/snapshot',{headers,cache:'no-store'});if(response.status===401){error.textContent=token?'That token was not accepted.':'';token='';sessionStorage.removeItem('kcd2o-dashboard-token');if(!auth.open)auth.showModal();return}if(!response.ok)throw new Error(`HTTP ${response.status}`);render(await response.json())}catch(e){if(Date.now()-lastOk>refresh*2){document.body.classList.add('offline-data');$('live-state').classList.add('offline');text('last-update','Telemetry unavailable')}}finally{clearTimeout(timer);timer=setTimeout(load,document.hidden?Math.max(refresh,5000):refresh)}}
form.addEventListener('submit',event=>{event.preventDefault();token=$('token').value.trim();if(!token)return;sessionStorage.setItem('kcd2o-dashboard-token',token);error.textContent='';auth.close();load()});document.addEventListener('visibilitychange',()=>{if(!document.hidden){clearTimeout(timer);load()}});addEventListener('resize',draw,{passive:true});setInterval(()=>text('clock',new Date().toLocaleTimeString([], {hour:'2-digit',minute:'2-digit',second:'2-digit'})),1000);load();
})();)DASHBOARD";
}

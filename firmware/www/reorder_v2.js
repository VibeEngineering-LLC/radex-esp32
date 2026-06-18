// reorder_v2.js — пост-рендер группировка строк Web UI на web_server v2 + лимит лога.
// Таблица v2 находится в shadow DOM: <esp-app>.shadowRoot ▸ <esp-entity-table>.shadowRoot ▸ <table>.
// Лог <esp-log> — тоже custom-element с собственным shadowRoot, внешний CSS не пробивает.
// Lit перерисовывает <tbody> при каждом state-update — поэтому MutationObserver + поллинг.
//
// 4 группы (порядок):
//   1. Прибор   — радон / температура / влажность
//   2. Народмон — switch / select / 3 метрики / кнопка
//   3. Сигналы  — BLE/WiFi/IP/MAC/API + сервисные кнопки сети
//   4. Система  — uptime / safe mode / factory reset

(function () {
  'use strict';

  var GROUPS = [
    { title: 'Прибор',   patterns: [/радон/i, /Температура/i, /Влажность/i] },
    { title: 'Народмон', patterns: [/Народмон/i, /Выгружать/i, /^R1$/, /^D1$/, /^T1$/, /^BAT1$/] },
    { title: 'Сигналы',  patterns: [/BLE/i, /WiFi/i, /IP-адрес/, /MAC/, /API/i, /Сбросить счётчики/i, /реконнект/i, /SSID/i, /BSSID/i] },
    { title: 'Система',  patterns: [/Время работы/i, /Safe Mode/i, /Factory Reset/i, /Uptime/i, /Перезагрузить/i] }
  ];

  function findGroup(name) {
    for (var i = 0; i < GROUPS.length; i++) {
      for (var j = 0; j < GROUPS[i].patterns.length; j++) {
        if (GROUPS[i].patterns[j].test(name)) return i;
      }
    }
    return GROUPS.length;
  }

  function findEntityTbody() {
    var espApp = document.querySelector('esp-app');
    if (!espApp || !espApp.shadowRoot) return null;
    var ent = espApp.shadowRoot.querySelector('esp-entity-table');
    if (!ent || !ent.shadowRoot) return null;
    var tbl = ent.shadowRoot.querySelector('table');
    if (!tbl) return null;
    return (tbl.tBodies && tbl.tBodies[0]) ? tbl.tBodies[0] : tbl;
  }

  function rowName(tr) {
    var c = tr.cells && tr.cells[0];
    return c ? (c.textContent || '').trim() : '';
  }

  var inFlight = false;
  var lastSig = null;

  function signature(tbody) {
    var s = [];
    for (var i = 0; i < tbody.children.length; i++) {
      var tr = tbody.children[i];
      s.push((tr.getAttribute('data-radex-group') || 'r') + ':' + rowName(tr));
    }
    return s.join('|');
  }

  function reorder() {
    if (inFlight) return false;
    var tbody = findEntityTbody();
    if (!tbody) return false;
    var rows = Array.prototype.slice.call(tbody.children).filter(function (n) {
      return n.tagName === 'TR' && !n.hasAttribute('data-radex-group');
    });
    if (!rows.length) return false;

    var bins = [];
    for (var g = 0; g <= GROUPS.length; g++) bins.push([]);
    rows.forEach(function (tr) {
      var n = rowName(tr);
      bins[n ? findGroup(n) : GROUPS.length].push(tr);
    });

    var flat = [];
    for (var k = 0; k <= GROUPS.length; k++) {
      bins[k].forEach(function (tr) { flat.push(tr); });
    }
    var allGood = true;
    for (var x = 0; x < flat.length; x++) {
      if (flat[x] !== rows[x]) { allGood = false; break; }
    }
    var hasHdrs = !!tbody.querySelector('tr[data-radex-group]');
    if (allGood && hasHdrs) return true;

    inFlight = true;
    try {
      Array.prototype.slice.call(tbody.querySelectorAll('tr[data-radex-group]')).forEach(function (h) { h.remove(); });
      var ncols = rows[0].cells ? Math.max(1, rows[0].cells.length) : 3;
      while (tbody.firstChild) tbody.removeChild(tbody.firstChild);
      for (var i = 0; i <= GROUPS.length; i++) {
        if (!bins[i].length) continue;
        var title = (i < GROUPS.length) ? GROUPS[i].title : 'Прочее';
        var hdr = document.createElement('tr');
        hdr.setAttribute('data-radex-group', String(i));
        var td = document.createElement('td');
        td.colSpan = ncols;
        td.textContent = title;
        td.style.cssText = 'background:#1976d2;color:#fff;font-weight:bold;padding:6px 10px;font-size:14px;border-top:2px solid #0d47a1;border-bottom:1px solid #0d47a1;text-align:left';
        hdr.appendChild(td);
        tbody.appendChild(hdr);
        bins[i].forEach(function (tr) { tbody.appendChild(tr); });
      }
      lastSig = signature(tbody);
    } finally {
      Promise.resolve().then(function () { inFlight = false; });
    }
    return true;
  }

  function scheduleReorder() {
    if (scheduleReorder._raf) return;
    scheduleReorder._raf = requestAnimationFrame(function () {
      scheduleReorder._raf = 0;
      try { reorder(); } catch (e) { /* swallow */ }
    });
  }

  function attachObservers() {
    var tbody = findEntityTbody();
    if (!tbody) return false;
    if (attachObservers._attached === tbody) return true;
    attachObservers._attached = tbody;
    try {
      var mo = new MutationObserver(function () {
        if (inFlight) return;
        if (signature(tbody) === lastSig) return;
        scheduleReorder();
      });
      mo.observe(tbody, { childList: true });
    } catch (e) { /* ignore */ }
    scheduleReorder();
    return true;
  }

  // ── Лимит высоты Debug Log ──
  // <esp-log> — custom element с собственным shadowRoot. Внешний CSS не пробивает,
  // инжектим inline style на host + на содержимое shadowRoot.
  function limitLogHeight() {
    var espApp = document.querySelector('esp-app');
    if (!espApp || !espApp.shadowRoot) return false;
    var espLog = espApp.shadowRoot.querySelector('esp-log');
    if (!espLog) return false;
    if (espLog.style.maxHeight !== '240px') {
      espLog.style.display = 'block';
      espLog.style.maxHeight = '240px';
      espLog.style.overflowY = 'auto';
    }
    var sr = espLog.shadowRoot;
    if (sr) {
      var inner = sr.querySelector('textarea, pre, code, div.log, .log');
      if (inner && inner.style.maxHeight !== '240px') {
        inner.style.maxHeight = '240px';
        inner.style.height = '240px';
        inner.style.overflowY = 'auto';
        inner.style.resize = 'vertical';
      }
    }
    return true;
  }

  function init() {
    function loop() {
      try {
        if (attachObservers()) scheduleReorder();
        limitLogHeight();
      } catch (e) { /* ignore */ }
    }
    loop();
    setInterval(loop, 2500);
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }
})();
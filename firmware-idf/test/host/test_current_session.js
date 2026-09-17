// #RADEX-285: from ряда «текущая сессия». Функция берётся из web/index.src.html как есть.
// Запуск: node test/host/test_current_session.js  -> exit code = число провалов.
const fs = require('fs'), path = require('path');
const src = fs.readFileSync(path.join(__dirname, '../../web/index.src.html'), 'utf8');
const m = src.match(/function currentSessionFrom\([\s\S]*?\n}/);
if (!m) { console.log('RED   функция currentSessionFrom не найдена'); process.exit(1); }
const currentSessionFrom = new Function(m[0] + '; return currentSessionFrom;')();
const now = 1789600000;   // «сейчас» для всех случаев
const cases = [
  ['активный замер есть', [{start: 1788000000, active: false}, {start: 1789590000, active: true}], {from: 1789590000, label: 'текущий замер'}],
  ['активного нет',       [{start: 1788000000, active: false}],                                    {from: 0, label: 'вся история платы'}],
  ['start в будущем',     [{start: now + 3600, active: true}],                                     {from: 0, label: 'вся история платы'}],
];
let fail = 0;
for (const [name, tests, want] of cases) {
  const got = currentSessionFrom(tests, now);
  const ok = got.from === want.from && got.label === want.label;
  if (!ok) fail++;
  console.log(`${ok ? 'GREEN' : 'RED  '} ${name}: from=${got.from} label="${got.label}"`);
}
// #RADEX-286: история содержит точки ДО start — среднее и число измерений только по точкам после start
const hb = src.match(/function historyPointsSince\([\s\S]*?function historyWeightedMean\([\s\S]*?\n}/);
const H = hb ? new Function(hb[0] + '; return {historyPointsSince, historyMeasurements, historyWeightedMean};')() : null;
const hist = [{t: 1788000000, r: 300, w: 60}, {t: 1789589000, r: 200}, {t: 1789590000, r: 100}, {t: 1789590600, r: 120, w: 3}];
const since = H ? H.historyPointsSince(hist, 1789590000) : [];
const hOk = !!H && since.length === 2 && H.historyMeasurements(since) === 4 && H.historyWeightedMean(since) === 115;
if (!hOk) fail++;
console.log(`${hOk ? 'GREEN' : 'RED  '} история с точками до start: точек=${since.length} измерений=${H ? H.historyMeasurements(since) : '-'} среднее=${H ? H.historyWeightedMean(since) : '-'}`);
// #RADEX-268/271: числа и формулировки из документа Цапалова 2 (п.2, п.5)
const fn = name => new Function(src.match(new RegExp('function ' + name + '\\([\\s\\S]*?\\n}'))[0] + '; return ' + name + ';')();
const annualRange = fn('annualRange'), concl = fn('methodConclusionLines');
const rg = annualRange({verdict: 'exceeds', c: 326, crit1: 425});
const t = [
  ['диапазон 326 ± 99, границы 227…425', rg && rg.half === 99 && rg.lower === 227 && rg.upper === 425],
  ['нижняя граница не менее 0', annualRange({verdict: 'uncertain', c: 50, crit1: 130}).lower === 0],
  ['итог п.5 при 341 сут (дословно)', JSON.stringify(concl({verdict: 'exceeds', days: 341, kp: 1.05, crit2: 250, c_rl: 200, restricted: false}, 300).lines)
     === JSON.stringify(['1. Критерий (1) не выполнен при продолжительности теста более 10* месяцев,', '2. Критерий (2) выполнен,', 'Поэтому помещение не соответствует нормативу, согласно §7.1.2.'])],
  ['297 сут (9,9 мес) — без «более 10 месяцев» и сноски', (c => !c.lines[0].includes('10*') && c.note === '')(concl({verdict: 'uncertain', days: 297, kp: 1.1, crit2: 150, c_rl: 200}, 300))],
  // аудит 267 D1: плата решила crit2 > c_rl по точному 200.04, в JSON округлено до 200.0
  ['D1: критерий (2) по флагу платы crit2_met, не по округлённому числу', concl({verdict: 'exceeds', days: 150, kp: 1.2, crit2: 200.0, crit2_met: true, c_rl: 200, restricted: false}, 300).lines[1] === '2. Критерий (2) выполнен,'],
  // D2: ограниченный режим — Kp не задан (-1)
  ['D2: ограниченный режим — критерий (2) не определён', concl({verdict: 'uncertain', days: 20, kp: -1, crit2: 0, crit2_met: false, c_rl: 200, restricted: true}, 300).lines[1] === '2. Критерий (2) не определён,'],
  ['300 сут (10,0 мес) — со сноской', concl({verdict: 'exceeds', days: 300, kp: 1.09, crit2: 150, c_rl: 200}, 300).note.startsWith('*целесообразно')],
];
// #RADEX-287: боевой образец — старый прибор/режим 12 промежутков по 3600 с, после start нового замера 5 по 600 с
const cycleFromPoints = new Function(src.match(/function cycleFromPoints\([\s\S]*?\n    }/)[0] + '; return cycleFromPoints;')();
const cycPts = [], cycStart = 1789600000;
for (let k = 12; k >= 0; k--) cycPts.push({t: cycStart - k * 3600, r: 100});
for (let k = 1; k <= 5; k++) cycPts.push({t: cycStart + k * 600, r: 100 + k});
const cyc = cycleFromPoints(cycPts, cycStart);
t.push(['#287: цикл с start замера — 10 мин, а не 60 из прежних промежутков', cyc.minutes === 10 && cyc.samples === 5 && cyc.stable]);
// #RADEX-290: продолжительность теста на границах суток
const fmtDur = new Function(fn('formatSpanMDH').toString() + src.match(/function formatTestDuration\([\s\S]*?\n}/)[0] + '; return formatTestDuration;')();
t.push(['#290: 16,5 ч → «16 ч»', fmtDur(16.5 * 3600) === '16 ч']);
t.push(['#290: 23 ч 59 мин → «23 ч»', fmtDur(23 * 3600 + 59 * 60) === '23 ч']);
t.push(['#290: ровно 24 ч → «1 сут (1 сут 0 ч)»', fmtDur(86400) === '1 сут (1 сут 0 ч)']);
t.push(['#290: 341 сут 16 ч → «341 сут (11 мес 11 сут 16 ч)»', fmtDur(341 * 86400 + 16 * 3600) === '341 сут (11 мес 11 сут 16 ч)']);
// #RADEX-291: текст подтверждения «Начало теста» называет, что тест уже идёт
const startConfirmMessage = new Function(src.match(/function startConfirmMessage\([\s\S]*?\n}/)[0] + '; return startConfirmMessage;')();
t.push(['#291: замер по умолчанию — сообщает, что уже идёт', startConfirmMessage({started: true, finished: false, explicit: false, start: 1789577734}).startsWith('Тест уже идёт')]);
t.push(['#291: замер не начат — обычный текст без «уже идёт»', !startConfirmMessage({started: false}).includes('уже идёт')]);
t.push(['#291: тест завершён — обычный текст (новый тест это не рестарт)', !startConfirmMessage({started: true, finished: true}).includes('уже идёт')]);
// #RADEX-292: журнал прибора — реальные записи из Захвата 3 (reports/radex-ble-sniff-app-2026-09-16.md):
// №3 time_raw=0x323da9a9, ОА=125.21, T×10=263; №2 time_raw=0x323da751, ОА=148.49, T×10=264; RH обеих 42.
const journalFns = new Function(fn('journalStatusText').toString() + fn('journalRecordRows').toString()
  + '; return {journalStatusText, journalRecordRows};')();
const jRecs = [{number: 3, time_raw: 0x323da9a9, oa: 125.21, t_x10: 263, humidity: 42},
               {number: 2, time_raw: 0x323da751, oa: 148.49, t_x10: 264, humidity: 42}];
const jRows = journalFns.journalRecordRows(jRecs);
t.push(['#292: время — секунды от самой ранней записи (не дата)', jRows[0].t === 600 && jRows[1].t === 0]);
t.push(['#292: ОА округлена, T с одним знаком, RH как есть', jRows[0].oa === 125 && jRows[0].temp === '26.3' && jRows[0].rh === 42]);
t.push(['#292: ok без truncated — «прочитан полностью»', journalFns.journalStatusText({valid: true, ok: true}) === 'прочитан полностью']);
t.push(['#292: ok truncated — упоминает 64 записи', journalFns.journalStatusText({valid: true, ok: true, truncated: true}).indexOf('64') >= 0]);
t.push(['#292: sequence mismatch — не выдумывает «ok»', journalFns.journalStatusText({valid: true, ok: false, seq_mismatch: true, status: 'sequence mismatch'}).indexOf('последовательности') >= 0]);
t.push(['#292: mtu N < 27 — «MTU слишком мал»', journalFns.journalStatusText({valid: true, ok: false, status: 'mtu 23 < 27'}).indexOf('MTU') >= 0]);
t.push(['#292: busy — сеанс идёт, не путается со старым результатом', journalFns.journalStatusText({busy: true, valid: true, ok: true}) === 'сеанс идёт…']);
t.push(['#292: нет записей — таблица не рисуется', journalFns.journalRecordRows([]).length === 0 && journalFns.journalRecordRows(null).length === 0]);
// #RADEX-293: кнопка «Сохранить N точек» — русское склонение (1/2-4/5-20/21…)
const jSave = new Function(fn('ruCount').toString() + fn('journalSaveButtonText').toString()
  + '; return {ruCount, journalSaveButtonText};')();
t.push(['#293: 0 точек', jSave.journalSaveButtonText(0) === 'Сохранить 0 точек в историю']);
t.push(['#293: 1 точка → "точку"', jSave.journalSaveButtonText(1) === 'Сохранить 1 точку в историю']);
t.push(['#293: 2 точки → "точки"', jSave.journalSaveButtonText(2) === 'Сохранить 2 точки в историю']);
t.push(['#293: 5 точек → "точек"', jSave.journalSaveButtonText(5) === 'Сохранить 5 точек в историю']);
t.push(['#293: 21 точка → "точку" (не "точек")', jSave.journalSaveButtonText(21) === 'Сохранить 21 точку в историю']);
t.push(['#293: 11 точек → "точек" (искл. 11-14)', jSave.journalSaveButtonText(11) === 'Сохранить 11 точек в историю']);
for (const [name, ok] of t) { if (!ok) fail++; console.log(`${ok ? 'GREEN' : 'RED  '} ${name}`); }
console.log(`итого (js): красных ${fail} из ${cases.length + 1 + t.length}`);
process.exit(fail);

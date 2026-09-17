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
for (const [name, ok] of t) { if (!ok) fail++; console.log(`${ok ? 'GREEN' : 'RED  '} ${name}`); }
console.log(`итого (js): красных ${fail} из ${cases.length + 1 + t.length}`);
process.exit(fail);

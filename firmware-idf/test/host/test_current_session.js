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
console.log(`итого (js): красных ${fail} из ${cases.length}`);
process.exit(fail);

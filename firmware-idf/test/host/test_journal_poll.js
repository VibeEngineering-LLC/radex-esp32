// #RADEX-294 (O4): опрос /api/journal — каждый тик только в сеансе, иначе раз в 10 тиков.
// Запуск: node test/host/test_journal_poll.js [путь к index.src.html] -> exit code = число провалов.
const fs = require('fs'), path = require('path');
const src = fs.readFileSync(process.argv[2] || path.join(__dirname, '../../web/index.src.html'), 'utf8');
// #SA-верификация 17.09 (D5): нежадный [\s\S]*? тут захватывал 12 строк вместо
// одной — граница до первого '}' в ТОЙ ЖЕ строке жёстче и не съедает соседний
// многострочный journalTick.
const m = src.match(/function journalTickDue\([^)]*\)\s*\{[^}]*\}/);
if (!m) { console.log('RED   функция journalTickDue не найдена'); process.exit(1); }
const due = new Function(m[0] + '; return journalTickDue;')();
const idle = n => { let c = 0; for (let i = 0; i < n; i++) if (due(false, i)) c++; return c; };
const busy = n => { let c = 0; for (let i = 0; i < n; i++) if (due(true, i)) c++; return c; };
const cases = [
  ['в покое за 30 тиков (90 с) — 3 запроса', idle(30) === 3],
  ['первый тик после открытия вкладки не пропущен', due(false, 0) === true],
  ['в покое тики 1..9 пропущены', [1,2,3,4,5,6,7,8,9].every(i => due(false, i) === false)],
  ['в сеансе — каждый тик', busy(30) === 30],
  // применение (D5): гейт реально стоит внутри journalTick, не только существует как функция
  ['journalTick вызывает гейт периодичности', src.includes('if (!force && !journalTickDue(journalActive, journalTickN++)) return;')],
  ['journalTick не запускает fetch поверх идущего (R1)', src.includes('if (journalFetching) return;')],
  // D1/D4 — кнопки; R4 — открытие вкладки идёт через diagTick(true) -> journalTick(forceJournal)
  ['кнопки форсируют тик в обход гейта (D1/D4)', (src.match(/journalTick\(true\)/g) || []).length >= 2],
  ['открытие вкладки форсирует тик в обход гейта (R4)', src.includes('diagTick(true)') && src.includes('journalTick(forceJournal)')],
];
let fail = 0;
for (const [name, ok] of cases) { if (!ok) fail++; console.log(`${ok ? 'GREEN' : 'RED  '} ${name}`); }
console.log(`журнал: провалов ${fail}`);
process.exit(fail);

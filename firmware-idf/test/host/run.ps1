# #RADEX-281: host tests of the NUS journal (gcc from espressif/idf:v6.1 image).
# Usage: pwsh -File test/host/run.ps1   -> exit code = number of failed test binaries (0 = all green)
$fw = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
# core.autocrlf=true gives this file CRLF, and the here-string then carries CR into bash inside docker
# ("-Imain<CR>": no test binary compiles, exit code 2 looks like "2 failed"). CR is stripped before docker.
$body = @'
set -u; f=0; n=0; W="-std=c99 -Wall -Wextra -Werror -Imain"
t() { o=$1; a=$2; shift 2; if gcc $W -o /tmp/$o "$@" -lm; then n=$((n+1)); /tmp/$o $a || f=$((f+1)); else echo "COMPILE FAILED: $o"; f=$((f+1)); fi; }
t tp "" test/host/test_journal_parse.c main/radex_journal_parse.c
t tt "" test/host/test_journal_track.c main/radex_journal_track.c main/radex_journal_parse.c
t tl "" test/host/test_target_label.c main/target_label.c
t tm "" test/host/test_radon_method.c
t tu "" test/host/test_label_utf8.c main/label_utf8.c
t tg "" test/host/test_radon_test_guard.c
t tjc "" test/host/test_journal_calib.c
t thc "" test/host/test_http_cache.c
t tue test/host/radex_usb_vectors.txt test/host/test_radex_ekosf.c main/radex_ekosf.c
t tus test/host/radex_usb_vectors.txt test/host/test_radex_sessions.c main/radex_ekosf.c
echo "C-TESTS: built=$n failed=$f"
[ $n -gt 0 ] || { echo "FATAL: no C test binary was built (CRLF in the script? gcc missing?)"; exit 99; }
exit $f
'@ -replace "`r", ""
docker run --rm --entrypoint bash -v "${fw}:/p" -w /p espressif/idf:v6.1 -c $body
$f = $LASTEXITCODE
if ($f -eq 99) { Write-Host "FATAL: host C tests did not build at all - see message above"; exit 99 }
# R7: JS host tests (page-source tests) run on the host node, not in the docker image.
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
node "$here/test_current_session.js" | Out-Host; if ($LASTEXITCODE -ne 0) { $f += $LASTEXITCODE }
node "$here/test_journal_poll.js" | Out-Host; if ($LASTEXITCODE -ne 0) { $f += $LASTEXITCODE }
exit $f

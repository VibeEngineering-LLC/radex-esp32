# #RADEX-281: host tests of the NUS journal (gcc from espressif/idf:v6.1 image).
# Usage: pwsh -File test/host/run.ps1   -> exit code = number of failed test binaries (0 = all green)
$fw = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
docker run --rm --entrypoint bash -v "${fw}:/p" -w /p espressif/idf:v6.1 -c @'
set -u; f=0; W="-std=c99 -Wall -Wextra -Werror -Imain"
gcc $W -o /tmp/tp test/host/test_journal_parse.c main/radex_journal_parse.c -lm && /tmp/tp || f=$((f+1))
gcc $W -o /tmp/tt test/host/test_journal_track.c main/radex_journal_track.c main/radex_journal_parse.c -lm && /tmp/tt || f=$((f+1))
gcc $W -o /tmp/tl test/host/test_target_label.c main/target_label.c && /tmp/tl || f=$((f+1))
gcc $W -o /tmp/tm test/host/test_radon_method.c && /tmp/tm || f=$((f+1))
gcc $W -o /tmp/tu test/host/test_label_utf8.c main/label_utf8.c && /tmp/tu || f=$((f+1))
gcc $W -o /tmp/tg test/host/test_radon_test_guard.c && /tmp/tg || f=$((f+1))
gcc $W -o /tmp/tjc test/host/test_journal_calib.c && /tmp/tjc || f=$((f+1))
gcc $W -o /tmp/thc test/host/test_http_cache.c && /tmp/thc || f=$((f+1))
gcc $W -o /tmp/tue test/host/test_radex_ekosf.c main/radex_ekosf.c -lm && /tmp/tue test/host/radex_usb_vectors.txt || f=$((f+1))
gcc $W -o /tmp/tus test/host/test_radex_sessions.c main/radex_ekosf.c -lm && /tmp/tus test/host/radex_usb_vectors.txt || f=$((f+1))
exit $f
'@
$f = $LASTEXITCODE
# R7: JS host tests (page-source tests) run on the host node, not in the docker image.
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
node "$here/test_current_session.js" | Out-Host; if ($LASTEXITCODE -ne 0) { $f += $LASTEXITCODE }
node "$here/test_journal_poll.js" | Out-Host; if ($LASTEXITCODE -ne 0) { $f += $LASTEXITCODE }
exit $f

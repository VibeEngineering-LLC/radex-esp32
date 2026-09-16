# #RADEX-281: host tests of the NUS journal (gcc from espressif/idf:v6.1 image).
# Usage: pwsh -File test/host/run.ps1   -> exit code = number of failed test binaries (0 = all green)
$fw = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
docker run --rm --entrypoint bash -v "${fw}:/p" -w /p espressif/idf:v6.1 -c @'
set -u; f=0; W="-std=c99 -Wall -Wextra -Werror -Imain"
gcc $W -o /tmp/tp test/host/test_journal_parse.c main/radex_journal_parse.c -lm && /tmp/tp || f=$((f+1))
gcc $W -o /tmp/tt test/host/test_journal_track.c main/radex_journal_track.c main/radex_journal_parse.c -lm && /tmp/tt || f=$((f+1))
exit $f
'@
exit $LASTEXITCODE

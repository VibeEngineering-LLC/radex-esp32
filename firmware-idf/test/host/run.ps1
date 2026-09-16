# #RADEX-281: host test of the NUS journal parser (gcc from espressif/idf:v6.1 image).
# Usage: pwsh -File test/host/run.ps1   -> exit code = test result (0 = all green)
$fw = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
docker run --rm --entrypoint bash -v "${fw}:/p" -w /p espressif/idf:v6.1 -c `
  "gcc -std=c99 -Wall -Wextra -Werror -Imain -o /tmp/t test/host/test_journal_parse.c main/radex_journal_parse.c -lm && /tmp/t"
exit $LASTEXITCODE

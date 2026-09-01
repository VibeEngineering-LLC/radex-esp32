# Обёртка: собирает build.sh в Docker (путь проекта с кириллицей, поэтому
# PowerShell, а не Git Bash — тот подменяет /project).
$p = Split-Path -Parent $MyInvocation.MyCommand.Path
docker run --rm -v "${p}:/project" -w /project espressif/idf:v6.1 bash /project/build.sh

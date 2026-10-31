param([switch]$Clean)
$ErrorActionPreference = 'Stop'

$Build = Join-Path $PSScriptRoot 'build'
$Flags = @('-O2', '-Wall', '-DHAVE_CONFIG_H', '-Iwin', '-Ilib', '-Isrc')
$Sources = @('src\grep.c', 'src\dfasearch.c', 'src\kwsearch.c',
             'src\kwset.c', 'src\searchutils.c', 'win\compat.c')
# Add the lib\*.c files listed in build.log

if ($Clean) { Remove-Item $Build -Recurse -Force -ErrorAction Ignore }
New-Item -ItemType Directory -Force $Build | Out-Null

$objects = foreach ($src in $Sources) {
    $obj = Join-Path $Build (($src -replace '\\', '_') -replace '\.c$', '.o')
    $stale = -not (Test-Path $obj) -or
             (Get-Item $src).LastWriteTime -gt (Get-Item $obj).LastWriteTime
    if ($stale) {
        Write-Host "CC $src"
        & gcc @Flags -c $src -o $obj
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    $obj
}

$Lib = 'lib\libgreputils.a'
& gcc @objects $Lib $Lib -liconv -lntdll -static '-Wl,--stack,8388608' -o (Join-Path $Build 'grep.exe')
if ($LASTEXITCODE -ne 0) { Write-Host "Link failed" -ForegroundColor Red; exit $LASTEXITCODE }
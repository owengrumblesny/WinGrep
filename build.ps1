param([switch]$Clean)
$ErrorActionPreference = 'Stop'

Push-Location $PSScriptRoot
try {
    $Build = 'build'
    $Flags = @('-O2', '-DHAVE_CONFIG_H', '-Iwin', '-Ilib', '-Isrc')

    # Our code: warnings on.
    $Sources = @('src\grep.c', 'src\dfasearch.c', 'src\kwsearch.c',
                 'src\kwset.c', 'src\searchutils.c', 'win\compat.c', 'win\fts.c')

    # Inherited gnulib code: warnings off.
    $LibSources = @(
        'argmatch', 'basename-lgpl', 'c-strcasecmp', 'calloc', 'chdir-long',
        'cloexec', 'close', 'close-stream', 'closeout', 'dfa', 'dup-safer',
        'dup2', 'error', 'exclude', 'exitfail', 'fchdir', 'fcntl', 'fd-hook',
        'fd-safer', 'filenamecat-lgpl', 'fnmatch', 'fopen', 'fpending', 'free',
        'fstat', 'fstatat', 'getcwd-lgpl', 'getdtablesize', 'getprogname',
        'hash', 'localcharset', 'localeconv', 'localeinfo', 'lseek', 'malloc',
        'malloc\dynarray_resize', 'malloca', 'mbchar', 'mbrlen', 'mbrtowc',
        'mbscasecmp', 'mbsinit', 'memchr2', 'memrchr', 'msvc-inval',
        'msvc-nothrow', 'nl_langinfo', 'obstack', 'open', 'open-safer',
        'openat', 'openat-die', 'openat-proc', 'openat-safer', 'quotearg',
        'rawmemchr', 'read', 'realloc', 'reallocarray', 'regex', 'safe-read',
        'save-cwd', 'setlocale_null', 'stat', 'stat-w32', 'stpcpy', 'strdup',
        'strerror', 'strerror-override', 'stripslash', 'strnlen1', 'strstr',
        'uniwidth\width', 'version-etc', 'version-etc-fsf', 'wcrtomb',
        'wcwidth', 'xalloc-die', 'xbinary-io', 'xmalloc', 'xstrtoimax'
    ) | ForEach-Object { "lib\$_.c" }

    if ($Clean) { Remove-Item $Build -Recurse -Force -ErrorAction Ignore }
    New-Item -ItemType Directory -Force $Build | Out-Null

    function Build-Object ($src, $extra) {
        $obj = Join-Path $Build (($src -replace '\\', '_') -replace '\.c$', '.o')
        $stale = -not (Test-Path $obj) -or
                 (Get-Item $src).LastWriteTime -gt (Get-Item $obj).LastWriteTime
        if ($stale) {
            Write-Host "CC $src"
            & gcc @Flags @extra -c $src -o $obj
            if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        }
        $obj
    }

    $objects = @($Sources | ForEach-Object { Build-Object $_ @('-Wall') })
    $objects += @($LibSources | ForEach-Object { Build-Object $_ @('-w') })

    # The manifest makes UTF-8 the process code page and allows long paths.
    $Resource = Join-Path $Build 'wingrep.res.o'
    $stale = -not (Test-Path $Resource)
    foreach ($src in 'win\wingrep.rc', 'win\wingrep.manifest') {
        if (-not $stale) { $stale = (Get-Item $src).LastWriteTime -gt (Get-Item $Resource).LastWriteTime }
    }
    if ($stale) {
        Write-Host "RC win\wingrep.rc"
        & windres win\wingrep.rc -O coff -o $Resource
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    $objects += $Resource

    & gcc @objects -liconv -lntdll -static '-Wl,--stack,8388608' -o (Join-Path $Build 'grep.exe')
    if ($LASTEXITCODE -ne 0) { Write-Host "Link failed" -ForegroundColor Red; exit $LASTEXITCODE }
}
finally {
    Pop-Location
}

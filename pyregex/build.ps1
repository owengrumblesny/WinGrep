# Written by Owen Grumbles (owengrumblesny@gmail.com)
#Under GPL 3.0 License or Later.

$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    # Cython turns the .pyx into C, then gcc turns the C into a Python module.
    python -m cython -3 wgre.pyx -o wgre.c
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    $Base = python -c "import sys; print(sys.base_prefix)"
    $Suffix = python -c "import sysconfig; print(sysconfig.get_config_var('EXT_SUFFIX'))"
    $Dll = python -c "import sys; print(f'python3{sys.version_info.minor}.dll')"

    & gcc -O2 -shared "-I$Base\Include" wgre.c "$Base\$Dll" -static-libgcc -o "wgre$Suffix"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
finally {
    Pop-Location
}
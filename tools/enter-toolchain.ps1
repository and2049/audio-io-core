$ErrorActionPreference = 'Stop'

$llvmRoot = if ($env:LLVM_ROOT) { $env:LLVM_ROOT } else { Join-Path $env:ProgramFiles 'LLVM' }
$llvmBin = Join-Path $llvmRoot 'bin'
foreach ($tool in @('clang-cl.exe', 'clang-format.exe')) {
    if (-not (Test-Path (Join-Path $llvmBin $tool))) {
        throw "Missing $tool in $llvmBin. Install LLVM or set LLVM_ROOT."
    }
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw 'Visual Studio C++ Build Tools are required for the Windows SDK and C++ standard library.'
}
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $installation) {
    throw 'No Visual Studio installation with x64 C++ tools was found.'
}

& (Join-Path $installation 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
if (-not $env:VSCMD_VER) {
    throw 'Visual Studio developer environment initialization failed.'
}
$env:PATH = "$llvmBin;$env:PATH"
& (Join-Path $llvmBin 'clang-cl.exe') --version
if ($LASTEXITCODE -ne 0) {
    throw 'Clang verification failed.'
}

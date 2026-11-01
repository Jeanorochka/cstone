param(
    [ValidateSet('gcc','clang','msvc')][string]$Compiler = 'gcc',
    [ValidateSet('Debug','Release','RelWithDebInfo')][string]$Configuration = 'Release',
    [switch]$Shared,
    [switch]$AddressSanitizer,
    [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$suffix = if ($Shared) { '-shared' } else { '' }
if ($AddressSanitizer) { $suffix += '-asan' }
$buildRoot = Join-Path $projectRoot "work\mvp-$Compiler$suffix"
$cmakePath = (Get-Command cmake -ErrorAction Stop).Source
$arguments = @('-S',$projectRoot,'-B',$buildRoot,'-DCSTONE_WARNINGS_AS_ERRORS=ON')
$arguments += if ($Shared) { '-DBUILD_SHARED_LIBS=ON' } else { '-DBUILD_SHARED_LIBS=OFF' }
if ($AddressSanitizer) { $arguments += '-DCSTONE_ADDRESS_SANITIZER=ON' }

function Invoke-Checked([string]$Program,[string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Command failed with exit code $LASTEXITCODE" }
}

if ($Compiler -eq 'msvc') {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (!(Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer was not found.' }
    $version = & $vswhere -latest -property installationVersion
    $major = [int]($version.Split('.')[0])
    $generator = switch ($major) { 18 { 'Visual Studio 18 2026' }; 17 { 'Visual Studio 17 2022' }; 16 { 'Visual Studio 16 2019' }; default { throw 'Select a supported Visual Studio generator in CMake.' } }
    $arguments += @('-G',$generator,'-A','x64')
} else {
    $arguments += "-DCMAKE_BUILD_TYPE=$Configuration"
    $compilerPath = (Get-Command $Compiler -ErrorAction Stop).Source
    $ninja = Get-Command ninja -ErrorAction SilentlyContinue
    if ($ninja) { $ninjaPath = $ninja.Source } else {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (!(Test-Path -LiteralPath $vswhere)) { throw 'Ninja is required. Install it or use -Compiler msvc.' }
        $installation = & $vswhere -latest -property installationPath
        $ninjaPath = Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
        if (!(Test-Path -LiteralPath $ninjaPath)) { throw 'Ninja was not found. Use -Compiler msvc or install Ninja.' }
    }
    $arguments += @('-G','Ninja',"-DCMAKE_MAKE_PROGRAM=$ninjaPath","-DCMAKE_C_COMPILER=$compilerPath")
    if ($Compiler -eq 'clang') { $arguments += '-DCMAKE_C_COMPILER_TARGET=x86_64-w64-windows-gnu' }
}
Invoke-Checked $cmakePath $arguments
Invoke-Checked $cmakePath @('--build',$buildRoot,'--config',$Configuration,'--parallel')
if (!$SkipTests) { Invoke-Checked (Get-Command ctest -ErrorAction Stop).Source @('--test-dir',$buildRoot,'-C',$Configuration,'--output-on-failure') }
Write-Output "Built Cstone in $buildRoot"

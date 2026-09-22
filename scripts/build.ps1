[CmdletBinding(PositionalBinding=$false)]
param(
    [string]$BuildDir = '',
    [ValidateSet('Debug','Release','RelWithDebInfo','MinSizeRel')][string]$BuildType = 'Release',
    [ValidateSet('ON','OFF')][string]$Wx = 'ON',
    [ValidateSet('ON','OFF')][string]$RequireWx = 'OFF',
    [ValidateSet('ON','OFF')][string]$Cli = 'ON',
    [ValidateSet('ON','OFF')][string]$MinimalRelease = 'ON',
    [int]$Parallel = 0,
    [string]$Target = '',
    [string]$Generator = '',
    [string]$Platform = '',
    [string]$NeoSharedRoot = '',
    [string]$VcpkgRoot = $env:VCPKG_INSTALLATION_ROOT,
    [string]$VcpkgTriplet = 'x64-windows-static',
    [string]$InstallPrefix = '',
    [switch]$Clean,
    [Parameter(ValueFromRemainingArguments=$true)][string[]]$ExtraCMakeArgs
)

$ErrorActionPreference = 'Stop'
$RootDir = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path $RootDir 'build'
} elseif (-not [System.IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir = Join-Path $RootDir $BuildDir
}
$BuildDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($BuildDir)

if ($Clean -and (Test-Path -LiteralPath $BuildDir)) {
    $root = [System.IO.Path]::GetFullPath($RootDir).TrimEnd('\','/')
    $build = [System.IO.Path]::GetFullPath($BuildDir).TrimEnd('\','/')
    if (-not $build.StartsWith($root + [System.IO.Path]::DirectorySeparatorChar,
                              [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean a build directory outside the repository: $build"
    }
    Remove-Item -Recurse -Force -LiteralPath $BuildDir
}

if ([string]::IsNullOrWhiteSpace($NeoSharedRoot)) {
    $resolved = & cmake -P (Join-Path $RootDir 'cmake/NeoSharedSource.cmake')
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $NeoSharedRoot = ($resolved | Select-Object -Last 1).Trim()
}
if (-not [System.IO.Path]::IsPathRooted($NeoSharedRoot)) {
    $NeoSharedRoot = Join-Path $RootDir $NeoSharedRoot
}
$NeoSharedRoot = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($NeoSharedRoot)
if (-not (Test-Path -LiteralPath (Join-Path $NeoSharedRoot 'CMakeLists.txt'))) {
    throw "neoshared CMakeLists.txt not found under: $NeoSharedRoot"
}

$args = @('-S', $RootDir, '-B', $BuildDir,
    "-DCMAKE_BUILD_TYPE=$BuildType",
    "-DNEOSHARED_ROOT=$NeoSharedRoot",
    "-DNEOMDL_BUILD_WX_GUI=$Wx",
    "-DNEOMDL_REQUIRE_WX_GUI=$RequireWx",
    "-DNEOMDL_BUILD_CLI=$Cli",
    "-DNEO_MINIMAL_RELEASE=$MinimalRelease")
if (-not [string]::IsNullOrWhiteSpace($Generator)) {
    $args += @('-G', $Generator)
    if (-not [string]::IsNullOrWhiteSpace($Platform)) { $args += @('-A', $Platform) }
}
if (-not [string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    $VcpkgRoot = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($VcpkgRoot)
    $toolchain = Join-Path $VcpkgRoot 'scripts\buildsystems\vcpkg.cmake'
    if (-not (Test-Path -LiteralPath $toolchain)) { throw "vcpkg toolchain not found: $toolchain" }
    $args += "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
    $args += "-DVCPKG_TARGET_TRIPLET=$VcpkgTriplet"
    $overlay = Join-Path $NeoSharedRoot 'vcpkg-ports'
    if (Test-Path -LiteralPath $overlay) { $args += "-DVCPKG_OVERLAY_PORTS=$overlay" }
}
if (-not [string]::IsNullOrWhiteSpace($InstallPrefix)) {
    $args += "-DCMAKE_INSTALL_PREFIX=$InstallPrefix"
}
$args += $ExtraCMakeArgs

& cmake @args
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$buildArgs = @('--build', $BuildDir, '--config', $BuildType)
if ($Parallel -gt 0) { $buildArgs += @('--parallel', $Parallel) }
if (-not [string]::IsNullOrWhiteSpace($Target)) { $buildArgs += @('--target', $Target) }
& cmake @buildArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if (-not [string]::IsNullOrWhiteSpace($InstallPrefix) -and [string]::IsNullOrWhiteSpace($Target)) {
    & cmake --install $BuildDir --config $BuildType
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

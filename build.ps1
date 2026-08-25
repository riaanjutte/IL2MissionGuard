[CmdletBinding()]
param(
    [ValidateSet('Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$dotnetCandidates = @(
    (Join-Path $env:USERPROFILE '.dotnet10\dotnet.exe'),
    (Get-Command dotnet -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -ErrorAction SilentlyContinue)
) | Where-Object { $_ -and (Test-Path -LiteralPath $_) }
$dotnet = $dotnetCandidates | Where-Object {
    (& $_ --list-sdks 2>$null) -match '^10\.'
} | Select-Object -First 1
if (-not $dotnet) {
    throw '.NET 10 SDK was not found. Install the .NET 10 SDK from https://dotnet.microsoft.com/download/dotnet/10.0.'
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio Installer (vswhere.exe) was not found.'
}

$msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) {
    throw 'MSBuild was not found. Install Visual Studio 2022 Build Tools with Desktop development with C++.'
}

& $msbuild (Join-Path $root 'IL2MissionGuard.sln') /m /p:Configuration=$Configuration /p:Platform=x64
if ($LASTEXITCODE -ne 0) {
    throw "MSBuild failed with exit code $LASTEXITCODE."
}

$tests = Join-Path $root 'tests\IL2MissionGuard.Tests\bin\IL2MissionGuard.Tests.exe'
& $tests
if ($LASTEXITCODE -ne 0) {
    throw "Regression tests failed with exit code $LASTEXITCODE."
}

$managedTests = Join-Path $root 'tests\IL2MissionGuard.ManagedTests\IL2MissionGuard.ManagedTests.csproj'
& $dotnet run --project $managedTests -c $Configuration
if ($LASTEXITCODE -ne 0) {
    throw "Managed regression tests failed with exit code $LASTEXITCODE."
}

$release = Join-Path $root 'artifacts\release\win-x64'
New-Item -ItemType Directory -Path $release -Force | Out-Null
$project = Join-Path $root 'src\IL2MissionGuard.Wpf\IL2MissionGuard.Wpf.csproj'
& $dotnet publish $project -c $Configuration -r win-x64 --self-contained true -o $release
if ($LASTEXITCODE -ne 0) {
    throw "Managed publish failed with exit code $LASTEXITCODE."
}

$file = Get-Item -LiteralPath (Join-Path $release 'IL2MissionGuard.exe')
$hash = Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256
[pscustomobject]@{
    File = $file.FullName
    Version = $file.VersionInfo.FileVersion
    Length = $file.Length
    SHA256 = $hash.Hash
}

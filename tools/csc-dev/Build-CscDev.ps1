<# Build and stage the CSC development CE from tracked source and public vcpkg inputs. #>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$VcpkgRoot,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$MSBuild = 'MSBuild.exe'
)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path -LiteralPath "$PSScriptRoot/../..").Path
$vcpkg = (Resolve-Path -LiteralPath (Join-Path $VcpkgRoot 'vcpkg.exe')).Path
$buildTool = (Get-Command -Name $MSBuild -CommandType Application).Source
$output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
if (Get-Item -LiteralPath $output -Force -ErrorAction SilentlyContinue) {
    throw "Choose a fresh output directory. Already exists: $output"
}
$work = Join-Path $output 'build'
$installed = Join-Path $work 'vcpkg_installed'
New-Item -ItemType Directory -Path $work | Out-Null
& $vcpkg install '--triplet=x64-windows-static' "--x-manifest-root=$PSScriptRoot" "--x-install-root=$installed"
if ($LASTEXITCODE) { throw "vcpkg install failed ($LASTEXITCODE)" }

# Use CE's existing pragma and Release x64 project. The compatibility name
# lives in the staging directory so a clean checkout never needs a local lib.
$library = Join-Path $installed 'x64-windows-static/lib/minhook.x64.lib'
$libraryDirectory = Join-Path $work 'lib'
New-Item -ItemType Directory -Path $libraryDirectory | Out-Null
Copy-Item -LiteralPath $library -Destination (Join-Path $libraryDirectory 'libMinHook.x64.lib')
$buildOutput = Join-Path $work 'release'
$intermediates = (Join-Path $work 'obj').Replace('\','/') + '/'
$project = Join-Path $repo 'CivilizationVI_CommunityExtension/CivilizationVI_CommunityExtension.vcxproj'
$buildParameters = @($project, '/m', '/v:minimal', '/p:Configuration=Release', '/p:Platform=x64',
    "/p:CE_OUTPUT_DIR=$buildOutput", "/p:IntDir=$intermediates", "/p:CE_DEV_LIBRARY_DIR=$libraryDirectory")
& $buildTool @buildParameters
if ($LASTEXITCODE) { throw "CE build failed ($LASTEXITCODE)" }

# The CSC SQL capability gate uses this dev ID. Retain WildW's package XML
# and UI and change the same ID and DLL prefix in both frontend declarations.
$upstreamId = '3351473b-0746-417a-a618-2b66a04d8f3d'
$devId = 'a6ce92d2-7814-473e-b05f-153c1d9c62e6'
$upstreamPrefix = 'GameCore_XP2_CE'
$devPrefix = 'GameCore_XP2_CE_AI'
$modSource = Join-Path $repo 'CivilizationVI_CommunityExtension_Mod'
$manifest = New-Object System.Xml.XmlDocument
$manifest.Load((Join-Path $modSource 'CommunityExtension.modinfo'))
$mod = $manifest.DocumentElement
$dllEntry = $mod.SelectSingleNode("Files/File[text()='Binaries/Win64/$($upstreamPrefix)_FinalRelease.dll']")
$config = Get-Content -LiteralPath (Join-Path $modSource 'Config.sql') -Raw
if ($mod.GetAttribute('id') -ne $upstreamId -or !$dllEntry -or
    !$config.Contains($upstreamId) -or !$config.Contains($upstreamPrefix)) {
    throw 'The upstream CE package layout changed. Review the development ID and selector.'
}
$mod.SetAttribute('id', $devId)
$mod.SetAttribute('version', '1')
$mod.SelectSingleNode('Properties/Name').InnerText = 'Community Extension - CSC AI capabilities'
$mod.SelectSingleNode('Properties/Description').InnerText = 'Development build for Civ Supply Chains.'
$mod.SelectSingleNode('Properties/Authors').InnerText = 'WildW; hennogous development fork'
$mod.SelectSingleNode('Properties/AffectsSavedGames').InnerText = '1'
$dllEntry.InnerText = "Binaries/Win64/$($devPrefix)_FinalRelease.dll"

$package = Join-Path $output 'CE CSC Development'
foreach ($directory in 'Data','UI','Binaries/Win64') {
    New-Item -ItemType Directory -Path (Join-Path $package $directory) | Out-Null
}
Copy-Item -LiteralPath (Join-Path $modSource 'Dummy.sql') -Destination (Join-Path $package 'Data/Dummy.sql')
foreach ($file in 'CEWatermark.xml','CEWatermark.lua') {
    Copy-Item -LiteralPath (Join-Path $modSource $file) -Destination (Join-Path $package "UI/$file")
}
[IO.File]::WriteAllText((Join-Path $package 'Data/Config.sql'),
    $config.Replace($upstreamId, $devId).Replace($upstreamPrefix, $devPrefix), [Text.UTF8Encoding]::new($false))
$binary = Join-Path $buildOutput "Binaries/Win64/$($upstreamPrefix)_FinalRelease.dll"
Copy-Item -LiteralPath $binary -Destination (Join-Path $package $dllEntry.InnerText)
$manifest.Save((Join-Path $package 'CE_AI_Development.modinfo'))

Write-Output "Built CE development package at $package"

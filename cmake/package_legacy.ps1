param(
    [Parameter(Mandatory)][string]$BuildDirectory,
    [Parameter(Mandatory)][string]$Destination
)
$ErrorActionPreference = 'Stop'
$output = Join-Path $BuildDirectory 'Release'
$executable = Join-Path $output 'TTPlayerRebuild.exe'
$report = Join-Path $output 'legacy-imports.json'
if (-not (Test-Path -LiteralPath $executable) -or -not (Test-Path -LiteralPath $report)) {
    throw 'Build and audit the legacy Release player before packaging.'
}
$audit = Get-Content -LiteralPath $report -Raw -Encoding UTF8 | ConvertFrom-Json
if ($audit.minimum_subsystem -ne '5.01' -or $audit.architecture -ne 'x86' -or
    $audit.inventories -notcontains '5.1.2600.txt' -or $audit.inventories -notcontains '6.1.7600.txt') {
    throw 'The import report is not an XP / Win7 audit.'
}
if (@($audit.imports.'ttpcomm.dll').Count -ne 1 -or $audit.imports.'ttpcomm.dll'[0] -cne '#3') {
    throw 'The legacy EXE must load ttpcomm.dll at startup to initialize its XP TLS.'
}
$hash = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant()
if ($hash -cne $audit.sha256) { throw 'The EXE changed after the import audit; rebuild it.' }
# XP/Win7 版不静态导入 WebView2Loader.dll，但创建 WebView2 环境时要
# LoadLibrary 动态解析导出函数；这个 DLL 必须随包。XP 上不会加载它（Plan
# 直接判不支持），多带一个文件无副作用。
$loader = Join-Path $output 'WebView2Loader.dll'
if (-not (Test-Path -LiteralPath $loader)) {
    throw 'WebView2Loader.dll is missing beside the legacy EXE; Win7 WebView2 needs it.'
}
$loaderHash = (Get-FileHash -LiteralPath $loader -Algorithm SHA256).Hash.ToLowerInvariant()
$package = Join-Path $BuildDirectory ('legacy-package-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $package | Out-Null
# 按交付要求：包内可执行文件命名为 ttplayer.exe（而非中间构建名 TTPlayerRebuild.exe）。
Copy-Item -LiteralPath $executable -Destination (Join-Path $package 'ttplayer.exe')
Copy-Item -LiteralPath $loader -Destination $package
@("$hash  ttplayer.exe", "$loaderHash  WebView2Loader.dll") |
    Set-Content -LiteralPath (Join-Path $package 'SHA256SUMS.txt') -Encoding UTF8
$parent = Split-Path ([IO.Path]::GetFullPath($Destination)) -Parent
New-Item -ItemType Directory -Force -Path $parent | Out-Null
Compress-Archive -LiteralPath (Join-Path $package 'ttplayer.exe'),
    (Join-Path $package 'WebView2Loader.dll'),
    (Join-Path $package 'SHA256SUMS.txt') -DestinationPath $Destination -Force
Write-Output "Legacy package: $Destination"

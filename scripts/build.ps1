# OptiScaler-F5-DLSSNR-Multipass 汉化版构建脚本（GitHub Actions windows-latest）
#
# 前置：调用方已把 MSBuild 加进 PATH（microsoft/setup-msbuild@v2），
#       且已跑过 scripts/apply_patch.py。
#
# 用法:
#   pwsh scripts/build.ps1 -SourceDir <仓库根> -OutDir <产物目录>

param(
    [Parameter(Mandatory = $true)][string]$SourceDir,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [string]$MsbuildPath = ""
)

$ErrorActionPreference = 'Stop'

# 1. 找 MSBuild。CI 里 setup-msbuild 已放进 PATH；本地用 vswhere 兜底。
if ($MsbuildPath -and (Test-Path $MsbuildPath)) {
    $msbuild = $MsbuildPath
    Write-Host "MSBuild(指定): $msbuild"
} else {
    $cmd = Get-Command msbuild -ErrorAction SilentlyContinue
    if ($cmd) {
        $msbuild = $cmd.Source
    } else {
        $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
        if (-not (Test-Path $vswhere)) { throw "找不到 msbuild，也找不到 vswhere" }
        $msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
    }
    if (-not $msbuild) { throw "找不到 MSBuild" }
    Write-Host "MSBuild: $msbuild"
}

# 2. 编译
Push-Location $SourceDir
try {
    & $msbuild OptiScaler.sln /m /p:Configuration=Release /p:Platform=x64 /verbosity:minimal
    if ($LASTEXITCODE -ne 0) { throw "MSBuild 失败，退出码 $LASTEXITCODE" }
} finally {
    Pop-Location
}

# 3. 收集产物。上游的 Release 后置事件已经把运行库、Licenses、setup 脚本、
#    以及我们的 font\ 都拷进 x64\Release\a\，整目录拿走即可。
$prod = Join-Path $SourceDir "x64\Release\a"
if (-not (Test-Path $prod)) { throw "找不到构建产物: $prod" }

New-Item $OutDir -ItemType Directory -Force | Out-Null
Copy-Item (Join-Path $prod "*") $OutDir -Recurse -Force

# 4. 自检：汉化版的两个标志物必须在产物里，否则说明补丁没生效就打包了
$dll = Join-Path $OutDir "OptiScaler.dll"
if (-not (Test-Path $dll)) { throw "产物缺 OptiScaler.dll" }
$font = Join-Path $OutDir "font\wqy-microhei.ttc"
if (-not (Test-Path $font)) { throw "产物缺 font\wqy-microhei.ttc —— 中文字体会渲染不出来" }

Write-Host "产物目录: $OutDir"
Get-ChildItem $OutDir -File | Select-Object Name, Length | Format-Table -AutoSize

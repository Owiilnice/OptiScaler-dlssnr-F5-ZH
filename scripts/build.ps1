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
    [string]$MsbuildPath = "",
    # 是否把 NVIDIA Streamline（OptiScaler 自己输出 DLSS-G 时用的运行时）打进包。
    # 对应上游 package_release.ps1 的 -IncludeDlssFrameGeneration。
    [bool]$IncludeStreamline = $true,
    # 分发 NVIDIA 的 DLL 需要显式同意其许可；不给就拒绝打包（与上游行为一致）。
    [switch]$AcceptNvidiaLicenses
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

# 4. 剔除构建中间产物。
#    整目录照搬会把转发器的 PDB/LIB/EXP 一起带出去。PDB 里有完整符号表和
#    构建机的源码路径，对用户没用、还泄漏路径，不该随包发布。
#    上游的 package_release.ps1 也是这么删的（*.pdb *.lib *.exp *.ilk）。
$junk = @(Get-ChildItem $OutDir -Recurse -File |
          Where-Object { $_.Extension -in '.pdb', '.lib', '.exp', '.ilk' })
foreach ($f in $junk) {
    Write-Host ("  剔除 {0} ({1:N2} MB)" -f $f.Name, ($f.Length / 1MB))
    Remove-Item $f.FullName -Force
}

# 5. 补上游发布包里那些「来自仓库根、不在 x64\Release\a\」的文件。
#    编译后置事件不拷这些，只搬 a\ 会漏掉。
#    （对应 package_release.ps1 里的 $sourceFiles + docs + redist。）
foreach ($f in @('OptiScaler.ini', 'setup_windows.bat', 'setup_linux.sh',
                 'get_streamline.ps1', 'README.md', 'INSTALL-DLSSNR.md', 'LICENSE')) {
    $p = Join-Path $SourceDir $f
    if (Test-Path $p) { Copy-Item $p $OutDir -Force }
    else { Write-Host "  (上游没有，跳过) $f" }
}
foreach ($d in @('docs', 'redist')) {
    $p = Join-Path $SourceDir $d
    if (Test-Path $p) { Copy-Item $p $OutDir -Recurse -Force }
}

# 5.5 NVIDIA Streamline —— 让 OptiScaler 自己输出 DLSS-G 时用的运行时。
#
#     上游 package_release.ps1 在 -IncludeDlssFrameGeneration 时会调 get_streamline.ps1，
#     由它按 redist\streamline\manifest.json 下载 NVIDIA 官方 SDK、校验 SHA-256 与
#     Authenticode 签名、只挑 allowlist 里的 6 个 DLL 放进 OptiScaler\streamline\。
#
#     本脚本原来只搬了 redist\（清单），没执行下载 —— 结果发布包里缺整个
#     OptiScaler\streamline\ 目录（6 个 DLL、约 11 MB），凡是
#     FG Output = DLSSG 或 FGNvngxReplacement 的用法都会缺件。2026-10-22 补上。
if ($IncludeStreamline) {
    if (-not $AcceptNvidiaLicenses) {
        throw ("IncludeStreamline 会随包分发 NVIDIA 的 DLL，需要 -AcceptNvidiaLicenses。" +
               "先读 docs/DLSS-FRAME-GENERATION.md 与其中的 NVIDIA 许可；" +
               "或传 -IncludeStreamline `$false 出一个不含 Streamline 的包。")
    }

    $slDir = Join-Path $OutDir 'streamline'
    if (Test-Path -LiteralPath $slDir) {
        throw "产物里已存在 streamline 目录（$slDir）：不接受来源不明的 Stack，先清掉再打包。"
    }

    Write-Host "取 NVIDIA Streamline（DLSS-G 输出运行时）..."
    # get_streamline.ps1 用 throw 报错，$ErrorActionPreference = 'Stop' 会让失败直接中断构建
    # —— 这是想要的：宁可这次不发版，也不要悄悄发出一个缺件的包。
    & (Join-Path $SourceDir 'get_streamline.ps1') -Destination $slDir -AcceptNvidiaLicenses
}

# 6. 自检：汉化版的两个标志物必须在产物里，否则说明补丁没生效就打包了
$dll = Join-Path $OutDir "OptiScaler.dll"
if (-not (Test-Path $dll)) { throw "产物缺 OptiScaler.dll" }
$font = Join-Path $OutDir "font\wqy-microhei.ttc"
if (-not (Test-Path $font)) { throw "产物缺 font\wqy-microhei.ttc —— 中文字体会渲染不出来" }

# Streamline 若声明要带，6 个 DLL 一个都不能少（清单见 redist\streamline\manifest.json）
if ($IncludeStreamline) {
    $slDir = Join-Path $OutDir 'streamline'
    $slNeed = @('sl.interposer.dll', 'sl.common.dll', 'sl.dlss_g.dll',
                'sl.reflex.dll', 'sl.pcl.dll', 'nvngx_dlssg.dll')
    foreach ($n in $slNeed) {
        if (-not (Test-Path (Join-Path $slDir $n))) { throw "产物缺 streamline\$n" }
    }
    Write-Host "Streamline: 6 个 DLL 齐全"
} else {
    Write-Host "Streamline: 按 -IncludeStreamline `$false 跳过（包内不会有 DLSS-G 输出运行时）"
}

# 7. 生成 SHA256SUMS.txt（上游发布包里也有，用于校验下载完整性）。
#    显式用无 BOM 的 UTF-8：PowerShell 5.1 的 -Encoding utf8 会写 BOM，
#    校验工具读到 BOM 会认不出第一行。
#    先归一化成绝对路径再算相对名 —— 否则 $OutDir 带尾斜杠或写成相对路径时
#    Substring 会切错位置，生成一堆残缺文件名。
$root = (Get-Item $OutDir).FullName
$sums = Get-ChildItem $root -Recurse -File |
        Where-Object { $_.Name -ne 'SHA256SUMS.txt' } |
        Sort-Object FullName |
        ForEach-Object {
            $rel = $_.FullName.Substring($root.Length).TrimStart('\', '/') -replace '\\', '/'
            "{0}  {1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $rel
        }
[System.IO.File]::WriteAllLines((Join-Path $root 'SHA256SUMS.txt'), $sums,
                                (New-Object System.Text.UTF8Encoding $false))

Write-Host "产物目录: $OutDir"
$files = Get-ChildItem $root -Recurse -File
Write-Host ("合计 {0} 个文件，{1:N1} MB" -f $files.Count, (($files | Measure-Object Length -Sum).Sum / 1MB))

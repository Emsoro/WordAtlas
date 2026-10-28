param(
    [string]$Config = "Release",
    [string]$Generator = "Visual Studio 17 2022",
    [string]$Arch = "x64",
    [switch]$Clean,        # 删除 build/ 后重建
    [switch]$SkipWeb,      # 跳过 npm 构建，直接用现有 frontend/
    [switch]$SkipAtlas,    # 发布时不重新同步 atlas 媒体（首次之后通常已同步）
    [switch]$DevMode,      # 不嵌入外壳：exe 从 frontend/ 目录读取，改 UI 免重编
    [switch]$NoPublish     # 只编译 exe，不整理 dist\WordAtlas
)

$ErrorActionPreference = "Stop"
$ProjectDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$WebDir = Join-Path $ProjectDir "web"
$FrontendDir = Join-Path $ProjectDir "frontend"
$BuildDir = Join-Path $ProjectDir "build"
$DistDir = Join-Path $ProjectDir "dist\WordAtlas"

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "Cleaning build directory..." -ForegroundColor Yellow
    Remove-Item -Recurse -Force $BuildDir
}

# ============================================================================
# 1) 前端外壳：vite 只产出 index.html + assets/*，再补上品牌图/音效等小资源
#    （atlas 媒体 600MB+ 不进外壳，运行时从 exe 同目录的 atlas/ 读取）
# ============================================================================
if (-not $SkipWeb) {
    if (-not (Test-Path (Join-Path $WebDir "node_modules"))) {
        Write-Host "Installing web dependencies (npm ci)..." -ForegroundColor Cyan
        Push-Location $WebDir
        try { npm ci } finally { Pop-Location }
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }

    Write-Host "Building web shell -> frontend/ ..." -ForegroundColor Cyan
    Push-Location $WebDir
    try { npm run build:desktop } finally { Pop-Location }
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

# 外壳所需的静态小资源（图标 / 品牌图 / 音效）每次都补齐，vite 不会复制它们
New-Item -ItemType Directory -Force -Path $FrontendDir | Out-Null
foreach ($item in @("favicon.svg", "icon-512.png", "manifest.webmanifest", "brand", "sfx")) {
    $source = Join-Path $WebDir "public\$item"
    if (Test-Path $source) {
        Copy-Item -Path $source -Destination $FrontendDir -Recurse -Force
    }
}

if (-not (Test-Path (Join-Path $FrontendDir "index.html"))) {
    Write-Error "frontend/index.html 不存在，请先不加 -SkipWeb 运行一次。"
    exit 1
}

# ============================================================================
# 2) CMake + MSVC
# ============================================================================
$vcvars = $null
foreach ($edition in @("Enterprise", "Community", "Professional")) {
    $candidate = "${env:ProgramFiles}\Microsoft Visual Studio\2022\$edition\VC\Auxiliary\Build\vcvarsall.bat"
    if (Test-Path $candidate) { $vcvars = $candidate; break }
}
if (-not $vcvars) {
    Write-Error "Visual Studio 2022 not found. Please install VS 2022 with C++ workload."
    exit 1
}

$cmakeArgs = @("-G", $Generator, "-A", $Arch, "-B", $BuildDir, "-S", $ProjectDir)
if ($DevMode) {
    $cmakeArgs += "-DEMBED_FRONTEND=OFF"
    Write-Host "Dev mode: frontend loaded from ./frontend (no rebuild needed for UI tweaks)" -ForegroundColor Yellow
}

Write-Host "Configuring: $Generator / $Arch / $Config" -ForegroundColor Cyan
try { & $vcvars $Arch | Out-Null } catch { Write-Host "vcvarsall skipped (cmake locates MSVC itself)" -ForegroundColor DarkGray }
cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Building $Config..." -ForegroundColor Cyan
cmake --build $BuildDir --config $Config
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$exe = Join-Path $BuildDir "$Config\WordAtlas.exe"
if (-not (Test-Path $exe)) {
    Write-Error "Build finished but $exe is missing."
    exit 1
}

# ============================================================================
# 3) 开发模式：把外壳与媒体链接到 exe 旁边，方便直接双击调试
# ============================================================================
if ($DevMode) {
    $runDir = Join-Path $BuildDir $Config
    $devFrontend = Join-Path $runDir "frontend"
    New-Item -ItemType Directory -Force -Path $devFrontend | Out-Null
    Copy-Item -Path (Join-Path $FrontendDir "*") -Destination $devFrontend -Recurse -Force

    $atlasLink = Join-Path $runDir "atlas"
    if (-not (Test-Path $atlasLink)) {
        New-Item -ItemType Junction -Path $atlasLink -Target (Join-Path $WebDir "public\atlas") | Out-Null
        Write-Host "Linked $atlasLink -> web\public\atlas" -ForegroundColor DarkGray
    }

    Write-Host "Build succeeded: $exe" -ForegroundColor Green
    Write-Host "Dev layout: $runDir (frontend/ + atlas\)" -ForegroundColor Green
    return
}

# ============================================================================
# 4) 发布：exe + atlas 媒体目录，整个文件夹拷走即可离线运行
# ============================================================================
if (-not $NoPublish) {
    New-Item -ItemType Directory -Force -Path $DistDir | Out-Null
    Copy-Item -Path $exe -Destination $DistDir -Force

    if (-not $SkipAtlas) {
        Write-Host "Publishing atlas media (robocopy /MIR)..." -ForegroundColor Cyan
        $sourceAtlas = Join-Path $WebDir "public\atlas"
        $targetAtlas = Join-Path $DistDir "atlas"
        robocopy $sourceAtlas $targetAtlas /MIR /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
        if ($LASTEXITCODE -ge 8) { Write-Error "robocopy failed ($LASTEXITCODE)"; exit $LASTEXITCODE }
    }

    Copy-Item -Path (Join-Path $ProjectDir "README.md") -Destination $DistDir -Force -ErrorAction SilentlyContinue

    $exeSize = [math]::Round((Get-Item (Join-Path $DistDir "WordAtlas.exe")).Length / 1MB, 2)
    $atlas = Get-ChildItem (Join-Path $DistDir "atlas") -Recurse -File -ErrorAction SilentlyContinue |
        Measure-Object Length -Sum
    Write-Host ("Distribution: {0}" -f $DistDir) -ForegroundColor Green
    Write-Host ("  WordAtlas.exe  {0} MB" -f $exeSize) -ForegroundColor Green
    Write-Host ("  atlas/         {0} files, {1:N0} MB" -f $atlas.Count, ($atlas.Sum / 1MB)) -ForegroundColor Green
} else {
    Write-Host "Build succeeded: $exe" -ForegroundColor Green
}

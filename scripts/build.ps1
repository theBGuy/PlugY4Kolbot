<#
.SYNOPSIS
    Builds every PlugY4Kolbot binary (x86) with the installed Visual Studio 2022 toolset:
    PlugY.dll, the PlugY.exe launcher, PatchD2File.exe, RestoreD2File.exe and the
    PlugY4KolbotUpdater.exe updater.

.DESCRIPTION
    Output: build/out/<Config>/ holds the five binaries, their PDBs, PlugY.map
    and build.log. Intermediates: build/out/<Config>/obj/<dll|run|patch|restore|update>/.
    This script is the canonical definition of the sources and of every compiler
    and linker flag; the comments say why a flag is there. Every source is
    recompiled on each run (no dependency tracking), so the result never depends
    on stale objects. /Brepro and /PDBALTPATH make two builds of the same checkout
    byte-identical. Exits 1 on the first failing step and prints its command line.

.PARAMETER Config
    Release (default) or Debug.

.PARAMETER Crt
    MT (default, static CRT: no VC redistributable needed) or MD (DLL CRT). The
    updater and the tests always use the static CRT.

.PARAMETER Clean
    Delete the output directory (build/out/<Config>, or its -Define variant) before building.

.PARAMETER Define
    Extra preprocessor symbols for PlugY.dll only, each a C identifier with an
    optional =value (e.g. PLUGY_TEST_HOOKS for the live test harness); a comma
    list is split, so pwsh -File works too. Such a build lands in
    build/out/<Config>-<symbols joined by '-', '=' as '_'>/ and never replaces
    the release binaries.

.PARAMETER Test
    After the five binaries, also build tests/UpdateCheckTests.cpp,
    tests/UpdaterTests.cpp, tests/SessionLogTests.cpp,
    tests/SaveTransferTests.cpp, tests/MovedItemsPageTests.cpp,
    tests/ReleasedConnectionsTests.cpp, tests/StatBonusTests.cpp,
    tests/ServerMessageRulesTests.cpp, tests/UpdateNoticeRulesTests.cpp,
    tests/IniSectionTests.cpp, tests/CharacterCommitTests.cpp,
    tests/SpriteLoadRulesTests.cpp, tests/SharedStashStoreTests.cpp,
    tests/SharedStashLegacyTests.cpp, tests/SharedStashTransportTests.cpp and
    tests/SharedStashItemPacketTests.cpp, tests/SharedStashCubeSwapTests.cpp and
    tests/SharedStashCleanupTests.cpp and tests/SharedStashNativeRulesTests.cpp into
    build/out/<Config>/tests/ (console, static CRT) and run them. Exits 1 when a
    test fails.

.PARAMETER TestArgs
    Extra arguments for UpdateCheckTests.exe, e.g. -TestArgs --live to add the
    checks against the real GitHub API.
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release',
    [ValidateSet('MT', 'MD')]
    [string]$Crt = 'MT',
    [switch]$Clean,
    [switch]$Test,
    [string[]]$TestArgs,
    [string[]]$Define = @()
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

# Each symbol becomes a /D argument and part of the output directory name, so only
# plain identifiers (with an optional simple value) are accepted.
$Define = @($Define | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
foreach ($symbol in $Define) {
    if ($symbol -cnotmatch '\A[A-Za-z_][A-Za-z0-9_]*(=[A-Za-z0-9_.]*)?\z') {
        throw "-Define '$symbol' is not a C identifier with an optional =value"
    }
}

# vcvarsall output is imported into this process; when the script is run in-process
# (./build.ps1) that would leak into the caller, so the finally block restores it.
$SavedEnv = @{}
Get-ChildItem Env: | ForEach-Object { $SavedEnv[$_.Name] = $_.Value }

$RepoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$OutName = $Config
if ($Define.Count) { $OutName += '-' + (($Define | ForEach-Object { $_.Replace('=', '_') }) -join '-') }
$OutDir = Join-Path (Join-Path $RepoRoot 'build\out') $OutName
$ObjRoot = Join-Path $OutDir 'obj'
$LogFile = Join-Path $OutDir 'build.log'

# Every source of PlugY.dll. An explicit list (not a glob), so a stray .cpp in
# src/plugy/ never joins the build and the order, which sets the link order, is stable.
$DllSources = @(
    'src\plugy\BigStash.cpp'
    'src\plugy\ClientSaveFile.cpp'
    'src\plugy\Commands.cpp'
    'src\plugy\Common.cpp'
    'src\plugy\CustomLibraries.cpp'
    'src\plugy\D2functions.cpp'
    'src\plugy\D2wrapper.cpp'
    'src\plugy\Error.cpp'
    'src\plugy\ExtendedSaveFile.cpp'
    'src\plugy\ExtraOptions.cpp'
    'src\plugy\GlobalVariable.cpp'
    'src\plugy\HostJoin.cpp'
    'src\plugy\InfinityStash.cpp'
    'src\plugy\INIfile.cpp'
    'src\plugy\Interface_Skills.cpp'
    'src\plugy\Interface_Stash.cpp'
    'src\plugy\Interface_Stats.cpp'
    'src\plugy\Language.cpp'
    'src\plugy\LoadPlayerData.cpp'
    'src\plugy\LocalizedStrings.cpp'
    'src\plugy\MainScreen.cpp'
    'src\plugy\ModifMemory.cpp'
    'src\plugy\NewInterface_CubeListing.cpp'
    'src\plugy\NewInterface_Runewords.cpp'
    'src\plugy\NewInterface_Stats.cpp'
    'src\plugy\NewInterface_StatsPageTwo.cpp'
    'src\plugy\NewInterfaces.cpp'
    'src\plugy\OthersFeatures.cpp'
    'src\plugy\Parameters.cpp'
    'src\plugy\PlayerCustomData.cpp'
    'src\plugy\PlugY.cpp'
    'src\plugy\PlugYFiles.cpp'
    'src\plugy\SavePath.cpp'
    'src\plugy\SavePlayerData.cpp'
    'src\plugy\SharedSaveFile.cpp'
    'src\plugy\SharedStashSnapshot.cpp'
    'src\plugy\SharedStashNative.cpp'
    'src\plugy\SharedStashTransport.cpp'
    'src\plugy\SharedStashSession.cpp'
    'src\plugy\SkillPerLevelUp.cpp'
    'src\plugy\SkillsPoints.cpp'
    'src\plugy\SpriteLoad.cpp'
    'src\plugy\StashProtocol.cpp'
    'src\plugy\StatBonus.cpp'
    'src\plugy\StatPerLevelUp.cpp'
    'src\plugy\StatsPoints.cpp'
    'src\plugy\UberQuest.cpp'
    'src\plugy\UpdateClient.cpp'
    'src\plugy\UpdateServer.cpp'
    'src\plugy\Windowed.cpp'
    'src\plugy\WorldEvent.cpp'
    'src\common\VersionInfo.cpp'
    'src\common\UpdateCheck.cpp'
    'src\common\SessionLog.cpp'
    'src\common\CharacterCommit.cpp'
    'src\common\SharedStashStore.cpp'
    'src\common\SharedStashCleanup.cpp'
    'third_party\sqlite\sqlite3.c'
)
if ($Define -match '^PLUGY_TEST_HOOKS(?:=|$)') {
    $DllSources += 'src\plugy\SharedStashTest.cpp'
}

function Write-Log([string]$Text) {
    Write-Host $Text
    [System.IO.File]::AppendAllText($LogFile, $Text + [Environment]::NewLine)
}

# Runs a native tool, tees its output to the log, and aborts on failure.
function Invoke-Tool([string]$Exe, [string[]]$Arguments) {
    $cmdLine = '"' + $Exe + '" ' + (($Arguments | ForEach-Object {
        if ($_ -match '\s') { '"' + $_ + '"' } else { $_ }
    }) -join ' ')
    [System.IO.File]::AppendAllText($LogFile, '> ' + $cmdLine + [Environment]::NewLine)
    # Under 'Stop', Windows PowerShell 5.1 turns any redirected stderr line into
    # a terminating error; failures are judged by exit code instead.
    $ErrorActionPreference = 'Continue'
    $output = & $Exe @Arguments 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    foreach ($line in $output) {
        if ($line.Length -gt 0) { Write-Log $line }
    }
    if ($code -ne 0) {
        Write-Host ''
        Write-Host "FAILED (exit $code): $cmdLine" -ForegroundColor Red
        exit 1
    }
}

function Restore-Environment {
    foreach ($name in @(Get-ChildItem Env: | ForEach-Object { $_.Name })) {
        if (-not $SavedEnv.ContainsKey($name)) { Remove-Item -LiteralPath ('Env:' + $name) }
    }
    foreach ($name in $SavedEnv.Keys) {
        [Environment]::SetEnvironmentVariable($name, $SavedEnv[$name], 'Process')
    }
}

# Compiles each source separately into obj/<Key>/, compiles the resource script
# (when $Target.Rc names one), and links. $Target carries the per-binary flags;
# the shared ones are script-level.
function Build-Target([hashtable]$Target) {
    $objDir = Join-Path $ObjRoot $Target.Key
    New-Item -ItemType Directory -Force -Path $objDir | Out-Null
    Write-Log ''
    Write-Log ('=== ' + $Target.Out)

    $clArgs = $Target.ClFlags + ($Target.Defines | ForEach-Object { '/D' + $_ })
    # /FS: back-to-back cl.exe runs otherwise race mspdbsrv for the shared compiler
    # PDB (C1041). It changes PDB locking only, not code generation.
    $clArgs += @(('/Fd' + (Join-Path $objDir 'vc.pdb')), '/FS', '/Brepro')
    $objects = @()
    # __FILE__ expands to the source path as given on the command line (no /FC), so
    # compiling repo-relative paths from the repo root keeps the build machine's
    # directory out of the binaries and makes them independent of the checkout path.
    Push-Location -LiteralPath $RepoRoot
    try {
        foreach ($rel in $Target.Sources) {
            $obj = Join-Path $objDir ([System.IO.Path]::GetFileNameWithoutExtension($rel) + '.obj')
            Invoke-Tool $Cl ($clArgs + @(('/Fo' + $obj), $rel))
            $objects += $obj
        }
    } finally {
        Pop-Location
    }

    $resources = @()
    if ($Target.Rc) {
        $res = Join-Path $objDir ([System.IO.Path]::GetFileNameWithoutExtension($Target.Rc) + '.res')
        Invoke-Tool $Rc (@('/nologo') + $Target.RcFlags + @('/fo', $res, (Join-Path $RepoRoot $Target.Rc)))
        $resources = @($res)
    }

    # %_PDB% is expanded by link.exe itself (to the PDB file name without a path),
    # so the debug directory carries no build-machine path.
    $linkArgs = $Target.LinkFlags + @('/Brepro', '/PDBALTPATH:%_PDB%',
        ('/OUT:' + (Join-Path $OutDir $Target.Out)),
        ('/PDB:' + (Join-Path $OutDir $Target.Pdb))
    )
    # link.exe reuses an existing PDB and carries its record into the binary's /Brepro identity stamps (measured:
    # a rerun over the old PDB changed only those), so each link starts from none, a Debug one without its .ilk.
    $pdbPath = Join-Path $OutDir $Target.Pdb
    if (Test-Path -LiteralPath $pdbPath) { Remove-Item -LiteralPath $pdbPath -Force }
    # Incremental (Debug) links would otherwise drop the .ilk next to the binary.
    if ($Config -eq 'Debug') {
        $ilkPath = Join-Path $objDir ([System.IO.Path]::GetFileNameWithoutExtension($Target.Out) + '.ilk')
        if (Test-Path -LiteralPath $ilkPath) { Remove-Item -LiteralPath $ilkPath -Force }
        $linkArgs += '/ILK:' + $ilkPath
    }
    $linkArgs += $Target.LinkExtra + $objects + $resources + $Libs
    Invoke-Tool $Link $linkArgs
}

try {
    # --- Output directories -------------------------------------------------------
    if ($Clean -and (Test-Path -LiteralPath $OutDir)) {
        Remove-Item -LiteralPath $OutDir -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $ObjRoot | Out-Null
    [System.IO.File]::WriteAllText($LogFile, "PlugY4Kolbot build: Config=$Config Crt=$Crt" + [Environment]::NewLine)
    if ($Define.Count) { Write-Log ('Extra PlugY.dll defines: ' + ($Define -join ' ')) }

    # --- Toolchain discovery --------------------------------------------------------
    # FCT_ASM in d2functions.h relies on __declspec(naked) + inline __asm, which only
    # the x86-targeting compiler accepts, so the environment must be vcvarsall x86.
    $VsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $VsWhere)) { throw "vswhere.exe not found at $VsWhere" }
    $VsPath = & $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $VsPath) { throw 'No Visual Studio installation with the VC x86/x64 tools was found.' }
    $VsPath = @($VsPath)[0]
    $VcVars = Join-Path $VsPath 'VC\Auxiliary\Build\vcvarsall.bat'
    if (-not (Test-Path -LiteralPath $VcVars)) { throw "vcvarsall.bat not found at $VcVars" }

    # A batch file sidesteps cmd.exe quote-stripping differences between PS 5.1 and 7.
    # vcvarsall shells out to vswhere.exe by name, so its directory goes on PATH first.
    $EnvBat = Join-Path $ObjRoot 'vcvars-x86.cmd'
    $VsWhereDir = Split-Path -Parent $VsWhere
    [System.IO.File]::WriteAllText($EnvBat, "@set `"PATH=$VsWhereDir;%PATH%`"`r`n@call `"$VcVars`" x86 >nul`r`n@if errorlevel 1 exit /b 1`r`n@set`r`n")
    $envLines = & cmd.exe /d /c "`"$EnvBat`""
    if ($LASTEXITCODE -ne 0) { throw "vcvarsall.bat x86 failed (exit $LASTEXITCODE)" }
    foreach ($line in $envLines) {
        $eq = $line.IndexOf('=')
        if ($eq -gt 0) {
            [Environment]::SetEnvironmentVariable($line.Substring(0, $eq), $line.Substring($eq + 1), 'Process')
        }
    }
    # cl.exe and link.exe silently splice these variables into every command line;
    # dropping them keeps the flags below the complete, reproducible set.
    foreach ($name in @('CL', '_CL_', 'LINK', '_LINK_')) {
        if (Test-Path -LiteralPath ('Env:' + $name)) {
            Write-Log "Ignoring inherited environment variable $name"
            Remove-Item -LiteralPath ('Env:' + $name)
        }
    }

    $Cl = (Get-Command cl.exe -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
    $Link = (Get-Command link.exe -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
    $Rc = (Get-Command rc.exe -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
    if ($Cl -notmatch '\\bin\\Host[^\\]+\\x86\\cl\.exe$') { throw "cl.exe is not the x86-targeting compiler: $Cl" }
    Write-Log "Visual Studio: $VsPath"
    Write-Log "cl.exe:        $Cl"
    Write-Log "link.exe:      $Link"
    Write-Log "rc.exe:        $Rc"

    # --- Flags ----------------------------------------------------------------------
    # Every binary: MBCS, /W3, no precompiled headers, /EHsc. -Crt picks the CRT for
    # all but the updater; MT (the default) means no VC redistributable on the player's machine.
    $Release = $Config -eq 'Release'
    $NDebug = if ($Release) { 'NDEBUG' } else { '_DEBUG' }
    $CrtFlag = '/' + $Crt + $(if ($Release) { '' } else { 'd' })
    # Release: speed optimization with intrinsics, function-level linking, whole-program
    # optimization and a separate PDB; the linker drops unreferenced code and folds
    # identical functions. Debug: no optimization, run-time checks and
    # edit-and-continue debug info with incremental links. No /Gm: VS2022 deprecates it.
    $OptFlags = if ($Release) { @('/O2', '/Oi', '/GL', '/Gy', '/Zi', $CrtFlag) } else { @('/Od', '/RTC1', '/ZI', $CrtFlag) }
    $OptLink = if ($Release) { @('/INCREMENTAL:NO', '/OPT:REF', '/OPT:ICF', '/LTCG') } else { @('/INCREMENTAL') }

    # The VS2008 default library set; one that nothing uses adds no import. The src/common
    # sources also pull their own via #pragma comment (Version, winhttp, bcrypt).
    $Libs = @('kernel32.lib', 'user32.lib', 'gdi32.lib', 'winspool.lib', 'comdlg32.lib',
              'advapi32.lib', 'shell32.lib', 'ole32.lib', 'oleaut32.lib', 'uuid.lib',
              'odbc32.lib', 'odbccp32.lib')

    # DLL target (_WINDLL), __cdecl (/Gd). VS2008-era code generation (x87 floating
    # point, unguarded function-local statics) restored by /arch:IA32 /Zc:threadSafeInit-,
    # which newer cl.exe changed by default. The resource compiler gets only the
    # character-set define and culture 1033, as VS2008 passed. winhttp.dll is
    # delay-loaded: the update check can be off, and Game.exe must not load it at startup.
    $Dll = @{
        Key       = 'dll'
        Sources   = $DllSources
        Rc        = 'src\plugy\PlugY.rc'
        Out       = 'PlugY.dll'
        Pdb       = 'PlugY.pdb'
        ClFlags   = @('/nologo', '/c', '/W3', '/EHsc', '/Gd', '/arch:IA32', '/Zc:threadSafeInit-') + $OptFlags
        Defines   = @($NDebug, 'WIN32', '_WINDOWS', '_CRT_NON_CONFORMING_SWPRINTFS', '_CRT_SECURE_NO_WARNINGS',
                      '_USRDLL', 'PLUGY_EXPORTS', '_WINDLL', '_MBCS') + $Define
        RcFlags   = @('/d', '_MBCS', '/l', '0x409', '/i', (Join-Path $RepoRoot 'src\plugy'))
        LinkFlags = @('/NOLOGO', '/DLL', '/SUBSYSTEM:WINDOWS', '/MACHINE:X86', '/DEBUG',
                      '/DYNAMICBASE', '/NXCOMPAT', '/MANIFEST:EMBED') + $OptLink
        LinkExtra = @(('/MAP:' + (Join-Path $OutDir 'PlugY.map')),
                      ('/IMPLIB:' + (Join-Path (Join-Path $ObjRoot 'dll') 'PlugY.lib')),
                      '/DELAYLOAD:winhttp.dll', 'winhttp.lib', 'delayimp.lib')
    }

    # The three EXEs keep the compiler's default /arch and thread-safe statics: none of
    # their compiled code runs inside Game.exe (the launcher injects hand-assembled bytes).
    $ExeCl = @('/nologo', '/c', '/W3', '/EHsc') + $OptFlags
    $ExeLink = @('/NOLOGO', '/SUBSYSTEM:WINDOWS', '/MACHINE:X86', '/DEBUG') + $OptLink +
               @('/DYNAMICBASE', '/NXCOMPAT', '/MANIFEST:EMBED', "/MANIFESTUAC:level='asInvoker'")
    $ExeRc = @('/d', '_MBCS', '/d', $NDebug, '/l', '0x409')
    $Run = @{
        Key       = 'run'
        Sources   = @('src\launcher\PlugYRun.cpp', 'src\common\VersionInfo.cpp')
        Rc        = 'src\launcher\PlugYRun.rc'
        Out       = 'PlugY.exe'
        Pdb       = 'PlugYRun.pdb'
        ClFlags   = $ExeCl
        Defines   = @('WIN32', $NDebug, '_WINDOWS', '_MBCS', '_CRT_SECURE_NO_WARNINGS')
        RcFlags   = $ExeRc
        LinkFlags = $ExeLink
        LinkExtra = @()
    }
    # PatchD2File and RestoreD2File are the same source; RESTORE selects the mode.
    # They run as administrator from the Diablo II folder (the installer, or their own
    # elevated restart): version.dll is delay-loaded, after main limits the search to
    # System32, and 0x800 (LOAD_LIBRARY_SEARCH_SYSTEM32) covers the static imports.
    $Patch = @{
        Key       = 'patch'
        Sources   = @('src\patcher\PlugYInstall.cpp', 'src\common\VersionInfo.cpp')
        Rc        = 'src\patcher\PlugYInstall.rc'
        Out       = 'PatchD2File.exe'
        Pdb       = 'PatchD2File.pdb'
        ClFlags   = $ExeCl
        Defines   = @('WIN32', $NDebug, '_WINDOWS', '_MBCS')
        RcFlags   = $ExeRc
        LinkFlags = $ExeLink
        LinkExtra = @('/DELAYLOAD:version.dll', 'delayimp.lib', '/DEPENDENTLOADFLAG:0x800')
    }
    $Restore = $Patch.Clone()
    $Restore.Key = 'restore'
    $Restore.Out = 'RestoreD2File.exe'
    $Restore.Pdb = 'RestoreD2File.pdb'
    $Restore.Defines = $Patch.Defines + @('RESTORE')
    $Restore.RcFlags = $ExeRc + @('/d', 'RESTORE')

    # The updater and the tests: the shipped optimization flags, but always the static
    # CRT, so they run on a player's machine without a VC redistributable whatever -Crt is.
    $StaticOpt = @($OptFlags | Where-Object { $_ -ne $CrtFlag }) + @('/MT' + $(if ($Release) { '' } else { 'd' }))
    $ConsoleLink = @('/NOLOGO', '/SUBSYSTEM:CONSOLE', '/MACHINE:X86', '/DEBUG') + $OptLink +
                   @('/DYNAMICBASE', '/NXCOMPAT', '/MANIFEST:EMBED', "/MANIFESTUAC:level='asInvoker'")
    # A console program: it prints progress and asks before installing. asInvoker keeps
    # Windows from virtualizing its writes to Program Files; it elevates itself when needed.
    $Update = @{
        Key       = 'update'
        Sources   = @('src\updater\PlugYUpdate.cpp', 'src\updater\UpdaterLogic.cpp', 'src\updater\UpdaterIniMerge.cpp',
                      'src\common\UpdateCheck.cpp', 'src\common\UpdateDownload.cpp')
        Rc        = 'src\updater\PlugYUpdate.rc'
        Out       = 'PlugY4KolbotUpdater.exe'
        Pdb       = 'PlugY4KolbotUpdater.pdb'
        ClFlags   = @('/nologo', '/c', '/W3', '/EHsc') + $StaticOpt
        Defines   = @('WIN32', $NDebug, '_CONSOLE', '_MBCS', '_CRT_SECURE_NO_WARNINGS')
        RcFlags   = $ExeRc
        LinkFlags = $ConsoleLink
        # DLL planting: winhttp, bcrypt and version.dll are not KnownDLLs, so an elevated run
        # would load copies planted beside the exe. They are delay-loaded (wmain first limits
        # the search to System32), and 0x800 (LOAD_LIBRARY_SEARCH_SYSTEM32) covers the
        # remaining static imports.
        LinkExtra = @('winhttp.lib', 'bcrypt.lib', 'version.lib', 'shell32.lib', 'advapi32.lib',
                      '/DELAYLOAD:winhttp.dll', '/DELAYLOAD:bcrypt.dll', '/DELAYLOAD:version.dll', 'delayimp.lib',
                      '/DEPENDENTLOADFLAG:0x800')
    }

    foreach ($target in @($Dll, $Run, $Patch, $Restore, $Update)) { Build-Target $target }

    if ($Test) {
        New-Item -ItemType Directory -Force -Path (Join-Path $OutDir 'tests') | Out-Null
        $Tests = @{
            Key       = 'tests'
            Sources   = @('tests\UpdateCheckTests.cpp', 'src\common\UpdateCheck.cpp', 'src\common\UpdateDownload.cpp')
            Rc        = $null
            Out       = 'tests\UpdateCheckTests.exe'
            Pdb       = 'tests\UpdateCheckTests.pdb'
            ClFlags   = @('/nologo', '/c', '/W3', '/EHsc') + $StaticOpt
            Defines   = @('WIN32', $NDebug, '_CONSOLE', '_MBCS', '_CRT_SECURE_NO_WARNINGS')
            RcFlags   = @()
            LinkFlags = $ConsoleLink
            LinkExtra = @('winhttp.lib', 'bcrypt.lib')
        }
        Build-Target $Tests
        $UpdaterTests = @{
            Key       = 'updtests'
            Sources   = @('tests\UpdaterTests.cpp', 'src\updater\UpdaterLogic.cpp', 'src\updater\UpdaterIniMerge.cpp')
            Rc        = $null
            Out       = 'tests\UpdaterTests.exe'
            Pdb       = 'tests\UpdaterTests.pdb'
            ClFlags   = @('/nologo', '/c', '/W3', '/EHsc') + $StaticOpt
            Defines   = @('WIN32', $NDebug, '_CONSOLE', '_MBCS', '_CRT_SECURE_NO_WARNINGS')
            RcFlags   = @()
            LinkFlags = $ConsoleLink
            LinkExtra = @()
        }
        Build-Target $UpdaterTests
        $SessionLogTests = @{
            Key       = 'logtests'
            Sources   = @('tests\SessionLogTests.cpp', 'src\common\SessionLog.cpp')
            Rc        = $null
            Out       = 'tests\SessionLogTests.exe'
            Pdb       = 'tests\SessionLogTests.pdb'
            ClFlags   = @('/nologo', '/c', '/W3', '/EHsc') + $StaticOpt
            Defines   = @('WIN32', $NDebug, '_CONSOLE', '_MBCS', '_CRT_SECURE_NO_WARNINGS')
            RcFlags   = @()
            LinkFlags = $ConsoleLink
            LinkExtra = @()
        }
        Build-Target $SessionLogTests
        $SaveTransferTests = @{
            Key       = 'savetests'
            Sources   = @('tests\SaveTransferTests.cpp')
            Rc        = $null
            Out       = 'tests\SaveTransferTests.exe'
            Pdb       = 'tests\SaveTransferTests.pdb'
            ClFlags   = @('/nologo', '/c', '/W3', '/EHsc') + $StaticOpt
            Defines   = @('WIN32', $NDebug, '_CONSOLE', '_MBCS', '_CRT_SECURE_NO_WARNINGS')
            RcFlags   = @()
            LinkFlags = $ConsoleLink
            LinkExtra = @()
        }
        Build-Target $SaveTransferTests
        $MovedItemsPageTests = @{
            Key       = 'movedtests'
            Sources   = @('tests\MovedItemsPageTests.cpp')
            Rc        = $null
            Out       = 'tests\MovedItemsPageTests.exe'
            Pdb       = 'tests\MovedItemsPageTests.pdb'
            ClFlags   = @('/nologo', '/c', '/W3', '/EHsc') + $StaticOpt
            Defines   = @('WIN32', $NDebug, '_CONSOLE', '_MBCS', '_CRT_SECURE_NO_WARNINGS')
            RcFlags   = @()
            LinkFlags = $ConsoleLink
            LinkExtra = @()
        }
        Build-Target $MovedItemsPageTests
        $ReleasedConnectionsTests = @{
            Key       = 'releasedtests'
            Sources   = @('tests\ReleasedConnectionsTests.cpp')
            Rc        = $null
            Out       = 'tests\ReleasedConnectionsTests.exe'
            Pdb       = 'tests\ReleasedConnectionsTests.pdb'
            ClFlags   = @('/nologo', '/c', '/W3', '/EHsc') + $StaticOpt
            Defines   = @('WIN32', $NDebug, '_CONSOLE', '_MBCS', '_CRT_SECURE_NO_WARNINGS')
            RcFlags   = @()
            LinkFlags = $ConsoleLink
            LinkExtra = @()
        }
        Build-Target $ReleasedConnectionsTests
        $StatBonusTests = @{
            Key       = 'statbonustests'
            Sources   = @('tests\StatBonusTests.cpp')
            Rc        = $null
            Out       = 'tests\StatBonusTests.exe'
            Pdb       = 'tests\StatBonusTests.pdb'
            ClFlags   = @('/nologo', '/c', '/W3', '/EHsc') + $StaticOpt
            Defines   = @('WIN32', $NDebug, '_CONSOLE', '_MBCS', '_CRT_SECURE_NO_WARNINGS')
            RcFlags   = @()
            LinkFlags = $ConsoleLink
            LinkExtra = @()
        }
        Build-Target $StatBonusTests
        $ServerMessageRulesTests = @{
            Key       = 'servermessagetests'
            Sources   = @('tests\ServerMessageRulesTests.cpp')
            Rc        = $null
            Out       = 'tests\ServerMessageRulesTests.exe'
            Pdb       = 'tests\ServerMessageRulesTests.pdb'
            ClFlags   = @('/nologo', '/c', '/W3', '/EHsc') + $StaticOpt
            Defines   = @('WIN32', $NDebug, '_CONSOLE', '_MBCS', '_CRT_SECURE_NO_WARNINGS')
            RcFlags   = @()
            LinkFlags = $ConsoleLink
            LinkExtra = @()
        }
        Build-Target $ServerMessageRulesTests
        $UpdateNoticeRulesTests = @{
            Key       = 'updatenoticetests'
            Sources   = @('tests\UpdateNoticeRulesTests.cpp')
            Rc        = $null
            Out       = 'tests\UpdateNoticeRulesTests.exe'
            Pdb       = 'tests\UpdateNoticeRulesTests.pdb'
            ClFlags   = @('/nologo', '/c', '/W3', '/EHsc') + $StaticOpt
            Defines   = @('WIN32', $NDebug, '_CONSOLE', '_MBCS', '_CRT_SECURE_NO_WARNINGS')
            RcFlags   = @()
            LinkFlags = $ConsoleLink
            LinkExtra = @()
        }
        Build-Target $UpdateNoticeRulesTests
        $IniSectionTests = @{
            Key       = 'inisectiontests'
            Sources   = @('tests\IniSectionTests.cpp')
            Rc        = $null
            Out       = 'tests\IniSectionTests.exe'
            Pdb       = 'tests\IniSectionTests.pdb'
            ClFlags   = @('/nologo', '/c', '/W3', '/EHsc') + $StaticOpt
            Defines   = @('WIN32', $NDebug, '_CONSOLE', '_MBCS', '_CRT_SECURE_NO_WARNINGS')
            RcFlags   = @()
            LinkFlags = $ConsoleLink
            LinkExtra = @()
        }
        Build-Target $IniSectionTests
        $CharacterCommitTests = @{
            Key       = 'committests'
            Sources   = @('tests\CharacterCommitTests.cpp', 'src\common\CharacterCommit.cpp')
            Rc        = $null
            Out       = 'tests\CharacterCommitTests.exe'
            Pdb       = 'tests\CharacterCommitTests.pdb'
            ClFlags   = @('/nologo', '/c', '/W3', '/EHsc') + $StaticOpt
            Defines   = @('WIN32', $NDebug, '_CONSOLE', '_MBCS', '_CRT_SECURE_NO_WARNINGS')
            RcFlags   = @()
            LinkFlags = $ConsoleLink
            LinkExtra = @()
        }
        Build-Target $CharacterCommitTests
        $SpriteLoadRulesTests = @{
            Key       = 'spriteloadtests'
            Sources   = @('tests\SpriteLoadRulesTests.cpp')
            Rc        = $null
            Out       = 'tests\SpriteLoadRulesTests.exe'
            Pdb       = 'tests\SpriteLoadRulesTests.pdb'
            ClFlags   = @('/nologo', '/c', '/W3', '/EHsc') + $StaticOpt
            Defines   = @('WIN32', $NDebug, '_CONSOLE', '_MBCS', '_CRT_SECURE_NO_WARNINGS')
            RcFlags   = @()
            LinkFlags = $ConsoleLink
            LinkExtra = @()
        }
        Build-Target $SpriteLoadRulesTests
        $SharedStashStoreTests = @{
            Key       = 'sharedstashtests'
            Sources   = @('tests\SharedStashStoreTests.cpp', 'src\common\SharedStashStore.cpp', 'third_party\sqlite\sqlite3.c')
            Rc        = $null
            Out       = 'tests\SharedStashStoreTests.exe'
            Pdb       = 'tests\SharedStashStoreTests.pdb'
            ClFlags   = @('/nologo', '/c', '/W3', '/EHsc') + $StaticOpt
            Defines   = @('WIN32', $NDebug, '_CONSOLE', '_MBCS', '_CRT_SECURE_NO_WARNINGS')
            RcFlags   = @()
            LinkFlags = $ConsoleLink
            LinkExtra = @()
        }
        Build-Target $SharedStashStoreTests
        $SharedStashLegacyTests = @{
            Key       = 'sharedstashlegacytests'
            Sources   = @('tests\SharedStashLegacyTests.cpp')
            Rc        = $null
            Out       = 'tests\SharedStashLegacyTests.exe'
            Pdb       = 'tests\SharedStashLegacyTests.pdb'
            ClFlags   = @('/nologo', '/c', '/W3', '/EHsc') + $StaticOpt
            Defines   = @('WIN32', $NDebug, '_CONSOLE', '_MBCS', '_CRT_SECURE_NO_WARNINGS')
            RcFlags   = @()
            LinkFlags = $ConsoleLink
            LinkExtra = @()
        }
        Build-Target $SharedStashLegacyTests
        $SharedStashTransportTests = @{
            Key       = 'sharedstashtransporttests'
            Sources   = @('tests\SharedStashTransportTests.cpp')
            Rc        = $null
            Out       = 'tests\SharedStashTransportTests.exe'
            Pdb       = 'tests\SharedStashTransportTests.pdb'
            ClFlags   = @('/nologo', '/c', '/W3', '/EHsc', '/Gy') + $StaticOpt
            Defines   = @('WIN32', $NDebug, '_CONSOLE', '_MBCS', '_CRT_NON_CONFORMING_SWPRINTFS', '_CRT_SECURE_NO_WARNINGS')
            RcFlags   = @()
            LinkFlags = $ConsoleLink + @('/OPT:REF')
            LinkExtra = @()
        }
        Build-Target $SharedStashTransportTests
        $SharedStashItemPacketTests = @{
            Key       = 'sharedstashitempackettests'
            Sources   = @('tests\SharedStashItemPacketTests.cpp')
            Rc        = $null
            Out       = 'tests\SharedStashItemPacketTests.exe'
            Pdb       = 'tests\SharedStashItemPacketTests.pdb'
            ClFlags   = @('/nologo', '/c', '/W3', '/EHsc') + $StaticOpt
            Defines   = @('WIN32', $NDebug, '_CONSOLE', '_MBCS', '_CRT_SECURE_NO_WARNINGS')
            RcFlags   = @()
            LinkFlags = $ConsoleLink
            LinkExtra = @()
        }
        Build-Target $SharedStashItemPacketTests
        $SharedStashCubeSwapTests = @{
            Key       = 'sharedstashcubeswaptests'
            Sources   = @('tests\SharedStashCubeSwapTests.cpp')
            Rc        = $null
            Out       = 'tests\SharedStashCubeSwapTests.exe'
            Pdb       = 'tests\SharedStashCubeSwapTests.pdb'
            ClFlags   = @('/nologo', '/c', '/W3', '/EHsc') + $StaticOpt
            Defines   = @('WIN32', $NDebug, '_CONSOLE', '_MBCS', '_CRT_SECURE_NO_WARNINGS')
            RcFlags   = @()
            LinkFlags = $ConsoleLink
            LinkExtra = @()
        }
        Build-Target $SharedStashCubeSwapTests
        $SharedStashCleanupTests = @{
            Key       = 'sharedstashcleanuptests'
            Sources   = @('tests\SharedStashCleanupTests.cpp', 'src\common\SharedStashCleanup.cpp', 'src\common\SharedStashStore.cpp', 'third_party\sqlite\sqlite3.c')
            Rc        = $null
            Out       = 'tests\SharedStashCleanupTests.exe'
            Pdb       = 'tests\SharedStashCleanupTests.pdb'
            ClFlags   = @('/nologo', '/c', '/W3', '/EHsc') + $StaticOpt
            Defines   = @('WIN32', $NDebug, '_CONSOLE', '_MBCS', '_CRT_SECURE_NO_WARNINGS')
            RcFlags   = @()
            LinkFlags = $ConsoleLink
            LinkExtra = @('user32.lib')
        }
        Build-Target $SharedStashCleanupTests
        $SharedStashNativeRulesTests = @{
            Key       = 'sharedstashnativerulestests'
            Sources   = @('tests\SharedStashNativeRulesTests.cpp')
            Rc        = $null
            Out       = 'tests\SharedStashNativeRulesTests.exe'
            Pdb       = 'tests\SharedStashNativeRulesTests.pdb'
            ClFlags   = @('/nologo', '/c', '/W3', '/EHsc', '/Gy') + $StaticOpt
            Defines   = @('WIN32', $NDebug, '_CONSOLE', '_MBCS', '_CRT_NON_CONFORMING_SWPRINTFS', '_CRT_SECURE_NO_WARNINGS')
            RcFlags   = @()
            LinkFlags = $ConsoleLink + @('/OPT:REF')
            LinkExtra = @()
        }
        Build-Target $SharedStashNativeRulesTests

        $testArguments = @('--fixtures', (Join-Path $RepoRoot 'tests\fixtures'),
                           '--scratch', (Join-Path (Join-Path $OutDir 'tests') 'scratch'))
        if ($TestArgs) { $testArguments += $TestArgs }
        Write-Log ''
        Write-Log '=== UpdateCheckTests.exe'
        Invoke-Tool (Join-Path $OutDir $Tests.Out) $testArguments
        Write-Log ''
        Write-Log '=== UpdaterTests.exe'
        Invoke-Tool (Join-Path $OutDir $UpdaterTests.Out) @()
        Write-Log ''
        Write-Log '=== SessionLogTests.exe'
        Invoke-Tool (Join-Path $OutDir $SessionLogTests.Out) @()
        Write-Log ''
        Write-Log '=== SaveTransferTests.exe'
        Invoke-Tool (Join-Path $OutDir $SaveTransferTests.Out) @()
        Write-Log ''
        Write-Log '=== MovedItemsPageTests.exe'
        Invoke-Tool (Join-Path $OutDir $MovedItemsPageTests.Out) @((Join-Path $RepoRoot 'game-files\PlugY\LocalizedStrings.ini'))
        Write-Log ''
        Write-Log '=== ReleasedConnectionsTests.exe'
        Invoke-Tool (Join-Path $OutDir $ReleasedConnectionsTests.Out) @()
        Write-Log ''
        Write-Log '=== StatBonusTests.exe'
        Invoke-Tool (Join-Path $OutDir $StatBonusTests.Out) @()
        Write-Log ''
        Write-Log '=== ServerMessageRulesTests.exe'
        Invoke-Tool (Join-Path $OutDir $ServerMessageRulesTests.Out) @()
        Write-Log ''
        Write-Log '=== UpdateNoticeRulesTests.exe'
        Invoke-Tool (Join-Path $OutDir $UpdateNoticeRulesTests.Out) @()
        Write-Log ''
        Write-Log '=== IniSectionTests.exe'
        Invoke-Tool (Join-Path $OutDir $IniSectionTests.Out) @(
            (Join-Path $RepoRoot 'game-files\PlugY.ini'),
            (Join-Path $RepoRoot 'game-files\PlugY\PlugYDefault.ini'),
            (Join-Path $RepoRoot 'game-files\PlugY\PlugYFixed.ini'),
            (Join-Path $RepoRoot 'game-files\PlugY\LocalizedStrings.ini'))
        Write-Log ''
        Write-Log '=== CharacterCommitTests.exe'
        Invoke-Tool (Join-Path $OutDir $CharacterCommitTests.Out) @()
        Write-Log ''
        Write-Log '=== SpriteLoadRulesTests.exe'
        Invoke-Tool (Join-Path $OutDir $SpriteLoadRulesTests.Out) @()
        Write-Log ''
        Write-Log '=== SharedStashStoreTests.exe'
        Invoke-Tool (Join-Path $OutDir $SharedStashStoreTests.Out) @()
        Write-Log ''
        Write-Log '=== SharedStashLegacyTests.exe'
        Invoke-Tool (Join-Path $OutDir $SharedStashLegacyTests.Out) @()
        Write-Log ''
        Write-Log '=== SharedStashTransportTests.exe'
        Invoke-Tool (Join-Path $OutDir $SharedStashTransportTests.Out) @()
        Write-Log ''
        Write-Log '=== SharedStashItemPacketTests.exe'
        Invoke-Tool (Join-Path $OutDir $SharedStashItemPacketTests.Out) @()
        Write-Log ''
        Write-Log '=== SharedStashCubeSwapTests.exe'
        Invoke-Tool (Join-Path $OutDir $SharedStashCubeSwapTests.Out) @()
        Write-Log ''
        Write-Log '=== SharedStashCleanupTests.exe'
        Invoke-Tool (Join-Path $OutDir $SharedStashCleanupTests.Out) @()
        Write-Log ''
        Write-Log '=== SharedStashNativeRulesTests.exe'
        Invoke-Tool (Join-Path $OutDir $SharedStashNativeRulesTests.Out) @()
    }

    $warnings = @(Get-Content -LiteralPath $LogFile | Where-Object { $_ -match ': warning [A-Z]+\d+' }).Count
    $tested = if ($Test) { '; update-check, updater, session-log, save-transfer, moved-items-page, released-connections, stat-bonus, server-message, update-notice, ini-section, character-commit, sprite-load, shared-stash-store, shared-stash-legacy, shared-stash-transport, shared-stash-item-packet, shared-stash-cube-swap and shared-stash-cleanup tests passed' } else { '' }
    Write-Host ''
    $defined = if ($Define.Count) { ', PlugY.dll with ' + ($Define -join ' ') } else { '' }
    Write-Host "Built PlugY.dll, PlugY.exe, PatchD2File.exe, RestoreD2File.exe, PlugY4KolbotUpdater.exe ($Config, /$Crt$defined) with $warnings warning(s) in $OutDir$tested" -ForegroundColor Green
    exit 0
} finally {
    Restore-Environment
}

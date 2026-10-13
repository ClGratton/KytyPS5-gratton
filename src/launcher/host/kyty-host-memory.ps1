# Windows helper of the launcher's "Host memory" window (docs/host-memory.md).
#
# NOT TESTED ON REAL WINDOWS. It was written without a Windows machine at hand; the decision logic that
# chooses what to ask of it is unit-tested (tests/HostMemoryPolicyTests.cpp), this script is not.
#
#   kyty-host-memory.ps1 -Action Status                          print key=value lines (no admin needed)
#   kyty-host-memory.ps1 -Action Apply [-EnableMemoryCompression] [-SystemManagedPagefile] [-DryRun]
#   kyty-host-memory.ps1 -Action Undo [-DryRun]
#
# The launcher compiles this text in and passes it to an elevated powershell.exe as an encoded command, so the file
# next to the launcher is never what runs as administrator (and the script must stay under about 11 KB: UTF-16 and base64 make the command line 2.7 times as long, and Windows allows 32767 characters).
# The launcher starts Apply and Undo through a UAC prompt. Apply records the previous settings in
# %ProgramData%\KytyHostMemory\state.json; Undo puts them back. The settings themselves (memory compression,
# pagefile) are kept by Windows across reboots; no service or scheduled task is installed.
# Memory compression and a changed pagefile take effect after a restart.
param(
    [Parameter(Mandatory = $true)][ValidateSet('Status', 'Apply', 'Undo')][string]$Action,
    [switch]$EnableMemoryCompression,
    [switch]$SystemManagedPagefile,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
$StateDir  = Join-Path $env:ProgramData 'KytyHostMemory'
$StateFile = Join-Path $StateDir 'state.json'
$LogFile   = Join-Path $StateDir 'last-run.log'

function Write-Log([string]$Text) {
    Write-Output $Text
    if ($Action -ne 'Status' -and -not $DryRun -and (Test-Admin)) {
        Initialize-StateDir
        Add-Content -Path $LogFile -Value $Text
    }
}

# The state file decides what Undo restores, so the folder must be writable by administrators only: a folder a normal user
# created beforehand (or can still write to) is refused.
function Initialize-StateDir {
    if (Test-Path $StateDir) {
        $owner = (Get-Acl $StateDir).Owner
        if ($owner -notmatch '(Administrators|SYSTEM|TrustedInstaller)$') { throw "refusing $StateDir: owned by $owner" }
        return
    }
    New-Item -ItemType Directory -Path $StateDir -Force | Out-Null
    $acl = New-Object System.Security.AccessControl.DirectorySecurity
    $acl.SetAccessRuleProtection($true, $false)
    foreach ($who in 'NT AUTHORITY\SYSTEM', 'BUILTIN\Administrators') {
        $acl.AddAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule(
            $who, 'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow')))
    }
    Set-Acl -Path $StateDir -AclObject $acl
}

function Test-EmulatorRunning {
    return $null -ne (Get-Process -Name 'kyty_emulator*' -ErrorAction SilentlyContinue)
}

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    return ([Security.Principal.WindowsPrincipal]$id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-PagefileMode {
    $cs = Get-CimInstance Win32_ComputerSystem
    if ($cs.AutomaticManagedPagefile) { return 'system' }
    $settings = @(Get-CimInstance Win32_PageFileSetting -ErrorAction SilentlyContinue)
    if ($settings.Count -eq 0) { return 'none' }
    # "System managed size" chosen per drive is stored as initial = maximum = 0.
    $sized = @($settings | Where-Object { $_.InitialSize -ne 0 -or $_.MaximumSize -ne 0 })
    if ($sized.Count -eq 0) { return 'system' }
    return 'fixed'
}

function Get-MemoryCompression {
    try { return [bool](Get-MMAgent).MemoryCompression } catch { return $null }
}

if ($Action -eq 'Status') {
    $cs   = Get-CimInstance Win32_ComputerSystem
    $perf = Get-CimInstance Win32_PerfRawData_PerfOS_Memory      # CommitLimit and CommittedBytes are in bytes
    $use  = @(Get-CimInstance Win32_PageFileUsage -ErrorAction SilentlyContinue)
    $pagefileMiB = 0
    foreach ($u in $use) { $pagefileMiB += [int64]$u.AllocatedBaseSize }
    $compression = Get-MemoryCompression
    "ram_mib=" + [int64]($cs.TotalPhysicalMemory / 1MB)
    "commit_limit_mib=" + [int64]($perf.CommitLimit / 1MB)
    "commit_charged_mib=" + [int64]($perf.CommittedBytes / 1MB)
    "pagefile_mode=" + (Get-PagefileMode)
    "pagefile_mib=" + $pagefileMiB
    "memory_compression=" + $(if ($null -eq $compression) { 'unknown' } elseif ($compression) { 'true' } else { 'false' })
    "emulator_running=" + $(if (Test-EmulatorRunning) { 1 } else { 0 })
    "applied=" + $(if (Test-Path $StateFile) { 1 } else { 0 })
    exit 0
}

if (-not $DryRun -and -not (Test-Admin)) { Write-Log 'error: needs administrator rights (the launcher asks for them)'; exit 1 }
if (Test-EmulatorRunning) {
    if ($DryRun) { Write-Log 'note: an emulator is running; the real run would refuse' }
    else { Write-Log 'error: an emulator is running; close it and try again'; exit 1 }
}
if (-not $DryRun) { Initialize-StateDir; Set-Content -Path $LogFile -Value '' }

function Do-Step([string]$Description, [scriptblock]$Block) {
    if ($DryRun) { Write-Log "would: $Description"; return }
    & $Block
    Write-Log "done: $Description"
}

if ($Action -eq 'Apply') {
    if (-not $EnableMemoryCompression -and -not $SystemManagedPagefile) { Write-Log 'error: nothing to apply'; exit 2 }

    # Record the previous settings once; a second Apply must not record our own settings as "previous".
    if (-not (Test-Path $StateFile)) {
        $cs = Get-CimInstance Win32_ComputerSystem
        $state = @{
            MemoryCompression        = Get-MemoryCompression
            AutomaticManagedPagefile = [bool]$cs.AutomaticManagedPagefile
            PageFiles                = @(Get-CimInstance Win32_PageFileSetting -ErrorAction SilentlyContinue |
                                         ForEach-Object { @{ Name = $_.Name; InitialSize = $_.InitialSize; MaximumSize = $_.MaximumSize } })
        }
        Do-Step "record the previous settings in $StateFile" {
            Initialize-StateDir
            $state | ConvertTo-Json -Depth 4 | Set-Content -Path $StateFile
        }
    }
    if ($EnableMemoryCompression) {
        Do-Step 'turn memory compression on (after a restart)' { Enable-MMAgent -MemoryCompression }
    }
    if ($SystemManagedPagefile) {
        Do-Step 'let Windows manage the pagefile size (after a restart)' {
            $cs = Get-CimInstance Win32_ComputerSystem
            Set-CimInstance -InputObject $cs -Property @{ AutomaticManagedPagefile = $true }
            Get-CimInstance Win32_PageFileSetting -ErrorAction SilentlyContinue | Remove-CimInstance
        }
    }
    Write-Log 'restart Windows for the changes to take effect'
    exit 0
}

if ($Action -eq 'Undo') {
    if (-not (Test-Path $StateFile)) { Write-Log 'nothing to undo: the launcher has not changed these settings'; exit 0 }
    $state = Get-Content -Path $StateFile -Raw | ConvertFrom-Json
    if ($state.MemoryCompression -eq $false) {
        Do-Step 'turn memory compression off again (after a restart)' { Disable-MMAgent -MemoryCompression }
    }
    if (-not $state.AutomaticManagedPagefile) {
        Do-Step 'restore the previous pagefile settings (after a restart)' {
            $cs = Get-CimInstance Win32_ComputerSystem
            Set-CimInstance -InputObject $cs -Property @{ AutomaticManagedPagefile = $false }
            foreach ($p in @($state.PageFiles)) {
                if ($null -eq $p -or [string]::IsNullOrEmpty($p.Name)) { continue }
                New-CimInstance -ClassName Win32_PageFileSetting -Property @{
                    Name = $p.Name; InitialSize = [uint32]$p.InitialSize; MaximumSize = [uint32]$p.MaximumSize } | Out-Null
            }
        }
    }
    Do-Step "remove $StateFile" { Remove-Item -Path $StateFile -Force }
    Write-Log 'restart Windows for the changes to take effect'
    exit 0
}

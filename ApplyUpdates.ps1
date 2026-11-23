param([switch]$VerifyOnly, [string]$Target)
$ErrorActionPreference = 'Stop'
$taskSource = [IO.Path]::GetFullPath($PSScriptRoot)
# Apply in place by default. The old hard-coded Desktop path belonged to one
# machine, so a copy anywhere else failed; -Target or RRToolTarget still selects
# another installation explicitly.
$taskTarget = if ($Target) { [IO.Path]::GetFullPath($Target) }
              elseif ($env:RRToolTarget) { [IO.Path]::GetFullPath($env:RRToolTarget) }
              else { $taskSource }
$taskUpdates = Get-Content -LiteralPath (Join-Path $taskSource 'updates.json') -Raw | ConvertFrom-Json
$taskPending = @()
foreach ($taskUpdate in $taskUpdates) {
    if ($taskUpdate.path -notmatch '^(?:(?:2_src_port|1_run)/[A-Za-z0-9_]+\.(?:h|c|cpp|html|exe|rc)|2_src_port/assets/weapons_atlas\.(?:png|svg)|offsets/(?:item_pricing_fields|party_fields|vehicle_owner_fields|inventory_map|messages_map)\.json|offsets/type_index\.txt|docs/[A-Za-z0-9_]+\.md|tools/[A-Za-z0-9_]+\.py)$') { throw 'Invalid update path' }
    $taskSourceFile = [IO.Path]::GetFullPath((Join-Path $taskSource $taskUpdate.path))
    $taskTargetFile = [IO.Path]::GetFullPath((Join-Path $taskTarget $taskUpdate.path))
    if (-not $taskSourceFile.StartsWith($taskSource + '\', [StringComparison]::OrdinalIgnoreCase) -or
        -not $taskTargetFile.StartsWith($taskTarget + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Update escaped the project directory' }
    # A null hash marks a build output: the executable in this folder is
    # authoritative and changes on every rebuild, so only its presence and its
    # copy are checked. Source files keep a pinned hash.
    $taskPinned = $taskUpdate.after -and $taskUpdate.after.Length -eq 64
    if ($taskPinned) {
        if ((Get-FileHash -LiteralPath $taskSourceFile -Algorithm SHA256).Hash -ne $taskUpdate.after) { throw "Updated file failed verification: $($taskUpdate.path)" }
    } elseif (-not (Test-Path -LiteralPath $taskSourceFile -PathType Leaf)) {
        throw "Missing update file: $($taskUpdate.path)"
    }
    $taskCurrentHash = if (Test-Path -LiteralPath $taskTargetFile -PathType Leaf) { (Get-FileHash -LiteralPath $taskTargetFile -Algorithm SHA256).Hash } else { $null }
    if ($taskPinned) {
        if ($taskCurrentHash -eq $taskUpdate.after) { continue }
        if ($taskCurrentHash -ne $taskUpdate.before) { throw "The target file changed after preparation: $($taskUpdate.path). No files were replaced." }
    } elseif ($taskCurrentHash -eq (Get-FileHash -LiteralPath $taskSourceFile -Algorithm SHA256).Hash) {
        continue
    }
    $taskPending += $taskUpdate
}
if ($VerifyOnly) {
    Write-Output "UPDATE VERIFICATION OK: $($taskUpdates.Count) files, $($taskPending.Count) pending. No Desktop files changed."
    exit 0
}
$taskAdmin = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $taskAdmin.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run APPLY_AND_RUN.bat as administrator.'
}
if ($taskPending.Count) {
    $taskBackup = Join-Path $taskTarget ('backup_before_loot_update_' + (Get-Date -Format 'yyyyMMdd_HHmmss_fff'))
    foreach ($taskUpdate in $taskPending) {
        if (-not (Test-Path -LiteralPath (Join-Path $taskTarget $taskUpdate.path) -PathType Leaf)) { continue }
        $taskBackupFile = Join-Path $taskBackup $taskUpdate.path
        New-Item -ItemType Directory -Path (Split-Path -Parent $taskBackupFile) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $taskTarget $taskUpdate.path) -Destination $taskBackupFile
    }
    foreach ($taskUpdate in $taskPending) {
        $taskTargetFile = Join-Path $taskTarget $taskUpdate.path
        New-Item -ItemType Directory -Path (Split-Path -Parent $taskTargetFile) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $taskSource $taskUpdate.path) -Destination $taskTargetFile -Force
        if ($taskUpdate.after -and $taskUpdate.after.Length -eq 64 -and
            (Get-FileHash -LiteralPath $taskTargetFile -Algorithm SHA256).Hash -ne $taskUpdate.after) { throw "Copy verification failed: $($taskUpdate.path). Backup: $taskBackup" }
    }
    Write-Output "Applied $($taskPending.Count) updates. Backup: $taskBackup"
} else {
    Write-Output 'The Desktop project is already updated.'
}

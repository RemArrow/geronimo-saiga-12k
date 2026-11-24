# Runs the SaigaDev harness in the installed game, flat (-nohmd) in a window, with the built Saiga12 plugin.
# The player's SaveGames, every GML config and the plugins folder are snapshotted first and put back after the
# game is closed (hash-verified); the harness and a test copy of the plugin never stay installed.
# Run it plainly (never pipe it into Select-Object -First: that stops the script before the restore).
param([int]$Timeout = 240, [string]$Out = "$PSScriptRoot\..\build\shots")
$ErrorActionPreference = "Stop"
$g = "C:\Program Files (x86)\Steam\steamapps\common\GERONIMO\Geronimo\Binaries\Win64"
$saves = "$env:LOCALAPPDATA\Geronimo\Saved\SaveGames"
$build = Resolve-Path "$PSScriptRoot\..\build"
$snap = Join-Path $env:TEMP ("saiga_snap_" + (Get-Date -Format yyyyMMdd_HHmmss))
if (Get-Process -Name "Geronimo*" -ErrorAction SilentlyContinue) { "Geronimo is running - not launching"; exit 1 }
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path

$hash = { param($p) Get-ChildItem $p -File -Recurse | Where-Object { $_.FullName -notlike "*\CustomMaps\Mods\*" } | Sort-Object FullName | ForEach-Object { $_.FullName.Substring($p.Length) + ':' + (Get-FileHash $_.FullName -Algorithm MD5).Hash } }
robocopy $saves "$snap\SaveGames" /MIR /NJH /NJS /NFL /NDL /NP | Out-Null
robocopy "$g\GML\config" "$snap\config" /MIR /NJH /NJS /NFL /NDL /NP | Out-Null
robocopy "$g\GML\plugins" "$snap\plugins" /MIR /XD "$g\GML\plugins\CustomMaps\Mods" /NJH /NJS /NFL /NDL /NP | Out-Null
$before = (& $hash $saves) + (& $hash "$g\GML\config") + (& $hash "$g\GML\plugins")
$appid = -not (Test-Path "$g\steam_appid.txt")
# The loader may be switched off (the GML Manager's toggle): on for the run, put back after.
$doorstop = "$g\doorstop_config.ini"
$doorstopBefore = [IO.File]::ReadAllBytes($doorstop)
"snapshot: $snap"

$log = "$g\GML\LogOutput.log"
try {
    foreach ($p in "Saiga12", "SaigaDev") { New-Item -ItemType Directory -Force "$g\GML\plugins\$p" | Out-Null }
    Copy-Item "$build\Saiga12\Saiga12.dll" "$g\GML\plugins\Saiga12\"
    Copy-Item "$build\tests\SaigaDev\SaigaDev.dll" "$g\GML\plugins\SaigaDev\"
    Set-Content "$g\GML\config\com.remarrow.saigadev.cfg" "[Test]`r`nShotDir = $Out`r`n"
    if ($appid) { Set-Content "$g\steam_appid.txt" "2019620" -NoNewline }
    $ds = [Text.Encoding]::UTF8.GetString($doorstopBefore)
    if ($ds -match '(?m)^enabled\s*=\s*false') { [IO.File]::WriteAllText($doorstop, ($ds -replace '(?m)^enabled\s*=\s*false', 'enabled = true')); "GML was switched off: on for this run" }
    $t0 = Get-Date
    Start-Process -FilePath "$g\Geronimo-Win64-Shipping.exe" -ArgumentList "-nohmd -windowed -ResX=1280 -ResY=720" | Out-Null
    $deadline = $t0.AddSeconds($Timeout)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep 3
        if ((Test-Path $log) -and (Get-Item $log).LastWriteTime -gt $t0 -and (Select-String -Path $log -Pattern "DEV TEST COMPLETE" -Quiet)) { "harness finished after $([int]((Get-Date) - $t0).TotalSeconds) s"; break }
        if (-not (Get-Process -Name "Geronimo-Win64-Shipping" -ErrorAction SilentlyContinue) -and ((Get-Date) - $t0).TotalSeconds -gt 30) { "THE GAME EXITED"; break }
    }
    if ((Get-Date) -ge $deadline) { "TIMEOUT after $Timeout s" }
} finally {
    Get-Process -Name "Geronimo*" -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep 4
    if (Test-Path $log) { Copy-Item $log "$Out\LogOutput.log" -Force }
    if ($appid) { Remove-Item "$g\steam_appid.txt" -ErrorAction SilentlyContinue }
    [IO.File]::WriteAllBytes($doorstop, $doorstopBefore)
    robocopy "$snap\SaveGames" $saves /MIR /NJH /NJS /NFL /NDL /NP | Out-Null
    robocopy "$snap\config" "$g\GML\config" /MIR /NJH /NJS /NFL /NDL /NP | Out-Null
    robocopy "$snap\plugins" "$g\GML\plugins" /MIR /XD "$g\GML\plugins\CustomMaps\Mods" /NJH /NJS /NFL /NDL /NP | Out-Null
    $after = (& $hash $saves) + (& $hash "$g\GML\config") + (& $hash "$g\GML\plugins")
    if (Compare-Object $before $after) { "RESTORE MISMATCH - snapshot kept at $snap" } else { "saves, configs and plugins restored (hash verified)" }
}

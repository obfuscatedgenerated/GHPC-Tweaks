$exePath = Join-Path (Get-Location) 'GHWT.exe'
Copy-Item $exePath "$exePath.backup" -ErrorAction Stop
$bytes = [System.IO.File]::ReadAllBytes($exePath)
$peHeaderOffset = [BitConverter]::ToInt32($bytes, 0x3C)
$characteristicsOffset = $peHeaderOffset + 22
$characteristics = [BitConverter]::ToUInt16($bytes, $characteristicsOffset)
if ($characteristics -band 0x20) { Write-Host 'Already large address aware.' }
else {
    $patched = [BitConverter]::GetBytes([uint16]($characteristics -bor 0x20))
    $bytes[$characteristicsOffset] = $patched[0]
    $bytes[$characteristicsOffset + 1] = $patched[1]
    [System.IO.File]::WriteAllBytes($exePath, $bytes)
    Write-Host 'Patched: GHWT can now use 4 GB. Backup saved as GHWT.exe.backup'
}

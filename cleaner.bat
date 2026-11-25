@echo off
setlocal EnableExtensions EnableDelayedExpansion
set "PS=%TEMP%\ariel_cleaner.ps1"

> "%PS%" (
  for /f "usebackq delims=" %%A in (`findstr /b "::PS::" "%~f0"`) do (
    set "line=%%A"
    set "line=!line:~6!"
    echo(!line!
  )
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%PS%" %*
set "code=%ERRORLEVEL%"
del "%PS%" >nul 2>&1
exit /b %code%

::PS::param([switch]$Force)
::PS::
::PS::$ErrorActionPreference = 'SilentlyContinue'
::PS::$removed = [ordered]@{}
::PS::$cleanerScript = $MyInvocation.MyCommand.Path
::PS::if (-not $cleanerScript) { $cleanerScript = Join-Path $env:TEMP 'ariel_cleaner.ps1' }
::PS::
::PS::function Mark($name, $count) {
::PS::  if ($count -gt 0) { $removed[$name] = $count }
::PS::  Write-Host ('  {0}: {1}' -f $name, $count)
::PS::}
::PS::
::PS::if (-not $Force) {
::PS::  $answer = Read-Host 'Remove Ariel artifacts from this PC [Y/N]'
::PS::  if ($answer -notmatch '^[Yy]') { Write-Host 'cancelled'; exit 0 }
::PS::}
::PS::Write-Host 'scanning...'
::PS::
::PS::function Test-Admin {
::PS::  $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
::PS::  $principal = New-Object Security.Principal.WindowsPrincipal($identity)
::PS::  return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
::PS::}
::PS::
::PS::function Test-ArielFile($path) {
::PS::  if (-not $path) { return $false }
::PS::  try {
::PS::    $item = Get-Item -LiteralPath $path -Force -ErrorAction Stop
::PS::    if ($item.PSIsContainer -or $item.Length -gt 52428800) { return $false }
::PS::    $bytes = [IO.File]::ReadAllBytes($item.FullName)
::PS::    $text = [Text.Encoding]::ASCII.GetString($bytes)
::PS::    return ($text.Contains('Ariel data summary') -or $text.Contains('by ihateapples on github'))
::PS::  } catch { return $false }
::PS::}
::PS::
::PS::function Test-ArielText($path) {
::PS::  if (-not $path) { return $false }
::PS::  try {
::PS::    $item = Get-Item -LiteralPath $path -Force -ErrorAction Stop
::PS::    if ($item.PSIsContainer -or $item.Length -gt 2097152) { return $false }
::PS::    $text = Get-Content -LiteralPath $path -Raw -ErrorAction Stop
::PS::    return ($text -match 'Ariel data summary' -or $text -match 'by ihateapples on github' -or
::PS::      $text -match 'ariel injection' -or $text -match '__ARIEL_WEBHOOK__' -or
::PS::      $text -match 'ariel\.injected' -or $text -match 'index\.js\.ariel')
::PS::  } catch { return $false }
::PS::}
::PS::
::PS::function Get-CommandPath($value) {
::PS::  $text = [string]$value
::PS::  if ($text -match '^"(.+)"$') { return $Matches[1] }
::PS::  return $text
::PS::}
::PS::
::PS::function Get-Roots {
::PS::  $list = @()
::PS::  $paths = @(
::PS::    $env:TEMP,
::PS::    $env:TMP,
::PS::    $env:APPDATA,
::PS::    $env:LOCALAPPDATA,
::PS::    [Environment]::GetFolderPath('Desktop'),
::PS::    [Environment]::GetFolderPath('MyDocuments'),
::PS::    (Join-Path $env:USERPROFILE 'Downloads')
::PS::  )
::PS::  foreach ($p in $paths) {
::PS::    if ($p -and (Test-Path -LiteralPath $p) -and ($list -notcontains $p)) { $list += $p }
::PS::  }
::PS::  return $list
::PS::}
::PS::
::PS::$roots = Get-Roots
::PS::$outputDirs = @()
::PS::
::PS::foreach ($root in $roots) {
::PS::  Get-ChildItem -LiteralPath $root -Directory -Recurse -Force -Depth 6 -ErrorAction SilentlyContinue |
::PS::    Where-Object { $_.Name -ieq 'output' } |
::PS::    ForEach-Object {
::PS::      $dir = $_.FullName
::PS::      $zips = @(Get-ChildItem -LiteralPath $dir -Filter 'ariel*.zip' -File -ErrorAction SilentlyContinue)
::PS::      $summary = Join-Path $dir 'summary.txt'
::PS::      $isAriel = $false
::PS::      if ($zips.Count -gt 0) { $isAriel = $true }
::PS::      if (Test-Path -LiteralPath $summary) {
::PS::        $text = Get-Content -LiteralPath $summary -Raw -ErrorAction SilentlyContinue
::PS::        if ($text -match 'Ariel data summary') { $isAriel = $true }
::PS::      }
::PS::      if ($isAriel -and ($outputDirs -notcontains $dir)) { $outputDirs += $dir }
::PS::    }
::PS::}
::PS::
::PS::$count = 0
::PS::foreach ($dir in $outputDirs) {
::PS::  Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
::PS::  if (-not (Test-Path -LiteralPath $dir)) { $count++ }
::PS::}
::PS::Mark 'output folders' $count
::PS::
::PS::$count = 0
::PS::foreach ($root in $roots) {
::PS::  Get-ChildItem -LiteralPath $root -Recurse -Force -File -Filter 'ariel*.zip' -Depth 6 -ErrorAction SilentlyContinue |
::PS::    ForEach-Object {
::PS::      $path = $_.FullName
::PS::      Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
::PS::      if (-not (Test-Path -LiteralPath $path)) { $count++ }
::PS::    }
::PS::}
::PS::Mark 'ariel zips' $count
::PS::
::PS::$count = 0
::PS::foreach ($root in $roots) {
::PS::  Get-ChildItem -LiteralPath $root -Recurse -Force -File -Filter '*.exe' -Depth 6 -ErrorAction SilentlyContinue |
::PS::    ForEach-Object {
::PS::      $path = $_.FullName
::PS::      if ($_.Length -le 52428800) {
::PS::        if (Test-ArielFile $path) {
::PS::          Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
::PS::          if (-not (Test-Path -LiteralPath $path)) { $count++ }
::PS::        }
::PS::      }
::PS::    }
::PS::}
::PS::Mark 'payload executables' $count
::PS::
::PS::$count = 0
::PS::$artifactFilters = @(
::PS::  'ariel*.exe', 'ariel*.zip', 'ariel.injected',
::PS::  'index.js.ariel.js', 'index.js.ariel.bak'
::PS::)
::PS::foreach ($root in $roots) {
::PS::  foreach ($filter in $artifactFilters) {
::PS::    Get-ChildItem -LiteralPath $root -Recurse -Force -File -Filter $filter -Depth 6 -ErrorAction SilentlyContinue |
::PS::      ForEach-Object {
::PS::        $path = $_.FullName
::PS::        if ($path -ne $cleanerScript) {
::PS::          Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
::PS::          if (-not (Test-Path -LiteralPath $path)) { $count++ }
::PS::        }
::PS::      }
::PS::  }
::PS::}
::PS::Mark 'named artifacts' $count
::PS::
::PS::$count = 0
::PS::foreach ($root in $roots) {
::PS::  Get-ChildItem -LiteralPath $root -Recurse -Force -File -Depth 6 -ErrorAction SilentlyContinue |
::PS::    Where-Object {
::PS::      $_.Length -lt 2097152 -and $_.Extension -in @('.js', '.json', '.txt', '.ps1') -and
::PS::      (Test-ArielText $_.FullName)
::PS::    } |
::PS::    ForEach-Object {
::PS::      $path = $_.FullName
::PS::      if ($path -ne $cleanerScript) {
::PS::        Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
::PS::        if (-not (Test-Path -LiteralPath $path)) { $count++ }
::PS::      }
::PS::    }
::PS::}
::PS::Mark 'marker text files' $count
::PS::
::PS::$count = 0
::PS::$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
::PS::$startupFolder = [Environment]::GetFolderPath('Startup')
::PS::$installDirs = @()
::PS::foreach ($name in @('Ariel', 'SecurityHealthSystray')) {
::PS::  $candidate = Join-Path $env:APPDATA $name
::PS::  if (Test-Path -LiteralPath $candidate) { $installDirs += $candidate }
::PS::}
::PS::if ($env:APPDATA -and (Test-Path -LiteralPath $env:APPDATA)) {
::PS::  Get-ChildItem -LiteralPath $env:APPDATA -Directory -Force -ErrorAction SilentlyContinue |
::PS::    ForEach-Object {
::PS::      $dir = $_.FullName
::PS::      if ($installDirs -notcontains $dir) {
::PS::        $marked = @(Get-ChildItem -LiteralPath $dir -Filter '*.exe' -File -Force -ErrorAction SilentlyContinue |
::PS::          Where-Object { Test-ArielFile $_.FullName })
::PS::        if ($marked.Count -gt 0) { $installDirs += $dir }
::PS::      }
::PS::    }
::PS::}
::PS::
::PS::function Is-ArielCommand($value) {
::PS::  $path = Get-CommandPath $value
::PS::  if (-not $path) { return $false }
::PS::  if (Test-ArielFile $path) { return $true }
::PS::  foreach ($dir in $installDirs) {
::PS::    if ($path.StartsWith($dir + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
::PS::      return $true
::PS::    }
::PS::  }
::PS::  return $false
::PS::}
::PS::
::PS::$runProperties = Get-ItemProperty -Path $runKey -ErrorAction SilentlyContinue
::PS::if ($runProperties) {
::PS::  foreach ($property in $runProperties.PSObject.Properties) {
::PS::    if ($property.Name -like 'PS*') { continue }
::PS::    $value = [string]$property.Value
::PS::    $remove = $false
::PS::    if ($property.Name -ieq 'Ariel') { $remove = $true }
::PS::    if ($property.Name -ieq 'SecurityHealthSystray' -and (Is-ArielCommand $value)) { $remove = $true }
::PS::    if (Is-ArielCommand $value) { $remove = $true }
::PS::    if ($remove) {
::PS::      Remove-ItemProperty -Path $runKey -Name $property.Name -Force -ErrorAction SilentlyContinue
::PS::      $still = Get-ItemProperty -Path $runKey -Name $property.Name -ErrorAction SilentlyContinue
::PS::      if (-not $still) { $count++ }
::PS::    }
::PS::  }
::PS::}
::PS::
::PS::$shell = $null
::PS::try { $shell = New-Object -ComObject WScript.Shell } catch {}
::PS::if ($startupFolder -and (Test-Path -LiteralPath $startupFolder)) {
::PS::  Get-ChildItem -LiteralPath $startupFolder -Filter '*.lnk' -File -Force -ErrorAction SilentlyContinue |
::PS::    ForEach-Object {
::PS::      $shortcut = $_.FullName
::PS::      $target = ''
::PS::      if ($shell) {
::PS::        try { $target = $shell.CreateShortcut($shortcut).TargetPath } catch {}
::PS::      }
::PS::      $remove = $false
::PS::      if ($_.BaseName -ieq 'Ariel') { $remove = $true }
::PS::      if ($_.BaseName -ieq 'SecurityHealthSystray' -and (Is-ArielCommand $target)) { $remove = $true }
::PS::      if (Is-ArielCommand $target) { $remove = $true }
::PS::      if ($remove) {
::PS::        Remove-Item -LiteralPath $shortcut -Force -ErrorAction SilentlyContinue
::PS::        if (-not (Test-Path -LiteralPath $shortcut)) { $count++ }
::PS::      }
::PS::    }
::PS::}
::PS::
::PS::foreach ($dir in $installDirs) {
::PS::  if (Test-Path -LiteralPath $dir) {
::PS::    Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
::PS::    if (-not (Test-Path -LiteralPath $dir)) { $count++ }
::PS::  }
::PS::}
::PS::Mark 'persistence' $count
::PS::
::PS::$count = 0
::PS::$discordRoots = @()
::PS::if ($env:LOCALAPPDATA) {
::PS::  $exact = Join-Path $env:LOCALAPPDATA 'Discord'
::PS::  if (Test-Path -LiteralPath $exact) { $discordRoots += $exact }
::PS::  Get-ChildItem -LiteralPath $env:LOCALAPPDATA -Directory -Force -ErrorAction SilentlyContinue |
::PS::    Where-Object { $_.Name -match '^discord' } |
::PS::    ForEach-Object {
::PS::      if ($discordRoots -notcontains $_.FullName) { $discordRoots += $_.FullName }
::PS::    }
::PS::}
::PS::foreach ($discord in $discordRoots) {
::PS::  Get-ChildItem -LiteralPath $discord -Directory -Recurse -Force -Depth 5 -ErrorAction SilentlyContinue |
::PS::    Where-Object { $_.Name -like 'discord_desktop_core-*' } |
::PS::    ForEach-Object {
::PS::      $moduleDir = $_.FullName
::PS::      $indexes = @(
::PS::        (Join-Path $moduleDir 'index.js'),
::PS::        (Join-Path $moduleDir 'discord_desktop_core\index.js')
::PS::      )
::PS::      foreach ($index in $indexes) {
::PS::        if (-not (Test-Path -LiteralPath $index)) { continue }
::PS::        $coreDir = Split-Path -Parent $index
::PS::        $backup = Join-Path $coreDir 'index.js.ariel.bak'
::PS::        $inject = Join-Path $coreDir 'index.js.ariel.js'
::PS::        $marker = Join-Path $coreDir 'ariel.injected'
::PS::        if (Test-Path -LiteralPath $backup) {
::PS::          Copy-Item -LiteralPath $backup -Destination $index -Force -ErrorAction SilentlyContinue
::PS::          $count++
::PS::        } else {
::PS::          $text = Get-Content -LiteralPath $index -Raw -ErrorAction SilentlyContinue
::PS::          if ($text -match 'ariel') {
::PS::            'module.exports = require("./core.asar");' | Set-Content -LiteralPath $index -Encoding ASCII -Force
::PS::            $count++
::PS::          }
::PS::        }
::PS::        foreach ($f in @($backup, $inject, $marker)) {
::PS::          if (Test-Path -LiteralPath $f) {
::PS::            Remove-Item -LiteralPath $f -Force -ErrorAction SilentlyContinue
::PS::            if (-not (Test-Path -LiteralPath $f)) { $count++ }
::PS::          }
::PS::        }
::PS::      }
::PS::    }
::PS::
::PS::  Get-ChildItem -LiteralPath $discord -Directory -Recurse -Force -Depth 5 -ErrorAction SilentlyContinue |
::PS::    Where-Object { $_.Name -ieq 'app' -and $_.Parent -and $_.Parent.Name -ieq 'resources' } |
::PS::    ForEach-Object {
::PS::      $appFolder = $_.FullName
::PS::      $loader = Join-Path $appFolder 'index.js'
::PS::      $package = Join-Path $appFolder 'package.json'
::PS::      $ours = $false
::PS::      if (Test-Path -LiteralPath $loader) {
::PS::        $text = Get-Content -LiteralPath $loader -Raw -ErrorAction SilentlyContinue
::PS::        if ($text -match 'ariel') { $ours = $true }
::PS::      }
::PS::      if (Test-Path -LiteralPath $package) {
::PS::        $text = Get-Content -LiteralPath $package -Raw -ErrorAction SilentlyContinue
::PS::        if ($text -match 'ariel') { $ours = $true }
::PS::      }
::PS::      if ($ours) {
::PS::        Remove-Item -LiteralPath $loader -Force -ErrorAction SilentlyContinue
::PS::        Remove-Item -LiteralPath $package -Force -ErrorAction SilentlyContinue
::PS::        if (-not (Get-ChildItem -LiteralPath $appFolder -Force -ErrorAction SilentlyContinue)) {
::PS::          Remove-Item -LiteralPath $appFolder -Force -ErrorAction SilentlyContinue
::PS::        }
::PS::        $count++
::PS::      }
::PS::    }
::PS::
::PS::  Get-ChildItem -LiteralPath $discord -Recurse -Force -File -Depth 6 -ErrorAction SilentlyContinue |
::PS::    Where-Object {
::PS::      $_.Name -match 'ariel' -or (($_.Extension -in @('.js', '.json', '.txt')) -and (Test-ArielText $_.FullName))
::PS::    } |
::PS::    ForEach-Object {
::PS::      $path = $_.FullName
::PS::      Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
::PS::      if (-not (Test-Path -LiteralPath $path)) { $count++ }
::PS::    }
::PS::}
::PS::Mark 'discord injection' $count
::PS::
::PS::$count = 0
::PS::if ($env:TEMP) {
::PS::  Get-ChildItem -LiteralPath $env:TEMP -Filter 'ariel_exclusions_*.ps1' -Force -ErrorAction SilentlyContinue |
::PS::    ForEach-Object {
::PS::      $path = $_.FullName
::PS::      Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
::PS::      if (-not (Test-Path -LiteralPath $path)) { $count++ }
::PS::    }
::PS::}
::PS::Mark 'temp scripts' $count
::PS::
::PS::$count = 0
::PS::if (Test-Admin) {
::PS::  $existing = @(Get-MpPreference -ErrorAction SilentlyContinue | Select-Object -ExpandProperty ExclusionPath)
::PS::  $targets = @()
::PS::  if ($env:TEMP) { $targets += $env:TEMP }
::PS::  $targets += $outputDirs
::PS::  $targets += $installDirs
::PS::  $normalizedTargets = @()
::PS::  foreach ($target in $targets) {
::PS::    if (-not $target) { continue }
::PS::    $normalizedTargets += $target.TrimEnd([char]'\', [char]'/').ToLowerInvariant()
::PS::  }
::PS::  $appdataLower = ''
::PS::  if ($env:APPDATA) { $appdataLower = $env:APPDATA.TrimEnd([char]'\', [char]'/').ToLowerInvariant() }
::PS::  foreach ($item in $existing) {
::PS::    if (-not $item) { continue }
::PS::    $normalized = $item.TrimEnd([char]'\', [char]'/')
::PS::    $lower = $normalized.ToLowerInvariant()
::PS::    $remove = $false
::PS::    foreach ($target in $normalizedTargets) {
::PS::      if ($lower -eq $target) {
::PS::        $remove = $true
::PS::        break
::PS::      }
::PS::    }
::PS::    if ($lower -like '*\ariel\*' -or $lower -like '*/ariel/*' -or $lower -like '*\ariel.exe' -or $lower -like '*/ariel.exe') {
::PS::      $remove = $true
::PS::    }
::PS::    if ($appdataLower -and $lower.StartsWith($appdataLower + '\')) {
::PS::      $tail = $lower.Substring($appdataLower.Length + 1)
::PS::      if (($tail -eq 'securityhealthsystray') -or $tail.StartsWith('securityhealthsystray\') -or ($tail -eq 'ariel') -or $tail.StartsWith('ariel\')) {
::PS::        $remove = $true
::PS::      }
::PS::    }
::PS::    if ($remove) {
::PS::      Remove-MpPreference -ExclusionPath $item -ErrorAction SilentlyContinue
::PS::      $after = @(Get-MpPreference -ErrorAction SilentlyContinue | Select-Object -ExpandProperty ExclusionPath)
::PS::      if ($after -notcontains $item) { $count++ }
::PS::    }
::PS::  }
::PS::} else {
::PS::  Write-Host 'defender exclusions skipped: admin required'
::PS::}
::PS::Mark 'defender exclusions' $count
::PS::
::PS::Write-Host ''
::PS::Write-Host 'Ariel cleanup complete:'
::PS::foreach ($key in $removed.Keys) {
::PS::  Write-Host ('  {0}: {1}' -f $key, $removed[$key])
::PS::}
::PS::Write-Host 'Reboot recommended.'

param(
  [string]$SourcePath = (Join-Path $PSScriptRoot "..\icon-vectorxr.png"),
  [string]$OutputDir = (Join-Path $PSScriptRoot "..\app\src-tauri\icons")
)

$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Resolve-FullPath {
  param([string]$Path)

  $resolved = Resolve-Path -LiteralPath $Path -ErrorAction Stop
  return $resolved.ProviderPath
}

function New-ResizedPng {
  param(
    [System.Drawing.Bitmap]$Source,
    [int]$Size,
    [string]$Path
  )

  $bitmap = New-Object System.Drawing.Bitmap($Size, $Size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  try {
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
      $graphics.Clear([System.Drawing.Color]::Transparent)
      $graphics.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceOver
      $graphics.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
      $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
      $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
      $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
      $graphics.DrawImage($Source, 0, 0, $Size, $Size)
    } finally {
      $graphics.Dispose()
    }

    $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
  } finally {
    $bitmap.Dispose()
  }
}

function New-IcoFile {
  param(
    [string[]]$PngPaths,
    [string]$IcoPath
  )

  $frames = foreach ($pngPath in $PngPaths) {
    $bytes = [System.IO.File]::ReadAllBytes($pngPath)
    $image = [System.Drawing.Image]::FromFile($pngPath)
    try {
      [PSCustomObject]@{
        Width = $image.Width
        Height = $image.Height
        Bytes = $bytes
      }
    } finally {
      $image.Dispose()
    }
  }

  $fileStream = [System.IO.File]::Create($IcoPath)
  try {
    $writer = New-Object System.IO.BinaryWriter($fileStream)
    try {
      $writer.Write([UInt16]0)
      $writer.Write([UInt16]1)
      $writer.Write([UInt16]$frames.Count)

      $offset = 6 + ($frames.Count * 16)
      foreach ($frame in $frames) {
        $writer.Write([byte]($(if ($frame.Width -ge 256) { 0 } else { $frame.Width })))
        $writer.Write([byte]($(if ($frame.Height -ge 256) { 0 } else { $frame.Height })))
        $writer.Write([byte]0)
        $writer.Write([byte]0)
        $writer.Write([UInt16]1)
        $writer.Write([UInt16]32)
        $writer.Write([UInt32]$frame.Bytes.Length)
        $writer.Write([UInt32]$offset)
        $offset += $frame.Bytes.Length
      }

      foreach ($frame in $frames) {
        $writer.Write($frame.Bytes)
      }
    } finally {
      $writer.Dispose()
    }
  } finally {
    $fileStream.Dispose()
  }
}

$sourceFullPath = Resolve-FullPath $SourcePath

if (-not (Test-Path -LiteralPath $OutputDir)) {
  New-Item -ItemType Directory -Path $OutputDir | Out-Null
}

$outputFullPath = Resolve-Path -LiteralPath $OutputDir
$outputDirPath = $outputFullPath.ProviderPath

$sourceImage = [System.Drawing.Bitmap]::FromFile($sourceFullPath)
try {
  if ($sourceImage.Width -ne $sourceImage.Height) {
    throw "Source icon must be square. Got ${($sourceImage.Width)}x${($sourceImage.Height)}."
  }

  New-ResizedPng -Source $sourceImage -Size 512 -Path (Join-Path $outputDirPath "icon.png")

  $icoFrames = @(16, 24, 32, 48, 64, 128, 256)
  $tempDir = Join-Path $outputDirPath ".icon-build"
  if (Test-Path -LiteralPath $tempDir) {
    Remove-Item -LiteralPath $tempDir -Recurse -Force
  }
  New-Item -ItemType Directory -Path $tempDir | Out-Null

  try {
    $icoPngPaths = foreach ($size in $icoFrames) {
      $framePath = Join-Path $tempDir "$size.png"
      New-ResizedPng -Source $sourceImage -Size $size -Path $framePath
      $framePath
    }

    New-IcoFile -PngPaths $icoPngPaths -IcoPath (Join-Path $outputDirPath "icon.ico")
  } finally {
    if (Test-Path -LiteralPath $tempDir) {
      Remove-Item -LiteralPath $tempDir -Recurse -Force
    }
  }
} finally {
  $sourceImage.Dispose()
}

Write-Host "Generated Tauri icons from $sourceFullPath"
Write-Host "Output: $outputDirPath"

# SPDX-License-Identifier: GPL-2.0-or-later
param(
  [string]$Device = '192.168.1.50',
  [string]$MyIp = '192.168.1.10',
  [string]$HostName = $env:COMPUTERNAME,
  [string]$Mac = '',
  [string]$OutDir = (Join-Path $PSScriptRoot 'out'),
  [int]$TimeoutSec = 120,
  [switch]$CheckPortOnly
)
$ErrorActionPreference = 'Stop'
if (-not $Mac) {
  $ifx = (Get-NetIPAddress -AddressFamily IPv4 -IPAddress $MyIp).InterfaceIndex
  $Mac = (Get-NetAdapter -InterfaceIndex $ifx).MacAddress
}
Add-Type -Path (Join-Path $PSScriptRoot 'MfppScan.cs') -ReferencedAssemblies System.IO.Compression
if ($CheckPortOnly) {
  $l = New-Object Net.Sockets.TcpListener ([Net.IPAddress]::Any), 3892
  try { $l.Start(); "TCP 3892 свободен"; $l.Stop(); exit 0 } catch { "TCP 3892 занят: $($_.Exception.InnerException.SocketErrorCode)"; exit 10 }
}
$guid = (Get-NetAdapter -InterfaceIndex (Get-NetIPAddress -AddressFamily IPv4 -IPAddress $MyIp).InterfaceIndex).InterfaceGuid
if (Test-Path 'C:\Windows\System32\Npcap\wpcap.dll') { [MfppScan]::NpcapDevice = "\Device\NPF_$guid" }
"Device=$Device MyIp=$MyIp Host=$HostName MAC=$Mac Out=$OutDir"
exit [MfppScan]::Run($Device, $MyIp, $HostName, $Mac, $OutDir, $TimeoutSec)

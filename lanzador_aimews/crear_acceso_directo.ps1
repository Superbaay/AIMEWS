# Crea (o actualiza) el acceso directo "AIMEWS" en el escritorio.
# Abre lanzador.pyw con pythonw.exe (sin consola) y usa logo.ico como icono.
# Uso: powershell -ExecutionPolicy Bypass -File crear_acceso_directo.ps1
$carpeta = Split-Path -Parent $MyInvocation.MyCommand.Path
$pythonw = "$env:LOCALAPPDATA\Programs\Python\Python314\pythonw.exe"   # el mismo Python que corre la API (uvicorn, psutil, Pillow)
if (-not (Test-Path $pythonw)) {
    $pythonw = (Get-Command pythonw.exe -ErrorAction Stop).Source
}
if (-not (Test-Path "$carpeta\logo.ico")) {
    & ($pythonw -replace 'pythonw\.exe$', 'python.exe') "$carpeta\crear_logo.py"
}

# GetFolderPath respeta el escritorio redirigido a OneDrive
$escritorio = [Environment]::GetFolderPath("Desktop")
$ruta = Join-Path $escritorio "AIMEWS.lnk"

$shell = New-Object -ComObject WScript.Shell
$acceso = $shell.CreateShortcut($ruta)
$acceso.TargetPath = $pythonw
$acceso.Arguments = "`"$carpeta\lanzador.pyw`""
$acceso.WorkingDirectory = $carpeta
$acceso.IconLocation = "$carpeta\logo.ico,0"
$acceso.Description = "AIMEWS: iniciar API, sitio web, tunel de Cloudflare y base de datos"   # ASCII: PowerShell 5.1 lee este archivo como ANSI
$acceso.Save()
Write-Output "Acceso directo creado: $ruta"

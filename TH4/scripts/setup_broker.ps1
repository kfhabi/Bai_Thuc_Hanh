param(
    [ValidateNotNullOrEmpty()]
    [string]$Username = 'sv23110186'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$image = 'eclipse-mosquitto:2.0.22'

if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    throw 'Docker CLI was not found. Install/start Docker Desktop, then open a new PowerShell window.'
}

Push-Location -LiteralPath $projectRoot
try {
    Write-Host 'Checking Docker Engine...'
    & docker info --format '{{.ServerVersion}}'
    if ($LASTEXITCODE -ne 0) {
        throw 'Docker Engine is not ready. Start Docker Desktop with Linux containers and try again.'
    }

    & docker compose version
    if ($LASTEXITCODE -ne 0) {
        throw 'Docker Compose is not available. Enable/update Docker Desktop and try again.'
    }

    $secretsPath = Join-Path $projectRoot 'secrets'
    New-Item -ItemType Directory -Path $secretsPath -Force | Out-Null
    $passwordPath = Join-Path $secretsPath 'passwd'

    $passwordArgs = @(
        'run', '--rm', '-it',
        '--mount', "type=bind,source=$secretsPath,target=/secrets",
        $image,
        'mosquitto_passwd'
    )
    if (-not (Test-Path -LiteralPath $passwordPath -PathType Leaf)) {
        $passwordArgs += '-c'
    }
    $passwordArgs += @('/secrets/passwd', $Username)

    Write-Host "Enter the MQTT password twice for $Username. Input is hidden."
    Write-Host 'The local secrets/passwd file contains password hashes. Keep secrets/ outside Git.'
    & docker @passwordArgs
    if ($LASTEXITCODE -ne 0) {
        throw 'Password setup failed. Check that Docker Desktop can access this project drive and retry.'
    }
    if (-not (Test-Path -LiteralPath $passwordPath -PathType Leaf)) {
        throw 'The password file was not created. Check Docker Desktop bind-mount access to the project drive.'
    }

    & docker compose up -d
    if ($LASTEXITCODE -ne 0) {
        throw 'Broker startup failed. Run docker compose logs broker to inspect the error.'
    }
    # Reload the password file if this script changed an existing account.
    & docker compose restart broker
    if ($LASTEXITCODE -ne 0) {
        throw 'Broker restart failed. Run docker compose logs broker to inspect the error.'
    }
    & docker compose ps
    if ($LASTEXITCODE -ne 0) {
        throw 'Unable to read broker status.'
    }
    & docker compose logs --tail 30 broker
    if ($LASTEXITCODE -ne 0) {
        throw 'Unable to read broker logs.'
    }
}
finally {
    Pop-Location
}

param(
    [string]$Destination = 'build/game-next'
)

$ErrorActionPreference = 'Stop'
Push-Location (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent)
try {
    function Invoke-MacrossPython {
        param([string[]]$PythonArguments)
        & python @PythonArguments
        if ($LASTEXITCODE -ne 0) {
            throw "Python step failed: $($PythonArguments -join ' ')"
        }
    }

    Invoke-MacrossPython @('tools/localization/export_structured_text.py')
    foreach ($macrossTextGroup in @('guide', 'systemtext', 'subquesttext')) {
        Invoke-MacrossPython @('tools/localization/import_guide_draft.py', $macrossTextGroup)
    }
    Invoke-MacrossPython @('tools/localization/gop_strings.py')
    Invoke-MacrossPython @('tools/localization/build_gop_archive.py')
    Invoke-MacrossPython @('tools/play/prepare_game.py', '--destination', $Destination)
    Invoke-MacrossPython @('tools/play/verify_game.py')
    Invoke-MacrossPython @('tools/localization/analyze_korean_encoding.py')
}
finally {
    Pop-Location
}

# Fix all remaining action compilation errors
$basePath = 'c:\Users\Randy\Github\S.A.K.-Utility\src\actions'

# Remove Q_EMIT scanStarted() and Q_EMIT executionStarted()
Get-ChildItem "$basePath\*.cpp" | ForEach-Object {
    $content = Get-Content $_.FullName -Raw
    $changed = $false
    
    # Remove standalone Q_EMIT scanStarted();
    if ($content -match '\s+Q_EMIT\s+scanStarted\(\);[\r\n]+') {
        $content = $content -replace '\s+Q_EMIT\s+scanStarted\(\);[\r\n]+', "`n"
        $changed = $true
    }
    
    # Remove standalone Q_EMIT executionStarted();
    if ($content -match '\s+Q_EMIT\s+executionStarted\(\);[\r\n]+') {
        $content = $content -replace '\s+Q_EMIT\s+executionStarted\(\);[\r\n]+', "`n"
        $changed = $true
    }
    
    # Fix result.applicable in ExecutionResult contexts (should be result.success)
    if ($content -match 'ExecutionResult\s+result;[\s\r\n]+\s+result\.applicable') {
        $content = $content -replace '(ExecutionResult\s+result;[\s\r\n]+\s+)result\.applicable', '$1result.success'
        $changed = $true
    }
    
    # Add missing includes
    if ($content -match 'QRegularExpression' -and $content -notmatch '#include <QRegularExpression>') {
        $content = $content -replace '(#include <QProcess>)', "$1`n#include <QRegularExpression>"
        $changed = $true
    }
    
    if ($content -match 'QStandardPaths' -and $content -notmatch '#include <QStandardPaths>') {
        $content = $content -replace '(#include <QProcess>)', "$1`n#include <QStandardPaths>"
        $changed = $true
    }
    
    if ($changed) {
        Set-Content -Path $_.FullName -Value $content -NoNewline
        Write-Output "Fixed: $($_.Name)"
    }
}

Write-Output "Complete"

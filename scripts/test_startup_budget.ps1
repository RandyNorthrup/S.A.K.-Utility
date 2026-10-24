# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later

[CmdletBinding()]
param(
    [switch]$SourceContractOnly,
    [switch]$FatalRuntimeOnly,
    [switch]$FatalMutationDrill,
    [ValidateSet('All', 'Startup', 'Accessibility')]
    [string]$TeardownBranch = 'All'
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
if ($SourceContractOnly -and $FatalRuntimeOnly) { throw 'Select source-only or fatal-runtime-only, not both.' }
if ($FatalMutationDrill -and -not $FatalRuntimeOnly) { throw 'FatalMutationDrill requires FatalRuntimeOnly.' }
if ($TeardownBranch -ne 'All' -and -not $SourceContractOnly) {
    throw 'A selected teardown branch is allowed only with SourceContractOnly; native gates require both branches.'
}

function Get-StartupOwnershipCode {
    param([string]$Source)

    # This is a bounded source-shape contract, not a C++ semantic/UB test. Mask
    # literals and comments before matching executable spelling. Splice continued
    # lines first so a continued // comment cannot contribute apparent code.
    $spliced = [regex]::Replace($Source, '\\\r?\n', '')
    $nonCode = @'
(?s://[^\r\n]*|/\*.*?\*/|(?:u8|u|U|L)?R"(?<delimiter>[^\s()\\]{0,16})\(.*?\)\k<delimiter>"|(?:u8|u|U|L)?"(?:\\.|[^"\\])*"|(?:u8|u|U|L)?'(?:\\.|[^'\\])*')
'@
    $code = [regex]::Replace($spliced, $nonCode.Trim(), [Text.RegularExpressions.MatchEvaluator]{
        param($match)
        return (' ' * $match.Length)
    })
    if ($code -match '/\*|\*/|["'']') {
        throw 'QApplication ownership contract: unclosed comment or literal.'
    }
    return $code
}

function Get-StartupFunctionBody {
    param([string]$Code, [string]$Signature, [string]$Name, [switch]$Span)

    $definitions = [regex]::Matches($Code, $Signature + '\s*\{')
    if ($definitions.Count -ne 1) {
        throw "QApplication ownership contract: exactly one $Name definition with the required signature is required."
    }
    $start = $definitions[0].Index + $definitions[0].Length
    $depth = 1
    for ($index = $start; $index -lt $Code.Length; $index++) {
        if ($Code[$index] -eq '{') { $depth++ }
        if ($Code[$index] -eq '}') { $depth-- }
        if ($depth -eq 0) {
            if ($Span) { return [pscustomobject]@{ Start=$definitions[0].Index; End=$index+1 } }
            return $Code.Substring($start, $index - $start)
        }
    }
    throw "QApplication ownership contract: unclosed $Name body."
}

function Get-StartupFunctionSource {
    param([string]$Source, [string]$Signature, [string]$Name, [switch]$Struct)
    $logical = [regex]::Replace($Source, '\\\r?\n', '')
    $code = Get-StartupOwnershipCode $Source
    $span = Get-StartupFunctionBody -Code $code -Signature $Signature -Name $Name -Span
    $definition = $logical.Substring($span.Start, $span.End - $span.Start)
    if ($Struct) { $definition += ';' }
    return $definition
}

function Assert-StartupApplicationOwnership {
    param([string]$Source)

    $code = Get-StartupOwnershipCode -Source $Source
    if ($code -match '\bstatic\s+QApplication\b|\bQApplication\s+static\b') {
        throw 'QApplication ownership contract: static QApplication outlives the argc owner.'
    }
    $initializer = Get-StartupFunctionBody -Code $code -Name 'initializeApp' -Signature (
        '\bvoid\s+initializeApp\s*\(\s*QApplication\s*&\s*app\s*,\s*' +
        'const\s+RuntimeOptions\s*&\s*options\s*\)')
    if ($initializer -match '\b(?:QApplication|argc|argv)\b') {
        throw 'QApplication ownership contract: initializeApp must configure the supplied app, not own argument storage or another app.'
    }
    $body = Get-StartupFunctionBody -Code $code -Name 'runApplication' -Signature (
        '\bint\s+runApplication\s*\(\s*int\s+argc\s*,\s*char\s*\*\s*argv\s*\[\s*\]\s*\)')
    if ($body -match '(?m)^\s*#') {
        throw 'QApplication ownership contract: conditional preprocessing inside the ownership body requires explicit review.'
    }
    $ownership = [regex]::Matches($body, '\bQApplication\s+app\s*\(\s*argc\s*,\s*argv\s*\)\s*;')
    $initialize = [regex]::Matches($body, '\binitializeApp\s*\(\s*app\s*,\s*options\s*\)\s*;')
    $show = [regex]::Matches($body, '\breturn\s+showMainWindow\s*\(\s*app\s*,')
    if ($ownership.Count -ne 1 -or $initialize.Count -ne 1 -or $show.Count -ne 1 -or
        [regex]::Matches($body, '\bQApplication\b').Count -ne 1) {
        throw 'QApplication ownership contract: runApplication must own one local app, initialize it and return showMainWindow(app, ...).'
    }
    if ($ownership[0].Index -ge $initialize[0].Index -or $initialize[0].Index -ge $show[0].Index) {
        throw 'QApplication ownership contract: construct, initialize and show must remain in that order.'
    }
    foreach ($statement in @($ownership[0], $initialize[0], $show[0])) {
        $prefix = $body.Substring(0, $statement.Index)
        $depth = [regex]::Matches($prefix, '\{').Count - [regex]::Matches($prefix, '\}').Count
        if ($depth -ne 0 -or $prefix -notmatch '(?:^|[;{}])\s*$') {
            throw 'QApplication ownership contract: ownership, initialization and show must be unconditional function-scope statements.'
        }
    }
}

function Assert-NoUnsupportedFontBackendOverride {
    param([string]$Source)

    $code = Get-StartupOwnershipCode -Source $Source
    if ($code -match '\bforceGdiFontDatabase\b' -or
        $Source -match '\b(?:QT_NO_DIRECTWRITE|SAK_USE_DIRECTWRITE)\b') {
        throw 'Startup backend contract: unsupported no-op DirectWrite override remains.'
    }
}

function Assert-StartupNormalTeardown {
    param([string]$Source, [string]$Branch = 'All')

    # Source-shape proof only: these named helpers must not reference _Exit and
    # must return their event-loop result normally. This does not analyze indirect
    # calls or prove Qt destruction; the existing real-app gates remain required.
    $code = Get-StartupOwnershipCode -Source $Source
    $definitions = @(
        @{
            Branch = 'Startup'; Name = 'runMainEventLoop'
            Signature = '\bint\s+runMainEventLoop\s*\(\s*QApplication\s*&\s*app\s*,\s*' +
                'bool\s+startup_smoke_test\s*(?:,\s*bool\s+headless_smoke_test\s*)?\)'
        },
        @{
            Branch = 'Accessibility'; Name = 'runAccessibilityAuditHeadless'
            Signature = '\bint\s+runAccessibilityAuditHeadless\s*\(\s*QApplication\s*&\s*app\s*,\s*' +
                'sak\s*::\s*MainWindow\s*&\s*main_window\s*\)'
        }
    )
    foreach ($definition in $definitions) {
        if ($Branch -ne 'All' -and $definition.Branch -ne $Branch) { continue }
        $body = Get-StartupFunctionBody -Code $code -Name $definition.Name -Signature $definition.Signature
        if ($body -match '\b_Exit\b') {
            throw "Normal teardown contract: $($definition.Name) references _Exit and bypasses automatic destruction."
        }
        $returns = [regex]::Matches($body, '\breturn\s+result\s*;')
        if ($returns.Count -ne 1) {
            throw "Normal teardown contract: $($definition.Name) requires one executable return result statement."
        }
        $prefix = $body.Substring(0, $returns[0].Index)
        $suffix = $body.Substring($returns[0].Index + $returns[0].Length)
        $depth = [regex]::Matches($prefix, '\{').Count - [regex]::Matches($prefix, '\}').Count
        if ($depth -ne 0 -or $prefix -notmatch '(?:^|[;{}])\s*$' -or $suffix.Trim().Length -ne 0) {
            throw "Normal teardown contract: $($definition.Name) must finish with an unconditional function-scope return result."
        }
    }
}

function Test-StartupNormalTeardownContract {
    $valid = @'
int runMainEventLoop(QApplication& app, bool startup_smoke_test, bool headless_smoke_test) {
    const int result = app.exec();
    // STARTUP_SITE
    return result;
}
int runAccessibilityAuditHeadless(QApplication& app, sak::MainWindow& main_window) {
    const int result = app.exec();
    // ACCESSIBILITY_SITE
    return result;
}
'@
    $valid = $valid.Replace("`r`n", "`n")
    Assert-StartupNormalTeardown $valid
    Assert-StartupNormalTeardown ($valid.Replace(', bool headless_smoke_test', ''))
    $positiveCount = 2
    $negativeCount = 0
    foreach ($branch in @('Startup', 'Accessibility')) {
        $marker = if ($branch -eq 'Startup') { '// STARTUP_SITE' } else { '// ACCESSIBILITY_SITE' }
        $name = if ($branch -eq 'Startup') { 'runMainEventLoop' } else { 'runAccessibilityAuditHeadless' }
        foreach ($trivia in @(
            '/* std::_Exit(0); */',
            'const char* message = "std::_Exit(0); return result;";',
            'const char* raw_message = R"detail(std::_Exit(0); " return result;)detail";')) {
            Assert-StartupNormalTeardown ($valid.Replace($marker, $trivia)) -Branch $branch
            $positiveCount++
        }
        $cases = [ordered]@{
            direct_exit = $valid.Replace($marker, 'std::_Exit(0);')
            spaced_exit = $valid.Replace($marker, 'std /* explanation */ :: _Exit (0);')
            aliased_exit = $valid.Replace($marker, 'auto stop = &std::_Exit; stop(0);')
            continued_exit = $valid.Replace($marker, "std::_Ex\`nit(0);")
            commented_return = $valid.Replace($marker + "`n    return result;", $marker + "`n    /* return result; */")
            quoted_return = $valid.Replace($marker + "`n    return result;", $marker + "`n" + '    const char* fake = "return result;";')
            conditional_return = $valid.Replace($marker + "`n    return result;", $marker + "`n    if (ready) return result;")
        }
        foreach ($case in $cases.GetEnumerator()) {
            $caught = $null
            try { Assert-StartupNormalTeardown $case.Value -Branch $branch }
            catch { $caught = $_.Exception.Message }
            $reason = if ($case.Key.EndsWith('_exit')) { "$name references _Exit" }
                elseif ($case.Key -eq 'conditional_return') { "$name must finish with an unconditional function-scope return result" }
                else { "$name requires one executable return result statement" }
            if ($null -eq $caught -or -not $caught.StartsWith('Normal teardown contract: ' + $reason)) {
                throw "Normal teardown fixture $branch/$($case.Key) did not refuse its own guard: $caught"
            }
            $negativeCount++
        }
    }
    Write-Host "Normal teardown source fixtures passed: $positiveCount positive controls and $negativeCount independent refusals."
}

function Test-StartupApplicationOwnershipContract {
    $valid = @'
void initializeApp(QApplication& app, const RuntimeOptions& options) {
    app.setApplicationName(options.name);
}

int runApplication(int argc, char* argv[]) {
    const RuntimeOptions options = runtimeOptionsFromArgs(argc, argv);
    QApplication app(argc, argv);
    initializeApp(app, options);
    return showMainWindow(app, options.startup_smoke_test);
}
'@
    Assert-StartupApplicationOwnership -Source $valid
    $trivia = @'
/* QApplication& initializeApp(int argc, char* argv[], const RuntimeOptions& options) {
    static QApplication app(argc, argv); }
*/
const char* diagnostic = "static QApplication app(argc, argv); { \"quoted\" }";
const char* raw_diagnostic = R"detail(static QApplication app(argc, argv); " } )detail";
const char punctuation = '}';
'@
    Assert-StartupApplicationOwnership -Source ($trivia + "`n" + $valid)
    $cases = [ordered]@{
        static_owner = $valid.Replace('QApplication app(argc, argv);', 'static QApplication app(argc, argv);')
        by_value_factory = $valid.Replace('void initializeApp(QApplication& app, const RuntimeOptions& options)',
            'QApplication& initializeApp(int argc, char* argv[], const RuntimeOptions& options)')
        initializer_constructs = $valid.Replace('app.setApplicationName(options.name);', 'QApplication other(argc, argv);')
        missing_owner = $valid.Replace('QApplication app(argc, argv);', '')
        missing_initialize = $valid.Replace('initializeApp(app, options);', '')
        missing_show = $valid.Replace('return showMainWindow(app, options.startup_smoke_test);', 'return 0;')
        reverse_order = $valid.Replace('QApplication app(argc, argv);', '@@OWNER@@').Replace(
            'initializeApp(app, options);', 'QApplication app(argc, argv);').Replace(
            '@@OWNER@@', 'initializeApp(app, options);')
        short_lived_owner = $valid.Replace('QApplication app(argc, argv);', '{ QApplication app(argc, argv); }')
        conditional_initialize = $valid.Replace('initializeApp(app, options);', 'if (ready) initializeApp(app, options);')
        commented_owner = $valid.Replace('QApplication app(argc, argv);', '/* QApplication app(argc, argv); */')
        quoted_owner = $valid.Replace('QApplication app(argc, argv);', 'const char* fake = "QApplication app(argc, argv);";')
        raw_quoted_owner = $valid.Replace('QApplication app(argc, argv);', 'const char* fake = R"tag(QApplication app(argc, argv);)tag";')
        commented_initializer = $valid.Replace('initializeApp(app, options);', '// initializeApp(app, options);')
        continued_comment = $valid.Replace('QApplication app(argc, argv);', "// owner follows \`n    QApplication app(argc, argv);")
        disabled_owner = $valid.Replace('QApplication app(argc, argv);', "#if 0`n    QApplication app(argc, argv);`n#endif")
        unclosed_comment = $valid + '/*'
        unclosed_literal = $valid + '"'
    }
    $reasons = @{
        static_owner = 'static QApplication outlives'
        by_value_factory = 'exactly one initializeApp definition'
        initializer_constructs = 'initializeApp must configure the supplied app'
        reverse_order = 'construct, initialize and show must remain in that order'
        short_lived_owner = 'unconditional function-scope statements'
        conditional_initialize = 'unconditional function-scope statements'
        disabled_owner = 'conditional preprocessing inside the ownership body'
        unclosed_comment = 'unclosed comment or literal'
        unclosed_literal = 'unclosed comment or literal'
    }
    foreach ($case in $cases.GetEnumerator()) {
        $refused = $false
        try { Assert-StartupApplicationOwnership -Source $case.Value }
        catch {
            if ($_.Exception.Message -notmatch '^QApplication ownership contract:') { throw }
            $reason = 'runApplication must own one local app, initialize it and return showMainWindow'
            if ($reasons.ContainsKey($case.Key)) { $reason = $reasons[$case.Key] }
            if (-not $_.Exception.Message.Contains($reason)) {
                throw "QApplication ownership fixture $($case.Key) failed for an unexpected reason: $($_.Exception.Message)"
            }
            $refused = $true
        }
        if (-not $refused) { throw "QApplication ownership negative fixture was accepted: $($case.Key)" }
    }
    Write-Host "QApplication ownership source fixtures passed: two positive controls and $($cases.Count) independent refusals."
}

function Assert-StartupFatalBoundary {
    param([string]$Source)
    $code = Get-StartupOwnershipCode $Source
    $reporter = Get-StartupFunctionBody -Code $code -Name 'reportFatalErrorTo' -Signature (
        '\bvoid\s+reportFatalErrorTo\s*\(\s*const\s+char\s*\*\s*detail\s*,\s*bool\s+interactive\s*,\s*' +
        'const\s+FatalErrorOutput\s*&\s*output\s*\)\s*noexcept')
    foreach ($helper in @('hasRawArgument', 'writeFatalStderr', 'reportFatalErrorTo', 'reportFatalError')) {
        $body = Get-StartupFunctionBody -Code $code -Name $helper -Signature (
            '\b(?:bool|void)\s+' + $helper + '\s*\([^)]*\)\s*noexcept')
        if ($body -match '\b(?:Q[A-Z]\w*|showCriticalLogged)\b') {
            throw "Fatal boundary contract: $helper must not use Qt after teardown."
        }
    }
    if ($reporter -notmatch '\bwriteFatalStderr\s*\(' -or $reporter -notmatch '\blogCritical\s*\(' -or
        $reporter -notmatch '\boutput\s*\.\s*dialog\s*\(' -or $reporter -notmatch '\boutput\s*\.\s*last_error\s*\(') {
        throw 'Fatal boundary contract: native dialog and checked diagnostic sinks are required.'
    }
    $boundary = Get-StartupFunctionBody -Code $code -Name 'runApplicationGuarded' -Signature (
        '\bint\s+runApplicationGuarded\s*\(\s*int\s+argc\s*,\s*char\s*\*\s*argv\s*\[\s*\]\s*,\s*' +
        'ApplicationRunner\s+runner\s*=\s*runApplication\s*,\s*FatalReporter\s+reporter\s*=\s*reportFatalError\s*\)\s*noexcept')
    if ($boundary -match '\b(?:Q[A-Z]\w*|showCriticalLogged)\b' -or
        [regex]::Matches($boundary, '\breporter\s*\(').Count -ne 2 -or
        $boundary -notmatch '\breturn\s+runner\s*\(\s*argc\s*,\s*argv\s*\)\s*;') {
        throw 'Fatal boundary contract: both exception branches require Qt-free reporting after the runner unwinds.'
    }
    $entry = Get-StartupFunctionBody -Code $code -Name 'main' -Signature '\bint\s+main\s*\(\s*int\s+argc\s*,\s*char\s*\*\s*argv\s*\[\s*\]\s*\)'
    if ($entry -match '\b(?:showCriticalLogged|QMessageBox|QApplication|runApplication)\b' -or
        $entry -notmatch '\breturn\s+runApplicationGuarded\s*\(\s*argc\s*,\s*argv\s*\)\s*;') {
        throw 'Fatal boundary contract: main must delegate to the guarded runner without a Qt catch dialog.'
    }
}

function Test-StartupFatalBoundaryContract {
    param([string]$Source)
    Assert-StartupFatalBoundary $Source
    Assert-StartupFatalBoundary ($Source + "`n" + '/* QString QApplication QMessageBox */')
    $cases = [ordered]@{
        known_catch = @('reporter(error.what(), interactive);', '(void)error;', 'both exception branches')
        unknown_catch = @('reporter("Unknown unhandled exception", interactive);', '(void)interactive;', 'both exception branches')
        raw_qt = @('std::strcmp(argv[i], name) == 0', 'QString::fromLatin1(argv[i]) == name', 'hasRawArgument must not use Qt')
        writer_qt = @('const int written = output.write', 'QString text_copy; const int written = output.write', 'writeFatalStderr must not use Qt')
        reporter_qt = @('writeFatalStderr("Fatal error:", output);', 'QMessageBox::critical(nullptr, "Fatal", detail);', 'reportFatalErrorTo must not use Qt')
        wrapper_qt = @('reportFatalErrorTo(detail, interactive, FatalErrorOutput{});', 'QMessageBox::critical(nullptr, "Fatal", detail);', 'reportFatalError must not use Qt')
        native_error = @('output.last_error()', 'DWORD{}', 'native dialog and checked diagnostic sinks')
        entry_qt = @('return runApplicationGuarded(argc, argv);', 'return runApplication(argc, argv);', 'main must delegate')
    }
    foreach ($case in $cases.GetEnumerator()) {
        $pattern, $replacement, $reason = $case.Value
        if ([regex]::Matches($Source, [regex]::Escape($pattern)).Count -ne 1) {
            throw "Fatal source drill requires one exact production target: $($case.Key)"
        }
        $caught = $null
        try { Assert-StartupFatalBoundary ($Source.Replace($pattern, $replacement)) }
        catch { $caught = $_.Exception.Message }
        if ($null -eq $caught -or -not $caught.StartsWith('Fatal boundary contract: ') -or -not $caught.Contains($reason)) {
            throw "Fatal source drill $($case.Key) did not refuse its own guard: $caught"
        }
    }
    Write-Host "Fatal boundary source fixtures passed: 2 positive controls and $($cases.Count) independent refusals."
}

function Test-StartupFatalRuntime {
    param([string]$Source, [string]$ExpectedAssertion)
    . (Join-Path $ProjectRoot 'scripts/process_capture.ps1')
    $root = Join-Path $ProjectRoot ('build/fatal-boundary-probe-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $root -ErrorAction Stop | Out-Null
    $definitions = @(
        (Get-StartupFunctionSource $Source '\bbool\s+hasRawArgument\s*\([^)]*\)\s*noexcept' 'hasRawArgument'),
        (Get-StartupFunctionSource $Source '\bstruct\s+FatalErrorOutput' 'FatalErrorOutput' -Struct),
        (Get-StartupFunctionSource $Source '\bvoid\s+writeFatalStderr\s*\([^)]*\)\s*noexcept' 'writeFatalStderr'),
        (Get-StartupFunctionSource $Source '\bvoid\s+reportFatalErrorTo\s*\([^)]*\)\s*noexcept' 'reportFatalErrorTo'),
        (Get-StartupFunctionSource $Source '\bvoid\s+reportFatalError\s*\([^)]*\)\s*noexcept' 'reportFatalError')
    )
    $logical = [regex]::Replace($Source, '\\\r?\n', '')
    $code = Get-StartupOwnershipCode $Source
    foreach ($name in @('ApplicationRunner', 'FatalReporter')) {
        $alias = [regex]::Matches($code, '\busing\s+' + $name + '\s*=[^;]+;')
        if ($alias.Count -ne 1) { throw "Fatal probe requires one actual $name alias" }
        $definitions += $logical.Substring($alias[0].Index, $alias[0].Length)
    }
    $definitions += Get-StartupFunctionSource $Source '\bint\s+runApplicationGuarded\s*\([^)]*\)\s*noexcept' 'runApplicationGuarded'
    $prefix = @'
#include <windows.h>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cwchar>
#include <exception>
#include <stdexcept>
namespace probe {
std::array<const char*,64> logs{};
int log_count=0;
extern int writes;
int first_log_writes=-1;
DWORD native_error=0, logged_native_error=0;
int logged_write_errno=0, logged_flush_errno=0;
}
namespace sak {
template<class... Args> void logCritical(const char* message, const Args&... args) noexcept {
    if(probe::log_count==0) probe::first_log_writes=probe::writes;
    if(probe::log_count < static_cast<int>(probe::logs.size())) probe::logs[probe::log_count++]=message;
    if constexpr(sizeof...(Args)>0) {
        const std::array<unsigned long,sizeof...(Args)> values{static_cast<unsigned long>(args)...};
        if constexpr(sizeof...(Args)==1) probe::logged_native_error=values[0];
        if constexpr(sizeof...(Args)==2) {
            probe::logged_write_errno=static_cast<int>(values[0]);
            probe::logged_flush_errno=static_cast<int>(values[1]);
        }
    }
    probe::native_error=5678; // Logging may overwrite the thread's last-error value.
}
}
int runApplication(int,char*[]) { return 0; }
'@
    $suffix = @'
namespace probe {
int checks=0, writes=0, flushes=0, dialogs=0, reports=0, last_error_calls=0;
bool write_fails=false, flush_fails=false, dialog_fails=false, alive=false, reported_after_unwind=false;
bool reported_interactive=false, bad_dialog=false;
std::array<char,4096> output{};
std::array<char,256> detail{};
void check(bool value,const char* name) { if(!value) throw std::runtime_error(name); ++checks; }
void reset() {
    log_count=writes=flushes=dialogs=reports=last_error_calls=0;
    first_log_writes=-1;
    native_error=logged_native_error=0;
    logged_write_errno=logged_flush_errno=0;
    write_fails=flush_fails=dialog_fails=alive=reported_after_unwind=reported_interactive=bad_dialog=false;
    output.fill(0); detail.fill(0); logs.fill(nullptr);
}
int fakeWrite(FILE*,const char* format,...) {
    ++writes;
    va_list args; va_start(args,format);
    const auto used=std::strlen(output.data());
    const int count=std::vsnprintf(output.data()+used,output.size()-used,format,args);
    va_end(args);
    if(write_fails) { errno=EACCES; return -1; }
    return count;
}
int fakeFlush(FILE*) { ++flushes; errno=EIO; return flush_fails ? EOF : 0; }
int WINAPI fakeDialog(HWND owner,LPCWSTR text,LPCWSTR title,UINT flags) {
    ++dialogs;
    bad_dialog=owner!=nullptr || std::wcslen(text)==0 || std::wcslen(title)==0 ||
        flags!=(MB_OK|MB_ICONERROR|MB_TASKMODAL) || std::wcsstr(text,L"any available")==nullptr;
    native_error=1234;
    return dialog_fails ? 0 : IDOK;
}
DWORD WINAPI fakeLastError() { ++last_error_calls; return native_error; }
bool hasLog(const char* text) {
    for(int i=0;i<log_count;++i) if(std::strstr(logs[i],text)!=nullptr) return true;
    return false;
}
void capturedReport(const char* text,bool interactive) noexcept {
    ++reports; reported_after_unwind=!alive; reported_interactive=interactive;
    const int copied=std::snprintf(detail.data(),detail.size(),"%s",text);
    if(copied<0 || static_cast<std::size_t>(copied)>=detail.size()) detail[0]=0;
}
struct Scope { Scope(){alive=true;} ~Scope(){alive=false;} };
int known(int,char*[]) { Scope owner; throw std::runtime_error("known-fatal-detail"); }
int unknown(int,char*[]) { Scope owner; throw 42; }
int normal(int,char*[]) { return 7; }
int rewriteKnown(int argc,char* argv[]) { argv[argc-1]=argv[0]; return known(argc,argv); }
int rewriteUnknown(int argc,char* argv[]) { argv[argc-1]=argv[0]; return unknown(argc,argv); }
void testOutput() {
    FatalErrorOutput sink;
    check(sink.write==&std::fprintf && sink.flush==&std::fflush && sink.dialog==&::MessageBoxW &&
        sink.last_error==&::GetLastError,"production native sink bindings");
    sink.write=&fakeWrite; sink.flush=&fakeFlush; sink.dialog=&fakeDialog; sink.last_error=&fakeLastError;
    reset(); reportFatalErrorTo("exact detail",true,sink);
    check(dialogs==1 && !bad_dialog,"native notice contract");
    check(hasLog("exact detail") && std::strstr(output.data(),"exact detail")!=nullptr,"both diagnostic sinks");
    check(writes==2 && flushes==2,"each stderr record is flushed");
    check(first_log_writes==2,"primary stderr precedes lazy logger initialization");
    check(last_error_calls==0,"successful native notice does not read stale last error");
    reset(); reportFatalErrorTo("headless detail",false,sink);
    check(dialogs==0 && hasLog("headless detail") && writes==2,"headless preserves errors without modal");
    reset(); write_fails=true; reportFatalErrorTo("write failure",false,sink);
    check(hasLog("Fatal stderr output failed") && flushes==2 && logged_write_errno==EACCES &&
        logged_flush_errno==0,"write failure retains errno before flush");
    reset(); flush_fails=true; reportFatalErrorTo("flush failure",false,sink);
    check(hasLog("Fatal stderr output failed") && logged_flush_errno==EIO && logged_write_errno==0,"flush failure is reported");
    reset(); dialog_fails=true; reportFatalErrorTo("native failure",true,sink);
    check(hasLog("Native fatal error notice failed") && last_error_calls==1 && logged_native_error==1234 &&
        std::strstr(output.data(),"Windows error 1234")!=nullptr,"native last error retained");
    reset(); write_fails=flush_fails=dialog_fails=true; reportFatalErrorTo("all channels fail",true,sink);
    check(hasLog("Native fatal error notice failed") && hasLog("Fatal stderr output failed") &&
        writes==3 && flushes==3 && dialogs==1 && log_count==6,"all failure channels terminate without recursion");
}
void testBoundary() {
    char name[]="sak"; char smoke[]="--smoke-test"; char startup[]="--startup-smoke-test"; char audit[]="--accessibility-audit";
    char* normal_args[]={name,nullptr};
    reset(); check(runApplicationGuarded(1,normal_args,&normal,&capturedReport)==7 && reports==0,"normal result preserved");
    reset(); check(runApplicationGuarded(1,normal_args,&known,&capturedReport)==1,"known exception fails");
    check(reports==1 && reported_after_unwind && reported_interactive && std::strcmp(detail.data(),"known-fatal-detail")==0,"known exception after owner destruction");
    reset(); check(runApplicationGuarded(1,normal_args,&unknown,&capturedReport)==1,"unknown release exception fails");
    check(reports==1 && reported_after_unwind && reported_interactive && std::strcmp(detail.data(),"Unknown unhandled exception")==0,"unknown exception after owner destruction");
    for(char* option : {smoke,startup,audit}) {
        for(ApplicationRunner runner : {&rewriteKnown,&rewriteUnknown}) {
            char* args[]={name,option,nullptr}; reset();
            check(runApplicationGuarded(2,args,runner,&capturedReport)==1 && reports==1 &&
                reported_after_unwind && !reported_interactive,"explicit noninteractive fatal mode survives argv rewrite");
        }
    }
    char similar[]="--smoke-test-extra"; char* similar_args[]={name,similar,nullptr}; reset();
    check(runApplicationGuarded(2,similar_args,&known,&capturedReport)==1 && reported_interactive,"mode matching uses complete argument");
}
}
int main() {
    try { probe::testOutput(); probe::testBoundary(); std::printf("SAK_FATAL_BOUNDARY_PROBE_OK checks=%d\n",probe::checks); return 0; }
    catch(const std::exception& error) { std::fprintf(stderr,"Fatal boundary probe assertion failed: %s\n",error.what()); return 2; }
}
'@
    $utf8 = [Text.UTF8Encoding]::new($false)
    [IO.File]::WriteAllText((Join-Path $root 'probe.cpp'), $prefix + "`n" + ($definitions -join "`n") + "`n" + $suffix, $utf8)
    $cmakeText = @'
cmake_minimum_required(VERSION 3.28)
project(SakFatalBoundaryProbe LANGUAGES CXX)
add_executable(fatal_boundary_probe probe.cpp)
target_compile_features(fatal_boundary_probe PRIVATE cxx_std_23)
target_compile_definitions(fatal_boundary_probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX NDEBUG)
target_compile_options(fatal_boundary_probe PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
target_link_libraries(fatal_boundary_probe PRIVATE user32)
'@
    [IO.File]::WriteAllText((Join-Path $root 'CMakeLists.txt'), $cmakeText, $utf8)
    $cmake = (Get-Command cmake.exe -ErrorAction Stop).Source
    $build = Join-Path $root 'build'
    $configure = Invoke-CapturedProcess -FilePath $cmake -WorkingDirectory $root -TimeoutSeconds 90 -Arguments @('-S',$root,'-B',$build,'-G','Visual Studio 17 2022','-A','x64')
    [IO.File]::WriteAllText((Join-Path $root 'configure.log'), $configure.stdout + $configure.stderr, $utf8)
    if ($configure.timed_out -or $configure.exit_code -ne 0) { throw "Fatal probe configure failed: $root" }
    $compile = Invoke-CapturedProcess -FilePath $cmake -WorkingDirectory $root -TimeoutSeconds 90 -Arguments @('--build',$build,'--config','Release','--target','fatal_boundary_probe')
    [IO.File]::WriteAllText((Join-Path $root 'build.log'), $compile.stdout + $compile.stderr, $utf8)
    if ($compile.timed_out -or $compile.exit_code -ne 0) { throw "Fatal probe build failed: $root" }
    $run = Invoke-CapturedProcess -FilePath (Join-Path $build 'Release/fatal_boundary_probe.exe') -WorkingDirectory $root -TimeoutSeconds 15
    [IO.File]::WriteAllText((Join-Path $root 'run.log'), $run.stdout + $run.stderr, $utf8)
    if ($ExpectedAssertion) {
        if ($run.timed_out -or $run.exit_code -ne 2 -or
            -not $run.stderr.Contains('Fatal boundary probe assertion failed: ' + $ExpectedAssertion)) {
            throw "Fatal mutation did not compile and fail its own runtime assertion: $root; $($run.stdout) $($run.stderr)"
        }
        Write-Host "Actual fatal boundary independent RED ($ExpectedAssertion): $root"
        return
    }
    if ($run.timed_out -or $run.exit_code -ne 0 -or $run.stdout -cnotmatch '^SAK_FATAL_BOUNDARY_PROBE_OK checks=23\z') {
        throw "Fatal probe runtime failed: $root; $($run.stdout) $($run.stderr)"
    }
    Write-Host "Actual fatal boundary nonmodal probe passed: $root"
}

function Test-StartupFatalMutations {
    param([string]$Source)
    # Mutate the exact extracted production functions, never the live tree. Every
    # variant must compile under /W4 /WX and fail its own executable assertion.
    $cases = [ordered]@{
        known_catch = @('reporter(error.what(), interactive);', '(void)error;', 'known exception after owner destruction')
        unknown_catch = @('reporter("Unknown unhandled exception", interactive);', '(void)interactive;', 'unknown exception after owner destruction')
        native_error = @('const DWORD native_error = output.last_error();', 'const DWORD native_error = 0;', 'native last error retained')
        native_return = @('MB_OK | MB_ICONERROR | MB_TASKMODAL) != 0', 'MB_OK | MB_ICONERROR | MB_TASKMODAL) != -1', 'native last error retained')
        stderr_write = @('written < 0 || flushed != 0', 'written < -1 || flushed != 0', 'write failure retains errno before flush')
        stderr_flush = @('written < 0 || flushed != 0', 'written < 0 || flushed > 0', 'flush failure is reported')
        headless_reporter = @('if (!interactive)', 'if (interactive)', 'native notice contract')
        smoke_mode = @('!hasRawArgument(argc, argv, "--smoke-test")', '!hasRawArgument(argc, argv, "--smoke-test-disabled")', 'explicit noninteractive fatal mode survives argv rewrite')
        startup_mode = @('!hasRawArgument(argc, argv, "--startup-smoke-test")', '!hasRawArgument(argc, argv, "--startup-smoke-test-disabled")', 'explicit noninteractive fatal mode survives argv rewrite')
        audit_mode = @('!hasRawArgument(argc, argv, "--accessibility-audit")', '!hasRawArgument(argc, argv, "--accessibility-audit-disabled")', 'explicit noninteractive fatal mode survives argv rewrite')
    }
    foreach ($case in $cases.GetEnumerator()) {
        $pattern, $replacement, $assertion = $case.Value
        if ([regex]::Matches($Source, [regex]::Escape($pattern)).Count -ne 1) {
            throw "Fatal compiled drill requires one exact production target: $($case.Key)"
        }
        Write-Host "Fatal compiled mutation: $($case.Key)"
        Test-StartupFatalRuntime -Source ($Source.Replace($pattern, $replacement)) -ExpectedAssertion $assertion
    }
    Write-Host "Actual fatal boundary compiled mutation drills passed: $($cases.Count) independent runtime refusals."
}

Test-StartupApplicationOwnershipContract
Test-StartupNormalTeardownContract
$mainSource = Join-Path $ProjectRoot 'src\main.cpp'
$mainText = Get-Content -LiteralPath $mainSource -Raw
Assert-StartupApplicationOwnership -Source $mainText
Assert-NoUnsupportedFontBackendOverride -Source $mainText
Assert-StartupNormalTeardown -Source $mainText -Branch $TeardownBranch
Assert-StartupFatalBoundary -Source $mainText
Test-StartupFatalBoundaryContract -Source $mainText
if ($SourceContractOnly) {
    Write-Host 'QApplication ownership, normal teardown and fatal boundary source contracts passed; real-app gates were not run.'
    exit 0
}
Test-StartupFatalRuntime -Source $mainText
if ($FatalMutationDrill) { Test-StartupFatalMutations -Source $mainText }
if ($FatalRuntimeOnly) { exit 0 }

$gate = Join-Path $ProjectRoot "scripts\check_startup_budget.ps1"
$capture = Join-Path $ProjectRoot "scripts\process_capture.ps1"
$exe = Join-Path $ProjectRoot "build\Release\sak_utility.exe"

foreach ($required in @($gate, $capture, $exe)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Startup-budget regression test input missing: $required"
    }
}

$source = Get-Content -LiteralPath $gate -Raw
if ($source -match '\bStart-Process\b') {
    throw "Startup budget regressed to Start-Process redirected capture."
}
if ($source -notmatch '\bInvoke-CapturedProcess\b') {
    throw "Startup budget no longer uses the shared captured-process seam."
}

. $capture
$hostPath = (Get-Process -Id $PID).Path
$red = Invoke-CapturedProcess -FilePath $hostPath -Arguments @(
    "-NoProfile",
    "-NonInteractive",
    "-File",
    $gate,
    "-BudgetMs",
    "1",
    "-HardTimeoutSec",
    "60",
    "-ExePath",
    $exe
) -TimeoutSeconds 90
if ($red.timed_out -or $red.exit_code -eq 0 -or $red.stderr -notmatch 'Startup budget EXCEEDED') {
    throw ("Startup budget 1 ms drill did not fail for the budget guard. " +
        "exit=$($red.exit_code), timed_out=$($red.timed_out), stderr=$($red.stderr)")
}

$green = Invoke-CapturedProcess -FilePath $hostPath -Arguments @(
    "-NoProfile",
    "-NonInteractive",
    "-File",
    $gate,
    "-BudgetMs",
    "30000",
    "-HardTimeoutSec",
    "60",
    "-ExePath",
    $exe
) -TimeoutSeconds 90
if ($green.timed_out -or $green.exit_code -ne 0 -or
    $green.stdout -notmatch 'Startup budget passed') {
    throw ("Startup budget real-app control failed. exit=$($green.exit_code), " +
        "timed_out=$($green.timed_out), stdout=$($green.stdout), stderr=$($green.stderr)")
}

Write-Host "Startup budget regression test passed: capture seam pinned, 1 ms red, real app green."
exit 0

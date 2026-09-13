<#
.SYNOPSIS
    Mechanical checks for the rules CLAUDE.md states in prose. Exits non-zero on any hit.

.DESCRIPTION
    Every rule here is one that was broken in the September 2026 review series and went
    unnoticed for weeks because nothing measured it. Prose rules are read once; this runs
    on every Run-AirsideTests.ps1 invocation, which is the pre-commit path.

    Rules:
      1. Include direction. Model/ and Solve/ never include Build|Tool|Present|Entities|
         Content; Tool/ never includes Present/; Build/ never includes Present|Tool; Solve/
         includes only CoreMinimal.h and Solve/. (Issue #31: Model<->Entities and
         Tool<->Present cycles shipped and stayed; issue #104: AirportOps' FuelService.cpp
         resolved a content default itself instead of taking it from Present/.)
      2. One log category per name across each unity-build module: Airside, AirportOps,
         AirsideEditor, AirsideTests and AirportOpsTests. It is a unity build, so two
         DEFINE_LOG_CATEGORY_STATIC of one name in different .cpp files collide at compile
         time - but only once the two land in the same Module.*.cpp blob, which is why it
         passed locally and failed later. (Issue #35 / PR #39; issue #104 widened this past
         Airside/AirportOps to the editor and test modules, none of which were checked.)
      3. No stacked doc comments in headers: a */ followed by /** with no declaration between
         means one of them lost its subject. (Issue #34.)
      4. The Piper fallback is called from exactly one production site. Any other call of a
         PiperMeridian*() function outside AircraftType.*, UAirsideSettings and, in the test
         modules, AirsideTestFixtures.cpp (TestAirframes::Piper(), #100) is a second source of
         truth for one aeroplane. (Issue #30.)

    Not checked here, deliberately: uninitialised FVector2D locals (issue #46). The idiom
    `FVector2D X; if (!Fill(X)) ...` is legitimate and appears ~60 times as out-params; the
    bug is ignoring the return value, which a regex cannot see. The rule lives in CLAUDE.md.

.PARAMETER Root
    Project root (the directory holding AirportMgr.uproject). Defaults to this script's parent.
#>
[CmdletBinding()]
param(
    [string] $Root = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'
$failures = New-Object System.Collections.Generic.List[string]

$plugin      = Join-Path $Root 'Plugins\Airside\Source\Airside'
$ops         = Join-Path $Root 'Plugins\AirportOps\Source\AirportOps'
$editor      = Join-Path $Root 'Plugins\Airside\Source\AirsideEditor'
$airsideTests = Join-Path $Root 'Plugins\Airside\Source\AirsideTests'
$opsTests    = Join-Path $Root 'Plugins\AirportOps\Source\AirportOpsTests'
$modules = @($plugin, $ops)
$trees   = @((Join-Path $Root 'Plugins\Airside\Source'), (Join-Path $Root 'Plugins\AirportOps\Source'), (Join-Path $Root 'Source\AirportMgr'))

# Rule 2's own module list, wider than $modules above: AirsideEditor, and the two test
# modules, are not subject to the Model/Tool/Build include-direction rule (a composition
# test freely includes Present/), but a DEFINE_LOG_CATEGORY_STATIC in any of them collides
# under the SAME unity-build failure mode as one in Airside or AirportOps - #104: 8 of them
# in the two test modules were unique only because nobody had yet picked the same word twice.
$logCategoryModules = @($plugin, $ops, $editor, $airsideTests, $opsTests)

function Get-Sources([string] $Dir, [string[]] $Ext) {
    if (-not (Test-Path $Dir)) { return @() }
    Get-ChildItem -Path $Dir -Recurse -File | Where-Object { $Ext -contains $_.Extension }
}

# --- 1. Include direction -----------------------------------------------------------------
# Layer -> regex of forbidden include prefixes, applied inside EACH module. Solve/ is
# handled separately as an allow-list.
$forbidden = @{
    'Model' = 'Build/|Tool/|Present/|Entities/|Content/'
    'Tool'  = 'Present/'
    'Build' = 'Present/|Tool/'
}
foreach ($module in $modules) {
    foreach ($layer in $forbidden.Keys) {
        foreach ($half in 'Public', 'Private') {
            $dir = Join-Path $module (Join-Path $half $layer)
            foreach ($file in Get-Sources $dir @('.h', '.cpp')) {
                $hits = Select-String -Path $file.FullName -Pattern ('#include\s+"(' + $forbidden[$layer] + ')')
                foreach ($h in $hits) {
                    $failures.Add("include-direction: $($file.FullName):$($h.LineNumber) $layer/ must not include $($h.Line.Trim())")
                }
            }
        }
    }
}

# --- 1b. Cross-plugin direction: Airside never includes AirportOps ------------------------
# AirportOps -> Airside is the only legal direction. A header from the ops plugin inside
# Airside would make movement depend on money, which is the boundary the plugin split exists
# to hold. Matched on CODE references - an include path, the API macro, or a UOps* class
# name (a forward declaration is the same leak with no #include to catch) - and NOT on the
# bare word, because a WHY comment that names the consumer ("AirportOps binds this") is
# exactly the kind of comment this codebase wants more of.
foreach ($file in Get-Sources (Join-Path $Root 'Plugins\Airside\Source') @('.h', '.cpp')) {
    $hits = Select-String -Path $file.FullName -Pattern '#include\s+"[^"]*AirportOps|AIRPORTOPS_API|\bUOps[A-Z]\w*'
    foreach ($h in $hits) {
        $failures.Add("cross-plugin: $($file.FullName):$($h.LineNumber) Airside must not reference AirportOps: $($h.Line.Trim())")
    }
}

foreach ($half in 'Public', 'Private') {
    $dir = Join-Path $plugin (Join-Path $half 'Solve')
    foreach ($file in Get-Sources $dir @('.h', '.cpp')) {
        $hits = Select-String -Path $file.FullName -Pattern '#include\s+"([^"]+)"' |
            Where-Object { $_.Matches[0].Groups[1].Value -notmatch '^(CoreMinimal\.h|Solve/)' }
        foreach ($h in $hits) {
            $failures.Add("solve-purity: $($file.FullName):$($h.LineNumber) Solve/ may include only CoreMinimal.h and Solve/: $($h.Line.Trim())")
        }
    }
}

# --- 2. Log category names unique within each module ------------------------------------
foreach ($module in $logCategoryModules) {
    $categories = @{}
    foreach ($file in Get-Sources $module @('.cpp', '.h')) {
        $hits = Select-String -Path $file.FullName -Pattern 'DEFINE_LOG_CATEGORY(_STATIC)?\(\s*(\w+)'
        foreach ($h in $hits) {
            $name = $h.Matches[0].Groups[2].Value
            if ($categories.ContainsKey($name)) {
                $failures.Add("log-category: $name defined in both $($categories[$name]) and $($file.FullName):$($h.LineNumber) - unity build collision; declare it once in the module's Public/*Log.h")
            }
            else {
                $categories[$name] = "$($file.FullName):$($h.LineNumber)"
            }
        }
    }
}

# --- 3. Stacked doc comments in headers -------------------------------------------------
foreach ($tree in $trees) {
    foreach ($file in Get-Sources $tree @('.h')) {
        $text = Get-Content -Raw -Path $file.FullName
        $m = [regex]::Matches($text, '\*/\s*\r?\n\s*/\*\*')
        foreach ($x in $m) {
            $line = ($text.Substring(0, $x.Index) -split "`n").Count
            $failures.Add("orphan-doc: $($file.FullName):$line a doc comment is followed by another doc comment; one of them lost its declaration")
        }
    }
}

# --- 4. Piper fallback has one production caller, one test-fixture caller ---------------
# Production code: only AircraftType.* and AirsideSettings.cpp. The two test modules are a
# unity build and once exempted this rule entirely (#100's evidence: 51 call sites across 14
# files) - now they get the same "one source of truth" rule, with AirsideTestFixtures.cpp
# (TestAirframes::Piper()) as their one allowed caller, exactly as AircraftType.cpp is
# production's.
$allowed = @('AircraftType.h', 'AircraftType.cpp', 'AirsideSettings.cpp', 'AirsideTestFixtures.cpp')
foreach ($tree in $trees) {
    foreach ($file in Get-Sources $tree @('.h', '.cpp')) {
        if ($allowed -contains $file.Name) { continue }
        # Word-boundary before "PiperMeridian" so BuildPiperMeridian() - a different concern,
        # constructing the content asset rather than reading a fallback figure - is not
        # mistaken for one of the accessor fallbacks (Ground/Climb/Approach/Engine/Wingspan/
        # Requirements) this rule exists to keep to one source of truth.
        $hits = Select-String -Path $file.FullName -Pattern '(?<![A-Za-z])PiperMeridian\w*\s*\('
        foreach ($h in $hits) {
            $reason = if ($file.FullName -match '\\(AirsideTests|AirportOpsTests)\\') {
                'go through TestAirframes::Piper() (AirsideTestFixtures.h)'
            } else {
                'go through UAirsideSettings::ResolveDefaultAirframe'
            }
            $failures.Add("content-default: $($file.FullName):$($h.LineNumber) calls a PiperMeridian*() fallback; $reason")
        }
    }
}

# --- Verdict -------------------------------------------------------------------------------
if ($failures.Count -eq 0) {
    Write-Host 'Check-Architecture: PASS (include direction, cross-plugin, log categories, doc comments, content default)' -ForegroundColor Green
    exit 0
}

foreach ($f in $failures) { Write-Host "  FAIL  $f" -ForegroundColor Red }
Write-Host ''
Write-Host "Check-Architecture: $($failures.Count) failure(s)." -ForegroundColor Red
exit 1

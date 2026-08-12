#==============================================================================
# Space Dust - the one place a script learns what this branch's plugin is called
#
# Every built file is named after SPACEDUST_PRODUCT_NAME in CMakeLists.txt:
#
#     Space Dust.vst3   Space Dust.exe   Space Dust_SharedCode.lib      (v1-maintenance)
#     Space Dust V2.vst3   Space Dust V2.exe   Space Dust V2_SharedCode.lib   (main)
#
# The two branches say it differently. main hoisted it into a variable that other
# things read; v1-maintenance still writes it straight into juce_add_plugin. Both
# are read here, so a script that ends up on either branch still finds the name
# rather than throwing on the one it was not written for.
#
# WHY THIS EXISTS
#
# A literal "Space Dust.vst3" written into a script that runs from the V2 tree
# does not fail in any way you would notice. It resolves - to V1. The deploy
# scripts delete the destination bundle before copying, so a run from this tree
# would have removed the INSTALLED V1 plugin and put a V2 build in its place,
# under V1's name, with no warning and nothing in the output to say so.
#
# Reading the name instead means a script can only ever act on the plugin its
# own tree builds. The two lines can be checked out side by side and neither can
# reach into the other's install.
#
# Dot-source it and call the function:
#
#     . "$PSScriptRoot\product-name.ps1"
#     $product = Get-SpaceDustProductName
#==============================================================================

function Get-SpaceDustProductName {
    param(
        # Where CMakeLists.txt lives. Defaults to this file's own folder, which
        # is the project root, so callers at the root need not pass anything.
        [string]$ProjectRoot = $PSScriptRoot
    )

    $cmakeLists = Join-Path $ProjectRoot 'CMakeLists.txt'

    if (-not (Test-Path $cmakeLists)) {
        throw "Cannot read the product name: no CMakeLists.txt at $cmakeLists"
    }

    $text = Get-Content $cmakeLists -Raw

    # main: the name is a variable, and package-installer.ps1 reads the same one.
    if ($text -match 'set\(SPACEDUST_PRODUCT_NAME\s+"([^"]+)"') {
        return $Matches[1]
    }

    # v1-maintenance: written straight into juce_add_plugin. Console apps declare
    # a PRODUCT_NAME of their own further down the file, so those lines are
    # skipped -- the plugin's is the first one that is not a console app.
    foreach ($line in (Get-Content $cmakeLists)) {
        if ($line -match 'juce_add_console_app') { continue }
        if ($line -match 'PRODUCT_NAME\s+"([^"]+)"') { return $Matches[1] }
    }

    throw "CMakeLists.txt declares no product name this script can read: $cmakeLists"
}

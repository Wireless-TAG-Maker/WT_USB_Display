param([Parameter(Mandatory=$true)][string]$App)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName PresentationFramework
$assembly = [Reflection.Assembly]::LoadFrom((Resolve-Path -LiteralPath $App).Path)

function Read-View([string]$name) {
    $stream = $assembly.GetManifestResourceStream($name)
    if (!$stream) { throw "Missing UI resource $name" }
    try { return ,([Windows.Markup.XamlReader]::Load($stream)) }
    finally { $stream.Dispose() }
}

# Loose XAML is embedded by csc. Parse it with the shipping framework before packaging.
$styles = Read-View 'DisplayStyles.xaml'
$main = Read-View 'DisplayView.xaml'
$about = Read-View 'DisplayAbout.xaml'
try {
    $main.Resources.MergedDictionaries.Add($styles)
    $about.Resources.MergedDictionaries.Add($styles)
    $main.Measure([Windows.Size]::new(656, 490))
    $about.Content.Measure([Windows.Size]::new(530, 600))
    foreach ($name in 'Status', 'Detail', 'Connect', 'Pause', 'More', 'Arrange', 'Standard', 'Full', 'Startup') {
        if (!$main.FindName($name)) { throw "Missing display control $name" }
    }
    foreach ($name in 'Links', 'Company', 'CloseButton') {
        if (!$about.FindName($name)) { throw "Missing About control $name" }
    }
} finally { $about.Close() }

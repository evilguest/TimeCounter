Add-Type -AssemblyName System.Drawing
$images = @()
foreach ($size in @(16,20,24,32,48,64,128,256)) {
    $bitmap = [System.Drawing.Bitmap]::new($size,$size)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.ScaleTransform($size/256.0,$size/256.0)
    $dark = [System.Drawing.SolidBrush]::new([System.Drawing.ColorTranslator]::FromHtml('#172332'))
    $red = [System.Drawing.SolidBrush]::new([System.Drawing.ColorTranslator]::FromHtml('#ff595e'))
    $green = [System.Drawing.SolidBrush]::new([System.Drawing.ColorTranslator]::FromHtml('#4ed9a3'))
    $ring = [System.Drawing.Pen]::new([System.Drawing.ColorTranslator]::FromHtml('#f2f5fa'),14)
    $ticks = [System.Drawing.Pen]::new([System.Drawing.ColorTranslator]::FromHtml('#aebbc9'),9)
    $hour = [System.Drawing.Pen]::new($ring.Color,17)
    $minute = [System.Drawing.Pen]::new($red.Color,17)
    foreach ($pen in @($ticks,$hour,$minute)) { $pen.StartCap = 'Round'; $pen.EndCap = 'Round' }
    $graphics.FillEllipse($dark,8,8,240,240)
    $graphics.DrawEllipse($ring,35,35,186,186)
    $graphics.DrawLine($ticks,128,58,128,72); $graphics.DrawLine($ticks,198,128,184,128)
    $graphics.DrawLine($ticks,128,198,128,184); $graphics.DrawLine($ticks,58,128,72,128)
    $graphics.DrawLine($hour,128,128,90,100); $graphics.DrawLine($minute,128,128,158,78)
    $graphics.FillEllipse($red,116,116,24,24)
    $graphics.FillEllipse($dark,172,172,64,64); $graphics.FillEllipse($green,182,182,44,44)
    $memory = [System.IO.MemoryStream]::new()
    $bitmap.Save($memory,[System.Drawing.Imaging.ImageFormat]::Png)
    $images += ,@{Size=$size;Bytes=$memory.ToArray()}
    foreach ($object in @($memory,$graphics,$bitmap,$dark,$red,$green,$ring,$ticks,$hour,$minute)) { $object.Dispose() }
}
$stream = [System.IO.File]::Create("$PSScriptRoot/../TimeCounter/app.ico")
$writer = [System.IO.BinaryWriter]::new($stream)
$writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$images.Count)
$offset = 6 + 16*$images.Count
foreach ($img in $images) {
    $dimension = if ($img.Size -eq 256) {0} else {$img.Size}
    $writer.Write([byte]$dimension); $writer.Write([byte]$dimension); $writer.Write([byte]0); $writer.Write([byte]0)
    $writer.Write([uint16]1); $writer.Write([uint16]32); $writer.Write([uint32]$img.Bytes.Length); $writer.Write([uint32]$offset)
    $offset += $img.Bytes.Length
}
foreach ($img in $images) { $writer.Write([byte[]]$img.Bytes) }
$writer.Dispose()

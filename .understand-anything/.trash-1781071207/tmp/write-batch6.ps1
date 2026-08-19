$json = Get-Content -Path "D:\theo_git_project\WIZnet-PICO-C-CHIP-TEST\.understand-anything\tmp\batch-6-content.txt" -Raw
[System.IO.File]::WriteAllText("D:\theo_git_project\WIZnet-PICO-C-CHIP-TEST\.understand-anything\intermediate\batch-6.json", $json, [System.Text.UTF8Encoding]::new($false))
Write-Host "Done"

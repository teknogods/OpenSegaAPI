Get-Content .\Opensegaapi\src\Opensegaapi.rc | ForEach-Object { $_ -replace "1.0.0.0", $APPVEYOR_BUILD_VERSION } | Set-Content .\Opensegaapi\src\Opensegaapi2.rc
del .\Opensegaapi\src\Opensegaapi.rc
mv .\Opensegaapi\src\Opensegaapi2.rc .\Opensegaapi\src\Opensegaapi.rc
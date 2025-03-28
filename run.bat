@echo off

:: Ensure Python is installed & run run.py

python --version >nul 2>&1

IF %ERRORLEVEL% EQU 0 (
    goto PYTHON_INSTALLED
) ELSE (
    echo Python is not installed. Installing it now.
    echo This may take a while...
    echo (it's possible that you'll be asked for admin permissions in a separate window)
)

powershell.exe -Command "Start-BitsTransfer -Source https://www.python.org/ftp/python/3.13.2/python-3.13.2-amd64.exe -Destination build\python_installer.exe"

build\python_installer.exe /passive PrependPath=1 InstallAllUsers=1 AssociateFiles=1
call :RefreshEnv

python --version >nul 2>&1

IF %ERRORLEVEL% EQU 0 (
    echo Python installed - continuing...
    goto PYTHON_INSTALLED
) ELSE (
    echo Couldn't install Python
    pause
    exit /b 1
)

:: Set one environment variable from registry key
:SetFromReg
    "%WinDir%\System32\Reg" QUERY "%~1" /v "%~2" > "%TEMP%\_envset.tmp" 2>NUL
    for /f "usebackq skip=2 tokens=2,*" %%A IN ("%TEMP%\_envset.tmp") do (
        echo/set "%~3=%%B"
    )
    goto :EOF

:: Get a list of environment variables from registry
:GetRegEnv
    "%WinDir%\System32\Reg" QUERY "%~1" > "%TEMP%\_envget.tmp"
    for /f "usebackq skip=2" %%A IN ("%TEMP%\_envget.tmp") do (
        if /I not "%%~A"=="Path" (
            call :SetFromReg "%~1" "%%~A" "%%~A"
        )
    )
    goto :EOF

:RefreshEnv
    echo/@echo off >"%TEMP%\_env.cmd"

    :: Slowly generating final file
    call :GetRegEnv "HKLM\System\CurrentControlSet\Control\Session Manager\Environment" >> "%TEMP%\_env.cmd"
    call :GetRegEnv "HKCU\Environment">>"%TEMP%\_env.cmd" >> "%TEMP%\_env.cmd"

    :: Special handling for PATH - mix both User and System
    call :SetFromReg "HKLM\System\CurrentControlSet\Control\Session Manager\Environment" Path Path_HKLM >> "%TEMP%\_env.cmd"
    call :SetFromReg "HKCU\Environment" Path Path_HKCU >> "%TEMP%\_env.cmd"

    :: Caution: do not insert space-chars before >> redirection sign
    echo/set "Path=%%Path_HKLM%%;%%Path_HKCU%%" >> "%TEMP%\_env.cmd"

    :: Cleanup
    del /f /q "%TEMP%\_envset.tmp" 2>nul
    del /f /q "%TEMP%\_envget.tmp" 2>nul

    :: capture user / architecture
    SET "OriginalUserName=%USERNAME%"
    SET "OriginalArchitecture=%PROCESSOR_ARCHITECTURE%"

    :: Set these variables
    call "%TEMP%\_env.cmd"

    :: Cleanup
    del /f /q "%TEMP%\_env.cmd" 2>nul

    :: reset user / architecture
    SET "USERNAME=%OriginalUserName%"
    SET "PROCESSOR_ARCHITECTURE=%OriginalArchitecture%"

    goto :EOF

:PYTHON_INSTALLED

REM Now run python with the same arguments as the bat script

python run.py %*

REM run.py will exit immediately, so wait for the user to press ENTER

pause

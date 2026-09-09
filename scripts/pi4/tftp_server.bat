@echo off
REM Orthox-64: TFTP server for Raspberry Pi 4 netboot.
REM See scripts/pi4/README.md ("netboot").
setlocal
set ROOT=%~dp0root
echo Serving %ROOT% on UDP 69 ... (Ctrl+C to stop)
python "%~dp0tftp_server.py" --root "%ROOT%" --port 69
endlocal
pause

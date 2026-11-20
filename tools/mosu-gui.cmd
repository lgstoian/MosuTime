@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem Launch the MosuTime Manager.  pythonw keeps a console window from opening.
where pythonw >nul 2>nul
if errorlevel 1 (
    python "%~dp0mosu_gui.py" %*
) else (
    start "" pythonw "%~dp0mosu_gui.py" %*
)

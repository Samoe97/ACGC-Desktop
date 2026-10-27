@echo off
rem Starts Animal Crossing as your live desktop wallpaper.
rem Play: the widget's Play button or Ctrl+Alt+P. Back to the wallpaper: Ctrl+Alt+W.
rem Quit: right-click the tray icon, then Quit.
cd /d "%~dp0"
start "" "%~dp0AnimalCrossing.exe" --wallpaper %*

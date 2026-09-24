@echo off
setlocal

set /p COMMIT_MSG="Message de commit: "

git add .
git commit -m "%COMMIT_MSG%"
git push

endlocal
pause
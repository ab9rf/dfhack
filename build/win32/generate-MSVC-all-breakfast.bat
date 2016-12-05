@echo off
IF EXIST DF_PATH.txt SET /P _DF_PATH=<DF_PATH.txt
IF NOT EXIST DF_PATH.txt SET _DF_PATH=%CD%\DF
mkdir VC2015_32
cd VC2015_32
echo generating a build folder
rem for /f "delims=" %%a in ('DATE /T') do @set myvar=breakfast-%BUILD_NUMBER%
set myvar=breakfast-%BUILD_NUMBER%
cmake ..\..\.. -G"Visual Studio 14" -DDFHACK_RELEASE="%myvar%" -DCMAKE_INSTALL_PREFIX="%_DF_PATH%" -DBUILD_DEVEL=1 -DBUILD_DEV_PLUGINS=1 -DBUILD_DF2MC=1 -DBUILD_DFUSION=1 -DBUILD_STONESENSE=1 -DBUILD_SERVER=1
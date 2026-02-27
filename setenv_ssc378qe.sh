#! /bin/bash

#############################################################################################################
### Set global Env
#############################################################################################################
export BUILD_TIME=$(date +"%Y%m%d%H%M")

export PATH="/opt/mstar/arm-sigmastar-linux-uclibcgnueabihf-9.1.0/bin:":$PATH
export CROSS_PREFIX=arm-sigmastar-linux-uclibcgnueabihf-9.1.0
export CFLAGS="-mcpu=cortex-a7 "
export OUT_NAME=thridlib_out
export OUT_PATH=$(pwd)/${OUT_NAME}





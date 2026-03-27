#! /bin/bash

#############################################################################################################
### Set global Env
#############################################################################################################
export BUILD_TIME=$(date +"%Y%m%d%H%M")

export PATH="/home/compile_tools/SSC308/gcc-11.1.0-20210608-sigmastar-glibc-x86_64_arm-linux-gnueabihf/bin:":$PATH
export CROSS_PREFIX=arm-linux-gnueabihf-sigmastar-11.1.0-
export CFLAGS="-mcpu=cortex-a7 "
export OUT_NAME=thridlib_out
export OUT_PATH=$(pwd)/${OUT_NAME}





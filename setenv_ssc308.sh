#! /bin/bash

#############################################################################################################
### Set global Env for SSC308 with custom compiler path
#############################################################################################################
export BUILD_TIME=$(date +"%Y%m%d%H%M")

# 使用实际的交叉编译工具链路径
export COMPILER_PATH="/home/compile_tools/SSC308/gcc-11.1.0-20210608-sigmastar-glibc-x86_64_arm-linux-gnueabihf"
export PATH="${COMPILER_PATH}/bin:$PATH"

# 交叉编译器前缀（完整名称）
export CROSS_COMPILE=arm-linux-gnueabihf-sigmastar-11.1.0-
export CC=${CROSS_COMPILE}gcc
export CXX=${CROSS_COMPILE}g++
export AR=${CROSS_COMPILE}ar
export STRIP=${CROSS_COMPILE}strip

# autoconf需要的host triplet（标准格式）
export HOST_TRIPLET=arm-linux-gnueabihf

export CFLAGS="-mcpu=cortex-a7 -fPIC"
export OUT_NAME=thridlib_out
export OUT_PATH=$(pwd)/${OUT_NAME}

echo "========================================="
echo "交叉编译环境已配置:"
echo "COMPILER_PATH=${COMPILER_PATH}"
echo "CROSS_PREFIX=${CROSS_PREFIX}"
echo "OUT_PATH=${OUT_PATH}"
echo "========================================="
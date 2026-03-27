#!/bin/bash

#############################################################################################################
### make
#############################################################################################################

rm -rf libiconv-1.16

tar -zxvf libiconv-1.16.tar.gz
cd libiconv-1.16
./configure --host=${HOST_TRIPLET} --enable-shared --enable-static
make ARCH=arm



rm -rf ${OUT_PATH}/libiconv/lib
rm -rf ${OUT_PATH}/libiconv/include

mkdir -p ${OUT_PATH}/libiconv/lib
mkdir -p ${OUT_PATH}/libiconv/include

##cp 
cp lib/.libs/libiconv.a ${OUT_PATH}/libiconv/lib/
cp include/iconv.h ${OUT_PATH}/libiconv/include/


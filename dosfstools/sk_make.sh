#!/bin/bash

#############################################################################################################
### make
#############################################################################################################

rm -rf dosfstools-4.1

tar -zxvf dosfstools-4.1.tar.gz
cp -a dosfstools-4.1_patch/* dosfstools-4.1/
cd dosfstools-4.1
./configure CC=${CROSS_PREFIX}-gcc  --host=${CROSS_PREFIX} CFLAGS=-I${OUT_PATH}/libiconv/include LDFLAGS=-L${OUT_PATH}/libiconv/lib LIBS=-liconv
make



rm -rf ${OUT_PATH}/dosfstools/bin

mkdir -p ${OUT_PATH}/dosfstools/bin

##cp 
cp src/fsck.fat ${OUT_PATH}/dosfstools/bin/
cp src/mkfs.fat ${OUT_PATH}/dosfstools/bin/
cp ../restart_mmc.sh ${OUT_PATH}/dosfstools/bin/

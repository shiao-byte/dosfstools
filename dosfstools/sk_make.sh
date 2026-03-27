#!/bin/bash

#############################################################################################################
### make
#############################################################################################################


cd dosfstools-4.1

chmod +x configure

# 清理旧配置，避免缓存错误路径
if [ -f Makefile ]; then
    make distclean 2>/dev/null || true
fi

./configure \
    CC=${CROSS_PREFIX}gcc \
    --host=${HOST_TRIPLET} \
    --without-udev \
    CFLAGS="-I${OUT_PATH}/libiconv/include ${CFLAGS}" \
    LDFLAGS=-L${OUT_PATH}/libiconv/lib \
    LIBS=-liconv

find . \( -name 'Makefile.in' -o -name '.m4aclocal' -o -name 'configure' \) -exec touch {} \;

make



rm -rf ${OUT_PATH}/dosfstools/bin

mkdir -p ${OUT_PATH}/dosfstools/bin

##cp 
cp src/fsck.fat ${OUT_PATH}/dosfstools/bin/
cp src/mkfs.fat ${OUT_PATH}/dosfstools/bin/
cp ../restart_mmc.sh ${OUT_PATH}/dosfstools/bin/

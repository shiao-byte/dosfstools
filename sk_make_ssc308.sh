#!/bin/bash

#############################################################################################################
### 使用SSC308工具链编译dosfstools补丁版
#############################################################################################################
pwd=$PWD
echo "当前目录: $pwd"
CHIP=ssc308

# 加载环境变量
if [ ! -f setenv_${CHIP}.sh ];then
	echo "❌ setenv_${CHIP}.sh 不存在"
	exit 1
fi

echo "✅ 加载环境配置..."
source setenv_${CHIP}.sh

# 验证编译器
if [ ! -f "${COMPILER_PATH}/bin/${CC}" ]; then
    echo "❌ 交叉编译器不存在: ${COMPILER_PATH}/bin/${CC}"
    exit 1
fi

echo "✅ 交叉编译器: $(${CC} --version | head -1)"

#############################################################################################################
### 编译流程
#############################################################################################################

# 清理旧编译
echo ""
echo "================================================"
echo "步骤1: 清理旧编译产物"
echo "================================================"
rm -rf ${OUT_PATH}
rm -rf libiconv/libiconv-1.16
rm -rf dosfstools/dosfstools-4.1

# 编译 libiconv
echo ""
echo "================================================"
echo "步骤2: 编译 libiconv 依赖库"
echo "================================================"
cd $pwd/libiconv

rm -rf libiconv-1.16
tar -zxvf libiconv-1.16.tar.gz
cd libiconv-1.16

echo "配置 libiconv..."
./configure --host=${HOST_TRIPLET} --enable-shared --enable-static

echo "编译 libiconv..."
make ARCH=arm

# 安装到输出目录
mkdir -p ${OUT_PATH}/libiconv/lib
mkdir -p ${OUT_PATH}/libiconv/include

cp lib/.libs/libiconv.a ${OUT_PATH}/libiconv/lib/
cp include/iconv.h ${OUT_PATH}/libiconv/include/

echo "✅ libiconv 编译完成"
ls -lh ${OUT_PATH}/libiconv/lib/libiconv.a

# 编译 dosfstools (应用补丁)
echo ""
echo "================================================"
echo "步骤3: 编译 dosfstools (应用补丁)"
echo "================================================"
cd $pwd/dosfstools

rm -rf dosfstools-4.1

echo "解压原始源码..."
tar -zxvf dosfstools-4.1.tar.gz

echo "应用补丁..."
cp -av dosfstools-4.1_patch/* dosfstools-4.1/
echo "✅ 补丁已应用"

cd dosfstools-4.1

echo "配置 dosfstools..."
./configure \
    --host=${HOST_TRIPLET} \
    --without-udev \
    CFLAGS="-I${OUT_PATH}/libiconv/include ${CFLAGS}" \
    LDFLAGS="-L${OUT_PATH}/libiconv/lib" \
    LIBS=-liconv

echo "编译 dosfstools..."
make

# 复制到输出目录
mkdir -p ${OUT_PATH}/dosfstools/bin

cp src/fsck.fat ${OUT_PATH}/dosfstools/bin/
cp src/mkfs.fat ${OUT_PATH}/dosfstools/bin/
cp ../restart_mmc.sh ${OUT_PATH}/dosfstools/bin/

echo "✅ dosfstools 编译完成"

# 验证编译结果
echo ""
echo "================================================"
echo "步骤4: 验证编译结果"
echo "================================================"
cd $pwd

echo "文件大小:"
ls -lh ${OUT_PATH}/dosfstools/bin/fsck.fat
ls -lh ${OUT_PATH}/dosfstools/bin/mkfs.fat

echo ""
echo "文件架构:"
file ${OUT_PATH}/dosfstools/bin/fsck.fat

echo ""
echo "检查补丁特征 (CLUSTER_MAX):"
strings ${OUT_PATH}/dosfstools/bin/fsck.fat | grep -i "cluster.*max" || echo "未找到CLUSTER_MAX特征"

# 打包
echo ""
echo "================================================"
echo "步骤5: 打包发布"
echo "================================================"
cd $pwd

# 添加版本信息
git log -1 > ${OUT_NAME}/version.txt 2>/dev/null || echo "非git仓库" > ${OUT_NAME}/version.txt
git remote -v >> ${OUT_NAME}/version.txt 2>/dev/null

# 打包
tar -zcvf ${OUT_NAME}.tar.gz ${OUT_NAME}

echo ""
echo "================================================"
echo "✅ 编译完成！"
echo "================================================"
echo "输出目录: ${OUT_PATH}"
echo "打包文件: ${OUT_NAME}.tar.gz"
echo ""
echo "编译产物:"
find ${OUT_PATH} -type f -name "fsck.fat" -o -name "mkfs.fat" -o -name "*.a"
echo ""
echo "可执行以下命令部署到设备:"
echo "  scp ${OUT_NAME}.tar.gz root@<设备IP>:/tmp/"
echo "  ssh root@<设备IP> 'cd /tmp && tar -zxvf ${OUT_NAME}.tar.gz && cp ${OUT_NAME}/dosfstools/bin/fsck.fat /usr/sbin/'"
echo ""
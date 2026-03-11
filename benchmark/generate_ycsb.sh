#!/usr/bin/env bash

set -e

# 1. 自动获取当前目录
BASE_DIR=$(pwd)

# 2. 设置 YCSB 工具路径 (刚刚解压的那个)
YCSB_DIR="${BASE_DIR}/ycsb-0.17.0"

# 3. 设置数据生成位置 (修改为你准备好的 /pmem0)
DATA_DIR="/pmem0/ycsb_data"

PREFILL_CONF="${BASE_DIR}/config/ycsb_prefill.conf"
CONFIGS=( "5050_uniform" "5050_zipf" "1090_uniform" "1090_zipf" )

# 确保目录存在
mkdir -p "${DATA_DIR}"

# 检查 YCSB 是否存在
if [ ! -d "${YCSB_DIR}" ]; then
  echo "Error: YCSB directory not found at ${YCSB_DIR}"
  exit 1
fi

# 切换到 YCSB 目录开始干活
cd "${YCSB_DIR}"

echo "GENERATING PREFILL DATA..."
./bin/ycsb load basic -P ${PREFILL_CONF} -s > "${DATA_DIR}/raw_prefill.dat"

echo "GENERATING YCSB DATA..."
for config in "${CONFIGS[@]}"
do
  echo "GENERATING ${config}..."
  ./bin/ycsb run basic -P ${PREFILL_CONF} \
          -P "${BASE_DIR}/config/ycsb_${config}.conf" \
          -s > "${DATA_DIR}/raw_ycsb_wl_${config}.dat"
done

# 回到 benchmark 目录进行格式转换
cd "${BASE_DIR}"
echo "CONVERTING DATA TO BINARY FORMAT..."

# 自动检测 python 命令
if ! command -v python3 &> /dev/null; then
    PYTHON_CMD="python"
else
    PYTHON_CMD="python3"
fi

$PYTHON_CMD convert_ycsb.py "${DATA_DIR}/raw_prefill.dat" "${DATA_DIR}/ycsb_prefill.dat"

for config in "${CONFIGS[@]}"
do
  echo "CONVERTING: ${config}..."
  $PYTHON_CMD convert_ycsb.py "${DATA_DIR}/raw_ycsb_wl_${config}.dat" "${DATA_DIR}/ycsb_wl_${config}.dat"
  # 删除原始大文件，节省空间
  rm "${DATA_DIR}/raw_ycsb_wl_${config}.dat"
done

echo "SUCCESS! All data generated in ${DATA_DIR}"

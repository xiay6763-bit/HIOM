import json
import re
import os
import matplotlib.pyplot as plt
from collections import defaultdict

# ================= 1. 配置区域 =================
# 数据文件路径 (绝对路径)
DATA_FILE = "/root/viper/results/ycsb/ycsb_2026-01-29-15-33.json"

# 图片保存目录
OUTPUT_DIR = "charts"

# 系统图例定义 (代码中识别的关键字 -> 图表中显示的标签)
FIXTURES = [
    ('Viper', 'Viper'),
    ('Dash', 'Dash'),
    ('Cceh', 'CCEH')
]

# 负载类型定义 (对应图表的四列)
BM_TYPES = ['5050_uniform', '1090_uniform', '5050_zipf', '1090_zipf']

# 样式定义 (颜色、标记、大小)
STYLES = {
    'Viper': {'color': '#1f77b4', 'marker': 'o', 'ms': 8},  # 蓝色 圆点
    'Dash':  {'color': '#ff7f0e', 'marker': 's', 'ms': 8},  # 橙色 方块
    'Cceh':  {'color': '#2ca02c', 'marker': '^', 'ms': 8}   # 绿色 三角
}

# 单位换算
MILLION = 1000000.0

# ================= 2. 数据解析函数 =================
def parse_data(file_path):
    print(f"正在读取数据: {file_path}")
    with open(file_path, 'r') as f:
        data = json.load(f)
    
    # 存储结构: runs[(系统名, 负载类型)] = [运行记录列表]
    runs = defaultdict(list)
    
    for bm in data['benchmarks']:
        name = bm['name']
        name_lower = name.lower()
        
        # 识别系统
        fixture = None
        if 'viper' in name_lower: fixture = 'Viper'
        elif 'dash' in name_lower: fixture = 'Dash'
        elif 'cceh' in name_lower: fixture = 'Cceh'
        if not fixture: continue
        
        # 识别负载类型
        bm_type = None
        for bt in BM_TYPES:
            if bt in name_lower:
                bm_type = bt
                break
        if not bm_type: continue
        
        # 识别线程数
        thread_match = re.search(r'threads:(\d+)', name)
        if not thread_match: continue
        threads = int(thread_match.group(1))
        
        # 提取数值
        # 吞吐量 (items_per_second)
        tp = bm.get('items_per_second', 0) / MILLION
        # 延迟 (hdr_median 或 hdr_mean), 转换为微秒 (us)
        # 假设原始单位是纳秒 (ns)
        lat = bm.get('hdr_median', 0) / 1000.0
        
        # 标记是 Throughput 还是 Latency 测试
        is_latency = '_lat' in name_lower
        
        record = {
            'threads': threads,
            'tp': tp,
            'lat': lat,
            'is_lat_bench': is_latency
        }
        
        runs[(fixture, bm_type)].append(record)
        
    # 按线程数排序
    for key in runs:
        runs[key].sort(key=lambda x: x['threads'])
        
    return runs

# ================= 3. 辅助绘图函数 =================
def hide_border(ax, show_left=True):
    ax.spines['top'].set_visible(False)
    ax.spines['right'].set_visible(False)
    if not show_left:
        ax.spines['left'].set_visible(False)
        ax.get_yaxis().set_visible(False)

# ================= 4. 主绘图逻辑 =================
def main():
    # 1. 准备数据和目录
    if not os.path.exists(DATA_FILE):
        print(f"错误: 找不到文件 {DATA_FILE}")
        return
    
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    runs = parse_data(DATA_FILE)
    
    # 2. 初始化画布 (2行4列)
    # 第一行: Latency, 第二行: Throughput
    fig, (lat_axes, tp_axes) = plt.subplots(2, 4, figsize=(24, 10))
    
    # 设置全局字体
    plt.rcParams.update({'font.size': 14, 'font.family': 'sans-serif'})
    
    # 3. 遍历四种负载类型进行绘图
    for i, bm_type in enumerate(BM_TYPES):
        lat_ax = lat_axes[i]
        tp_ax = tp_axes[i]
        
        # 设置子图标题
        titles = [
            "(a) UNIFORM R50:W50", "(b) UNIFORM R10:W90", 
            "(c) ZIPF R50:W50", "(d) ZIPF R10:W90"
        ]
        lat_ax.set_title(titles[i], fontsize=16, fontweight='bold')
        
        titles_tp = [
            "(e) UNIFORM R50:W50", "(f) UNIFORM R10:W90", 
            "(g) ZIPF R50:W50", "(h) ZIPF R10:W90"
        ]
        tp_ax.set_title(titles_tp[i], fontsize=16, fontweight='bold')

        # 遍历三个系统 (Viper, Dash, CCEH)
        for fixture, label in FIXTURES:
            data = runs.get((fixture, bm_type), [])
            if not data: continue
            
            style = STYLES[fixture]
            
            # 提取 Throughput 数据 (从 _tp 测试项)
            tp_data = [d for d in data if not d['is_lat_bench']]
            if tp_data:
                x = [d['threads'] for d in tp_data]
                y = [d['tp'] for d in tp_data]
                tp_ax.plot(x, y, label=label, **style, markeredgewidth=1, lw=2.5)
            
            # 提取 Latency 数据 (从 _lat 测试项)
            lat_data = [d for d in data if d['is_lat_bench']]
            if lat_data:
                x = [d['threads'] for d in lat_data]
                y = [d['lat'] for d in lat_data]
                lat_ax.plot(x, y, label=label, **style, markeredgewidth=1, lw=2.5)

    # 4. 统一坐标轴样式
    all_axes = list(lat_axes) + list(tp_axes)
    for ax in all_axes:
        ax.set_xticks([1, 8, 16, 24, 32, 36])
        ax.set_xlim(0, 38)
        ax.grid(axis='y', linestyle='--', alpha=0.5)
        hide_border(ax, show_left=True)
        ax.tick_params(axis='both', labelsize=12)

    # 5. 设置 Y 轴标签 (只在第一列显示)
    lat_axes[0].set_ylabel("Latency (us)", fontsize=16)
    tp_axes[0].set_ylabel("Throughput (Mops/s)", fontsize=16)
    
    # 统一 Throughput Y轴范围 (根据你的数据最高约14)
    for ax in tp_axes:
        ax.set_ylim(0, 20)

    # 6. 添加全局 X 轴标签
    fig.text(0.5, 0.04, "Number of Threads", ha='center', fontsize=18)
    
    # 7. 添加全局图例 (只取一个子图的句柄)
    handles, labels = tp_axes[0].get_legend_handles_labels()
    # 去重
    by_label = dict(zip(labels, handles))
    fig.legend(by_label.values(), by_label.keys(), 
               loc='upper center', bbox_to_anchor=(0.5, 0.98), 
               ncol=3, frameon=False, fontsize=16)

    # 8. 保存图片
    plt.tight_layout(rect=[0, 0.06, 1, 0.95]) # 留出空间给图例和X轴标签
    
    pdf_path = os.path.join(OUTPUT_DIR, 'ycsb_full_results.pdf')
    png_path = os.path.join(OUTPUT_DIR, 'ycsb_full_results.png')
    
    print(f"正在保存图片到 {OUTPUT_DIR}...")
    plt.savefig(pdf_path, format='pdf', bbox_inches='tight')
    plt.savefig(png_path, format='png', bbox_inches='tight', dpi=300)
    print(f"完成！\nPDF: {pdf_path}\nPNG: {png_path}")

if __name__ == "__main__":
    main()
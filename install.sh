#!/bin/bash
apt update -y 
apt install mediainfo -y
set -e


# 自动检测系统架构
ARCHITECTURE=$(uname -m)

# 根据架构设置下载链接
if [ "$ARCHITECTURE" == "x86_64" ]; then
    DOWNLOAD_URL="https://github.com/userdocs/qbittorrent-nox-static/releases/download/release-4.3.9_v1.2.15/x86_64-qbittorrent-nox"
elif [ "$ARCHITECTURE" == "aarch64" ]; then
    DOWNLOAD_URL="https://github.com/userdocs/qbittorrent-nox-static/releases/download/release-4.3.9_v1.2.15/aarch64-qbittorrent-nox"
else
    echo "Unsupported architecture: $ARCHITECTURE"
    exit 1
fi

INSTALL_PATH="/root/qbittorrent-nox"
SERVICE_FILE="/etc/systemd/system/qbittorrent.service"

# 下载 qBittorrent-nox
cd /root
wget -O qbittorrent-nox "$DOWNLOAD_URL"
chmod +x qbittorrent-nox

# 运行一次以生成默认配置
./qbittorrent-nox &   # 后台运行
sleep 2                      # 等待 2 秒
echo "y" | pkill -P $$ -f qbittorrent-nox || true  # 输入 "y" 并杀死子进程

# 创建 Systemd 服务文件
cat << EOF > "$SERVICE_FILE"
[Unit]
Description=qBittorrent Daemon Service
After=network.target

[Service]
LimitNOFILE=512000
User=root
ExecStart=$INSTALL_PATH

[Install]
WantedBy=multi-user.target
EOF
tcp_tune() {
    echo "==>  优化 TCP 设置中..."
    
    # 删除现有的 TCP 相关设置
    sed -i '/net.ipv4.tcp_no_metrics_save/d' /etc/sysctl.conf
    sed -i '/net.ipv4.tcp_ecn/d' /etc/sysctl.conf
    sed -i '/net.ipv4.tcp_frto/d' /etc/sysctl.conf
    sed -i '/net.ipv4.tcp_mtu_probing/d' /etc/sysctl.conf
    sed -i '/net.ipv4.tcp_rfc1337/d' /etc/sysctl.conf
    sed -i '/net.ipv4.tcp_sack/d' /etc/sysctl.conf
    sed -i '/net.ipv4.tcp_fack/d' /etc/sysctl.conf
    sed -i '/net.ipv4.tcp_window_scaling/d' /etc/sysctl.conf
    sed -i '/net.ipv4.tcp_adv_win_scale/d' /etc/sysctl.conf
    sed -i '/net.ipv4.tcp_moderate_rcvbuf/d' /etc/sysctl.conf
    sed -i '/net.ipv4.tcp_rmem/d' /etc/sysctl.conf
    sed -i '/net.ipv4.tcp_wmem/d' /etc/sysctl.conf
    sed -i '/net.core.rmem_max/d' /etc/sysctl.conf
    sed -i '/net.core.wmem_max/d' /etc/sysctl.conf
    sed -i '/net.ipv4.udp_rmem_min/d' /etc/sysctl.conf
    sed -i '/net.ipv4.udp_wmem_min/d' /etc/sysctl.conf
    sed -i '/net.core.default_qdisc/d' /etc/sysctl.conf
    sed -i '/net.ipv4.tcp_congestion_control/d' /etc/sysctl.conf

    # 添加新的 TCP 优化设置
    cat >> /etc/sysctl.conf << EOF
net.ipv4.tcp_no_metrics_save=1
net.ipv4.tcp_ecn=0
net.ipv4.tcp_frto=0
net.ipv4.tcp_mtu_probing=0
net.ipv4.tcp_rfc1337=1
net.ipv4.tcp_sack=1
net.ipv4.tcp_fack=1
net.ipv4.tcp_window_scaling=2
net.ipv4.tcp_adv_win_scale=2
net.ipv4.tcp_moderate_rcvbuf=1
net.ipv4.tcp_rmem=4096 65536 37331520
net.ipv4.tcp_wmem=4096 65536 37331520
net.core.rmem_max=37331520
net.core.wmem_max=37331520
net.ipv4.udp_rmem_min=8192
net.ipv4.udp_wmem_min=8192
net.core.default_qdisc=fq
net.ipv4.tcp_congestion_control=bbr
EOF

    # 应用系统配置
    sysctl -p && sysctl --system
    echo "TCP 设置已优化。"
}
tcp_tune
# 重新加载 Systemd 配置
echo "重新加载 Systemd 配置"
sleep 1
systemctl daemon-reload

# 启动并启用 qBittorrent 服务
echo "重新启动 qBittorrent 服务"
sleep 2   

systemctl restart qbittorrent
# 打印服务状态
sleep 1
systemctl status qbittorrent --no-pager

# 提示用户访问信息
echo "qBittorrent 已成功安装和配置。"
echo "请访问 http://<你的公网IP>:8080 进行登录。"
echo "默认用户名: admin"
echo "默认密码: adminadmin"

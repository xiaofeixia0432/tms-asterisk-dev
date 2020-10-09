# 获取本机IP地址
HOST_IP=$(ifconfig en0 | awk '/inet / { print $2 }')

echo '获得IP地址：'$HOST_IP

# 修改/etc/hosts文件
sed -i "" "s/^.*host.tms.asterisk/${HOST_IP} host.tms.asterisk/g" /etc/hosts

# 设置外部IP
sed -i "" "s/^external_media_address=.*/external_media_address=${HOST_IP}/g" ./conf/pjsip.conf
sed -i "" "s/^external_signaling_address=.*/external_signaling_address=${HOST_IP}/g" ./conf/pjsip.conf

# 启动容器
docker-compose -f docker-compose.13.yml up
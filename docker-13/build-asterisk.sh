#!/bin/bash
PROGNAME=$(basename $0)

# 获得宿主机地址（构建阶段不管用？）
# ping -q -c1 $HOST_DOMAIN > /dev/null 2>&1
# if [ $? -ne 0 ]; then
#   HOST_IP=$(ip route | awk 'NR==1 {print $3}')
#   echo -e "$HOST_IP\t$HOST_DOMAIN" >> /etc/hosts
# fi

# if test -z ${ASTERISK_VERSION}; then
#     echo "${PROGNAME}: ASTERISK_VERSION required" >&2
#     exit 1
# fi

set -ex

# 1.5 jobs per core works out okay
: ${JOBS:=$(( $(nproc) + $(nproc) / 2 ))}

# ./configure --with-resample --with-pjproject-bundled
./configure  --libdir=/usr/lib64 --with-pjproject-bundled --with-jansson-bundled
make menuselect/menuselect menuselect-tree menuselect.makeopts

# disable BUILD_NATIVE to avoid platform issues
menuselect/menuselect --disable BUILD_NATIVE menuselect.makeopts

# enable good things
menuselect/menuselect --enable BETTER_BACKTRACES menuselect.makeopts

# codecs
# menuselect/menuselect --enable codec_opus menuselect.makeopts
# menuselect/menuselect --enable codec_silk menuselect.makeopts

# # download more sounds
# for i in CORE-SOUNDS-EN MOH-OPSOUND EXTRA-SOUNDS-EN; do
#     for j in ULAW ALAW G722 GSM SLN16; do
#         menuselect/menuselect --enable $i-$j menuselect.makeopts
#     done
# done

# we don't need any sounds in docker, they will be mounted as volume
menuselect/menuselect --disable-category MENUSELECT_CORE_SOUNDS
menuselect/menuselect --disable-category MENUSELECT_MOH
menuselect/menuselect --disable-category MENUSELECT_EXTRA_SOUNDS

make -j ${JOBS} all
make install

# copy default configs
# cp /usr/src/asterisk/configs/basic-pbx/*.conf /etc/asterisk/
make samples

# set runuser and rungroup
sed -i -E 's/^;(run)(user|group)/\1\2/' /etc/asterisk/asterisk.conf

# Install opus, for some reason menuselect option above does not working
cd /usr/src/codecs/codec_opus-13.0_1.3.0-x86_64 && cp *.so /usr/lib/asterisk/modules/ && cp codec_opus_config-en_US.xml /var/lib/asterisk/documentation/

mkdir -p /etc/asterisk/ \
         /var/spool/asterisk/fax

chown -R asterisk:asterisk /etc/asterisk \
                           /var/*/asterisk \
                           /usr/*/asterisk
chmod -R 750 /var/spool/asterisk
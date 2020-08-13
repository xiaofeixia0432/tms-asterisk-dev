FROM jrottenberg/ffmpeg:4.2.3-centos7

RUN rpm -ivh https://mirrors.tuna.tsinghua.edu.cn/epel/6/x86_64/epel-release-6-8.noarch.rpm

RUN yum -y update

RUN yum -y install \
    cpp \
    gcc \
    gcc-c++ \
    make \
    ncurses \
    ncurses-devel \
    libxml2 \
    libxml2-devel \
    openssl-devel \
    newt-devel \
    libuuid-devel \
    net-snmp-devel \
    xinetd \
    tar \
    bzip2 \
    libffi-devel \
    sqlite-devel \
    curl \
    bison \
    patch \
    mpg123

COPY codec_opus-13.0_current-x86_64.tar.gz /usr/src/codecs/

RUN cd /usr/src/codecs && tar -zxf codec_opus-13.0_current-x86_64.tar.gz

RUN useradd --system asterisk

WORKDIR /usr/src/asterisk

COPY ./asterisk-13.33.0 ./

ENV HOST_DOMAIN host.docker.internal

COPY build-asterisk.sh ./
RUN chmod +x ./build-asterisk.sh && ./build-asterisk.sh

EXPOSE 5060/udp 5060/tcp
VOLUME /var/lib/asterisk/sounds /var/lib/asterisk/keys /var/lib/asterisk/phoneprov /var/spool/asterisk /var/log/asterisk

COPY docker-entrypoint.sh /
RUN chmod +x /docker-entrypoint.sh
ENTRYPOINT ["/docker-entrypoint.sh"]
CMD ["/usr/sbin/asterisk", "-vvvdddf", "-T", "-W", "-U", "asterisk", "-p"]
# rebuild in #33610
# docker build -t clickhouse/stateless-test .
ARG FROM_TAG=latest
FROM altinityinfra/test-base:$FROM_TAG

ARG odbc_driver_url="https://github.com/ClickHouse/clickhouse-odbc/releases/download/v1.1.4.20200302/clickhouse-odbc-1.1.4-Linux.tar.gz"

# golang version 1.13 on Ubuntu 20 is enough for tests
RUN apt-get update -y \
    && env DEBIAN_FRONTEND=noninteractive \
        apt-get install --yes --no-install-recommends \
            awscli='1.22.*' \
            brotli='1.0.*' \
            lz4='1.9.*' \
            expect='5.45.*' \
            golang='2:1.18~*' \
            lsof='4.93.*' \
            mysql-client='8.0*' \
            ncdu='1.15.*' \
            netcat-openbsd='1.218-*' \
            nodejs='12.22.*' \
            npm='8.5.*' \
            odbcinst='2.3.*' \
            openjdk-11-jre-headless='11.0.*' \
            openssl='3.0.*' \
            postgresql-client='14+*' \
            protobuf-compiler='3.12.*' \
            python3='3.10.*' \
            python3-lxml='4.8.*' \
            python3-pip='22.0.*' \
            python3-requests='2.25.*' \
            python3-termcolor='1.1.*' \
            qemu-user-static='1:6.2*' \
            sqlite3='3.37.*' \
            sudo='1.9.*' \
            tree='2.0.*' \
            unixodbc='2.3.*' \
            wget='1.21.*' \
            rustc='1.*' \
            cargo='1.*' \
            zstd='1.4.*' \
            file='1:5.41-*' \
            pv='1.6.*' \
            zip='3.0-*' \
            p7zip-full='16.02*' \
            rpm2cpio='4.17.*' \
            cpio='2.13*' \
    && apt-get clean

RUN pip3 install numpy~=1.26.3 scipy~=1.12.0 pandas~=1.5.3 Jinja2~=3.1.3

RUN mkdir -p /tmp/clickhouse-odbc-tmp \
   && wget -nv -O - ${odbc_driver_url} | tar --strip-components=1 -xz -C /tmp/clickhouse-odbc-tmp \
   && cp /tmp/clickhouse-odbc-tmp/lib64/*.so /usr/local/lib/ \
   && odbcinst -i -d -f /tmp/clickhouse-odbc-tmp/share/doc/clickhouse-odbc/config/odbcinst.ini.sample \
   && odbcinst -i -s -l -f /tmp/clickhouse-odbc-tmp/share/doc/clickhouse-odbc/config/odbc.ini.sample \
   && rm -rf /tmp/clickhouse-odbc-tmp

ENV TZ=Europe/Amsterdam
RUN ln -snf /usr/share/zoneinfo/$TZ /etc/localtime && echo $TZ > /etc/timezone

ENV NUM_TRIES=1
ENV MAX_RUN_TIME=0

# MinIO no longer publishes binaries on dl.min.io, take them from the mirror image
COPY --from=clickhouse/minio-minio:RELEASE.2024-07-31T05-46-26Z /usr/bin/minio /usr/bin/mc ./


RUN wget --no-verbose 'https://archive.apache.org/dist/hadoop/common/hadoop-3.3.1/hadoop-3.3.1.tar.gz' \
    && tar -xvf hadoop-3.3.1.tar.gz \
    && rm -rf hadoop-3.3.1.tar.gz

ENV MINIO_ROOT_USER="clickhouse"
ENV MINIO_ROOT_PASSWORD="clickhouse"
ENV EXPORT_S3_STORAGE_POLICIES=1

# TODO: check against 3.29.0, but 23.8.11 had azurite@3.28.0
# Current azurite dependencies need a newer Node.js than the Ubuntu one
RUN npm install -g azurite@3.29.0 \
    && npm install -g tslib@2.6.2 \
    && npm install -g node@20.20.2


COPY run.sh /
COPY setup_minio.sh /
COPY setup_hdfs_minicluster.sh /

CMD ["/bin/bash", "/run.sh"]

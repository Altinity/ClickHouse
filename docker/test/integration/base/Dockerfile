# rebuild in #33610
# docker build -t clickhouse/integration-test .
ARG FROM_TAG=latest
FROM altinityinfra/test-base:$FROM_TAG

SHELL ["/bin/bash", "-c"]

RUN apt-get update \
    && env DEBIAN_FRONTEND=noninteractive apt-get -y install \
        bsdutils='1:2.37.*' \
        curl='7.81.*' \
        default-jre='2:1.11-*' \
        g++='4:11.2.*' \
        gdb='12.1-*' \
        iproute2='5.15.*' \
        krb5-user='1.19.*' \
        libicu-dev='70.1-*' \
        libsqlite3-dev='3.37.*' \
        libsqliteodbc='0.999*' \
        lsof='4.93.*' \
        lz4='1.9.*' \
        odbc-postgresql='1:13.02.*' \
        odbcinst='2.3.*' \
        python3='3.10.*' \
        rpm2cpio='4.17.*' \
        sqlite3='3.37.*' \
        tar='1.34*' \
        tzdata \
        unixodbc='2.3.*' \
        python3-pip='22.0.*' \
        libcurl4-openssl-dev='7.81.*' \
        libssl-dev='3.0.*' \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/* /var/cache/debconf /tmp/*

RUN pip3 install pycurl~=7.45.2

# Architecture of the image when BuildKit/buildx is used
ARG TARGETARCH

# Install MySQL ODBC driver from RHEL rpm
# For reference https://downloads.mysql.com/archives/c-odbc/ RHEL
RUN arch=${TARGETARCH:-amd64} \
  && case $arch in \
      amd64) rarch=x86_64 ;; \
      arm64) rarch=aarch64 ;; \
    esac \
  && cd /tmp \
  && curl -o mysql-odbc.rpm "https://cdn.mysql.com/archives/mysql-connector-odbc-8.0/mysql-connector-odbc-8.0.32-1.el9.${rarch}.rpm" \
  && rpm2archive mysql-odbc.rpm \
  && tar xf mysql-odbc.rpm.tgz -C / ./usr/lib64/ \
  && rm mysql-odbc.rpm mysql-odbc.rpm.tgz \
  && ODBC_DIR=$(dpkg -L odbc-postgresql | rg '^/usr/lib/.*-linux-gnu/odbc$') \
  && ln -s /usr/lib64/libmyodbc8a.so "$ODBC_DIR" \
  && ln -s /usr/lib64/libmyodbc8a.so "$ODBC_DIR"/libmyodbc.so

# Unfortunately this is required for a single test for conversion data from zookeeper to clickhouse-keeper.
# ZooKeeper is not started by default, but consumes some space in containers.
# 777 perms used to allow anybody to start/stop ZooKeeper
ENV ZOOKEEPER_VERSION='3.6.3'
RUN curl "https://archive.apache.org/dist/zookeeper/zookeeper-${ZOOKEEPER_VERSION}/apache-zookeeper-${ZOOKEEPER_VERSION}-bin.tar.gz" | \
      tar -C opt -zxv && \
  mv /opt/apache-zookeeper-${ZOOKEEPER_VERSION}-bin /opt/zookeeper && \
  chmod -R 777 /opt/zookeeper && \
  echo $'tickTime=2500 \n\
tickTime=2500 \n\
dataDir=/zookeeper \n\
clientPort=2181 \n\
maxClientCnxns=80' > /opt/zookeeper/conf/zoo.cfg && \
  mkdir /zookeeper && \
  chmod -R 777 /zookeeper

ENV TZ=Etc/UTC
RUN ln -snf /usr/share/zoneinfo/$TZ /etc/localtime && echo $TZ > /etc/timezone

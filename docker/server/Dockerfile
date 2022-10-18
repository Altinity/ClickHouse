FROM ubuntu:20.04
#ARG repository="deb https://repo.clickhouse.tech/deb/stable/ main/"
ARG version=21.8.*

# set non-empty deb_location_url url to create a docker image
# from debs created by CI build, for example:

# docker build . -t altinity/clickhouse-server:${ALTINITY_BUILD_VERSION}.${ALTINITY_BUILD_FEATURE} --network=host --build-arg deb_location_url="http://0.0.0.0:8000" --build-arg BUILD=$ALTINITY_BUILD_VERSION --build-arg FEATURE=${ALTINITY_BUILD_FEATURE}

ARG deb_location_url=""

ARG BUILD
ARG FEATURE
ARG DEBIAN_FRONTEND=noninteractive

COPY su-exec.c /su-exec.c

RUN echo $BUILD $FEATURE
RUN groupadd -r clickhouse --gid=101 \
    && useradd -r -g clickhouse --uid=101 --home-dir=/var/lib/clickhouse --shell=/bin/bash clickhouse \
    && apt-get update \
    && apt-get install --yes --no-install-recommends \
        apt-transport-https \
        ca-certificates \
        dirmngr \
        gnupg \
        locales \
        wget \
        tzdata \
        lz4 \
    && rm -rf /tmp/clickhouse_debs \
    && mkdir -p /tmp/clickhouse_debs \
    && wget --progress=bar:force:noscroll "${deb_location_url}/clickhouse-common-static_${BUILD}.${FEATURE}_amd64.deb" -P /tmp/clickhouse_debs \
    && wget --progress=bar:force:noscroll "${deb_location_url}/clickhouse-client_${BUILD}.${FEATURE}_amd64.deb" -P /tmp/clickhouse_debs \
    && wget --progress=bar:force:noscroll "${deb_location_url}/clickhouse-server_${BUILD}.${FEATURE}_amd64.deb" -P /tmp/clickhouse_debs \
    && dpkg -i /tmp/clickhouse_debs/*.deb \
    && apt-get install -y --no-install-recommends tcc libc-dev && \
        tcc /su-exec.c -o /bin/su-exec && \
        chown root:root /bin/su-exec && \
        chmod 0755 /bin/su-exec && \
        rm /su-exec.c && \
        apt-get purge -y --auto-remove tcc libc-dev libc-dev-bin libc6-dev linux-libc-dev \
    && clickhouse-local -q 'SELECT * FROM system.build_options' \
    && rm -rf \
        /var/lib/apt/lists/* \
        /var/cache/debconf \
        /tmp/* \
    && apt-get clean \
    && mkdir -p /var/lib/clickhouse /var/log/clickhouse-server /etc/clickhouse-server /etc/clickhouse-client \
    && chmod ugo+Xrw -R /var/lib/clickhouse /var/log/clickhouse-server /etc/clickhouse-server /etc/clickhouse-client

# we need to allow "others" access to clickhouse folder, because docker container
# can be started with arbitrary uid (openshift usecase)


RUN locale-gen en_US.UTF-8
ENV LANG en_US.UTF-8
ENV LANGUAGE en_US:en
ENV LC_ALL en_US.UTF-8
ENV TZ UTC

RUN mkdir /docker-entrypoint-initdb.d

COPY docker_related_config.xml /etc/clickhouse-server/config.d/
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

EXPOSE 9000 8123 9009
VOLUME /var/lib/clickhouse

ENV CLICKHOUSE_CONFIG /etc/clickhouse-server/config.xml

ENTRYPOINT ["/entrypoint.sh"]

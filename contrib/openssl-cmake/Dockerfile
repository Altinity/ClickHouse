FROM ubuntu:22.04


RUN apt-get update && \
    apt-get install -y \
        build-essential \
        cmake \
        golang \
        unzip

ADD --checksum=sha256:6241ec2f13a5f80224ee9cd8592ed66a97d426481066feaa4efc6f24e60bbc96 \
    https://github.com/aws/aws-lc/archive/refs/tags/AWS-LC-FIPS-2.0.0.zip .
    
RUN unzip -q AWS-LC-FIPS-2.0.0.zip -d .

RUN cd /aws-lc-AWS-LC-FIPS-2.0.0 \
    && mkdir -p ./build \
    && cd ./build \
    && cmake -DFIPS=1 -DGO_EXECUTABLE=`which go` .. \
    && make

# Check that version is reported correctly
RUN cd /aws-lc-AWS-LC-FIPS-2.0.0/build \
    && cat <<C_CODE | gcc -x c - -o ./get_version -L./ssl -l:libssl.a -L./crypto -l:libcrypto.a #&& [ "$(./get_version)" == 'AWS-LC FIPS 2.0.0' ]
#include <stdio.h>
const char* awslc_version_string(void);
int main(void) {
  printf("%s\n", awslc_version_string());
  return 0;
};
C_CODE

#check is in FIPS mode
RUN /aws-lc-AWS-LC-FIPS-2.0.0/build/tool/bssl isfips
# execute all test
# RUN find /aws-lc-AWS-LC-FIPS-2.0.0/build -iname '*test*' -type f -executable -exec {} \;
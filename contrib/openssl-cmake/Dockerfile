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
    && <<C_CODE gcc -x c - -o ./check_version -L./ssl -l:libssl.a -L./crypto -l:libcrypto.a && ./check_version 'AWS-LC FIPS 2.0.0'
#include <stdio.h>
#include <string.h>
const char* awslc_version_string(void);
int expect(const char * expected, const char * actual) {
    if (strcmp(expected, actual) == 0)
        return 0;
    printf("expected %s but got %s", expected, actual);
    return 1;
}
int main(int argc, char * argv[]) {
    int result = 0;
    if (argc > 1)
        result |= expect((const char*)argv[1], awslc_version_string());
    return result;
};
C_CODE
RUN

#check is in FIPS mode
RUN test $(/aws-lc-AWS-LC-FIPS-2.0.0/build/tool/bssl isfips) = 1

# execute all test
RUN find /aws-lc-AWS-LC-FIPS-2.0.0/build -iname '*test*' -type f -executable -print -exec {} \;

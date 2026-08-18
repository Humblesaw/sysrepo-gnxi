FROM fedora:43 AS base

# dnf returns 1 when an error was handled by dnf (but still successful)
RUN dnf upgrade --refresh || (( $?==0 | $?==1 ))
RUN dnf group install -y c-development development-tools || (( $?==0 | $?==1 ))

# install libyang (libyang has to create pkg-config file for libyang-cpp)
RUN dnf install -y cmake pcre2-devel || (( $?==0 | $?==1 ))
RUN echo "/usr/local/lib64" > /etc/ld.so.conf.d/local.conf
WORKDIR /root
RUN git clone https://github.com/CESNET/libyang.git
WORKDIR /root/libyang
RUN git checkout devel
RUN mkdir build
WORKDIR /root/libyang/build
RUN cmake ..
RUN make -j4
RUN make install

# install sysrepo
RUN dnf install -y systemd-devel || (( $?==0 | $?==1 ))
WORKDIR /root
RUN git clone https://github.com/sysrepo/sysrepo.git
WORKDIR /root/sysrepo
RUN git checkout devel
RUN mkdir build
WORKDIR /root/sysrepo/build
RUN cmake ..
RUN make -j4
RUN make install

# install libyang-cpp
WORKDIR /root
RUN git clone https://github.com/CESNET/libyang-cpp.git
WORKDIR /root/libyang-cpp
RUN git checkout master
RUN mkdir build
WORKDIR /root/libyang-cpp/build
RUN cmake -DBUILD_TESTING=OFF ..
RUN make -j4
RUN make install

# install sysrepo-cpp
WORKDIR /root
RUN git clone https://github.com/sysrepo/sysrepo-cpp.git
WORKDIR /root/sysrepo-cpp
RUN git checkout master
RUN mkdir build
WORKDIR /root/sysrepo-cpp/build
RUN cmake -DBUILD_TESTING=OFF ..
RUN make -j4
RUN make install

# refresh
RUN ldconfig

# third-party dependencies
RUN dnf install -y grpc-devel protobuf-compiler protobuf-devel openssl-devel || (( $?==0 | $?==1 ))

# install sysrepo-gnxi
COPY . /root/sysrepo-gnxi
WORKDIR /root/sysrepo-gnxi
RUN mkdir build
WORKDIR /root/sysrepo-gnxi/build
RUN cmake -DENABLE_TESTS=ON -DENABLE_YANG_RPC=ON ..
RUN make -j4
RUN ctest --output-on-failure

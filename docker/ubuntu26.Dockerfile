FROM ubuntu:resolute AS base

RUN apt update
RUN apt install -y build-essential git

# install libyang (libyang has to create pkg-config file for libyang-cpp)
RUN apt install -y cmake libpcre2-dev pkg-config
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
RUN apt install -y libsystemd-dev
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
RUN apt install -y libgrpc++-dev protobuf-compiler-grpc libprotobuf-dev libssl-dev

# install sysrepo-gnxi
COPY . /root/sysrepo-gnxi
WORKDIR /root/sysrepo-gnxi
RUN mkdir build
WORKDIR /root/sysrepo-gnxi/build
RUN cmake -DENABLE_TESTS=ON -DENABLE_YANG_RPC=ON ..
RUN make -j4
RUN ctest --output-on-failure

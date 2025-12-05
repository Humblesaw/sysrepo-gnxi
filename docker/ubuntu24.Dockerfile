FROM ubuntu:noble AS base

RUN apt update
RUN apt install -y build-essential git

# install libyang (libyang has to create pkg-config file for libyang-cpp)
RUN apt install -y cmake libpcre2-dev pkg-config
WORKDIR /root
RUN git clone https://github.com/Humblesaw/libyang.git
WORKDIR /root/libyang
RUN git checkout new_gnmi
RUN mkdir build
WORKDIR /root/libyang/build
RUN cmake ..
RUN make -j4
RUN make install

# install sysrepo
RUN apt install -y libsystemd-dev
WORKDIR /root
RUN git clone https://github.com/Humblesaw/sysrepo.git
WORKDIR /root/sysrepo
RUN git checkout new_gnmi
RUN mkdir build
WORKDIR /root/sysrepo/build
RUN cmake ..
RUN make -j4
RUN make install

# install libyang-cpp
WORKDIR /root
RUN git clone https://github.com/Humblesaw/libyang-cpp.git
WORKDIR /root/libyang-cpp
RUN git checkout new_gnmi
RUN mkdir build
WORKDIR /root/libyang-cpp/build
RUN cmake -DBUILD_TESTING=OFF ..
RUN make -j4
RUN make install

# install sysrepo-cpp
WORKDIR /root
RUN git clone https://github.com/Humblesaw/sysrepo-cpp.git
WORKDIR /root/sysrepo-cpp
RUN git checkout new_gnmi
RUN mkdir build
WORKDIR /root/sysrepo-cpp/build
RUN cmake -DBUILD_TESTING=OFF ..
RUN make -j4
RUN make install

# third-party dependencies
RUN apt install -y libgrpc++-dev protobuf-compiler-grpc libprotobuf-dev

# install sysrepo-gnxi
WORKDIR /root
RUN git clone https://github.com/Humblesaw/sysrepo-gnxi.git
WORKDIR /root/sysrepo-gnxi
RUN git checkout new_gnmi
RUN mkdir build
WORKDIR /root/sysrepo-gnxi/build
RUN cmake -DENABLE_TESTS=ON ..
RUN make -j4
RUN ctest --output-on-failure

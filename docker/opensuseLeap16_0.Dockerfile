FROM opensuse/leap:16.0 AS base

RUN zypper update -y
RUN zypper install -y -t pattern devel_basis devel_C_C++
RUN zypper install -y git

# install libyang
RUN zypper install -y cmake pcre2-devel
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
RUN zypper install -y systemd-devel
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

# third-party dependencies
RUN zypper install -y grpc-devel protobuf-devel

# install sysrepo-gnxi
COPY . /root/sysrepo-gnxi
WORKDIR /root/sysrepo-gnxi
RUN mkdir build
WORKDIR /root/sysrepo-gnxi/build
RUN cmake -DENABLE_TESTS=ON ..
RUN make -j4
RUN ctest --output-on-failure

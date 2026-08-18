FROM archlinux:latest AS base

RUN pacman -Syu --noconfirm
RUN pacman -S --noconfirm base-devel git

# install libyang (libyang has to create pkg-config file for libyang-cpp)
RUN pacman -S --noconfirm cmake pcre2 pkgconf
WORKDIR /root
RUN git clone https://github.com/CESNET/libyang.git
WORKDIR /root/libyang
RUN git checkout devel
RUN mkdir build
WORKDIR /root/libyang/build
RUN cmake -DCMAKE_INSTALL_PREFIX=/usr ..
RUN make -j4
RUN make install

# install sysrepo
RUN pacman -S --noconfirm systemd-libs
WORKDIR /root
RUN git clone https://github.com/sysrepo/sysrepo.git
WORKDIR /root/sysrepo
RUN git checkout devel
RUN mkdir build
WORKDIR /root/sysrepo/build
RUN cmake -DCMAKE_INSTALL_PREFIX=/usr ..
RUN make -j4
RUN make install

# install libyang-cpp
WORKDIR /root
RUN git clone https://github.com/CESNET/libyang-cpp.git
WORKDIR /root/libyang-cpp
RUN git checkout master
RUN mkdir build
WORKDIR /root/libyang-cpp/build
RUN cmake -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTING=OFF ..
RUN make -j4
RUN make install

# install sysrepo-cpp
WORKDIR /root
RUN git clone https://github.com/sysrepo/sysrepo-cpp.git
WORKDIR /root/sysrepo-cpp
RUN git checkout master
RUN mkdir build
WORKDIR /root/sysrepo-cpp/build
RUN cmake -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTING=OFF ..
RUN make -j4
RUN make install

# refresh
RUN ldconfig

# third-party dependencies
RUN pacman -S --noconfirm grpc protobuf openssl

# install sysrepo-gnxi
COPY . /root/sysrepo-gnxi
WORKDIR /root/sysrepo-gnxi
RUN mkdir build
WORKDIR /root/sysrepo-gnxi/build
RUN cmake -DENABLE_TESTS=ON -DENABLE_YANG_RPC=ON ..
RUN make -j4
RUN ctest --output-on-failure

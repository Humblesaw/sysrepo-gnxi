FROM archlinux:latest AS base

RUN pacman -Syu --noconfirm
RUN pacman -S --noconfirm base-devel git

# install libyang (libyang has to create pkg-config file for libyang-cpp)
RUN pacman -S --noconfirm cmake pcre2 pkgconf
WORKDIR /root
RUN git clone https://github.com/Humblesaw/libyang.git
WORKDIR /root/libyang
RUN git checkout new_gnmi
RUN mkdir build
WORKDIR /root/libyang/build
RUN cmake -DCMAKE_INSTALL_PREFIX=/usr ..
RUN make -j4
RUN make install

# install sysrepo
RUN pacman -S --noconfirm systemd-libs
WORKDIR /root
RUN git clone https://github.com/Humblesaw/sysrepo.git
WORKDIR /root/sysrepo
RUN git checkout new_gnmi
RUN mkdir build
WORKDIR /root/sysrepo/build
RUN cmake -DCMAKE_INSTALL_PREFIX=/usr ..
RUN make -j4
RUN make install

# install libyang-cpp
WORKDIR /root
RUN git clone https://github.com/Humblesaw/libyang-cpp.git
WORKDIR /root/libyang-cpp
RUN git checkout new_gnmi
RUN mkdir build
WORKDIR /root/libyang-cpp/build
RUN cmake -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTING=OFF ..
RUN make -j4
RUN make install

# install sysrepo-cpp
WORKDIR /root
RUN git clone https://github.com/Humblesaw/sysrepo-cpp.git
WORKDIR /root/sysrepo-cpp
RUN git checkout new_gnmi
RUN mkdir build
WORKDIR /root/sysrepo-cpp/build
RUN cmake -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTING=OFF ..
RUN make -j4
RUN make install

# third-party dependencies
RUN pacman -S --noconfirm grpc protobuf

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

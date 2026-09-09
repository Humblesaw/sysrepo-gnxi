FROM opensuse/tumbleweed:latest

# build tools and third-party dependencies
RUN zypper dup -y \
    && zypper install -y -t pattern devel_basis devel_C_C++ \
    && zypper install -y git cmake pcre2-devel systemd-devel grpc-devel protobuf-devel libopenssl-devel \
    && zypper clean -a \
    && echo "/usr/local/lib64" > /etc/ld.so.conf.d/local.conf

# install libyang
WORKDIR /root
RUN git clone --depth 1 https://github.com/CESNET/libyang.git \
    && cd libyang \
    && git fetch --depth 1 origin 4a6b09b75a93be875cb1418f0dc47e2168b91a30 \
    && git checkout 4a6b09b75a93be875cb1418f0dc47e2168b91a30
WORKDIR /root/libyang/build
RUN cmake ..
RUN make -j"$(nproc)"
RUN make install

# install sysrepo
WORKDIR /root
RUN git clone --depth 1 https://github.com/sysrepo/sysrepo.git \
    && cd sysrepo \
    && git fetch --depth 1 origin 571033486ccedadf020fa06b535172261fab1a81 \
    && git checkout 571033486ccedadf020fa06b535172261fab1a81
WORKDIR /root/sysrepo/build
RUN cmake ..
RUN make -j"$(nproc)"
RUN make install

# install libyang-cpp
WORKDIR /root
RUN git clone --depth 1 --branch v11 https://github.com/CESNET/libyang-cpp.git
WORKDIR /root/libyang-cpp/build
RUN cmake -DBUILD_TESTING=OFF ..
RUN make -j"$(nproc)"
RUN make install

# install sysrepo-cpp
WORKDIR /root
RUN git clone --depth 1 --branch v10 https://github.com/sysrepo/sysrepo-cpp.git
WORKDIR /root/sysrepo-cpp/build
RUN cmake -DBUILD_TESTING=OFF ..
RUN make -j"$(nproc)"
RUN make install

# refresh
RUN ldconfig

# install sysrepo-gnxi
COPY . /root/sysrepo-gnxi
WORKDIR /root/sysrepo-gnxi/build
RUN cmake -DENABLE_TESTS=ON -DENABLE_YANG_RPC=ON ..
RUN make -j"$(nproc)"
RUN ctest --output-on-failure

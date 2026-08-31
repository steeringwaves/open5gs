FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive

RUN apt update -y && apt install -y && \
    apt install -y --no-install-recommends \
    software-properties-common ca-certificates \
    lsb-release \
    apt-transport-https \
    bc gcc git git-lfs zip unzip p7zip xz-utils tar rsync \
	python3-pip \
	python3-setuptools \
	python3-wheel \
	ninja-build \
	build-essential \
	flex \
	bison \
	git \
	cmake \
	meson \
	libsctp-dev \
	libgnutls28-dev \
	libgcrypt-dev \
	libssl-dev \
	libidn11-dev \
	libmongoc-dev \
	libbson-dev \
	libyaml-dev \
	libmicrohttpd-dev \
	libcurl4-gnutls-dev \
	libnghttp2-dev \
	libtins-dev \
	libtalloc-dev \
	iproute2 \
	ca-certificates \
	netbase \
	pkg-config \
	wget \
	docker.io docker-compose \
	nodejs npm \
		&& \
    apt-get clean

ARG LLVM_VERSION=20
RUN bash -c "$(wget -O - https://apt.llvm.org/llvm.sh)" && \
  apt-get update && \
  apt-get install -y clang-${LLVM_VERSION} clang-tools-${LLVM_VERSION} clang-format-${LLVM_VERSION} clangd-${LLVM_VERSION} clang-tidy-${LLVM_VERSION} lldb-${LLVM_VERSION} lld-${LLVM_VERSION} llvm-${LLVM_VERSION} && \
  ln -sf /usr/bin/clang-${LLVM_VERSION} /usr/bin/clang && \
  ln -sf /usr/bin/clang-format-${LLVM_VERSION} /usr/bin/clang-format && \
  ln -sf /usr/bin/clang-tidy-${LLVM_VERSION} /usr/bin/clang-tidy && \
  ln -sf /usr/bin/clangd-${LLVM_VERSION} /usr/bin/clangd && \
  ln -sf /usr/bin/lld-${LLVM_VERSION} /usr/bin/lld && \
  ln -sf /usr/bin/lldb-${LLVM_VERSION} /usr/bin/lldb && \
  ln -sf /usr/bin/llvm-ar-${LLVM_VERSION} /usr/bin/llvm-ar && \
  ln -sf /usr/bin/llvm-cov-${LLVM_VERSION} /usr/bin/llvm-cov && \
  ln -sf /usr/bin/llvm-symbolizer-${LLVM_VERSION} /usr/bin/llvm-symbolizer

# COPY packages/googletest-1.15.2-x86_64.tar.gz /docker-files/packages/
# RUN cd /docker-files/packages && \
#     tar -xf googletest-1.15.2-x86_64.tar.gz -C / && \
#     rm -rf /docker-files

# COPY packages/argp-standalone-1.5.0-x86_64.tar.gz /docker-files/packages/
# RUN cd /docker-files/packages && \
#     tar -xf argp-standalone-1.5.0-x86_64.tar.gz -C / && \
#     rm -rf /docker-files


ARG USER_UID=1000
ARG USER_GID=1000
ARG USER_NAME=dev
ARG USER_GROUP=dev
ARG USER_PASSWORD="dev"

# Check environment
RUN echo "${USER_UID} - ${USER_GID} - ${USER_NAME} - ${USER_GROUP} - ${USER_PASSWORD}"

# Change root password to root
# Add the dev user with uid 1000 and password dev
# Copy the default bashrc to the dev user
RUN (echo "root:$USER_PASSWORD" | chpasswd) || true && \
	groupadd -fg $USER_GID $USER_GROUP && \
	useradd -m -u ${USER_UID} ${USER_NAME} -g $USER_GROUP && echo "$USER_NAME:$USER_PASSWORD" | chpasswd && \
	cat /root/.bashrc > /home/$USER_NAME/.bashrc && chown $USER_NAME:$USER_GROUP /home/$USER_NAME/.bashrc

# Switch to the dev user
USER $USER_NAME

# Set the working directory
WORKDIR /home/$USER_NAME

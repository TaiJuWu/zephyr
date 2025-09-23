FROM ubuntu:24.04
RUN apt-get update

RUN apt install -y --no-install-recommends git cmake ninja-build gperf \
  ccache dfu-util device-tree-compiler wget python3-pip pipx \
   python3-dev python3-venv python3-tk \
  xz-utils file make gcc libsdl2-dev libmagic1

RUN pipx install west
ENV PATH="/root/.local/bin:$PATH"

WORKDIR /opt/zephyrproject
RUN west init /opt/zephyrproject

RUN west update
RUN west zephyr-export
RUN west packages pip --install

RUN west sdk install

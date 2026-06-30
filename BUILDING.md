# BUILDING

This project: **a-paxos-net-library**
Version: **0.1.5**

## Local build

```bash
# one-shot build + install
./build.sh install
```

Or run the steps manually:

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j"$(nproc || sysctl -n hw.ncpu || echo 4)"
sudo cmake --install .
```



## Install dependencies (from `deps.libraries`)


### System packages (required)

```bash
sudo apt-get update && sudo apt-get install -y libssl-dev zlib1g-dev
```



### Development tooling (optional)

```bash
sudo apt-get update && sudo apt-get install -y autoconf automake gdb libtool perl python3 python3-pip python3-venv valgrind
```



### 3rd-party/libuv


### libuv

Clone & build:

```bash
git clone --depth 1 --branch v1.48.0 --single-branch "https://github.com/libuv/libuv.git" "libuv"
cd "libuv"
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=${PREFIX:-/usr/local} -DBUILD_TESTING=OFF
cmake --build build -j"$(nproc)"
${SUDO}cmake --install build
cd ..
rm -rf "libuv"
```


### OpenSSL

Install via package manager:

```bash
sudo apt-get update && sudo apt-get install -y libssl-dev
```


### the-macro-library

Clone & build:

```bash
git clone --depth 1 --single-branch "https://github.com/contactandyc/the-macro-library.git" "the-macro-library"
cd "the-macro-library"
./build.sh clean
./build.sh install
cd ..
rm -rf "the-macro-library"
```


### a-paxos-core

Clone & build:

```bash
git clone --depth 1 --single-branch "https://github.com/contactandyc/a-paxos-core.git" "a-paxos-core"
cd "a-paxos-core"
./build.sh clean
./build.sh install
cd ..
rm -rf "a-paxos-core"
```


### ZLIB

Install via package manager:

```bash
sudo apt-get update && sudo apt-get install -y zlib1g-dev
```


### h2o

Clone & build:

```bash
git clone --depth 1 --branch v2.2.6 --single-branch "https://github.com/h2o/h2o.git" "h2o"
cd "h2o"
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=${PREFIX:-/usr/local} -DWITH_MRUBY=OFF -DBUILD_SHARED_LIBS=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build -j"$(nproc)"
${SUDO}cmake --install build
cd ..
rm -rf "h2o"
```


### h2o-c-library

Clone & build:

```bash
git clone --depth 1 --single-branch "https://github.com/contactandyc/h2o-c-library.git" "h2o-c-library"
cd "h2o-c-library"
./build.sh clean
./build.sh install
cd ..
rm -rf "h2o-c-library"
```


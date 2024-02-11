# BPF Tools

## Introduction

While name of this repo is bpf, it contains much more functionality. Repo contains a set of independent applications helpful for distributing a set of applications on a computing cluster.

## Building

Install requirements:

```bash
apt install clang-14 clang-tools-14 clangd-14 libclang-14-dev libclang-cpp14 libclang-cpp14-dev clang-14-doc clang-tidy-14 libbpf-dev bpfcc-tools clang libprocps-dev
```

Init all submodules:

```bash
git submodule update --init --recursive
```

Build libbpf-bootstrap if this is the first time you're building the repo:

```bash
mkdir libbpf-bootstrap/build
pushd libbpf-bootstrap/build
cmake ../examples/c
make
popd
```
Also rpclib:

```bash
pushd rpclib
git apply ../rpclib_fpic.patch
mkdir build
cd build
cmake ..
make j8
popd
```

and ld_preload:

```bash
pushd ld_preload
make
popd
```



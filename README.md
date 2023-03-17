# BPF Tools

## Introduction

While name of this repo is bpf, it contains much more functionality. Repo contains a set of independent applications helpful for distributing a set of applications on a computing cluster.

## Building

Init all submodules:

```bash
git submodule update --init --recursive
```

Build libbpf-bootstrap if this is the first time you're building the repo:

```bash
mkdir libbpf-bootstrap/build
pushd libbpf-bootstrap/build
cmake ../examples/c
make -j8
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



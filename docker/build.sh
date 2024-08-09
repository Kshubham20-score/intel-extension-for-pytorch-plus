#!/bin/bash

if [[ ${IMAGE_TYPE} = "xpu" ]]; then
    IMAGE_NAME=intel/intel-extension-for-pytorch:2.1.40-$IMAGE_TYPE-pip-base
    docker build --build-arg http_proxy=$http_proxy \
                 --build-arg https_proxy=$https_proxy \
                 --build-arg no_proxy=" " \
                 --build-arg NO_PROXY=" " \
                 --build-arg UBUNTU_VERSION=22.04 \
                 --build-arg PYTHON=python3.10 \
                 --build-arg ICD_VER=24.22.29735.27-914~22.04 \
                 --build-arg LEVEL_ZERO_GPU_VER=1.3.29735.27-914~22.04 \
                 --build-arg LEVEL_ZERO_VER=1.17.6-914~22.04 \
                 --build-arg LEVEL_ZERO_DEV_VER=1.17.6-914~22.04 \
                 --build-arg DPCPP_VER=2024.2.1-1079 \
                 --build-arg MKL_VER=2024.2.1-103 \
                 --build-arg CCL_VER=2021.13.1-31 \
                 --build-arg TORCH_VERSION=2.1.0.post3+cxx11.abi  \
                 --build-arg IPEX_VERSION=2.1.40+xpu \
                 --build-arg TORCHVISION_VERSION=0.16.0.post3+cxx11.abi \
                 --build-arg TORCHAUDIO_VERSION=2.1.0.post3+cxx11.abi \
                 --build-arg ONECCL_BIND_PT_VERSION=2.1.400+xpu \
                 --build-arg TORCH_WHL_URL=https://pytorch-extension.intel.com/release-whl/stable/xpu/us/ \
                 --build-arg IPEX_WHL_URL=https://pytorch-extension.intel.com/release-whl/stable/xpu/us/ \
                 --build-arg TORCHVISION_WHL_URL=https://pytorch-extension.intel.com/release-whl/stable/xpu/us/ \
                 --build-arg TORCHAUDIO_WHL_URL=https://pytorch-extension.intel.com/release-whl/stable/xpu/us/ \
                 --build-arg ONECCL_BIND_PT_WHL_URL=https://pytorch-extension.intel.com/release-whl/stable/xpu/us/ \
                 -t ${IMAGE_NAME} \
                 -f Dockerfile.prebuilt .
fi

#!/usr/bin/env bash
set -e

# Mode: Select to compile projects into wheel files or install wheel files compiled.
# High bit: 8 7 6 5 4 3 2 1 :Low bit
#           | | | | | | | └- Deploy env
#           | | | | | | └--- Prepare env
#           | | | | | └----- Install from prebuilt wheel files
#           | | | | └------- Undefined
#           | | | └--------- Undefined
#           | | └----------- Undefined
#           | └------------- Undefined
#           └--------------- Undefined
MODE=0x03
DPCPP_ROOT=
ONEMKL_ROOT=
ONECCL_ROOT=
MPI_ROOT=
PTI_ROOT=
AOT=
if [[ $# -eq 0 ]]; then
    echo "Usage: bash $0 <MODE> [DPCPPROOT] [MKLROOT] [CCLROOT] [MPIROOT] [PTIROOT] [AOT]"
    echo "Set MODE to 7 to install from wheel files. Set it to 3 to compile from source. When compiling from source, you need to set arguments below."
    echo "DPCPPROOT, MKLROOT, CCLROOT, MPIROOT and PTIROOT are mandatory, should be absolute or relative path to the root directory of DPC++ compiler, oneMKL, oneCCL, Intel(R) MPI and Profiling Tools Interfaces for GPU (PTI for GPU) respectively."
    echo "AOT should be set to the text string for environment variable USE_AOT_DEVLIST. Setting it to \"none\" to disable AOT."
    exit 1
fi

if [[ ! $1 =~ ^[0-9]+$ ]] && [[ ! $1 =~ ^0x[0-9a-fA-F]+$ ]]; then
    echo "Warning: Unexpected argument. Using default value."
else
    MODE=$1
fi
shift
if [[ $# -gt 0 ]]; then
    DPCPP_ROOT=$1
    shift
fi
if [[ $# -gt 0 ]]; then
    ONEMKL_ROOT=$1
    shift
fi
if [[ $# -gt 0 ]]; then
    ONECCL_ROOT=$1
    shift
fi
if [[ $# -gt 0 ]]; then
    MPI_ROOT=$1
    shift
fi
if [[ $# -gt 0 ]]; then
    PTI_ROOT=$1
    shift
fi
if [[ $# -gt 0 ]]; then
    AOT=$1
    shift
fi

# Save current directory path
BASEFOLDER=$( cd -- "$( dirname -- "${BASH_SOURCE[0]:-$0}" )" &> /dev/null && pwd )
WHEELFOLDER=${BASEFOLDER}/../wheels
AUX_INSTALL_SCRIPT=${WHEELFOLDER}/aux_install.sh
cd ${BASEFOLDER}/..

if [ $((${MODE} & 0x06)) -eq 2 ] &&
   ([ -z ${DPCPP_ROOT} ] ||
   [ -z ${ONEMKL_ROOT} ] ||
   [ -z ${ONECCL_ROOT} ] ||
   [ -z ${MPI_ROOT} ] ||
   [ -z ${PTI_ROOT} ] ||
   [ -z ${AOT} ]); then
    echo "Source code compilation is needed. Please set arguments DPCPP_ROOT, ONEMKL_ROOT, ONECCL_ROOT, MPI_ROOT, PTIROOT and AOT."
    echo "DPCPPROOT, MKLROOT, CCLROOT, MPIROOT and PTIROOT are mandatory, should be absolute or relative path to the root directory of DPC++ compiler, oneMKL, oneCCL, Intel(R) MPI and Profiling Tools Interfaces for GPU (PTI for GPU) respectively."
    echo "AOT should be set to the text string for environment variable USE_AOT_DEVLIST. Setting it to \"none\" to disable AOT."
    exit 2
fi

# Check existance of required Linux commands
if [ $((${MODE} & 0x02)) -ne 0 ]; then
    # Enter IPEX root dir
    cd ../../..

    if [ ! -f dependency_version.json ]; then
        echo "Please check if `pwd` is a valid Intel® Extension for PyTorch* source code directory."
        exit 3
    fi
    VER_TORCHCCL=$(python scripts/tools/compilation_helper/dep_ver_utils.py -f dependency_version.json -k torch-ccl:version)
    VER_TORCH=$(python scripts/tools/compilation_helper/dep_ver_utils.py -f dependency_version.json -k pytorch:version)
    VER_IPEX_MAJOR=$(grep "VERSION_MAJOR" version.txt | cut -d " " -f 2)
    VER_IPEX_MINOR=$(grep "VERSION_MINOR" version.txt | cut -d " " -f 2)
    VER_IPEX_PATCH=$(grep "VERSION_PATCH" version.txt | cut -d " " -f 2)
    VER_IPEX="${VER_IPEX_MAJOR}.${VER_IPEX_MINOR}.${VER_IPEX_PATCH}+xpu"
    # Enter IPEX parent dir
    cd ..

    # Clear previous compilation output
    if [ -d ${WHEELFOLDER} ]; then
        rm -rf ${WHEELFOLDER}
    fi
    mkdir ${WHEELFOLDER}

    echo "#!/bin/bash" > ${AUX_INSTALL_SCRIPT}
    echo "set -e" >> ${AUX_INSTALL_SCRIPT}
    if [ $((${MODE} & 0x04)) -ne 0 ]; then
        echo "python -m pip install torch==${VER_TORCH} intel-extension-for-pytorch==${VER_IPEX} oneccl-bind-pt==${VER_TORCHCCL} --extra-index-url https://pytorch-extension.intel.com/release-whl/stable/xpu/us/" >> ${AUX_INSTALL_SCRIPT}
    else
        if [ ! -f ${ONECCL_ROOT}/env/vars.sh ]; then
            echo "oneCCL environment ${ONECCL_ROOT} doesn't seem to exist."
            exit 4
        fi

        if [ ! -f ${MPI_ROOT}/env/vars.sh ]; then
            echo "MPI environment ${MPI_ROOT} doesn't seem to exist."
            exit 5
        fi

        if [ ! -f ${PTI_ROOT}/env/vars.sh ]; then
            echo "PTI environment ${PTI_ROOT} doesn't seem to exist."
            exit 6
        fi

        # Check existance of required Linux commands
        for CMD in git; do
            command -v ${CMD} > /dev/null || (echo "Error: Command \"${CMD}\" is required."; exit 7;)
        done

        # Install PyTorch and Intel® Extension for PyTorch*
        cp intel-extension-for-pytorch/scripts/compile_bundle.sh .
        sed -i "s/VER_IPEX=.*/VER_IPEX=/" compile_bundle.sh
        bash compile_bundle.sh ${DPCPP_ROOT} ${ONEMKL_ROOT} ${ONECCL_ROOT} ${MPI_ROOT} ${PTI_ROOT} ${AOT} 1
        cp pytorch/dist/*.whl ${WHEELFOLDER}
        cp intel-extension-for-pytorch/dist/*.whl ${WHEELFOLDER}
        cp torch-ccl/dist/*.whl ${WHEELFOLDER}
        rm -rf compile_bundle.sh llvm-project llvm-release pytorch torch-ccl
        echo "python -m pip install ./wheels/*.whl" >> ${AUX_INSTALL_SCRIPT}
        echo "rm -rf wheels" >> ${AUX_INSTALL_SCRIPT}
    fi
    echo "python -m pip install -r ./requirements.txt" >> ${AUX_INSTALL_SCRIPT}

    # Return back to llm directory
    cd ${BASEFOLDER}/..
fi
if [ $((${MODE} & 0x01)) -ne 0 ]; then
    export LIBRARY_PATH+=${CONDA_PREFIX}/lib
    bash ${AUX_INSTALL_SCRIPT}
fi

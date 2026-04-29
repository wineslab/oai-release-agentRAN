#!/bin/bash

# Check if cuBB_SDK is defined, if not, use default path
cuBB_Path="${cuBB_SDK:-/opt/nvidia/cuBB}"

cd "$cuBB_Path" || exit 1

# Restart MPS
# Export variables
export CUDA_DEVICE_MAX_CONNECTIONS=8
export CUDA_MPS_PIPE_DIRECTORY=/var
export CUDA_MPS_LOG_DIRECTORY=/var

# Stop existing MPS
echo quit | sudo -E nvidia-cuda-mps-control

# Start MPS
sudo -E nvidia-cuda-mps-control -d
sudo -E echo start_server -uid 0 | sudo -E nvidia-cuda-mps-control

# Start cuphycontroller_scf
# Check if an argument is provided
if [ $# -eq 0 ]; then
    # No argument provided, use default value
    argument="P5G_WNC_GH"
else
    # Argument provided, use it
    argument="$1"
fi

sudo -E "$cuBB_Path"/build.$(uname -m)/cuPHY-CP/cuphycontroller/examples/cuphycontroller_scf "$argument"
sudo -E ./build.$(uname -m)/cuPHY-CP/gt_common_libs/nvIPC/tests/pcap/pcap_collect

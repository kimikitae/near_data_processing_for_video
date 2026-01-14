#!/bin/bash

# 1. NVMe-oF Transport 생성
sudo scripts/rpc.py nvmf_create_transport -t TCP -u 131072 -m 20 -c 8192 -i 2097152

# 2. 로컬 PCIe NVMe 장치를 SPDK bdev로 attach
sudo scripts/rpc.py bdev_nvme_attach_controller -b NVMe0 -t PCIe -a 82:00.0

# 3. Subsystem 생성 (여기서 -d nvme0n1 제거!)
sudo scripts/rpc.py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001 -m "SPDK_Controller"

# 4. Namespace 추가 (SPDK가 만든 bdev 이름 사용: NVMe0n1)
sudo scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 NVMe0n1

# 5. 리스너 추가 (타겟 NQN)
sudo scripts/rpc.py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t tcp -a 192.168.100.2 -s 4420

# 6. Discovery 서브시스템에 리스너 추가
sudo scripts/rpc.py nvmf_subsystem_add_listener nqn.2014-08.org.nvmexpress.discovery -t tcp -a 192.168.100.2 -s 4420

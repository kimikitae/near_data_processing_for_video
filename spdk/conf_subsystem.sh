#!/bin/bash

sudo sudo scripts/rpc.py nvmf_create_transport -t TCP -u 131072 -m 20 -c 8192 -i 2097152 # SPDK의 TCP Transport 스펙을 확인하여 구성하세요. 좌측에 나와있는 것은 참고만 하세요.
sudo scripts/rpc.py bdev_nvme_attach_controller -b nvme1 -t PCIe -a 0000:82:00.0
sudo scripts/rpc.py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001 -d nvme1n1
sudo scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 nvme1n1
sudo scripts/rpc.py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t tcp -a 192.168.100.2 -s 4420
sudo scripts/rpc.py nvmf_subsystem_add_listener nqn.2014-08.org.nvmexpress.discovery -t tcp -a 192.168.100.2 -s 4420


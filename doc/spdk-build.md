# Build under CentOS 6.x #

## Prerequisites ##

update your CentOS 6.x to the latest version possible

    $ sudo yum update

install `gcc`; the version is 4.4.7 for CentOS 6.x.

    $ sudo yum install gcc

install `libpciaccess-devel`

    $ sudo yum install libpciaccess-devel

install `CUnit-devel`; first prepare the epel-repo source

    $ wget http://dl.fedoraproject.org/pub/epel/6/x86_64/epel-release-6-?.noarch.rpm

or

    $ wget https://dl.fedoraproject.org/pub/epel/epel-release-latest-6.noarch.rpm

and

    $ sudo rpm -Uvh epel-release-6-?.noarch.rpm

then

    $ sudo yum install CUnit-devel

install `libaio-devel`

    $ sudo yum install libaio-devel

## Download the sources ##

download SPDK and DPDK source codes

    $ wget https://github.com/spdk/spdk/archive/master.zip
    $ unzip master.zip
    $ wget http://dpdk.org/browse/dpdk/snapshot/dpdk-2.2.0.tar.gz
    $ tar zxvf dpdk-2.2.0.tar.gz

since the gcc of 4.4.7 doesn't support `_Static_assert`, try the following ugly
fix for `include/spdk/nvme_spec.h`

    #define ASSERT_CONCAT_(a, b) a##b
    #define ASSERT_CONCAT(a, b) ASSERT_CONCAT_(a, b)
    #ifdef __COUNTER__
        #define _Static_assert(e, m) \
            ;enum { ASSERT_CONCAT(static_assert_, __COUNTER__) = 1/(!!(e)) }
    #else
        #define _Static_assert(e, m) \
            ;enum { ASSERT_CONCAT(assert_line_, __LINE__) = 1/(!!(e)) }
    #endif

**NOTE** that commit `16a45d2` added a similar fallback macro for older version
of gcc.

## Build NVMe driver ##

first build DPDK

    $ make install T=x86_64-native-linuxapp-gcc DESTDIR=.

to make compilation work, edit `/usr/include/linux/virtio_net.h` and fix that
`u16` to `__u16`

    struct virtio_net_ctrl_mq {
        __u16 virtqueue_pairs;
    };

**NOTE** that to link DPDK static libraries to a shared library, libraries
under `$(DPDK_DIR)/lib/*` should be compiled with `-fPIC` (to emit
position-independent codes, suitable for dynamic linking), to do this, add
`EXTRA_CFLAGS="-fPIC"` when invoking `make`, or edit `mk/target/generic/rte.vars.mk`

    CFLAGS += $(TARGET_CFLAGS) -fPIC

then build the NVMe driver

    $ make DPDK_DIR=/path/to/dpdk/x86_64-native-linuxapp-gcc

before running SPDK applications, first umount the device and remove the kernel
nvme module, say

    $ sudo umount /dev/nvme0n1p1
    $ sudo rmmod nvme

and set up HugeTLB

    $ sudo mkdir -p /mnt/hugetlb
    $ sudo mount -t hugetlbfs nodev /mnt/hugetlb
    $ sudo echo 1024 > /proc/sys/vm/nr_hugepages

then run the applications, e.g. the `identity` example

    $ sudo ./identify
    EAL: Detected lcore 0 as core 0 on socket 0
    EAL: Detected lcore 1 as core 1 on socket 0
    EAL: Detected lcore 2 as core 2 on socket 0
    EAL: Detected lcore 3 as core 3 on socket 0
    EAL: Detected lcore 4 as core 4 on socket 0
    EAL: Detected lcore 5 as core 5 on socket 0
    EAL: Detected lcore 6 as core 6 on socket 0
    EAL: Detected lcore 7 as core 7 on socket 0
    EAL: Detected lcore 8 as core 0 on socket 1
    EAL: Detected lcore 9 as core 1 on socket 1
    EAL: Detected lcore 10 as core 2 on socket 1
    EAL: Detected lcore 11 as core 3 on socket 1
    EAL: Detected lcore 12 as core 4 on socket 1
    EAL: Detected lcore 13 as core 5 on socket 1
    EAL: Detected lcore 14 as core 6 on socket 1
    EAL: Detected lcore 15 as core 7 on socket 1
    EAL: Detected lcore 16 as core 0 on socket 0
    EAL: Detected lcore 17 as core 1 on socket 0
    EAL: Detected lcore 18 as core 2 on socket 0
    EAL: Detected lcore 19 as core 3 on socket 0
    EAL: Detected lcore 20 as core 4 on socket 0
    EAL: Detected lcore 21 as core 5 on socket 0
    EAL: Detected lcore 22 as core 6 on socket 0
    EAL: Detected lcore 23 as core 7 on socket 0
    EAL: Detected lcore 24 as core 0 on socket 1
    EAL: Detected lcore 25 as core 1 on socket 1
    EAL: Detected lcore 26 as core 2 on socket 1
    EAL: Detected lcore 27 as core 3 on socket 1
    EAL: Detected lcore 28 as core 4 on socket 1
    EAL: Detected lcore 29 as core 5 on socket 1
    EAL: Detected lcore 30 as core 6 on socket 1
    EAL: Detected lcore 31 as core 7 on socket 1
    EAL: Support maximum 128 logical core(s) by configuration.
    EAL: Detected 32 lcore(s)
    EAL: Setting up physically contiguous memory...
    EAL: Ask a virtual area of 0x200000 bytes
    EAL: Virtual area found at 0x7fc806e00000 (size = 0x200000)
    EAL: Ask a virtual area of 0x200000 bytes
    EAL: Virtual area found at 0x7fc806a00000 (size = 0x200000)
    EAL: Ask a virtual area of 0x3000000 bytes
    EAL: Virtual area found at 0x7fc803800000 (size = 0x3000000)
    EAL: Ask a virtual area of 0x8400000 bytes
    EAL: Virtual area found at 0x7fc7fb200000 (size = 0x8400000)
    EAL: Ask a virtual area of 0x8800000 bytes
    EAL: Virtual area found at 0x7fc7f2800000 (size = 0x8800000)
    EAL: Ask a virtual area of 0x8800000 bytes
    EAL: Virtual area found at 0x7fc7e9e00000 (size = 0x8800000)
    EAL: Ask a virtual area of 0x8800000 bytes
    EAL: Virtual area found at 0x7fc7e1400000 (size = 0x8800000)
    EAL: Ask a virtual area of 0x8800000 bytes
    EAL: Virtual area found at 0x7fc7d8a00000 (size = 0x8800000)
    EAL: Ask a virtual area of 0x8800000 bytes
    EAL: Virtual area found at 0x7fc7d0000000 (size = 0x8800000)
    EAL: Ask a virtual area of 0x8800000 bytes
    EAL: Virtual area found at 0x7fc7c7600000 (size = 0x8800000)
    EAL: Ask a virtual area of 0x400000 bytes
    EAL: Virtual area found at 0x7fc7c7000000 (size = 0x400000)
    EAL: Ask a virtual area of 0x400000 bytes
    EAL: Virtual area found at 0x7fc7c6a00000 (size = 0x400000)
    EAL: Ask a virtual area of 0x200000 bytes
    EAL: Virtual area found at 0x7fc7c6600000 (size = 0x200000)
    EAL: Ask a virtual area of 0x200000 bytes
    EAL: Virtual area found at 0x7fc7c6200000 (size = 0x200000)
    EAL: Ask a virtual area of 0x200000 bytes
    EAL: Virtual area found at 0x7fc7c5e00000 (size = 0x200000)
    EAL: Ask a virtual area of 0x200000 bytes
    EAL: Virtual area found at 0x7fc7c5a00000 (size = 0x200000)
    EAL: Ask a virtual area of 0x200000 bytes
    EAL: Virtual area found at 0x7fc7c5600000 (size = 0x200000)
    EAL: Ask a virtual area of 0x200000 bytes
    EAL: Virtual area found at 0x7fc7c5200000 (size = 0x200000)
    EAL: Ask a virtual area of 0x200000 bytes
    EAL: Virtual area found at 0x7fc7c4e00000 (size = 0x200000)
    EAL: Ask a virtual area of 0x200000 bytes
    EAL: Virtual area found at 0x7fc7c4a00000 (size = 0x200000)
    EAL: Ask a virtual area of 0x200000 bytes
    EAL: Virtual area found at 0x7fc7c4600000 (size = 0x200000)
    EAL: Ask a virtual area of 0x200000 bytes
    EAL: Virtual area found at 0x7fc7c4200000 (size = 0x200000)
    EAL: Ask a virtual area of 0x7800000 bytes
    EAL: Virtual area found at 0x7fc7bc800000 (size = 0x7800000)
    EAL: Ask a virtual area of 0x8800000 bytes
    EAL: Virtual area found at 0x7fc7b3e00000 (size = 0x8800000)
    EAL: Ask a virtual area of 0x8800000 bytes
    EAL: Virtual area found at 0x7fc7ab400000 (size = 0x8800000)
    EAL: Ask a virtual area of 0x8800000 bytes
    EAL: Virtual area found at 0x7fc7a2a00000 (size = 0x8800000)
    EAL: Ask a virtual area of 0x8800000 bytes
    EAL: Virtual area found at 0x7fc79a000000 (size = 0x8800000)
    EAL: Ask a virtual area of 0x8800000 bytes
    EAL: Virtual area found at 0x7fc791600000 (size = 0x8800000)
    EAL: Ask a virtual area of 0x8800000 bytes
    EAL: Virtual area found at 0x7fc788c00000 (size = 0x8800000)
    EAL: Ask a virtual area of 0x3e00000 bytes
    EAL: Virtual area found at 0x7fc784c00000 (size = 0x3e00000)
    EAL: Ask a virtual area of 0x400000 bytes
    EAL: Virtual area found at 0x7fc784600000 (size = 0x400000)
    EAL: Ask a virtual area of 0x400000 bytes
    EAL: Virtual area found at 0x7fc784000000 (size = 0x400000)
    EAL: Ask a virtual area of 0x200000 bytes
    EAL: Virtual area found at 0x7fc783c00000 (size = 0x200000)
    EAL: Ask a virtual area of 0x200000 bytes
    EAL: Virtual area found at 0x7fc783800000 (size = 0x200000)
    EAL: Ask a virtual area of 0x200000 bytes
    EAL: Virtual area found at 0x7fc783400000 (size = 0x200000)
    EAL: Ask a virtual area of 0x200000 bytes
    EAL: Virtual area found at 0x7fc783000000 (size = 0x200000)
    EAL: Ask a virtual area of 0x200000 bytes
    EAL: Virtual area found at 0x7fc782c00000 (size = 0x200000)
    EAL: Ask a virtual area of 0x200000 bytes
    EAL: Virtual area found at 0x7fc782800000 (size = 0x200000)
    EAL: Ask a virtual area of 0x200000 bytes
    EAL: Virtual area found at 0x7fc782400000 (size = 0x200000)
    EAL: Requesting 512 pages of size 2MB from socket 0
    EAL: Requesting 512 pages of size 2MB from socket 1
    EAL: TSC frequency is ~2394458 KHz
    EAL: Master lcore 0 is ready (tid=74538a0;cpuset=[0])
    =====================================================
    NVMe Controller at PCI bus 4, device 0, function 0
    =====================================================
    Controller Capabilities/Features
    ================================
    Vendor ID:                  1c5f
    Subsystem Vendor ID:        1c5f
    Serial Number:
    Model Number:               pblaze4-4T
    Firmware Version:           0090280
    Recommended Arb Burst:      1
    IEEE OUI Identifier:        cf e0 00
    Multi-Interface Cap:        00
    Max Data Transfer Size:     131072
    Error Recovery Timeout:     Unlimited

    Admin Command Set Attributes
    ============================
    Security Send/Receive:       Not Supported
    Format NVM:                  Supported
    Firmware Activate/Download:  Supported
    Abort Command Limit:         4
    Async Event Request Limit:   6
    Number of Firmware Slots:    3
    Firmware Slot 1 Read-Only:   Yes
    Per-Namespace SMART Log:     Yes
    Error Log Page Entries:      63

    NVM Command Set Attributes
    ==========================
    Submission Queue Entry Size
      Max:                       64
      Min:                       64
    Completion Queue Entry Size
      Max:                       16
      Min:                       16
    Number of Namespaces:        1
    Compare Command:             Not Supported
    Write Uncorrectable Command: Not Supported
    Dataset Management Command:  Supported
    Volatile Write Cache:        Not Present

    Arbitration
    ===========
    Arbitration Burst:           1
    Low Priority Weight:         1
    Medium Priority Weight:      1
    High Priority Weight:        1

    Power Management
    ================
    Number of Power States:      1
    Current Power State:         Power State #0
    Power State #0:  Max Power:  25.00 W

    Health Information
    ==================
    Critical Warnings:
      Available Spare Space:     OK
      Temperature:               OK
      Device Reliability:        OK
      Read Only:                 No
      Volatile Memory Backup:    OK
    Current Temperature:         302 Kelvin (29 Celsius)
    Temperature Threshold:       343 Kelvin (70 Celsius)
    Available Spare:             100%
    Life Percentage Used:        0%
    Data Units Read:             1616039
    Data Units Written:          526650
    Host Read Commands:          201573469
    Host Write Commands:         65754805
    Controller Busy Time:        0 minutes
    Power Cycles:                0
    Power On Hours:              1448 hours
    Unsafe Shutdowns:            0
    Unrecoverable Media Errors:  0
    Lifetime Error Log Entries:  0

    Namespace ID:1
    Deallocate:                  Supported
    Flush:                       Not Supported
    Size (in LBAs):              6254624768 (5964M)
    Capacity (in LBAs):          6254624768 (5964M)
    Utilization (in LBAs):       6254624768 (5964M)
    Thin Provisioning:           Not Supported
    Number of LBA Formats:       2
    Current LBA Format:          LBA Format #00
    LBA Format #00: Data Size:   512  Metadata Size:     0
    LBA Format #01: Data Size:  4096  Metadata Size:     0


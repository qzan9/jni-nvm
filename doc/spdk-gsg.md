# SPDK Getting Started on CentOS 6.x #

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

or

    $ sudo rpm -Uvh epel-release-latest-6.noarch.rpm

then

    $ sudo yum install CUnit-devel

install `libaio-devel`

    $ sudo yum install libaio-devel

## Download and prepare the sources ##

download SPDK and DPDK source codes

    $ wget https://github.com/spdk/spdk/archive/master.zip
    $ unzip master.zip
    $ wget http://dpdk.org/browse/dpdk/snapshot/dpdk-2.2.0.tar.gz
    $ tar zxvf dpdk-2.2.0.tar.gz

~~since the gcc of 4.4.7 doesn't support `_Static_assert`, try the following fix for `include/spdk/nvme_spec.h`~~

```c
    #define ASSERT_CONCAT_(a, b) a##b
    #define ASSERT_CONCAT(a, b) ASSERT_CONCAT_(a, b)
    #ifdef __COUNTER__
        #define _Static_assert(e, m) \
            ;enum { ASSERT_CONCAT(static_assert_, __COUNTER__) = 1/(!!(e)) }
    #else
        #define _Static_assert(e, m) \
            ;enum { ASSERT_CONCAT(assert_line_, __LINE__) = 1/(!!(e)) }
    #endif
```

**NOTE** that commit `16a45d2` added a similar fallback macro for older version of gcc in `include/spdk/assert.h`.

~~to make DPDK compilation work, edit `/usr/include/linux/virtio_net.h` and modify the `u16` to `__u16`~~

```c
    struct virtio_net_ctrl_mq {
        __u16 virtqueue_pairs;
    };
```

**NOTE** that this bug has been fixed for newer version of CentOS (6.7) and DPDK (2.2.0).

## Build DPDK and SPDK ##

first build DPDK

    $ cd /path/to/dpdk
    $ make install T=x86_64-native-linuxapp-gcc DESTDIR=.

**NOTE** that to link DPDK static libraries to a shared library, libraries under `$(DPDK_DIR)/lib/*` should be compiled with `-fPIC` (to emit position-independent codes, suitable for dynamic linking), to do this, add `EXTRA_CFLAGS="-fPIC"` when invoking `make`, or edit `mk/target/generic/rte.vars.mk`

    CFLAGS += $(TARGET_CFLAGS) -fPIC

to modify DPDK configuration and re-compile

    $ cd /path/to/dpdk/x86_64-natinve-linuxapp-gcc
    $ vi .config
    $ make

then build SPDK with `$DPDK_DIR` specified

    $ cd /path/to/spdk
    $ make DPDK_DIR=/path/to/dpdk/x86_64-native-linuxapp-gcc

## Prepare for running SPDK/DPDK applications ##

HugeTLBPage needs to be properly configured for SPDK/DPDK. for 2MB pages, just pass the hugepages option to the kernel, e.g., to reserve 2048 pages of 2MB, use:

    hugepages=2048

for CentOS 6.x, this requires you to edit `/etc/grub.conf`.

to use 1G pages, use the following kernel options

    default_hugepagesz=1G hugepagesze=1G hugepages=4

check the CPU flags, if `pdpe1gb` exists, then 1G-hugepages is supported.

for a dual-socket NUMA system, the number of hugepages reserved at boot time is generally divided equally between the two sockets if sufficient memory is present on both sockets.

pages that are used as huge pages are reserved inside the kernel and cannot be used for other purposes.

check `/proc/meminfo` to verify hugepage configuration

    $ grep -i hugepage /proc/meminfo

for 2MB pages, to modify the hugetlbpage configuration after the system has booted, try the following tweak:

    $ sudo echo 2048 > /proc/sys/vm/nr_hugepages

`nr_hugepages` indicates the current number of "persistent" huge pages in the kernel's huge page pool. the success or failure of huge page allocation depends on the amount of physically contiguous memory that is present in system at the time of the allocation attempt, so the dynamic allocation should be done as soon as possible after system boot to prevent memory from being fragmented in physically memory.

DPDK relies on the "hugetlbfs" interface to access the huge pages. hugetlbfs is a RAM-based filesystem, and every file on this filesystem is backed by huge pages and is accessed with `mmap()` or `read()`. hugetlbfs is a bare interface to the huge page capabilities of the underlying hardware.

to make hugetlbfs available for DPDK, perform the following steps:

    $ sudo mkdir -p /mnt/huge
    $ sudo mount -t hugetlbfs nodev /mnt/huge

to make the mount point permanent across reboots, add the following line to `/etc/fstab`

    nodev /mnt/huge hugetlbfs defaults 0 0

for 1G pages, specify the page size

    nodev /mnt/huge_1GB hugetlbfs pagesize=1GB 0 0

~~NVMe devices must be umounted and kernel nvme module be removed, say~~

    $ sudo umount /dev/nvme0n1p1
    $ sudo rmmod nvme

NVMe devices should be bound to `vfio-pci` or `uio_pci_generic`. VFIO driver is an IOMMU/device agnostic framework for exposing direct device access to userspace, in a secure, IOMMU protected environment. by contrast, the UIO framework has no notion of IOMMU protection, limited interrupt support, and requires root privilige to access things like PCI configuration space.

to load `uio_pci_generic`

    $ sudo modprobe uio_pci_generic

DPDK also includes a uio module `igb_uio`, which may be needed for some devices which lack support for legacy interrupts, e.g. virutal function (VF) devices

    $ sudo modprobe uio
    $ sudo insmod kmod/igb_uio.ko

DPDK release 1.7 onward provides VFIO support, and `vfio-pci` module must be loaded

    $ sudo modprobe vfio-pci

Note that in order to use VFIO, your kernel must support it. VFIO kernel modules have been included in the Linux kernel since version 3.6.0. also, to use VFIO, both kernel and BIOS must support and be configured to use I/O virtualization (such as Intel VT-d).

to bind to VFIO/UIO, NVMe devices must be first unbound from NVMe kernel driver, to do so, unload the whole driver

    $ sudo rmmod nvme

or write the device location (in the format of `DDDD:BB:DD.F`) to the `unbind` node of the currently attached driver

    $ echo $bdf > "/sys/bus/pci/devices/$bdf/driver/unbind"

then write the device ID (in the format of `VVVV DDDD SVVV SDDD CCCC MMMM PPPP`) to the `new_id` node of the driver to be attached to

    $ echo $ven_dev_id > "/sys/bus/pci/drivers/$driver_name/new_id"

then trigger a rescan of all PCI buses in the system

    $ echo "1" > "/sys/bus/pci/rescan"

to unregister the device and unbind from the driver

    $ echo $ven_dev_id > "/sys/bus/pci/devices/$bdf/driver/remove_id"
    $ echo $bdf > "/sys/bus/pci/drivers/nvme/bind"

to get the `$bdf` of the NVMe device

    $ lspci -mm -n | grep 0108 | tr -d '"' | awk -F " " '{print "0000:" $1}'

to get the `$ven_dev_id`

    $ lspci -n -s $bdf | cut -d' ' -f3 | sed 's/:/ /'

note that for NVMe devices, the base class code is `01h`, subclass code is `08h`, and the programming interface is `02h` (NVM Express).


# VFIO

CentOS 6.x下，启用`iommu=pt`内核选项再使用vfio框架（通过配置`/sys/bus/pci`）时可能会遇到设备无法连接的问题：

    Initialization timed out in state 2
    controller ready did not become 0 within 127500 ms
    did not shutdown within 5 seconds
    spdk_nvme_probe() failed

`dmesg`有如下报错：

    VFIO - User Level meta-driver version: 0.3
    mgag200 0000:09:03.0: BAR 6: [??? 0x00000000 flags 0x2] has bogus alignment

可通过卸载内核`nvme`驱动的方式实现对设备的用户级直接访问。


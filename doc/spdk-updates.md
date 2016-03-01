标签: SPDK

# SPDK更新整理

主要更新[^1]包括：

* 加入用户级I/OAT驱动（`lib/ioat`）：支持对应Xeon平台的DMA引擎拷贝卸载。

  用户级I/OAT支持的加入，也使得SPDK的概念更加充实，不再限于NVMe。

* 规范（`nvme_spec.h`）

  - 添加NVMe控制器内存缓冲定义。

  - 添加namespace命令枚举定义。

* API（`include/spdk/nvme.h`，`include/spdk/vtophys.h`等）

  - 所有公开API添加`spdk_`前缀。

  - 部分公开API重构，比如连接设备时可通过`spdk_nvme_probe()`而无需显式调用`libpciaccess`的API。

  - 公开及私有API调整。

  个人感觉SPDK的API层次划分还不是很完善，有些API的定位还比较模糊，相信官方还会持续迭代。

* PCI配置空间访问接口（`include/spdk/pci.h`）

  - 提供按字节与按字读写的接口。

  - PCI类型、标识等的定义整理至`include/spdk/pci_ids.h`。

* 配置脚本（`scripts/`）

  - 添加对vfio配置的支持。

  - 原`cleanup.sh`与`unbind.sh`整合为一个`setup.sh`脚本；若需恢复使用内核驱动，使用参数`reset`即可。

  - 驱动绑定/解绑不再通过“粗暴”的`rmmod nvme`，而是通过配置`/sys/bus/pci`树来实现。
    
* 其他

  - DPDK-2.2.0编译适配。

  - 其他代码清理与Bug修正。

[^1]: 自`commit 9322c25`之后的更新。
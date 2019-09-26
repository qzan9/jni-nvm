# JNI NVMe Access #

user-level NVMe SSD access for Java: take full advantage of the potential performance of new NVM technologies.

## HowTo Build ##

* follow `doc/spdk-gsg.md` to prepare SPDK/DPDK build environment.

* specify `DPDK_DIR` and `SPDK_DIR` and `JAVA_HOME` in `mk/common.mk`.

* then just `make` or `mvn package`.


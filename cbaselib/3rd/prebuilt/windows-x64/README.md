# Windows x64 预编译依赖

这些 Release 动态库与头文件来自 vcpkg commit `de36db04550a8625e3154b1a758c60035901f244`，triplet `x64-windows`（MSVC v143、动态 CRT）。构建命令：`vcpkg install openssl:x64-windows libmariadb:x64-windows`。vcpkg 使用上游源码及 port 补丁构建：OpenSSL 3.6.5 port-version 2、MariaDB Connector/C 3.4.11、zlib 1.4。重建时固定该 commit 与 triplet，整体替换匹配的头文件、import lib、DLL 和许可证；换编译器或版本后重新运行构建、真实 TLS 和 MySQL 回归并更新校验值。

来源：[OpenSSL](https://github.com/openssl/openssl/tree/openssl-3.6.5)、[MariaDB Connector/C](https://github.com/mariadb-corporation/mariadb-connector-c/tree/v3.4.11)、[zlib](https://github.com/madler/zlib/tree/v1.4)、[vcpkg ports](https://github.com/microsoft/vcpkg/tree/de36db04550a8625e3154b1a758c60035901f244/ports)。随包许可证分别见 `licenses/openssl-Apache-2.0.txt`、`licenses/mariadb-LGPL-2.1.txt`、`licenses/zlib.txt`；MariaDB 为 LGPL-2.1-or-later。发布者应遵守这些许可证，包括 LGPL 对动态库及源码提供的要求。

`libmariadb.dll` 依赖 `libz.dll`，其余非系统 DLL 依赖为 VC 运行库；其 TLS 使用 Windows 系统库。`libssl-3-x64.dll` 和可选 `legacy.dll` 依赖 `libcrypto-3-x64.dll`；还需 VC 运行库。六个 MariaDB 插件位于 `bin`，其中 `caching_sha2_password.dll` 为动态认证插件。Windows 默认按 DLL 搜索规则加载插件，部署时与程序放同一目录；OpenSSL legacy provider 如需启用须按 OpenSSL 的 provider 配置正确指定路径。

以下为文件 SHA-256：

```text
5eb3233b3d4d46db740299de9c6c39b6e175d3bc29f76205b368ea84e48469cf  libcrypto.lib
d50088d22593b5a9f32edf6e9ea475bd78f072890ae19158bdbc6b6d20723f8f  libmariadb.lib
fc72bc588e2bb34edcbd41c7836f4d8089c668f09eb54eadc19495bc1621a10b  libssl.lib
845e325fd79f2a8c810bbaa781c1749f51ea5117228c5253aa398dcf2048ec42  caching_sha2_password.dll
cb6bc7f503cba1f35f0718fa0e3ece9ecedf5fcb22bdc5f204a5514c9b57a245  client_ed25519.dll
9c738137ef7434f7ce294ae0e9b710bbc8823cb92220efd1308b44f496ab0d3f  dialog.dll
ea40c92f38a41c32b657d19046c73aad4e61e649b5c6523b800b91c5d72a7827  legacy.dll
30ea62b9c443be1925bbd6145744f72f832ff8e2fcff3dcecd545bddcb6791b3  libcrypto-3-x64.dll
33b8c4ce154e267b2d1ef9e636ab15fdfc117a17a393000de3afb53a13e34a3e  libmariadb.dll
71f3540dcfea83e5a389e3df008e8346384ab789ae6dee1061e971e5eac4133d  libssl-3-x64.dll
3ea3fbf36d7c5bb71743770ab4147e27723ea291ad00becec0e0bd23ce671e0c  libz.dll
9495b5331ed626bb2a81d73799f37ad00ac7a83a81582b9f3e71da9d454a24ed  mysql_clear_password.dll
6e729cf2e6c7b88fcb950a6d105a822106d3e3f67b2cda91f5ed5faf51345467  pvio_shmem.dll
e933ed17e25d2e8a55a020de201bc36eed51e750e269680a6c0cd4a77a424648  sha256_password.dll
```

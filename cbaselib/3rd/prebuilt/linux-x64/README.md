# Linux x86-64 预编译依赖

- OpenSSL 3.0.18，来源 https://www.openssl.org/source/openssl-3.0.18.tar.gz，源包 SHA-256：`d80c34f5cf902dccf1f1b5df5ebb86d0392e37049e5d73df1b3abae72e4ffe8b`。Apache License 2.0，见 `licenses/openssl-LICENSE.txt`。
- MariaDB Connector/C 3.4.11，来源 https://github.com/mariadb-corporation/mariadb-connector-c/archive/refs/tags/v3.4.11.tar.gz，源包 SHA-256：`7cdb35c571dd0c187f806f569285829d31b4aba4feb93957a034b0b1e887e276`。LGPL 2.1，见 `licenses/mariadb-COPYING.LIB`；内置 zlib 许可证见 `licenses/zlib-LICENSE`。
- 在 Ubuntu 20.04 x86-64、GCC 10.5.0、glibc 2.31 下构建；动态符号所需最高 glibc 版本分别是 crypto 2.25、ssl 2.14、mariadb 2.17。系统动态链接器及 libc、libpthread、libdl 仍由运行环境提供。
- Connector/C 静态编入 `mysql_native_password`、`caching_sha2_password`、`sha256_password`、socket 和 zlib 插件，不需要单独部署这些插件。`libmariadb.so.3` 依赖 `libssl.so.3` 和 `libcrypto.so.3`；`libssl.so.3` 依赖 `libcrypto.so.3`。两者均有 `$ORIGIN` RUNPATH，三个库应放在同一目录。
- OpenSSL 默认信任目录为 `/usr/lib/ssl`，Ubuntu 上其 `certs` 指向 `/etc/ssl/certs`；依赖目标机器提供有效的系统 CA。默认 provider 内置；其他 provider 未随包提供。

本次打包文件 SHA-256：

```text
741f6d1fcf00670b2fee31571f87a03ccc7835813600f5a2c5f4d3fa296f587b  libcrypto.so.3
d02252c69976bc744edabbc40098ac44470234da5b96bd4104ed5326de33f73e  libmariadb.so.3
2d9b78d5b0b466bd874c4156ab9a617ff0abb8e6ca1affa5acaef42fcc6765e6  libssl.so.3
```

重建方法：在同等基线解压两个源包，选择用户持久目录作为 `PREFIX`，分别执行以下命令。生成目录的 OpenSSL 头文件复制到 `include/openssl`，Connector/C 头文件复制到 `include/mysql`，三个 SONAME 文件复制到 `lib`，然后设置 RUNPATH。构建路径和工具链等会影响二进制哈希。

```sh
cd openssl-3.0.18
./Configure --prefix="$PREFIX/openssl" --openssldir=/usr/lib/ssl shared
make -j4 build_libs
make install_dev
cd ../mariadb-connector-c-3.4.11
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PREFIX/mariadb" -DWITH_SSL=OPENSSL \
  -DOPENSSL_ROOT_DIR="$PREFIX/openssl" \
  -DOPENSSL_INCLUDE_DIR="$PREFIX/openssl/include" \
  -DOPENSSL_SSL_LIBRARY="$PREFIX/openssl/lib64/libssl.so" \
  -DOPENSSL_CRYPTO_LIBRARY="$PREFIX/openssl/lib64/libcrypto.so" \
  -DCLIENT_PLUGIN_CACHING_SHA2_PASSWORD=STATIC \
  -DCLIENT_PLUGIN_SHA256_PASSWORD=STATIC -DWITH_UNIT_TESTS=OFF
cmake --build build -j4
cmake --install build
patchelf --set-rpath '$ORIGIN' lib/libssl.so.3
patchelf --set-rpath '$ORIGIN' lib/libmariadb.so.3
```

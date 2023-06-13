idea from [Scalable NUMA-aware Blocking Synchronization Primitives](https://www.usenix.org/conference/atc17/technical-sessions/presentation/kashyap)

## Preparation

### Meson

本项目使用Meson编译，考虑到服务器Meson版本可能老旧，请根据 [Getting Meson](https://mesonbuild.com/Getting-meson.html) 安装新版本Meson

### Argobots

本项目采用Argobots协程运行时，该组件需要手动编译安装，请从 [argobots项目](https://github.com/pmodels/argobots) 处获取v1.1版本安装，并确保argobots可以被pkg-config找到。

用于测试时推荐安装参数为：

```
./configure --enable-fast=O3 --enable-perf-opt --enable-affinity --disable-checks
```

### 其他

请在系统中安装完整的支持C++ 17的工具链，以及libnuma依赖。

## 编译

```
meson setup build --buildtype=release
meson compile -C build
```

## 运行测试

测试为对单个`unordered_map`进行读写，每个Argobots Execution Stream（ES）创建5个User Level Thread（ULT）

```
./build/simple_bench <mutex_name> <op_num> <read percent> <thread_num (per start_core)> [start_core ...]

<mutex_name>: 要测试的锁名称，提供Mutex、K42Lock、TTASLock、HTTAS、ABT_mutex、pthread_mutex、RWLock、pthread_rwlock、ABT_rwlock

<op_num>: 每个线程要执行的对哈希表的读写操作次数

<read_percent>: 读比例，仅在测试读写锁时有效

<thread_num (per start_core)>: 每个start_core开始的线程数

[start_core ...]: 起始核

例如thread_num: 2，start_core: 0 20 40 60，那么会启动8个线程，分别运行在0-1，20-21，40-41，60-61号CPU上
```

测试结果输出：

```
<mutex_name>,<Mops>,<read_percent>,<thread_num>,<start_core num>
```

锁类型解释：

- `Mutex`: 根据论文实现的MCS+K42队列互斥锁，支持NUMA Aware，简化了阻塞设计，将自旋和阻塞更改为`ABT_self_yield`
- `K42Lock`: K42自旋锁实现，无`yield`
- `TTASLock`: TTAS自旋锁实现，无`yield`
- `HTTAS`: 根据Cohort Lock思想，由两层TTAS构成的互斥锁，支持NUMA Aware，`ABT_self_yield`切换上下文
- `ABT_mutex`: Argobots运行时提供的互斥锁
- `pthread_mutex`: pthread提供的互斥锁，在linux下内部使用futex，`std::mutex`内部使用该锁
- `RWLock`: 可基于任意Cohort Lock实现的读写锁，本实现使用`HTTAS`，NUMA Aware读写锁
- `pthread_rwlock`: pthread提供的读写锁，`std::shared_mutex`内部使用该锁
- `ABT_rwlock`: Argobots运行时提供的读写锁

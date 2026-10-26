# Timer Priority Ventor 版本库

一个定时器库

## 文件解析

- timer.cpp ： 定时器类
- timer_tick.cpp : 定时器 tick 类 同一 tick 的定时器会被一起释放
- timer_queue.cpp : 定时器优先级队列 会用队列自动排列先后
- timer_array.cpp : 定时器数组，用来控制定时器大小
- timer_manager.cpp : 定时器管理类 可以把 timer 添加到定时器中实现定时
- timer_manager_array.cpp : 定时器管理类 使用Array的方式来判断超时
- timer_util.cpp : 定时器工具类，目前用来获取当前时间进行与定时器的时间戳进行比对
- timer_task.cpp : 任务类 作为传入定时器的

## 测试文件

- test_tick : tick 的加入与唤醒测试
- test_queue : queue 的加入与取出测试
- test_manager : 添加定时进行唤醒
- test_manager_thread : 多线程 -- 弃用
- test_manager_thread_2.cpp : 添加定时器进行唤醒
- test_manager_array.cpp : 测试array 版的 manager

## 程序逻辑

初始化定时器管理器
    |
    V
 初始化任务
    |
    V
 初始化定时器
    |
    V
 将定时器加入管理器
 |                                          |
 V                                          V
 存在Tick                               不存在Tick
 |                                          |
 V                                          V
 将定时器加入tick                       创建tick
  |                                          |
  |                                          V
  |                                  将定时器加入 Tick
  |                                          |
  |                                          V
  |                                  将tick加入 queue 与Vector
  |                                    /
  V                                   V  
  定时器检查，超时会取出

## 目前存在的问题

### 2021.9.14

- 不应该使用 Vector + Queue 应该自己写一个优先级 Vector
- 代码有些乱，应该整理一下啊

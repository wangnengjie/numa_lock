#include "cond.hh"
#include "mutex.hh"
#include <abt.h>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <numa.h>
#include <random>
#include <stdexcept>
#include <unordered_map>
#include <vector>

const std::size_t OP_NUM = 100000;

struct global_thread_arg {
  ABT_barrier b1;
  ABT_barrier b2;
  ABT_barrier b3;
  Cond cv1;
  Cond cv2;
  ABT_cond abt_cond;
  Mutex mu;
  ABT_mutex abt_mu;
  std::mutex sys_mu;
  std::condition_variable sys_cv1;
  std::condition_variable sys_cv2;
  int64_t value = 0;
  std::unordered_map<uint32_t, uint32_t> map;
};

auto get_us() -> int64_t {
  timeval tv;
  gettimeofday(&tv, nullptr);
  return tv.tv_usec + tv.tv_sec * 1000000L;
}

auto gen_kv() -> std::vector<std::pair<uint32_t, uint32_t>> {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<uint64_t> dis(0, OP_NUM);
  std::vector<std::pair<uint32_t, uint32_t>> kv;
  kv.reserve(OP_NUM);
  for (size_t i = 0; i < OP_NUM; i++) {
    kv.emplace_back(dis(gen), dis(gen));
  }
  return kv;
}

auto bench(void *_arg) -> void {
  auto arg = (global_thread_arg *)_arg;
  uint cpu_id, numa_id;
  int ret = getcpu(&cpu_id, &numa_id);
  if (ret == 0) {
    arg->mu.lock(numa_id);
    std::cout << "cpu_id: " << cpu_id << " numa_id: " << numa_id << std::endl;
    arg->mu.unlock();
  }
  auto kvs = gen_kv();
  ABT_barrier_wait(arg->b1);
  ABT_barrier_wait(arg->b2);
  for (auto &kv : kvs) {
    // arg->sys_mu.lock();
    arg->mu.lock();
    // ABT_mutex_lock(arg->abt_mu);
    arg->map[kv.first] = kv.second;
    // arg->value += kv.first + kv.second;
    // ABT_mutex_unlock(arg->abt_mu);
    arg->mu.unlock();
    // arg->sys_mu.unlock();
  }
  ABT_barrier_wait(arg->b3);
}

auto check_mutex(void *_arg) -> void {
  auto arg = (global_thread_arg *)_arg;
  uint cpu_id, numa_id;
  int ret = getcpu(&cpu_id, &numa_id);
  if (ret == 0) {
    arg->mu.lock(numa_id);
    std::cout << "cpu_id: " << cpu_id << " numa_id: " << numa_id << std::endl;
    arg->mu.unlock();
  }
  ABT_barrier_wait(arg->b1);
  ABT_barrier_wait(arg->b2);
  for (int64_t i = 0; (uint64_t)i < OP_NUM; i++) {
    arg->mu.lock(numa_id);
    // std::unique_lock<std::mutex> l(arg->sys_mu);
    //! we have two numa node
    if (numa_id & 1) {
      while (arg->value > 0) {
        arg->cv1.wait(&arg->mu, numa_id);
        // arg->sys_cv1.wait(l);
      }
      arg->value += i;
      arg->cv2.signal_one(numa_id);
      // arg->sys_cv2.notify_one();
    } else {
      while (arg->value < 0) {
        arg->cv2.wait(&arg->mu, numa_id);
        // arg->sys_cv2.wait(l);
      }
      arg->value -= i;
      arg->cv1.signal_one(numa_id);
      // arg->sys_cv1.notify_one();
    }
    arg->mu.unlock();
  }
  ABT_barrier_wait(arg->b3);
}

auto main(int argc, char **argv) -> int {
  // OS thread num
  int num_xstreams = 40;
  // ULT num
  int num_threads = num_xstreams;
  auto *xstreams = (ABT_xstream *)malloc(sizeof(ABT_xstream) * num_xstreams);
  auto *pools = (ABT_pool *)malloc(sizeof(ABT_pool) * num_xstreams);
  auto *threads = (ABT_thread *)malloc(sizeof(ABT_thread) * num_threads);

  ABT_init(argc, argv);
  ABT_xstream_self(&xstreams[0]);

  for (int i = 1; i < num_xstreams; i++) {
    ABT_xstream_create(ABT_SCHED_NULL, &xstreams[i]);
  }

  auto max_id = numa_max_node();
  auto num_cpus = numa_num_configured_cpus();
  assert(num_cpus >= num_xstreams);
  auto cpu_mask = numa_bitmask_alloc(num_cpus);
  for (int i = 0; i <= max_id; i++) {
    numa_bitmask_clearall(cpu_mask);
    numa_node_to_cpus(i, cpu_mask);
    int id = 0;
    for (int j = i; j < num_xstreams; j += (max_id + 1)) {
      for (; id < num_cpus; id++) {
        if (numa_bitmask_isbitset(cpu_mask, id)) {
          break;
        }
      }
      int ret = ABT_xstream_set_cpubind(xstreams[j], id);
      assert(ret == ABT_SUCCESS);
      id++;
    }
  }
  numa_bitmask_free(cpu_mask);

  for (int i = 0; i < num_xstreams; i++) {
    ABT_xstream_get_main_pools(xstreams[i], 1, &pools[i]);
  }

  global_thread_arg thread_arg;
  ABT_mutex_create(&thread_arg.abt_mu);
  ABT_cond_create(&thread_arg.abt_cond);
  ABT_barrier_create(num_threads + 1, &thread_arg.b1);
  ABT_barrier_create(num_threads + 1, &thread_arg.b2);
  ABT_barrier_create(num_threads + 1, &thread_arg.b3);
  thread_arg.map.reserve(OP_NUM);

  /* Create ULTs. */
  for (int i = 0; i < num_threads; i++) {
    ABT_thread_create(pools[i], bench, &thread_arg, ABT_THREAD_ATTR_NULL,
                      &threads[i]);
  }

  // wait kv gen
  ABT_barrier_wait(thread_arg.b1);
  uint64_t start_us = get_us();
  ABT_barrier_wait(thread_arg.b2);
  // run bench
  ABT_barrier_wait(thread_arg.b3);
  uint64_t end_us = get_us();

  /* Join and free ULTs. */
  for (int i = 0; i < num_threads; i++) {
    ABT_thread_free(&threads[i]);
  }

  /* Join and free secondary execution streams. */
  for (int i = 1; i < num_xstreams; i++) {
    ABT_xstream_join(xstreams[i]);
    ABT_xstream_free(&xstreams[i]);
  }

  /* Finalize Argobots. */
  ABT_finalize();
  free(xstreams);
  free(pools);
  free(threads);

  ABT_barrier_free(&thread_arg.b1);
  ABT_barrier_free(&thread_arg.b2);
  ABT_barrier_free(&thread_arg.b3);
  ABT_mutex_free(&thread_arg.abt_mu);
  ABT_cond_free(&thread_arg.abt_cond);

  std::cout << thread_arg.value << std::endl;
  uint64_t total_op = num_threads * OP_NUM;
  double sec = (end_us - start_us) / 1000000.0;
  std::cout << total_op / sec / 1000000 << "Mops" << std::endl;
  return 0;
}

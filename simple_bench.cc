#include "mutex.hh"
#include "spinlock.hh"
#include <abt.h>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

const double read_p = 0.99;

struct Context {
  uint64_t op_num;
  uint32_t thread_num;
  std::vector<uint32_t> start_cores;
  ABT_barrier b1;
  ABT_barrier b2;
  ABT_barrier b3;
  Mutex mu;
  Spinlock spin;
  ABT_mutex abt_mu;
  std::mutex sys_mu;
  std::shared_mutex sys_rwlock;
  std::unordered_map<uint32_t, uint32_t> map;
  uint64_t val;
  std::string target_mutex;
};

Context gctx;

auto get_us() -> int64_t {
  timeval tv;
  gettimeofday(&tv, nullptr);
  return tv.tv_usec + tv.tv_sec * 1000000L;
}

struct Request {
  uint64_t key;
  uint64_t value;
  bool write;
};

auto gen_kv(uint64_t op_num) -> std::vector<Request> {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<uint64_t> dis(0, op_num);
  std::uniform_int_distribution<uint32_t> type_dis;
  std::vector<Request> reqs;
  reqs.reserve(op_num);
  for (size_t i = 0; i < op_num; i++) {
    reqs.push_back(Request{
        dis(gen), dis(gen),
        type_dis(gen) > (std::numeric_limits<uint32_t>::max() * read_p)});
  }
  return reqs;
}

auto bench(void *) -> void {
  uint cpu_id, numa_id;
  getcpu(&cpu_id, &numa_id);
  // if (ret == 0) {
  //   gctx.sys_mu.lock();
  //   std::cout << "cpu_id: " << cpu_id << " numa_id: " << numa_id <<
  //   std::endl; gctx.sys_mu.unlock();
  // }
  auto reqs = gen_kv(gctx.op_num);
  ABT_barrier_wait(gctx.b1);
  ABT_barrier_wait(gctx.b2);
  if (gctx.target_mutex == "Mutex") {
    for (auto &req : reqs) {
      gctx.mu.lock(numa_id);
      gctx.map[req.key] = req.value;
      // gctx.value += kv.first + kv.second;
      gctx.mu.unlock();
    }
  } else if (gctx.target_mutex == "Spinlock") {
    for (auto &req : reqs) {
      gctx.spin.lock();
      gctx.map[req.key] = req.value;
      // gctx.val += req.key + req.value;
      gctx.spin.unlock();
    }
  } else if (gctx.target_mutex == "ABT_mutex") {
    for (auto &req : reqs) {
      ABT_mutex_lock(gctx.abt_mu);
      gctx.map[req.key] = req.value;
      // gctx.value += kv.first + kv.second;
      ABT_mutex_unlock(gctx.abt_mu);
    }
  } else if (gctx.target_mutex == "std::mutex") {
    for (auto &req : reqs) {
      gctx.sys_mu.lock();
      gctx.map[req.key] = req.value;
      // gctx.value += kv.first + kv.second;
      gctx.sys_mu.unlock();
    }
  } else if (gctx.target_mutex == "std::shared_mutex") {
    for (auto &req : reqs) {
      if (req.write) {
        gctx.sys_rwlock.lock();
        gctx.map[req.key] = req.value;
        // gctx.value += kv.first + kv.second;
        gctx.sys_rwlock.unlock();
      } else {
        gctx.sys_rwlock.lock_shared();
        { gctx.map.find(req.key); }
        // gctx.value += kv.first + kv.second;
        gctx.sys_rwlock.unlock_shared();
      }
    }
  }

  ABT_barrier_wait(gctx.b3);
}

auto main(int argc, char **argv) -> int {
  if (argc < 2) {
    std::cout
        << "simple_bench <mutex_name> <op_num> <thread_num (per start_core)> "
           "<start_core> [start_core ...]"
        << std::endl;
  }
  gctx.target_mutex = argv[1];
  gctx.op_num = std::stoull(argv[2]);
  gctx.thread_num = std::stoul(argv[3]);
  for (int i = 4; i < argc; i++) {
    gctx.start_cores.push_back(std::stoul(argv[i]));
  }

  int num_xstreams = gctx.thread_num * gctx.start_cores.size();

  auto *xstreams = (ABT_xstream *)malloc(sizeof(ABT_xstream) * num_xstreams);
  auto *pools = (ABT_pool *)malloc(sizeof(ABT_pool) * num_xstreams);
  auto *threads = (ABT_thread *)malloc(sizeof(ABT_thread) * num_xstreams);

  ABT_init(argc, argv);
  ABT_xstream_self(&xstreams[0]);
  for (int i = 1; i < num_xstreams; i++) {
    ABT_xstream_create(ABT_SCHED_NULL, &xstreams[i]);
  }

  for (uint32_t i = 0; i < gctx.start_cores.size(); i++) {
    for (uint32_t j = 0; j < gctx.thread_num; j++) {
      ABT_xstream_set_cpubind(xstreams[i * gctx.thread_num + j],
                              gctx.start_cores[i] + j);
    }
  }

  for (int i = 0; i < num_xstreams; i++) {
    ABT_xstream_get_main_pools(xstreams[i], 1, &pools[i]);
  }

  ABT_mutex_create(&gctx.abt_mu);
  ABT_barrier_create(num_xstreams + 1, &gctx.b1);
  ABT_barrier_create(num_xstreams + 1, &gctx.b2);
  ABT_barrier_create(num_xstreams + 1, &gctx.b3);
  gctx.map.reserve(gctx.op_num);

  /* Create ULTs. */
  for (int i = 0; i < num_xstreams; i++) {
    ABT_thread_create(pools[i], bench, nullptr, ABT_THREAD_ATTR_NULL,
                      &threads[i]);
  }

  uint64_t total_op = num_xstreams * gctx.op_num;
  uint64_t start_us, end_us;
  double sec;
  // wait kv gen
  ABT_barrier_wait(gctx.b1);
  start_us = get_us();
  ABT_barrier_wait(gctx.b2);
  // run bench Mutex
  ABT_barrier_wait(gctx.b3);
  end_us = get_us();

  sec = (end_us - start_us) / 1000000.0;
  std::cout << gctx.target_mutex << " performance: " << total_op / sec / 1000000
            << "Mops" << std::endl;

  /* Join and free ULTs. */
  for (int i = 0; i < num_xstreams; i++) {
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

  ABT_barrier_free(&gctx.b1);
  ABT_barrier_free(&gctx.b2);
  ABT_barrier_free(&gctx.b3);
  ABT_mutex_free(&gctx.abt_mu);
  return 0;
}

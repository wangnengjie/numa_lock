#include "mutex.hh"
#include "mutex_exp.hh"
#include "rwlock.hh"
#include "spinlock.hh"
#include <abt.h>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <mutex>
#include <pthread.h>
#include <random>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

const size_t ULT_PER_STREAM = 5;

struct Context {
  uint64_t op_num;
  double read_p;
  uint32_t thread_num;
  std::vector<uint32_t> start_cores;
  ABT_barrier b1;
  ABT_barrier b2;
  ABT_barrier b3;
  Mutex mu;
  experimental::Mutex mu_exp;
  HTicketLock htspin;
  K42Lock k42;
  TTASLock ttas;
  ABT_mutex abt_mu;
  pthread_mutex_t sys_mu = PTHREAD_MUTEX_INITIALIZER;
  RWLock rwlock;
  ABT_rwlock abt_rwlock;
  pthread_rwlock_t sys_rwlock = PTHREAD_RWLOCK_INITIALIZER;
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
        type_dis(gen) > (std::numeric_limits<uint32_t>::max() * gctx.read_p)});
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
  auto reqs = gen_kv(gctx.op_num / ULT_PER_STREAM);
  ABT_barrier_wait(gctx.b1);
  ABT_barrier_wait(gctx.b2);
  if (gctx.target_mutex == "Mutex") {
    for (auto &req : reqs) {
      gctx.mu.lock(numa_id);
      gctx.map[req.key] = req.value;
      // gctx.value += kv.first + kv.second;
      gctx.mu.unlock();
    }
  } else if (gctx.target_mutex == "Mutex_EXP") {
    auto my = new experimental::Mutex::QNode();
    for (auto &req : reqs) {
      gctx.mu_exp.lock(my, numa_id);
      gctx.map[req.key] = req.value;
      // gctx.val += req.key + req.value;
      gctx.mu_exp.unlock(&my);
    }
    delete my;
  } else if (gctx.target_mutex == "K42Lock") {
    for (auto &req : reqs) {
      gctx.k42.lock();
      gctx.map[req.key] = req.value;
      // gctx.val += req.key + req.value;
      gctx.k42.unlock();
    }
  } else if (gctx.target_mutex == "HTicketLock") {
    for (auto &req : reqs) {
      gctx.htspin.lock(numa_id);
      gctx.map[req.key] = req.value;
      // gctx.val += req.key + req.value;
      gctx.htspin.unlock();
    }
  } else if (gctx.target_mutex == "TTASLock") {
    for (auto &req : reqs) {
      gctx.ttas.lock();
      gctx.map[req.key] = req.value;
      // gctx.val += req.key + req.value;
      gctx.ttas.unlock();
    }
  } else if (gctx.target_mutex == "ABT_mutex") {
    for (auto &req : reqs) {
      ABT_mutex_lock(gctx.abt_mu);
      gctx.map[req.key] = req.value;
      // gctx.value += kv.first + kv.second;
      ABT_mutex_unlock(gctx.abt_mu);
    }
  } else if (gctx.target_mutex == "pthread_mutex") {
    for (auto &req : reqs) {
      // gctx.sys_mu.lock();
      pthread_mutex_lock(&gctx.sys_mu);
      gctx.map[req.key] = req.value;
      // gctx.value += kv.first + kv.second;
      pthread_mutex_unlock(&gctx.sys_mu);
    }
  } else if (gctx.target_mutex == "RWLock") {
    int rc = 0; // prevent compiler optimize read request
    for (auto &req : reqs) {
      if (req.write) {
        gctx.rwlock.wrlock();
        gctx.map[req.key] = req.value + rc;
        // gctx.value += kv.first + kv.second;
        gctx.rwlock.wrunlock();
      } else {
        gctx.rwlock.rdlock();
        if (gctx.map.count(req.key)) {
          rc++; // prevent compiler optimize
        }
        // gctx.value += kv.first + kv.second;
        gctx.rwlock.rdunlock();
      }
    }
  } else if (gctx.target_mutex == "pthread_rwlock") {
    int rc = 0; // prevent compiler optimize read request
    for (auto &req : reqs) {
      if (req.write) {
        pthread_rwlock_wrlock(&gctx.sys_rwlock);
        gctx.map[req.key] = req.value + rc; // use rc
        // gctx.value += kv.first + kv.second;
        pthread_rwlock_unlock(&gctx.sys_rwlock);
      } else {
        pthread_rwlock_rdlock(&gctx.sys_rwlock);
        if (gctx.map.count(req.key)) {
          rc++; // prevent compiler optimize
        }
        // gctx.value += kv.first + kv.second;
        pthread_rwlock_unlock(&gctx.sys_rwlock);
      }
    }
  } else if (gctx.target_mutex == "ABT_rwlock") {
    int rc = 0; // prevent compiler optimize read request
    for (auto &req : reqs) {
      if (req.write) {
        ABT_rwlock_wrlock(gctx.abt_rwlock);
        gctx.map[req.key] = req.value + rc; // use rc
        // gctx.value += kv.first + kv.second;
        ABT_rwlock_unlock(gctx.abt_rwlock);
      } else {
        ABT_rwlock_rdlock(gctx.abt_rwlock);
        if (gctx.map.count(req.key)) {
          rc++; // prevent compiler optimize
        }
        // gctx.value += kv.first + kv.second;
        ABT_rwlock_unlock(gctx.abt_rwlock);
      }
    }
  }

  ABT_barrier_wait(gctx.b3);
}

auto main(int argc, char **argv) -> int {
  if (argc < 2) {
    std::cout << "simple_bench <mutex_name> <op_num> <read percent> "
                 "<thread_num (per start_core)> "
                 "<start_core> [start_core ...]"
              << std::endl;
  }
  gctx.target_mutex = argv[1];
  gctx.op_num = std::stoull(argv[2]);
  gctx.read_p = std::stod(argv[3]);
  gctx.thread_num = std::stoul(argv[4]);
  for (int i = 5; i < argc; i++) {
    gctx.start_cores.push_back(std::stoul(argv[i]));
  }

  int num_xstreams = gctx.thread_num * gctx.start_cores.size();
  int num_ults = num_xstreams * ULT_PER_STREAM;

  auto *xstreams = (ABT_xstream *)malloc(sizeof(ABT_xstream) * num_xstreams);
  auto *pools = (ABT_pool *)malloc(sizeof(ABT_pool) * num_xstreams);
  auto *threads = (ABT_thread *)malloc(sizeof(ABT_thread) * num_ults);

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
  ABT_rwlock_create(&gctx.abt_rwlock);
  ABT_barrier_create(num_ults + 1, &gctx.b1);
  ABT_barrier_create(num_ults + 1, &gctx.b2);
  ABT_barrier_create(num_ults + 1, &gctx.b3);
  gctx.map.reserve(gctx.op_num * 2);

  /* Create ULTs. */
  for (int i = 0; i < num_xstreams; i++) {
    for (uint j = 0; j < ULT_PER_STREAM; j++) {
      ABT_thread_create(pools[i], bench, nullptr, ABT_THREAD_ATTR_NULL,
                        &threads[i * ULT_PER_STREAM + j]);
    }
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
  for (uint i = 0; i < num_xstreams * ULT_PER_STREAM; i++) {
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
  ABT_rwlock_free(&gctx.abt_rwlock);
  return 0;
}

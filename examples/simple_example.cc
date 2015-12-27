// Copyright (c) 2013, Facebook, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.
#include <cstdio>
#include <string>

#include "rocksdb/db.h"
#include "rocksdb/slice.h"
#include "rocksdb/options.h"
#include "util/coding.h"
#include "ssmem.h"
#include "utils.h"
#include "atomic_ops.h"
#include "rapl_read.h"
#include "latency.h"
#include "main_test_loop.h"
#include "common.h"
#include <getopt.h>
// #include "db/table_cache.h"
// #include "leveldb/stats.h"

extern __thread ssmem_allocator_t* alloc;
extern __thread unsigned long * seeds;
extern unsigned int levelmax;
// extern uint64_t unused_key_bits;

#ifndef ASCY_MEMTABLE
unsigned int levelmax;
unsigned int log_base = 2;
unsigned int size_pad_32;
__thread ssmem_allocator_t* alloc;
__thread unsigned long * seeds;
#endif


using namespace rocksdb;

std::string kDBPath = "/storage/rocksdb_simple_example";

// int main() {
//   DB* db;
//   Options options;
//   // Optimize RocksDB. This is the easiest way to get RocksDB to perform well
//   options.IncreaseParallelism();
//   // options.OptimizeLevelStyleCompaction();
//   // create the DB if it's not already present
//   options.create_if_missing = true;

//   // open DB
//   Status s = DB::Open(options, kDBPath, &db);
//   assert(s.ok());

//   // Put key-value
//   s = db->Put(WriteOptions(), "key1", "value");
//   assert(s.ok());
//   std::string value;
//   // get value
//   s = db->Get(ReadOptions(), "key1", &value);
//   assert(s.ok());
//   assert(value == "value");

//   // atomically apply a set of updates
//   {
//     WriteBatch batch;
//     batch.Delete("key1");
//     batch.Put("key2", value);
//     s = db->Write(WriteOptions(), &batch);
//   }

//   s = db->Get(ReadOptions(), "key1", &value);
//   assert(s.IsNotFound());

//   db->Get(ReadOptions(), "key2", &value);
//   assert(value == "value");

//   delete db;

//   return 0;
// }

// Igor's Multi-threaded test:
// namespace {
RETRY_STATS_VARS_GLOBAL;

size_t kNumThreads = 4;
size_t kTestSeconds = 10;
size_t kInitial = 1024;
size_t kRange = 2048;
size_t kUpdatePercent = 0;
size_t kBigValueSize = 256;
size_t kPostInitWait = 90;

size_t print_vals_num = 100;
size_t pf_vals_num = 1023;
size_t put, put_explicit = false;
double update_rate, put_rate, get_rate;

size_t size_after = 0;
int seed = 0;
uint64_t rand_max;
#define rand_min 1

static volatile int stop;
TEST_VARS_GLOBAL;

volatile ticks *putting_succ;
volatile ticks *putting_fail;
volatile ticks *getting_succ;
volatile ticks *getting_fail;
volatile ticks *removing_succ;
volatile ticks *removing_fail;
volatile ticks *putting_count;
volatile ticks *putting_count_succ;
volatile ticks *getting_count;
volatile ticks *getting_count_succ;
volatile ticks *removing_count;
volatile ticks *removing_count_succ;
volatile ticks *total;

#ifdef DEBUG
extern __thread uint32_t put_num_restarts;
extern __thread uint32_t put_num_failed_expand;
extern __thread uint32_t put_num_failed_on_new;
#endif

barrier_t barrier, barrier_global;

// struct IgorMTState {
//   DBTest* test;
//   port::AtomicPointer stop;
//   port::AtomicPointer counter[kNumThreads];
//   port::AtomicPointer thread_done[kNumThreads];
// };
inline Slice* BigSlice(size_t size) {
  char* buf = new char[size];
  return new Slice(buf, size);
}

struct IgorMTThread {
  // MTState* state;
  uint32_t id;
  DB* db;
};

void* IgorMTThreadBody(void* arg) {

  IgorMTThread* t = reinterpret_cast<IgorMTThread*>(arg);
  uint32_t id = t->id;
  DB* db = t->db;
  int phys_id = the_cores[id];
  set_cpu(phys_id);
  ssalloc_init();

  PF_INIT(3, SSPFD_NUM_ENTRIES, id);

#if defined(COMPUTE_LATENCY)
  volatile ticks my_putting_succ = 0;
  volatile ticks my_putting_fail = 0;
  volatile ticks my_getting_succ = 0;
  volatile ticks my_getting_fail = 0;
  volatile ticks my_removing_succ = 0;
  volatile ticks my_removing_fail = 0;
#endif
  uint64_t my_putting_count = 0;
  uint64_t my_getting_count = 0;
  uint64_t my_removing_count = 0;

  uint64_t my_putting_count_succ = 0;
  uint64_t my_getting_count_succ = 0;
  uint64_t my_removing_count_succ = 0;

#if defined(COMPUTE_LATENCY) && PFD_TYPE == 0
  volatile ticks start_acq, end_acq;
  volatile ticks correction = getticks_correction_calc();
#endif

  seeds = seed_rand();
#if GC == 1
  alloc = (ssmem_allocator_t*) malloc(sizeof(ssmem_allocator_t));
  assert(alloc != NULL);
  ssmem_alloc_init_fs_size(alloc, SSMEM_DEFAULT_MEM_SIZE, SSMEM_GC_FREE_SET_SIZE, id);
#endif
  // t->test->dbfull()->ThreadInitDB(id);
  // t->test->dbfull()->membuf_->MemBufferThreadInit(id);
  // t->test->dbfull()->table_cache_->TableCacheInitThread(id);
  // t->test->dbfull()->ThreadInitMembufferIterator();


  RR_INIT(phys_id);
  barrier_cross(&barrier);

  WriteOptions wo = WriteOptions();
  ReadOptions ro = ReadOptions();
  ro.snapshot = NULL;
  uint64_t key, c;
  
  char* buf = new char[8];
  Slice key_slice;

  std::string result;
  Status s;
  uint32_t scale_rem = (uint32_t) (update_rate * UINT_MAX);
  uint32_t scale_put = (uint32_t) (put_rate * UINT_MAX);
  // printf("## Scale put: %zu / Scale rem: %zu\n", scale_put, scale_rem);

  uint32_t num_elems_thread = (uint32_t) (kInitial / kNumThreads);
  int32_t missing = (uint32_t) kInitial - (num_elems_thread * kNumThreads);
  if (id < missing) {
      num_elems_thread++;
  }

#if INITIALIZE_FROM_ONE == 1
  num_elems_thread = (id == 0) * kInitial;
#endif



MEM_BARRIER;
#ifndef NO_INIT_FILL
// #ifdef INIT_SEQ
  size_t j;
  if (id == 0) {

    for (j = kRange - 1; j > kRange/2; j--) {
      if (j == 7*kRange/8) {
        printf("inserting: 25/100 done, j = %zu \n", j);
      } else if (j == 3 * kRange / 4) {
        printf("inserting: 50/100 done\n");
      } else if (j == 5 * kRange / 8) {
        printf("inserting: 75/100 done\n");
      }
      // memcpy(buf, &j, sizeof(j));
      // key_slice.assign(buf, sizeof(j));
      IntToSlice(key_slice, buf, j);
      db->Put(wo, key_slice, *BigSlice(kBigValueSize));

      // IntToSlice(key_slice, buf, kRange - j);
      // db->Put(wo, key_slice, *BigSlice(kBigValueSize));
    }
    // wait for compaction to finish
    struct timespec timeout;
    timeout.tv_sec = kPostInitWait;
    timeout.tv_nsec = 0;
    nanosleep(&timeout, NULL);
  }
// #else // no INIT_SEQ
//   int i;
//   for(i = 0; i < num_elems_thread; i++) {
//     key = (my_random(&(seeds[0]), &(seeds[1]), &(seeds[2])) % (rand_max + 1)) + rand_min;
//     // snprintf(keybuf, sizeof(keybuf), "%d", key);
//     // snprintf(valbuf, sizeof(valbuf), "%d", key);
//     memcpy(buf, &i, sizeof(i)); 
//     key_slice.assign(buf, sizeof(i));
//     s = db->Get(ro, key_slice, &result);
//     if (s.IsNotFound()) {
//       db->Put(wo, key_slice, key_slice);
//     } else {
//       i--;
//     }
//   }
// #endif // INIT_SEQ
#endif // NO_INIT_FILL

  // THREAD_INIT_STATS_VARS

  MEM_BARRIER;

  barrier_cross(&barrier);

  RETRY_STATS_ZERO();

  barrier_cross(&barrier_global);

  RR_START_SIMPLE();

  while (stop == 0) {

    c = (uint32_t)(my_random(&(seeds[0]),&(seeds[1]),&(seeds[2])));
    key = (c & rand_max) + rand_min;    
    
#ifdef SKEW9010
    if (likely(c < (uint32_t) (0.9 * UINT_MAX))) {
      key = (key/10)*10; //make last digit 0 with 90% probability (hot data)
    }  
    c = (uint32_t)(my_random(&(seeds[0]),&(seeds[1]),&(seeds[2]))); 
#endif // SKEW9010

    // memcpy(buf, &key, sizeof(key));
    // key_slice.assign(buf, sizeof(key)); 
    IntToSlice(key_slice, buf, key);   
                  
    if (unlikely(c <= scale_put)) {                 
        START_TS(1);              
        s = db->Put(wo, key_slice, *BigSlice(kBigValueSize));      
        if(s.ok()) {               
          END_TS(1, my_putting_count_succ);       
          ADD_DUR(my_putting_succ);         
          my_putting_count_succ++;          
        }               
      END_TS_ELSE(4, my_putting_count - my_putting_count_succ,    
      my_putting_fail);         
      my_putting_count++;           
    } else if(unlikely(c <= scale_rem)) {
      START_TS(2);              
      s = db->Delete(wo, key_slice);       
      if(s.ok()) {               
        END_TS(2, my_removing_count_succ);        
        ADD_DUR(my_removing_succ);          
        my_removing_count_succ++;         
      }               
      END_TS_ELSE(5, my_removing_count - my_removing_count_succ, my_removing_fail);          
      my_removing_count++;            
    } else {                 
      START_TS(0);              
      s = db->Get(ro, key_slice, &result);      
      if(!s.IsNotFound()) {               
        END_TS(0, my_getting_count_succ);       
        ADD_DUR(my_getting_succ);         
        my_getting_count_succ++;          
      }               
      END_TS_ELSE(3, my_getting_count - my_getting_count_succ,    
      my_getting_fail);         
      my_getting_count++;           
    }
  }

  barrier_cross(&barrier);
  RR_STOP_SIMPLE();

  barrier_cross(&barrier);

#if defined(COMPUTE_LATENCY)
  putting_succ[id] += my_putting_succ;
  putting_fail[id] += my_putting_fail;
  getting_succ[id] += my_getting_succ;
  getting_fail[id] += my_getting_fail;
  removing_succ[id] += my_removing_succ;
  removing_fail[id] += my_removing_fail;
#endif
  putting_count[id] += my_putting_count;
  getting_count[id] += my_getting_count;
  removing_count[id]+= my_removing_count;

  putting_count_succ[id] += my_putting_count_succ;
  getting_count_succ[id] += my_getting_count_succ;
  removing_count_succ[id]+= my_removing_count_succ;

  EXEC_IN_DEC_ID_ORDER(id, kNumThreads)
    {
      print_latency_stats(id, SSPFD_NUM_ENTRIES, print_vals_num);
      RETRY_STATS_SHARE();
    }
  EXEC_IN_DEC_ID_ORDER_END(&barrier);

  SSPFDTERM();
#if GC == 1
  // ssmem_term();
  // free(alloc);
#endif
  // COLLECT_THREAD_STATS  
  pthread_exit(NULL);
}

// }  // namespace

void IgorMultiThreaded() {

  DB* db;
  Options options;
  // Optimize RocksDB. This is the easiest way to get RocksDB to perform well
  options.IncreaseParallelism();
  // options.OptimizeLevelStyleCompaction();
  // create the DB if it's not already present
  options.create_if_missing = true;

  // open DB
  Status s = DB::Open(options, kDBPath, &db);
  assert(s.ok());

  set_cpu(the_cores[0]);

  struct timeval start, end;
  struct timespec timeout;
  timeout.tv_sec = kTestSeconds / 1000;
  timeout.tv_nsec = (kTestSeconds % 1000) * 1000000;

  stop = 0;

  // GLOBAL_INIT_STATS_VARS
  /* Initializes the local data */
  putting_succ = (ticks *) calloc(kNumThreads , sizeof(ticks));
  putting_fail = (ticks *) calloc(kNumThreads , sizeof(ticks));
  getting_succ = (ticks *) calloc(kNumThreads , sizeof(ticks));
  getting_fail = (ticks *) calloc(kNumThreads , sizeof(ticks));
  removing_succ = (ticks *) calloc(kNumThreads , sizeof(ticks));
  removing_fail = (ticks *) calloc(kNumThreads , sizeof(ticks));
  putting_count = (ticks *) calloc(kNumThreads , sizeof(ticks));
  putting_count_succ = (ticks *) calloc(kNumThreads , sizeof(ticks));
  getting_count = (ticks *) calloc(kNumThreads , sizeof(ticks));
  getting_count_succ = (ticks *) calloc(kNumThreads , sizeof(ticks));
  removing_count = (ticks *) calloc(kNumThreads , sizeof(ticks));
  removing_count_succ = (ticks *) calloc(kNumThreads , sizeof(ticks));

  pthread_t threads[kNumThreads];
  pthread_attr_t attr;
  int rc;
  void *status;

  barrier_init(&barrier_global, kNumThreads + 1);
  barrier_init(&barrier, kNumThreads);

  /* Initialize and set thread detached attribute */
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

  IgorMTThread* tds = (IgorMTThread*) malloc(kNumThreads * sizeof(IgorMTThread));

  long t;
  for(t = 0; t < kNumThreads; t++) {
    tds[t].id = t;
    tds[t].db = db;
    rc = pthread_create(&threads[t], &attr, IgorMTThreadBody, tds + t);
    if (rc) {
      printf("ERROR; return code from pthread_create() is %d\n", rc);
      exit(-1);
    }
  }

  /* Free attribute and wait for the other threads */
  pthread_attr_destroy(&attr);

  barrier_cross(&barrier_global);
  gettimeofday(&start, NULL);
  nanosleep(&timeout, NULL);

  stop = 1;
  gettimeofday(&end, NULL);
  kTestSeconds = (end.tv_sec * 1000 + end.tv_usec / 1000) - (start.tv_sec * 1000 + start.tv_usec / 1000);

  for(t = 0; t < kNumThreads; t++) {
    rc = pthread_join(threads[t], &status);
    if (rc) {
      printf("ERROR; return code from pthread_join() is %d\n", rc);
      exit(-1);
    }
  }

  free(tds);

  volatile ticks putting_suc_total = 0;
  volatile ticks putting_fal_total = 0;
  volatile ticks getting_suc_total = 0;
  volatile ticks getting_fal_total = 0;
  volatile ticks removing_suc_total = 0;
  volatile ticks removing_fal_total = 0;
  volatile uint64_t putting_count_total = 0;
  volatile uint64_t putting_count_total_succ = 0;
  volatile uint64_t getting_count_total = 0;
  volatile uint64_t getting_count_total_succ = 0;
  volatile uint64_t removing_count_total = 0;
  volatile uint64_t removing_count_total_succ = 0;

  for(t=0; t < kNumThreads; t++) {
    putting_suc_total += putting_succ[t];
    putting_fal_total += putting_fail[t];
    getting_suc_total += getting_succ[t];
    getting_fal_total += getting_fail[t];
    removing_suc_total += removing_succ[t];
    removing_fal_total += removing_fail[t];
    putting_count_total += putting_count[t];
    putting_count_total_succ += putting_count_succ[t];
    getting_count_total += getting_count[t];
    getting_count_total_succ += getting_count_succ[t];
    removing_count_total += removing_count[t];
    removing_count_total_succ += removing_count_succ[t];
  }

#if defined(COMPUTE_LATENCY)
  printf("#thread srch_suc srch_fal insr_suc insr_fal remv_suc remv_fal   ## latency (in cycles) \n"); fflush(stdout);
  long unsigned get_suc = (getting_count_total_succ) ? getting_suc_total / getting_count_total_succ : 0;
  long unsigned get_fal = (getting_count_total - getting_count_total_succ) ? getting_fal_total / (getting_count_total - getting_count_total_succ) : 0;
  long unsigned put_suc = putting_count_total_succ ? putting_suc_total / putting_count_total_succ : 0;
  long unsigned put_fal = (putting_count_total - putting_count_total_succ) ? putting_fal_total / (putting_count_total - putting_count_total_succ) : 0;
  long unsigned rem_suc = removing_count_total_succ ? removing_suc_total / removing_count_total_succ : 0;
  long unsigned rem_fal = (removing_count_total - removing_count_total_succ) ? removing_fal_total / (removing_count_total - removing_count_total_succ) : 0;
  printf("%-7zu %-8lu %-8lu %-8lu %-8lu %-8lu %-8lu\n", kNumThreads, get_suc, get_fal, put_suc, put_fal, rem_suc, rem_fal);
#endif

#define LLU long long unsigned int

  // int UNUSED pr = (int) (putting_count_total_succ - removing_count_total_succ);
  // if (size_after != (kInitial + pr)) {
  //   printf("// WRONG size. %zu + %d != %zu\n", kInitial, pr, size_after);
  //   assert(size_after == (kInitial + pr));
  // }

  printf("    : %-10s | %-10s | %-11s | %-11s | %s\n", "total", "success", "succ %", "total %", "effective %");
  uint64_t total = putting_count_total + getting_count_total + removing_count_total;
  double putting_perc = 100.0 * (1 - ((double)(total - putting_count_total) / total));
  double putting_perc_succ = (1 - (double) (putting_count_total - putting_count_total_succ) / putting_count_total) * 100;
  double getting_perc = 100.0 * (1 - ((double)(total - getting_count_total) / total));
  double getting_perc_succ = (1 - (double) (getting_count_total - getting_count_total_succ) / getting_count_total) * 100;
  double removing_perc = 100.0 * (1 - ((double)(total - removing_count_total) / total));
  double removing_perc_succ = (1 - (double) (removing_count_total - removing_count_total_succ) / removing_count_total) * 100;
  printf("srch: %-10llu | %-10llu | %10.1f%% | %10.1f%% | \n", (LLU) getting_count_total,
   (LLU) getting_count_total_succ,  getting_perc_succ, getting_perc);
  printf("insr: %-10llu | %-10llu | %10.1f%% | %10.1f%% | %10.1f%%\n", (LLU) putting_count_total,
   (LLU) putting_count_total_succ, putting_perc_succ, putting_perc, (putting_perc * putting_perc_succ) / 100);
  printf("rems: %-10llu | %-10llu | %10.1f%% | %10.1f%% | %10.1f%%\n", (LLU) removing_count_total,
   (LLU) removing_count_total_succ, removing_perc_succ, removing_perc, (removing_perc * removing_perc_succ) / 100);

  double throughput = (putting_count_total + getting_count_total + removing_count_total) * 1000.0 / kTestSeconds;
  printf("#txs %zu\t(%-10.0f\n", kNumThreads, throughput);
  printf("#Mops %.3f\n", throughput / 1e6);
  // PRINT_STATS


  RR_PRINT_UNPROTECTED(RAPL_PRINT_POW);
  RR_PRINT_CORRECTED();
  RETRY_STATS_PRINT(total, putting_count_total, removing_count_total, putting_count_total_succ + removing_count_total_succ);

  delete db;

  // pthread_exit(NULL);
}

int main(int argc, char** argv) {

  // setenv("LEVELDB_TESTS", "IgorMultiThreaded", true);
  // setenv("TEST_TMPDIR", "/storage/leveldbtest/", true);

  // if (argc > 1 && std::string(argv[1]) == "--benchmark") {
  //   BM_LogAndApply(1000, 1);
  //   BM_LogAndApply(1000, 100);
  //   BM_LogAndApply(1000, 10000);
  //   BM_LogAndApply(100, 100000);
  //   return 0;
  // }

  struct option long_options[] = {
    // These options don't set a flag
    {"help",                      no_argument,       NULL, 'h'},
    {"duration",                  required_argument, NULL, 'd'},
    {"initial-size",              required_argument, NULL, 'i'},
    {"num-threads",               required_argument, NULL, 'n'},
    {"range",                     required_argument, NULL, 'r'},
    {"update-rate",               required_argument, NULL, 'u'},
    {"num-buckets",               required_argument, NULL, 'b'},
    {"print-vals",                required_argument, NULL, 'v'},
    {"vals-pf",                   required_argument, NULL, 'f'},
    {NULL, 0, NULL, 0}
  };

  int i, c;
  while(1) {
    i = 0;
    c = getopt_long(argc, argv, "hAf:d:i:n:r:s:u:m:a:l:p:b:v:f:e:w:", long_options, &i);

    if(c == -1)
      break;

    if(c == 0 && long_options[i].flag == 0)
      c = long_options[i].val;

    switch(c) {
      case 0:
        /* Flag is automatically set */
        break;
      case 'h':
        printf("ASCYLIB -- stress test "
         "\n"
         "\n"
         "Usage:\n"
         "  %s [options...]\n"
         "\n"
         "Options:\n"
         "  -h, --help\n"
         "        Print this message\n"
         "  -d, --duration <int>\n"
         "        Test duration in milliseconds\n"
         "  -i, --initial-size <int>\n"
         "        Number of elements to insert before test\n"
         "  -n, --num-threads <int>\n"
         "        Number of threads\n"
         "  -r, --range <int>\n"
         "        Range of integer values inserted in set\n"
         "  -u, --update-rate <int>\n"
         "        Percentage of update transactions\n"
         , argv[0]);
        exit(0);
      case 'd':
        kTestSeconds = atoi(optarg);
        break;
      case 'i':
        kInitial = atol(optarg);
        break;
      case 'n':
        kNumThreads = atoi(optarg);
        break;
      case 'w':
        kPostInitWait = atoi(optarg);
        break;
      case 'r':
        kRange = atol(optarg);
        break;
      case 'u':
        kUpdatePercent = atoi(optarg);
        break;
      case '?':
      default:
        printf("Use -h or --help for help\n");
        exit(1);
    }
  }

  if (!is_power_of_two(kInitial)) {
      size_t initial_pow2 = pow2roundup(kInitial);
      printf("** rounding up initial (to make it power of 2): old: %zu / new: %zu\n", kInitial, initial_pow2);
      kInitial = initial_pow2;
  }

  if (kRange < kInitial) {
      kRange = 2 * kInitial;
  }

  if (!is_power_of_two(kRange)) {
      size_t range_pow2 = pow2roundup(kRange);
      printf("** rounding up range (to make it power of 2): old: %zu / new: %zu\n", kRange, range_pow2);
      kRange = range_pow2;
  }

  update_rate = kUpdatePercent / 100.0;
  put_rate = update_rate / 2.0;
  printf("## Update rate: %f / Put rate: %f\n", update_rate, put_rate);

  printf("## Initial: %zu / Range: %zu\n", kInitial, kRange);
  levelmax = floor_log_2(kInitial);
  printf("Levelmax = %d\n", levelmax);


  // unused_key_bits = 64 - floor_log_2(kRange);
  ssalloc_init();
  seeds = seed_rand();
  rand_max = kRange - 1;


  // // ASSERT_OK(PutNoWAL("Igor", ""));
#if GC == 1
  alloc = (ssmem_allocator_t*) malloc(sizeof(ssmem_allocator_t));
  assert(alloc != NULL);
  ssmem_alloc_init_fs_size(alloc, SSMEM_DEFAULT_MEM_SIZE, SSMEM_GC_FREE_SET_SIZE, 0);
#endif

#if defined(TIGHT_ALLOC)
  int level;
  size_t total_bytes = 0;
  size_t ns;
  for (level = 1; level <= levelmax; ++level) {
    // TODO remove magic number below
    // 32 = sizeof (key + val + 2*uint32 + sl_node_t*)
    ns = 32 + level * sizeof(void *);
    // if (ns % 32 != 0) {
    //   ns = 32 * (ns/32 + 1);
    // }
    // switch (log_base) {
    //   case 2:
        printf("nodes at level %d : %zu\n", level, (kInitial >> level));
        total_bytes += ns * (kInitial >> level);
        // break;
    //   case 4:
    //     printf("nodes at level %d : %d\n", level, 3 * (initial >> (2 * level)));
    //     total_bytes += ns * 3 * (initial >> (2 * level));
    //     break;
    //   case 8:
    //     printf("nodes at level %d : %d\n", level, 7 * (initial >> (3 * level)));
    //     total_bytes += ns * 7 * (initial >> (3 * level));
    //     break;
    // }
  }
  double kb = total_bytes/1024.0; 
  double mb = kb / 1024.0;
  printf("Sizeof initial: %.2f KB = %.2f MB = %.2f GB\n", kb, mb, mb / 1024);
#endif
  
  IgorMultiThreaded();
  return 0;
}
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/userfaultfd.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <vector>

static constexpr uint64_t PAGE = 4096;

static uint8_t*  g_primary;
static uint8_t*  g_alias;
static int64_t   g_delta;
static uint64_t  g_region;
static uint8_t*  g_cards;
static uint8_t*  g_summary;
static std::atomic<uint64_t>* g_dirty_bits;
static uint64_t  g_block = PAGE;
static std::atomic<uint64_t> g_faults {0};

static double Now() {
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return double(ts.tv_sec) * 1e6 + double(ts.tv_nsec) * 1e-3;
}

static void Die(const char* what) {
	fprintf(stderr, "%s: %s\n", what, strerror(errno));
	exit(2);
}

static void OnFault(int, siginfo_t* info, void*) {
	uint64_t off = uint64_t(reinterpret_cast<uint8_t*>(info->si_addr) - g_primary);
	if (off >= g_region) {
		_exit(3);
	}
	uint64_t start = off & ~(g_block - 1);
	mprotect(g_primary + start, g_block, PROT_READ | PROT_WRITE);
	for (uint64_t p = start / PAGE; p < (start + g_block) / PAGE; p++) {
		g_dirty_bits[p >> 6].fetch_or(1ull << (p & 63), std::memory_order_relaxed);
	}
	g_faults.fetch_add(1, std::memory_order_relaxed);
}

enum Mode { NONE, FAULT, UFFD, SELF, MODES };
static const char* MODE_NAMES[MODES] = {"none", "fault", "uffd-scan", "self-mark"};

struct Shared {
	int                  mode;
	int                  threads;
	int                  frames;
	int                  syncs;
	uint64_t             pages_per_thread;
	uint64_t             stores_per_page;
	uint64_t             hot_pages;
	pthread_barrier_t    go;
	pthread_barrier_t    done;
	std::atomic<bool>    stop {false};
	std::vector<double>  writer_us;
};

struct Writer {
	Shared*  sh;
	int      id;
	uint64_t seed;
};

static inline uint64_t Next(uint64_t& s) {
	s = s * 6364136223846793005ull + 1442695040888963407ull;
	return s >> 33;
}

static void* WriterMain(void* arg) {
	Writer*  w  = static_cast<Writer*>(arg);
	Shared*  sh = w->sh;
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(w->id % 16, &set);
	sched_setaffinity(0, sizeof(set), &set);
	const uint64_t base_page = uint64_t(w->id) * sh->hot_pages;
	const uint64_t chunk     = sh->pages_per_thread / uint64_t(sh->syncs);
	uint64_t       s         = w->seed;
	for (;;) {
		pthread_barrier_wait(&sh->go);
		if (sh->stop.load()) {
			break;
		}
		double t0 = Now();
		for (uint64_t i = 0; i < chunk; i++) {
			uint64_t page = base_page + Next(s) % sh->hot_pages;
			uint8_t* p    = g_primary + page * PAGE;
			if (sh->mode == SELF) {
				uint64_t* q = reinterpret_cast<uint64_t*>(p + g_delta);
				for (uint64_t k = 0; k < sh->stores_per_page; k++) {
					q[(k * 7) & 511] = s + k;
					g_cards[page]      = 1;
					g_summary[page >> 9] = 1;
				}
			} else {
				uint64_t* q = reinterpret_cast<uint64_t*>(p);
				for (uint64_t k = 0; k < sh->stores_per_page; k++) {
					q[(k * 7) & 511] = s + k;
				}
			}
		}
		sh->writer_us[size_t(w->id)] += Now() - t0;
		pthread_barrier_wait(&sh->done);
	}
	return nullptr;
}

int main(int argc, char** argv) {
	uint64_t region_mib = 4096;
	int      threads    = 11;
	uint64_t pages      = 11500;
	uint64_t stores     = 40;
	uint64_t hot_mib    = 64;
	int      frames     = 200;
	int      syncs      = 1;
	std::string modes   = "none,fault,uffd-scan,self-mark";
	for (int i = 1; i + 1 < argc; i += 2) {
		std::string k = argv[i];
		const char* v = argv[i + 1];
		if (k == "--region-mib") region_mib = strtoull(v, nullptr, 10);
		else if (k == "--threads") threads = atoi(v);
		else if (k == "--pages") pages = strtoull(v, nullptr, 10);
		else if (k == "--stores") stores = strtoull(v, nullptr, 10);
		else if (k == "--hot-mib") hot_mib = strtoull(v, nullptr, 10);
		else if (k == "--frames") frames = atoi(v);
		else if (k == "--syncs") syncs = atoi(v);
		else if (k == "--block") g_block = strtoull(v, nullptr, 10);
		else if (k == "--modes") modes = v;
		else {
			fprintf(stderr, "unknown %s\n", k.c_str());
			return 2;
		}
	}
	g_region = region_mib << 20;
	int fd   = memfd_create("guest", 0);
	if (fd < 0 || ftruncate(fd, off_t(g_region)) != 0) Die("memfd");
	g_primary = static_cast<uint8_t*>(mmap(nullptr, g_region, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
	g_alias   = static_cast<uint8_t*>(mmap(nullptr, g_region, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
	if (g_primary == MAP_FAILED || g_alias == MAP_FAILED) Die("mmap");
	g_delta           = g_alias - g_primary;
	const uint64_t np = g_region / PAGE;
	g_cards           = static_cast<uint8_t*>(calloc(np, 1));
	g_summary         = static_cast<uint8_t*>(calloc((np >> 9) + 1, 1));
	g_dirty_bits      = new std::atomic<uint64_t>[(np >> 6) + 1]();
	const uint64_t hot_pages = (hot_mib << 20) / PAGE;
	if (uint64_t(threads) * hot_pages > np) {
		fprintf(stderr, "hot set larger than region\n");
		return 2;
	}
	for (uint64_t p = 0; p < uint64_t(threads) * hot_pages; p++) {
		g_primary[p * PAGE] = 1;
		g_alias[p * PAGE]   = 1;
	}
	struct sigaction sa {};
	sa.sa_sigaction = OnFault;
	sa.sa_flags     = SA_SIGINFO | SA_NODEFER;
	sigaction(SIGSEGV, &sa, nullptr);

	int uffd = -1, pagemap = -1;
	printf("region %llu MiB | threads %d | pages/frame %llu (%.1f MiB) | stores/page %llu | hot %llu MiB/thread | syncs/frame %d | fault block %llu | frames %d\n",
	       static_cast<unsigned long long>(region_mib), threads, static_cast<unsigned long long>(pages), double(pages) * 4096 / 1048576.0,
	       static_cast<unsigned long long>(stores), static_cast<unsigned long long>(hot_mib), syncs, static_cast<unsigned long long>(g_block), frames);

	for (int mode = 0; mode < MODES; mode++) {
		if (("," + modes + ",").find(std::string(",") + MODE_NAMES[mode] + ",") == std::string::npos) continue;
		Shared sh;
		sh.mode             = mode;
		sh.threads          = threads;
		sh.frames           = frames;
		sh.syncs            = syncs;
		sh.pages_per_thread = pages / uint64_t(threads);
		sh.stores_per_page  = stores;
		sh.hot_pages        = hot_pages;
		sh.writer_us.assign(size_t(threads), 0.0);
		pthread_barrier_init(&sh.go, nullptr, unsigned(threads) + 1);
		pthread_barrier_init(&sh.done, nullptr, unsigned(threads) + 1);
		std::vector<pthread_t> tids(static_cast<size_t>(threads));
		std::vector<Writer>    ws(static_cast<size_t>(threads));
		for (int t = 0; t < threads; t++) {
			ws[size_t(t)] = Writer {&sh, t, 0x9E3779B97F4A7C15ull * uint64_t(t + 1)};
			pthread_create(&tids[size_t(t)], nullptr, WriterMain, &ws[size_t(t)]);
		}
		const uint64_t hot_bytes = uint64_t(threads) * hot_pages * PAGE;
		if (mode == FAULT) {
			if (mprotect(g_primary, hot_bytes, PROT_READ) != 0) Die("mprotect");
		}
		if (mode == UFFD) {
			uffd = int(syscall(SYS_userfaultfd, O_CLOEXEC | UFFD_USER_MODE_ONLY));
			if (uffd < 0) Die("userfaultfd");
			uffdio_api api {};
			api.api      = UFFD_API;
			api.features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED | UFFD_FEATURE_WP_HUGETLBFS_SHMEM;
			if (ioctl(uffd, UFFDIO_API, &api) != 0) Die("UFFDIO_API");
			uffdio_register reg {};
			reg.range.start = reinterpret_cast<uint64_t>(g_primary);
			reg.range.len   = g_region;
			reg.mode        = UFFDIO_REGISTER_MODE_WP;
			if (ioctl(uffd, UFFDIO_REGISTER, &reg) != 0) Die("UFFDIO_REGISTER");
			uffdio_writeprotect wp {};
			wp.range = reg.range;
			wp.mode  = UFFDIO_WRITEPROTECT_MODE_WP;
			if (ioctl(uffd, UFFDIO_WRITEPROTECT, &wp) != 0) Die("UFFDIO_WRITEPROTECT");
			pagemap = open("/proc/self/pagemap", O_RDONLY);
			if (pagemap < 0) Die("pagemap");
		}
		g_faults = 0;
		double   sync_us = 0.0;
		uint64_t dirty_found = 0;
		std::vector<page_region> regions(65536);
		double   t_start = Now();
		for (int f = 0; f < frames; f++) {
			for (int sy = 0; sy < syncs; sy++) {
				pthread_barrier_wait(&sh.go);
				pthread_barrier_wait(&sh.done);
				double t0 = Now();
				if (mode == FAULT) {
					const uint64_t words = (uint64_t(threads) * hot_pages) >> 6;
					for (uint64_t w = 0; w < words; w++) {
						uint64_t bits = g_dirty_bits[w].exchange(0, std::memory_order_relaxed);
						while (bits) {
							uint64_t first = uint64_t(__builtin_ctzll(bits));
							uint64_t run   = 1;
							while (first + run < 64 && ((bits >> (first + run)) & 1)) run++;
							mprotect(g_primary + ((w << 6) + first) * PAGE, run * PAGE, PROT_READ);
							dirty_found += run;
							bits &= run == 64 ? 0 : ~(((1ull << run) - 1) << first);
						}
					}
				} else if (mode == UFFD) {
					pm_scan_arg arg {};
					arg.size          = sizeof(arg);
					arg.flags         = PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC;
					arg.start         = reinterpret_cast<uint64_t>(g_primary);
					arg.end           = arg.start + g_region;
					arg.vec           = reinterpret_cast<uint64_t>(regions.data());
					arg.vec_len       = regions.size();
					arg.category_mask = PAGE_IS_WRITTEN;
					arg.return_mask   = PAGE_IS_WRITTEN;
					for (;;) {
						long n = ioctl(pagemap, PAGEMAP_SCAN, &arg);
						if (n < 0) Die("PAGEMAP_SCAN");
						for (long i = 0; i < n; i++) dirty_found += (regions[size_t(i)].end - regions[size_t(i)].start) / PAGE;
						if (arg.walk_end >= arg.end) break;
						arg.start = arg.walk_end;
					}
				} else if (mode == SELF) {
					const uint64_t groups = (np >> 9) + 1;
					for (uint64_t g = 0; g < groups; g++) {
						if (!g_summary[g]) continue;
						g_summary[g] = 0;
						uint8_t* c   = g_cards + (g << 9);
						for (uint64_t i = 0; i < 512; i += 8) {
							uint64_t v;
							memcpy(&v, c + i, 8);
							if (v) {
								dirty_found += uint64_t(__builtin_popcountll(v));
								memset(c + i, 0, 8);
							}
						}
					}
				}
				sync_us += Now() - t0;
			}
		}
		double total_us = Now() - t_start;
		sh.stop         = true;
		pthread_barrier_wait(&sh.go);
		for (auto t: tids) pthread_join(t, nullptr);
		if (mode == FAULT) mprotect(g_primary, hot_bytes, PROT_READ | PROT_WRITE);
		if (mode == UFFD) {
			close(uffd);
			close(pagemap);
		}
		double wsum = 0.0, wmax = 0.0;
		for (double v: sh.writer_us) {
			wsum += v;
			wmax = std::max(wmax, v);
		}
		printf("%-10s writers: %8.3f ms/frame summed over threads, %7.3f ms/frame slowest thread | sync: %7.3f ms/frame | wall %8.3f ms/frame | first-touch events/frame %8.1f | dirty pages found/frame %8.1f\n",
		       MODE_NAMES[mode], wsum / frames / 1e3, wmax / frames / 1e3, sync_us / frames / 1e3, total_us / frames / 1e3,
		       double(g_faults.load()) / frames, double(dirty_found) / frames);
		fflush(stdout);
	}
	return 0;
}

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sched.h>
#include <string>
#include <time.h>
#include <vector>

static constexpr uint64_t BASE       = 0x2000000000ull;
static constexpr uint64_t GUEST_SIZE = 64ull << 20;
static constexpr uint64_t MAT_ADDR   = BASE + 0x100000;
static constexpr uint64_t TEXTAB     = BASE + 0x200000;
static constexpr uint64_t SRT_ADDR   = BASE + 0x800000;
static constexpr uint64_t VTAB_ADDR  = BASE + 0xA00000;
static constexpr uint64_t OUT_ADDR   = BASE + 0xC00000;
static constexpr uint64_t CONST_ADDR = BASE + 0x1000000;
static constexpr uint64_t BUF_ADDR   = BASE + 0x2000000;
static constexpr uint32_t NBUF       = 4096;
static constexpr uint32_t HSLOTS     = 4096;
static constexpr uint32_t RSTRIDE    = 512;
static constexpr uint32_t TEXDIM     = 128;
static constexpr uint32_t W          = 1920;
static constexpr uint32_t H          = 1080;
static constexpr uint32_t SEED       = 12345;

#define CHECK(x)                                                                                                       \
	do {                                                                                                               \
		VkResult r_ = (x);                                                                                             \
		if (r_ != VK_SUCCESS) {                                                                                        \
			fprintf(stderr, "%s:%d: %s = %d\n", __FILE__, __LINE__, #x, int(r_));                                      \
			exit(2);                                                                                                   \
		}                                                                                                              \
	} while (0)

struct PC {
	uint64_t pt;
	uint64_t hash;
	uint64_t mirror;
	uint64_t outp;
	uint64_t arg;
	uint32_t a0;
	uint32_t a1;
};

struct Slot {
	uint64_t k0;
	uint64_t k1;
	uint32_t idx;
	uint32_t used;
	uint32_t p0;
	uint32_t p1;
};

struct Buf {
	VkBuffer       buf  = VK_NULL_HANDLE;
	VkDeviceMemory mem  = VK_NULL_HANDLE;
	uint64_t       addr = 0;
	uint8_t*       map  = nullptr;
	uint64_t       size = 0;
	uint32_t       type = 0;
};

static VkInstance       g_inst;
static VkPhysicalDevice g_phys;
static VkDevice         g_dev;
static VkQueue          g_queue;
static uint32_t         g_family;
static VkPhysicalDeviceMemoryProperties g_memprops;
static double           g_ts_period;

static double Now() {
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return double(ts.tv_sec) * 1e6 + double(ts.tv_nsec) * 1e-3;
}

static uint32_t MemType(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer) {
	for (int pass = 0; pass < 2; pass++) {
		for (uint32_t i = 0; i < g_memprops.memoryTypeCount; i++) {
			VkMemoryPropertyFlags f = g_memprops.memoryTypes[i].propertyFlags;
			if ((bits & (1u << i)) && (f & want) == want && (pass == 1 || (f & prefer) == prefer)) {
				return i;
			}
		}
	}
	fprintf(stderr, "no memory type\n");
	exit(2);
}

static Buf MakeBuf(uint64_t size, VkBufferUsageFlags usage, int kind) {
	const bool host = kind != 0;
	Buf                b;
	VkBufferCreateInfo ci {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	ci.size  = size;
	ci.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
	CHECK(vkCreateBuffer(g_dev, &ci, nullptr, &b.buf));
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(g_dev, b.buf, &mr);
	VkMemoryAllocateFlagsInfo fi {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
	fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
	VkMemoryAllocateInfo ai {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fi};
	ai.allocationSize  = mr.size;
	const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	ai.memoryTypeIndex             = kind == 1   ? MemType(mr.memoryTypeBits, hv, VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
	                                 : kind == 2 ? MemType(mr.memoryTypeBits, hv, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
	                                             : MemType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
	b.type = ai.memoryTypeIndex;
	CHECK(vkAllocateMemory(g_dev, &ai, nullptr, &b.mem));
	CHECK(vkBindBufferMemory(g_dev, b.buf, b.mem, 0));
	VkBufferDeviceAddressInfo di {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
	di.buffer = b.buf;
	b.addr    = vkGetBufferDeviceAddress(g_dev, &di);
	b.size    = size;
	if (host) {
		void* p = nullptr;
		CHECK(vkMapMemory(g_dev, b.mem, 0, VK_WHOLE_SIZE, 0, &p));
		b.map = static_cast<uint8_t*>(p);
	}
	return b;
}

static std::vector<uint32_t> LoadSpv(const std::string& path) {
	std::ifstream f(path, std::ios::binary | std::ios::ate);
	if (!f) {
		fprintf(stderr, "missing %s\n", path.c_str());
		exit(2);
	}
	size_t                n = size_t(f.tellg());
	std::vector<uint32_t> v(n / 4);
	f.seekg(0);
	f.read(reinterpret_cast<char*>(v.data()), std::streamsize(n));
	return v;
}

static VkShaderModule Module(const std::string& dir, const char* name) {
	auto                     code = LoadSpv(dir + "/" + name + ".spv");
	VkShaderModuleCreateInfo ci {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	ci.codeSize = code.size() * 4;
	ci.pCode    = code.data();
	VkShaderModule m;
	CHECK(vkCreateShaderModule(g_dev, &ci, nullptr, &m));
	return m;
}

static uint32_t HashKey(uint64_t k0, uint64_t k1) {
	uint32_t h = uint32_t(k0) ^ (uint32_t(k0 >> 32) * 0x9E3779B1u) ^ (uint32_t(k1) * 0x85EBCA6Bu);
	h ^= h >> 15;
	return h;
}

static uint32_t Sel(uint32_t d, uint32_t i, uint32_t seed, uint32_t ntex) {
	return (((d * 2654435761u) + (i * 40503u) + seed) >> 7) % ntex;
}

static uint64_t Rd64(const uint8_t* p) {
	uint64_t v;
	memcpy(&v, p, 8);
	return v;
}

static void Wr64(uint8_t* p, uint64_t v) {
	memcpy(p, &v, 8);
}

static void WrF(uint8_t* p, float v) {
	memcpy(p, &v, 4);
}

struct Rng {
	uint64_t s;
	uint32_t Next() {
		s = s * 6364136223846793005ull + 1442695040888963407ull;
		return uint32_t(s >> 33);
	}
	float Unit() { return float(Next() & 0xFFFFFF) / float(0x1000000); }
};

enum Arm { S_UBO, S_BDA, G1_PT, G1_CT, G2_PT, G2_CT, ARMS };
static const char* ARM_NAMES[ARMS] = {"cpu-ubo", "cpu-bda", "gpu-inv-pt", "gpu-inv-ct", "gpu-pre-pt", "gpu-pre-ct"};

int main(int argc, char** argv) {
	std::string spv       = "out/spv";
	uint32_t    ndraws    = 3800;
	uint32_t    ndisp     = 2600;
	uint32_t    npasses   = 180;
	uint32_t    rounds    = 4;
	uint32_t    block     = 40;
	double      overdraw  = 3.0;
	int         gputables = 0;
	int         cpu       = 2;
	int         ring_mem  = 2;
	std::string want_gpu;
	std::string arms_arg  = "cpu-ubo,cpu-bda,gpu-inv-pt,gpu-inv-ct,gpu-pre-pt,gpu-pre-ct";
	for (int i = 1; i + 1 < argc; i += 2) {
		std::string k = argv[i];
		const char* v = argv[i + 1];
		if (k == "--spv") spv = v;
		else if (k == "--draws") ndraws = uint32_t(atoi(v));
		else if (k == "--dispatches") ndisp = uint32_t(atoi(v));
		else if (k == "--passes") npasses = uint32_t(atoi(v));
		else if (k == "--rounds") rounds = uint32_t(atoi(v));
		else if (k == "--block") block = uint32_t(atoi(v));
		else if (k == "--overdraw") overdraw = atof(v);
		else if (k == "--gpu-tables") gputables = atoi(v);
		else if (k == "--cpu") cpu = atoi(v);
		else if (k == "--ring-mem") ring_mem = std::string(v) == "ram" ? 1 : std::string(v) == "copy" ? 3 : 2;
		else if (k == "--gpu") want_gpu = v;
		else if (k == "--arms") arms_arg = v;
		else {
			fprintf(stderr, "unknown %s\n", k.c_str());
			return 2;
		}
	}
	uint32_t KT = 8, KB = 4, CR = 24, NTEX = 512;
	{
		std::ifstream f(spv + "/config.txt");
		f >> KT >> KB >> CR >> NTEX;
	}
	if (cpu >= 0) {
		cpu_set_t set;
		CPU_ZERO(&set);
		CPU_SET(cpu, &set);
		sched_setaffinity(0, sizeof(set), &set);
	}
	std::vector<int> arms;
	for (int a = 0; a < ARMS; a++) {
		if (("," + arms_arg + ",").find(std::string(",") + ARM_NAMES[a] + ",") != std::string::npos) arms.push_back(a);
	}

	VkApplicationInfo app {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.apiVersion = VK_API_VERSION_1_3;
	VkInstanceCreateInfo ici {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	CHECK(vkCreateInstance(&ici, nullptr, &g_inst));
	uint32_t n = 0;
	vkEnumeratePhysicalDevices(g_inst, &n, nullptr);
	std::vector<VkPhysicalDevice> devs(n);
	vkEnumeratePhysicalDevices(g_inst, &n, devs.data());
	VkPhysicalDeviceProperties props {};
	g_phys = VK_NULL_HANDLE;
	for (auto d: devs) {
		vkGetPhysicalDeviceProperties(d, &props);
		if (want_gpu.empty() || std::string(props.deviceName).find(want_gpu) != std::string::npos) {
			g_phys = d;
			break;
		}
	}
	if (g_phys == VK_NULL_HANDLE) {
		fprintf(stderr, "no device matching '%s'\n", want_gpu.c_str());
		return 2;
	}
	vkGetPhysicalDeviceProperties(g_phys, &props);
	g_ts_period = props.limits.timestampPeriod;
	vkGetPhysicalDeviceMemoryProperties(g_phys, &g_memprops);
	uint32_t qn = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &qn, nullptr);
	std::vector<VkQueueFamilyProperties> qf(qn);
	vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &qn, qf.data());
	g_family = 0;
	for (uint32_t i = 0; i < qn; i++) {
		if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
			g_family = i;
			break;
		}
	}
	float                   prio = 1.0f;
	VkDeviceQueueCreateInfo qci {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = g_family;
	qci.queueCount       = 1;
	qci.pQueuePriorities = &prio;
	VkPhysicalDeviceVulkan13Features f13 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
	f13.dynamicRendering = VK_TRUE;
	VkPhysicalDeviceVulkan12Features f12 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
	f12.bufferDeviceAddress                       = VK_TRUE;
	f12.runtimeDescriptorArray                    = VK_TRUE;
	f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
	f12.scalarBlockLayout                         = VK_TRUE;
	f12.descriptorIndexing                        = VK_TRUE;
	VkPhysicalDeviceFeatures2 f2 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
	f2.features.shaderInt64                             = VK_TRUE;
	f2.features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
	VkDeviceCreateInfo dci {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2};
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos    = &qci;
	CHECK(vkCreateDevice(g_phys, &dci, nullptr, &g_dev));
	vkGetDeviceQueue(g_dev, g_family, 0, &g_queue);

	VkCommandPoolCreateInfo cpi {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	cpi.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	cpi.queueFamilyIndex = g_family;
	VkCommandPool pool;
	CHECK(vkCreateCommandPool(g_dev, &cpi, nullptr, &pool));
	VkCommandBufferAllocateInfo cai {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cai.commandPool        = pool;
	cai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cai.commandBufferCount = 2;
	VkCommandBuffer cbs[2];
	CHECK(vkAllocateCommandBuffers(g_dev, &cai, cbs));
	VkCommandBuffer   cb = cbs[0], cbw = cbs[1];
	VkFenceCreateInfo fci {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkFence           fence;
	CHECK(vkCreateFence(g_dev, &fci, nullptr, &fence));

	auto Submit = [&](VkCommandBuffer c) {
		VkSubmitInfo si {VK_STRUCTURE_TYPE_SUBMIT_INFO};
		si.commandBufferCount = 1;
		si.pCommandBuffers    = &c;
		CHECK(vkQueueSubmit(g_queue, 1, &si, fence));
		CHECK(vkWaitForFences(g_dev, 1, &fence, VK_TRUE, UINT64_MAX));
		CHECK(vkResetFences(g_dev, 1, &fence));
	};
	auto Begin = [&](VkCommandBuffer c) {
		VkCommandBufferBeginInfo bi {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		CHECK(vkBeginCommandBuffer(c, &bi));
	};

	const VkBufferUsageFlags xfer = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	Buf mirror   = MakeBuf(GUEST_SIZE, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | xfer, 0);
	Buf ptbuf    = MakeBuf(GUEST_SIZE / 4096 * 8, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | xfer, 0);
	Buf hashbuf  = MakeBuf(HSLOTS * sizeof(Slot), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | xfer, 0);
	Buf staging  = MakeBuf(GUEST_SIZE, xfer, 1);
	uint32_t nops = ndraws + ndisp;
	Buf ringh    = MakeBuf(uint64_t(nops) * RSTRIDE, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | xfer, ring_mem == 3 ? 1 : ring_mem);
	Buf ringg    = MakeBuf(uint64_t(nops) * RSTRIDE, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | xfer, 0);
	Buf drawlist = MakeBuf(uint64_t(nops) * 8, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, ring_mem == 3 ? 2 : ring_mem);
	Buf rb       = MakeBuf(uint64_t(nops) * KT * 32, xfer, 1);
	Buf imgrb    = MakeBuf(uint64_t(W) * H * 4, xfer, 1);

	std::vector<uint8_t>  guest(GUEST_SIZE, 0);
	std::vector<uint64_t> ptcpu(GUEST_SIZE / 4096);
	for (size_t i = 0; i < ptcpu.size(); i++) ptcpu[i] = mirror.addr + i * 4096;
	std::vector<Slot> hcpu(HSLOTS, Slot {});
	for (uint32_t m = 0; m < NTEX; m++) {
		uint64_t k0 = (BASE + 0x10000000ull + uint64_t(m) * 0x10000ull) | (uint64_t(0x38) << 52);
		uint64_t k1 = 0x0080008000000000ull | uint64_t(m * 2654435761u);
		uint8_t* t  = guest.data() + (MAT_ADDR - BASE) + m * 32;
		Wr64(t, k0);
		Wr64(t + 8, k1);
		Wr64(t + 16, uint64_t(m) * 77);
		Wr64(t + 24, ~uint64_t(m));
		uint32_t h = HashKey(k0, k1);
		for (uint32_t p = 0;; p++) {
			if (p >= 8) {
				fprintf(stderr, "hash table probe overflow\n");
				return 2;
			}
			Slot& s = hcpu[(h + p) & (HSLOTS - 1)];
			if (!s.used) {
				s = Slot {k0, k1, m, 1, 0, 0};
				break;
			}
		}
	}
	Rng rng {42};
	for (uint32_t b = 0; b < NBUF; b++) {
		for (uint32_t k = 0; k < 16; k++) WrF(guest.data() + (BUF_ADDR - BASE) + b * 64 + k * 4, 0.0005f * float((b + k) % 9));
	}
	std::vector<uint64_t> srt_addr(nops);
	std::vector<uint32_t> grid(nops);
	const double          mean_area = overdraw * 4.0 / double(ndraws);
	for (uint32_t d = 0; d < nops; d++) {
		uint64_t srt = SRT_ADDR + uint64_t(d) * 64;
		uint64_t tt  = TEXTAB + uint64_t(d) * KT * 32;
		uint64_t vt  = VTAB_ADDR + uint64_t(d) * KB * 16;
		uint64_t cc  = CONST_ADDR + uint64_t(d) * 256;
		srt_addr[d]  = srt;
		uint8_t* s   = guest.data() + (srt - BASE);
		Wr64(s, tt);
		Wr64(s + 8, vt);
		Wr64(s + 16, cc);
		for (uint32_t i = 0; i < KT; i++) {
			memcpy(guest.data() + (tt - BASE) + i * 32, guest.data() + (MAT_ADDR - BASE) + Sel(d, i, SEED, NTEX) * 32, 32);
		}
		for (uint32_t j = 0; j < KB; j++) {
			uint8_t* v = guest.data() + (vt - BASE) + j * 16;
			Wr64(v, BUF_ADDR + 64ull * ((d * 7 + j * 13) % NBUF));
			uint32_t stride = 16, count = 4;
			memcpy(v + 8, &stride, 4);
			memcpy(v + 12, &count, 4);
		}
		uint8_t* c    = guest.data() + (cc - BASE);
		float    side = float(std::sqrt(mean_area * (0.25 + 1.5 * rng.Unit())));
		side          = std::min(side, 1.9f);
		float x       = -1.0f + rng.Unit() * (2.0f - side);
		float y       = -1.0f + rng.Unit() * (2.0f - side);
		WrF(c, x);
		WrF(c + 4, y);
		WrF(c + 8, side);
		WrF(c + 12, side);
		for (uint32_t k = 4; k < 64; k++) WrF(c + k * 4, k < 16 ? 0.05f + 0.01f * float((d + k) % 7) : 0.0003f * float((d + k) % 11));
		grid[d] = 1u << (rng.Next() % 4);
	}
	if (gputables) {
		memset(guest.data() + (TEXTAB - BASE), 0, size_t(nops) * KT * 32);
	}

	auto Upload = [&](Buf& dst, const void* data, uint64_t size) {
		memcpy(staging.map, data, size);
		Begin(cbw);
		VkBufferCopy r {0, 0, size};
		vkCmdCopyBuffer(cbw, staging.buf, dst.buf, 1, &r);
		CHECK(vkEndCommandBuffer(cbw));
		Submit(cbw);
	};
	Upload(mirror, guest.data(), GUEST_SIZE);
	Upload(ptbuf, ptcpu.data(), ptcpu.size() * 8);
	Upload(hashbuf, hcpu.data(), hcpu.size() * sizeof(Slot));

	std::vector<VkImage>     images(NTEX);
	std::vector<VkImageView> views(NTEX);
	{
		VkImageCreateInfo ii {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ii.imageType   = VK_IMAGE_TYPE_2D;
		ii.format      = VK_FORMAT_R8G8B8A8_UNORM;
		ii.extent      = {TEXDIM, TEXDIM, 1};
		ii.mipLevels   = 1;
		ii.arrayLayers = 1;
		ii.samples     = VK_SAMPLE_COUNT_1_BIT;
		ii.usage       = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		VkMemoryRequirements mr {};
		for (uint32_t m = 0; m < NTEX; m++) {
			CHECK(vkCreateImage(g_dev, &ii, nullptr, &images[m]));
		}
		vkGetImageMemoryRequirements(g_dev, images[0], &mr);
		uint64_t             stride = (mr.size + mr.alignment - 1) / mr.alignment * mr.alignment;
		VkMemoryAllocateInfo ai {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		ai.allocationSize  = stride * NTEX;
		ai.memoryTypeIndex = MemType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
		VkDeviceMemory mem;
		CHECK(vkAllocateMemory(g_dev, &ai, nullptr, &mem));
		for (uint32_t m = 0; m < NTEX; m++) {
			CHECK(vkBindImageMemory(g_dev, images[m], mem, stride * m));
			uint8_t* p = staging.map;
			for (uint32_t y = 0; y < TEXDIM; y++) {
				for (uint32_t x = 0; x < TEXDIM; x++) {
					uint32_t v = (x * 3 + y * 5 + m * 17) & 0xFF;
					p[0]       = uint8_t((m * 37) & 0xFF);
					p[1]       = uint8_t(v);
					p[2]       = uint8_t((m * 91 + x) & 0xFF);
					p[3]       = 255;
					p += 4;
				}
			}
			Begin(cbw);
			VkImageMemoryBarrier ib {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
			ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			ib.image                                        = images[m];
			ib.subresourceRange                             = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			ib.oldLayout                                    = VK_IMAGE_LAYOUT_UNDEFINED;
			ib.newLayout                                    = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			ib.dstAccessMask                                = VK_ACCESS_TRANSFER_WRITE_BIT;
			vkCmdPipelineBarrier(cbw, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
			VkBufferImageCopy bic {};
			bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			bic.imageExtent      = {TEXDIM, TEXDIM, 1};
			vkCmdCopyBufferToImage(cbw, staging.buf, images[m], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
			ib.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			ib.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			vkCmdPipelineBarrier(cbw, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
			CHECK(vkEndCommandBuffer(cbw));
			Submit(cbw);
			VkImageViewCreateInfo vi {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
			vi.image            = images[m];
			vi.viewType         = VK_IMAGE_VIEW_TYPE_2D;
			vi.format           = ii.format;
			vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			CHECK(vkCreateImageView(g_dev, &vi, nullptr, &views[m]));
		}
	}
	VkSamplerCreateInfo sci {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	VkSampler sampler;
	CHECK(vkCreateSampler(g_dev, &sci, nullptr, &sampler));

	VkImage     color;
	VkImageView color_view;
	{
		VkImageCreateInfo ii {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ii.imageType   = VK_IMAGE_TYPE_2D;
		ii.format      = VK_FORMAT_R8G8B8A8_UNORM;
		ii.extent      = {W, H, 1};
		ii.mipLevels   = 1;
		ii.arrayLayers = 1;
		ii.samples     = VK_SAMPLE_COUNT_1_BIT;
		ii.usage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		CHECK(vkCreateImage(g_dev, &ii, nullptr, &color));
		VkMemoryRequirements mr;
		vkGetImageMemoryRequirements(g_dev, color, &mr);
		VkMemoryAllocateInfo ai {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		ai.allocationSize  = mr.size;
		ai.memoryTypeIndex = MemType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
		VkDeviceMemory mem;
		CHECK(vkAllocateMemory(g_dev, &ai, nullptr, &mem));
		CHECK(vkBindImageMemory(g_dev, color, mem, 0));
		VkImageViewCreateInfo vi {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vi.image            = color;
		vi.viewType         = VK_IMAGE_VIEW_TYPE_2D;
		vi.format           = ii.format;
		vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		CHECK(vkCreateImageView(g_dev, &vi, nullptr, &color_view));
	}

	VkDescriptorSetLayoutBinding b0[2] {};
	b0[0] = {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, NTEX, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
	b0[1] = {1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
	VkDescriptorSetLayoutCreateInfo li {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	li.bindingCount = 2;
	li.pBindings    = b0;
	VkDescriptorSetLayout l0, l1;
	CHECK(vkCreateDescriptorSetLayout(g_dev, &li, nullptr, &l0));
	VkDescriptorSetLayoutBinding b1 {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
	li.bindingCount = 1;
	li.pBindings    = &b1;
	CHECK(vkCreateDescriptorSetLayout(g_dev, &li, nullptr, &l1));
	VkDescriptorPoolSize       ps[3] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, NTEX}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2}};
	VkDescriptorPoolCreateInfo dpi {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	dpi.maxSets       = 3;
	dpi.poolSizeCount = 3;
	dpi.pPoolSizes    = ps;
	VkDescriptorPool dpool;
	CHECK(vkCreateDescriptorPool(g_dev, &dpi, nullptr, &dpool));
	VkDescriptorSetLayout       layouts[3] = {l0, l1, l1};
	VkDescriptorSetAllocateInfo dai {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dai.descriptorPool     = dpool;
	dai.descriptorSetCount = 3;
	dai.pSetLayouts        = layouts;
	VkDescriptorSet sets[3];
	CHECK(vkAllocateDescriptorSets(g_dev, &dai, sets));
	{
		std::vector<VkDescriptorImageInfo> ii(NTEX);
		for (uint32_t m = 0; m < NTEX; m++) ii[m] = {VK_NULL_HANDLE, views[m], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		VkDescriptorImageInfo  si {sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
		VkDescriptorBufferInfo bh {ringh.buf, 0, 384};
		VkDescriptorBufferInfo bg {ringg.buf, 0, 384};
		VkWriteDescriptorSet   w[4] {};
		for (auto& x: w) x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w[0].dstSet = sets[0], w[0].dstBinding = 0, w[0].descriptorCount = NTEX, w[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, w[0].pImageInfo = ii.data();
		w[1].dstSet = sets[0], w[1].dstBinding = 1, w[1].descriptorCount = 1, w[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, w[1].pImageInfo = &si;
		w[2].dstSet = sets[1], w[2].dstBinding = 0, w[2].descriptorCount = 1, w[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, w[2].pBufferInfo = &bh;
		w[3].dstSet = sets[2], w[3].dstBinding = 0, w[3].descriptorCount = 1, w[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, w[3].pBufferInfo = &bg;
		vkUpdateDescriptorSets(g_dev, 4, w, 0, nullptr);
	}
	const VkShaderStageFlags   all_stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
	VkPushConstantRange        pcr {all_stages, 0, sizeof(PC)};
	VkDescriptorSetLayout      pl_sets[2] = {l0, l1};
	VkPipelineLayoutCreateInfo pli {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	pli.setLayoutCount         = 2;
	pli.pSetLayouts            = pl_sets;
	pli.pushConstantRangeCount = 1;
	pli.pPushConstantRanges    = &pcr;
	VkPipelineLayout layout;
	CHECK(vkCreatePipelineLayout(g_dev, &pli, nullptr, &layout));

	auto Graphics = [&](const char* vs, const char* fs) {
		VkPipelineShaderStageCreateInfo st[2] {};
		st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		st[0].stage               = VK_SHADER_STAGE_VERTEX_BIT;
		st[0].module              = Module(spv, vs);
		st[0].pName               = "main";
		st[1].stage               = VK_SHADER_STAGE_FRAGMENT_BIT;
		st[1].module              = Module(spv, fs);
		st[1].pName               = "main";
		VkPipelineVertexInputStateCreateInfo   vin {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
		VkPipelineInputAssemblyStateCreateInfo ia {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
		ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		VkPipelineViewportStateCreateInfo vp {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
		vp.viewportCount = 1;
		vp.scissorCount  = 1;
		VkPipelineRasterizationStateCreateInfo rs {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
		rs.polygonMode = VK_POLYGON_MODE_FILL;
		rs.cullMode    = VK_CULL_MODE_NONE;
		rs.lineWidth   = 1.0f;
		VkPipelineMultisampleStateCreateInfo ms {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
		ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		VkPipelineColorBlendAttachmentState att {};
		att.colorWriteMask = 0xF;
		VkPipelineColorBlendStateCreateInfo cbs_ {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
		cbs_.attachmentCount = 1;
		cbs_.pAttachments    = &att;
		VkDynamicState                   dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
		VkPipelineDynamicStateCreateInfo ds {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
		ds.dynamicStateCount = 2;
		ds.pDynamicStates    = dyn;
		VkFormat                      fmt = VK_FORMAT_R8G8B8A8_UNORM;
		VkPipelineRenderingCreateInfo ri {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
		ri.colorAttachmentCount    = 1;
		ri.pColorAttachmentFormats = &fmt;
		VkGraphicsPipelineCreateInfo gi {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri};
		gi.stageCount          = 2;
		gi.pStages             = st;
		gi.pVertexInputState   = &vin;
		gi.pInputAssemblyState = &ia;
		gi.pViewportState      = &vp;
		gi.pRasterizationState = &rs;
		gi.pMultisampleState   = &ms;
		gi.pColorBlendState    = &cbs_;
		gi.pDynamicState       = &ds;
		gi.layout              = layout;
		VkPipeline p;
		CHECK(vkCreateGraphicsPipelines(g_dev, VK_NULL_HANDLE, 1, &gi, nullptr, &p));
		return p;
	};
	auto Compute = [&](const char* cs) {
		VkComputePipelineCreateInfo ci {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
		ci.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		ci.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
		ci.stage.module = Module(spv, cs);
		ci.stage.pName  = "main";
		ci.layout       = layout;
		VkPipeline p;
		CHECK(vkCreateComputePipelines(g_dev, VK_NULL_HANDLE, 1, &ci, nullptr, &p));
		return p;
	};
	VkPipeline gp[ARMS];
	gp[S_UBO] = Graphics("vs_ubo", "fs_ubo");
	gp[S_BDA] = Graphics("vs_bda", "fs_bda");
	gp[G1_PT] = Graphics("vs_g1pt", "fs_g1pt");
	gp[G1_CT] = Graphics("vs_g1ct", "fs_g1ct");
	gp[G2_PT] = gp[S_UBO];
	gp[G2_CT] = gp[S_UBO];
	VkPipeline resolve_pt = Compute("resolve_pt"), resolve_ct = Compute("resolve_ct"), tables = Compute("tables");
	VkPipeline op[ARMS];
	op[S_UBO] = op[S_BDA] = Compute("op_bda");
	op[G1_PT] = op[G2_PT] = Compute("op_g1pt");
	op[G1_CT] = op[G2_CT] = Compute("op_g1ct");

	VkQueryPoolCreateInfo qpi {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
	qpi.queryType  = VK_QUERY_TYPE_TIMESTAMP;
	qpi.queryCount = 2;
	VkQueryPool qpool;
	CHECK(vkCreateQueryPool(g_dev, &qpi, nullptr, &qpool));

	{
		Begin(cbw);
		VkImageMemoryBarrier ib {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		ib.image                                        = color;
		ib.subresourceRange                             = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		ib.oldLayout                                    = VK_IMAGE_LAYOUT_UNDEFINED;
		ib.newLayout                                    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		ib.dstAccessMask                                = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		vkCmdPipelineBarrier(cbw, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
		CHECK(vkEndCommandBuffer(cbw));
		Submit(cbw);
	}

	PC base {};
	base.pt     = ptbuf.addr;
	base.hash   = hashbuf.addr;
	base.mirror = mirror.addr;
	const uint64_t out_dev = mirror.addr + (OUT_ADDR - BASE);

	auto MemBarrier = [&](VkCommandBuffer c, VkPipelineStageFlags dst) {
		VkMemoryBarrier mb {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
		mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
		vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, dst, 0, 1, &mb, 0, nullptr, 0, nullptr);
	};
	auto Tables = [&](VkCommandBuffer c) {
		PC p   = base;
		p.arg  = MAT_ADDR;
		p.outp = TEXTAB;
		p.a0   = nops;
		p.a1   = SEED;
		vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, tables);
		vkCmdPushConstants(c, layout, all_stages, 0, sizeof(PC), &p);
		vkCmdDispatch(c, (nops + 63) / 64, 1, 1);
	};
	const VkPipelineStageFlags shader_stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;

	auto ResolveCpu = [&](uint32_t d, bool tex) {
		const uint8_t* s    = guest.data() + (srt_addr[d] - BASE);
		uint64_t       ptex = Rd64(s), pbuf = Rd64(s + 8), pcon = Rd64(s + 16);
		uint32_t*      r    = reinterpret_cast<uint32_t*>(ringh.map + uint64_t(d) * RSTRIDE);
		if (tex) {
			const uint8_t* tt = gputables ? rb.map + (ptex - TEXTAB) : guest.data() + (ptex - BASE);
			for (uint32_t i = 0; i < KT; i++) {
				uint64_t k0 = Rd64(tt + i * 32), k1 = Rd64(tt + i * 32 + 8);
				uint32_t h = HashKey(k0, k1), idx = 0;
				for (uint32_t p = 0; p < 8; p++) {
					const Slot& sl = hcpu[(h + p) & (HSLOTS - 1)];
					if (sl.k0 == k0 && sl.k1 == k1) {
						idx = sl.idx;
						break;
					}
				}
				r[i] = idx;
			}
		}
		const uint8_t* bt = guest.data() + (pbuf - BASE);
		for (uint32_t j = 0; j < KB; j++) {
			uint64_t o   = Rd64(bt + j * 16) - BASE;
			uint64_t dev = ptcpu[o >> 12] + (o & 0xFFF);
			memcpy(r + 16 + j * 2, &dev, 8);
		}
		memcpy(r + 32, guest.data() + (pcon - BASE), 256);
	};

	const uint32_t per_pass  = (ndraws + npasses - 1) / npasses;
	const uint32_t disp_pass = (ndisp + npasses - 1) / npasses;
	double         drain_us  = 0.0;

	auto Record = [&](int arm) {
		const bool cpu_arm = arm == S_UBO || arm == S_BDA;
		const bool g2      = arm == G2_PT || arm == G2_CT;
		drain_us           = 0.0;
		if (gputables && cpu_arm) {
			double t0 = Now();
			Begin(cbw);
			Tables(cbw);
			MemBarrier(cbw, VK_PIPELINE_STAGE_TRANSFER_BIT);
			VkBufferCopy r {TEXTAB - BASE, 0, uint64_t(nops) * KT * 32};
			vkCmdCopyBuffer(cbw, mirror.buf, rb.buf, 1, &r);
			CHECK(vkEndCommandBuffer(cbw));
			Submit(cbw);
			drain_us = Now() - t0;
		}
		double t0 = Now();
		Begin(cb);
		vkCmdResetQueryPool(cb, qpool, 0, 2);
		vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qpool, 0);
		const bool     ring_copy = ring_mem == 3 && cpu_arm;
		const uint64_t ring_addr = ring_copy ? ringg.addr : ringh.addr;
		if (ring_copy) {
			VkBufferCopy    r {0, 0, uint64_t(nops) * RSTRIDE};
			VkMemoryBarrier mb {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
			mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT;
			vkCmdCopyBuffer(cb, ringh.buf, ringg.buf, 1, &r);
			vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, shader_stages, 0, 1, &mb, 0, nullptr, 0, nullptr);
		}
		if (gputables && !cpu_arm) {
			Tables(cb);
			MemBarrier(cb, shader_stages);
		}
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &sets[0], 0, nullptr);
		uint32_t d = 0, c = ndraws;
		for (uint32_t p = 0; p < npasses; p++) {
			uint32_t first = d, count = std::min(per_pass, ndraws - d);
			uint32_t cend = std::min(c + disp_pass, nops);
			if (c < cend) {
				vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, op[arm]);
				vkCmdPushConstants(cb, layout, all_stages, 0, sizeof(PC), &base);
				for (; c < cend; c++) {
					struct {
						uint64_t outp, arg;
						uint32_t a0, a1;
					} pc {out_dev, 0, c - ndraws, 0};
					if (cpu_arm) {
						ResolveCpu(c, false);
						pc.arg = ring_addr + uint64_t(c) * RSTRIDE;
					} else {
						pc.arg = srt_addr[c];
					}
					vkCmdPushConstants(cb, layout, all_stages, 24, 24, &pc);
					vkCmdDispatch(cb, 1, 1, 1);
					MemBarrier(cb, shader_stages);
				}
			}
			if (count == 0) continue;
			if (g2) {
				uint64_t* dl = reinterpret_cast<uint64_t*>(drawlist.map);
				for (uint32_t i = 0; i < count; i++) dl[first + i] = srt_addr[first + i];
				PC pc   = base;
				pc.arg  = drawlist.addr;
				pc.outp = ringg.addr;
				pc.a0   = first;
				pc.a1   = count;
				vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, arm == G2_PT ? resolve_pt : resolve_ct);
				vkCmdPushConstants(cb, layout, all_stages, 0, sizeof(PC), &pc);
				vkCmdDispatch(cb, (count + 63) / 64, 1, 1);
				MemBarrier(cb, shader_stages);
			}
			VkRenderingAttachmentInfo ca {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
			ca.imageView   = color_view;
			ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			ca.loadOp      = p == 0 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
			ca.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
			VkRenderingInfo ri {VK_STRUCTURE_TYPE_RENDERING_INFO};
			ri.renderArea           = {{0, 0}, {W, H}};
			ri.layerCount           = 1;
			ri.colorAttachmentCount = 1;
			ri.pColorAttachments    = &ca;
			vkCmdBeginRendering(cb, &ri);
			VkViewport vp {0, 0, float(W), float(H), 0, 1};
			VkRect2D   sc {{0, 0}, {W, H}};
			vkCmdSetViewport(cb, 0, 1, &vp);
			vkCmdSetScissor(cb, 0, 1, &sc);
			vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gp[arm]);
			vkCmdPushConstants(cb, layout, all_stages, 0, sizeof(PC), &base);
			for (uint32_t i = 0; i < count; i++, d++) {
				uint32_t g = grid[d];
				if (arm == S_UBO || g2) {
					if (arm == S_UBO) ResolveCpu(d, true);
					uint32_t off = d * RSTRIDE;
					vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &sets[g2 || ring_copy ? 2 : 1], 1, &off);
					uint32_t a[2] = {d, g};
					vkCmdPushConstants(cb, layout, all_stages, 40, 8, a);
				} else {
					struct {
						uint64_t arg;
						uint32_t a0, a1;
					} pc {0, d, g};
					if (arm == S_BDA) {
						ResolveCpu(d, true);
						pc.arg = ring_addr + uint64_t(d) * RSTRIDE;
					} else {
						pc.arg = srt_addr[d];
					}
					vkCmdPushConstants(cb, layout, all_stages, 32, 16, &pc);
				}
				vkCmdDraw(cb, 6 * g * g, 1, 0, 0);
			}
			vkCmdEndRendering(cb);
		}
		vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qpool, 1);
		CHECK(vkEndCommandBuffer(cb));
		return Now() - t0;
	};

	std::vector<uint8_t> first_image;
	uint64_t             diff_bytes = 0;
	int                  diff_max   = 0;
	auto ImageHash = [&]() {
		Begin(cbw);
		VkImageMemoryBarrier ib {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		ib.image                                        = color;
		ib.subresourceRange                             = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		ib.oldLayout                                    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		ib.newLayout                                    = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		ib.srcAccessMask                                = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		ib.dstAccessMask                                = VK_ACCESS_TRANSFER_READ_BIT;
		vkCmdPipelineBarrier(cbw, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
		VkBufferImageCopy bic {};
		bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		bic.imageExtent      = {W, H, 1};
		vkCmdCopyImageToBuffer(cbw, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, imgrb.buf, 1, &bic);
		std::swap(ib.oldLayout, ib.newLayout);
		ib.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		ib.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		vkCmdPipelineBarrier(cbw, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
		CHECK(vkEndCommandBuffer(cbw));
		Submit(cbw);
		uint64_t h = 1469598103934665603ull;
		for (uint64_t i = 0; i < uint64_t(W) * H * 4; i++) h = (h ^ imgrb.map[i]) * 1099511628211ull;
		if (first_image.empty()) {
			first_image.assign(imgrb.map, imgrb.map + uint64_t(W) * H * 4);
		}
		diff_bytes = 0;
		diff_max   = 0;
		for (uint64_t i = 0; i < uint64_t(W) * H * 4; i++) {
			int dlt = std::abs(int(imgrb.map[i]) - int(first_image[i]));
			diff_bytes += dlt != 0;
			diff_max = std::max(diff_max, dlt);
		}
		return h;
	};

	auto TypeName = [&](uint32_t t) {
		VkMemoryPropertyFlags f = g_memprops.memoryTypes[t].propertyFlags;
		return std::string(f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? "device-local" : "host") + (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? "+visible" : "") +
		       (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? "+cached" : "");
	};
	printf("device %s | draws %u dispatches %u passes %u | KT %u KB %u CR %u NTEX %u | overdraw %.2f | gpu-tables %d | rounds %u x %u frames | cpu %d | ring %s | mirror %s\n",
	       props.deviceName, ndraws, ndisp, npasses, KT, KB, CR, NTEX, overdraw, gputables, rounds, block, cpu, TypeName(ringh.type).c_str(),
	       TypeName(mirror.type).c_str());
	std::vector<std::vector<double>> rec(ARMS), gpu(ARMS), wall(ARMS), drain(ARMS);
	std::vector<uint64_t>            hashes(ARMS, 0), dbytes(ARMS, 0);
	std::vector<int>                 dmax(ARMS, 0);
	for (uint32_t r = 0; r <= rounds; r++) {
		for (int arm: arms) {
			uint32_t frames = r == 0 ? 10 : block;
			for (uint32_t f = 0; f < frames; f++) {
				double   t_rec = Record(arm);
				double   t0    = Now();
				Submit(cb);
				double   t_wall = Now() - t0;
				uint64_t ts[2];
				CHECK(vkGetQueryPoolResults(g_dev, qpool, 0, 2, sizeof(ts), ts, 8, VK_QUERY_RESULT_64_BIT));
				if (r > 0) {
					rec[arm].push_back(t_rec);
					wall[arm].push_back(t_wall);
					gpu[arm].push_back(double(ts[1] - ts[0]) * g_ts_period * 1e-6);
					drain[arm].push_back(drain_us);
				}
			}
			if (r == rounds) {
				hashes[arm] = ImageHash();
				dbytes[arm] = diff_bytes;
				dmax[arm]   = diff_max;
			}
		}
	}
	auto Q = [](std::vector<double> v, double q) {
		std::sort(v.begin(), v.end());
		return v[size_t(q * double(v.size() - 1))];
	};
	for (int arm: arms) {
		double ops = double(ndraws + ndisp);
		printf("%-11s cpu_record_ms %7.3f (p10 %7.3f p90 %7.3f) = %6.3f us/op | gpu_ms %7.3f (p10 %7.3f p90 %7.3f) | submit_wait_ms %7.3f | drain_ms %6.3f | image %016llx diff %llu bytes max %d\n",
		       ARM_NAMES[arm], Q(rec[arm], 0.5) / 1e3, Q(rec[arm], 0.1) / 1e3, Q(rec[arm], 0.9) / 1e3, Q(rec[arm], 0.5) / ops, Q(gpu[arm], 0.5),
		       Q(gpu[arm], 0.1), Q(gpu[arm], 0.9), Q(wall[arm], 0.5) / 1e3, Q(drain[arm], 0.5) / 1e3, static_cast<unsigned long long>(hashes[arm]),
		       static_cast<unsigned long long>(dbytes[arm]), dmax[arm]);
	}
	return 0;
}

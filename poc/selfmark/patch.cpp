#include <Zydis/Zydis.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <signal.h>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <vector>

static constexpr uint64_t PAGE        = 4096;
static constexpr uint64_t REGION      = 64ull << 20;
static constexpr uint64_t CARDS_FIXED = 0x40000000ull;

static uint8_t* g_primary;
static uint8_t* g_alias;
static uint8_t* g_cards;
static uint8_t* g_code;
static uint64_t g_code_used;
static uint8_t* g_tramp;
static uint64_t g_tramp_used;
static ZydisDecoder g_decoder;
static uint64_t g_faults;
static uint64_t g_patched;
static double   g_patch_us;
static std::string g_refusal;

struct Regs {
	uint64_t gpr[16];
	uint64_t rflags;
	uint64_t pad;
	uint8_t  ymm[16][32];
};

extern "C" void CallStub(void* stub, Regs* regs);

__asm__(R"(
.text
.globl CallStub
.type CallStub,@function
CallStub:
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    push %rsi
    push %rdi
    mov %rsi, %rax
    vmovdqu 144(%rax), %ymm0
    vmovdqu 176(%rax), %ymm1
    vmovdqu 208(%rax), %ymm2
    vmovdqu 240(%rax), %ymm3
    vmovdqu 272(%rax), %ymm4
    vmovdqu 304(%rax), %ymm5
    vmovdqu 336(%rax), %ymm6
    vmovdqu 368(%rax), %ymm7
    vmovdqu 400(%rax), %ymm8
    vmovdqu 432(%rax), %ymm9
    vmovdqu 464(%rax), %ymm10
    vmovdqu 496(%rax), %ymm11
    vmovdqu 528(%rax), %ymm12
    vmovdqu 560(%rax), %ymm13
    vmovdqu 592(%rax), %ymm14
    vmovdqu 624(%rax), %ymm15
    mov 8(%rax), %rcx
    mov 16(%rax), %rdx
    mov 24(%rax), %rbx
    mov 40(%rax), %rbp
    mov 48(%rax), %rsi
    mov 56(%rax), %rdi
    mov 64(%rax), %r8
    mov 72(%rax), %r9
    mov 80(%rax), %r10
    mov 88(%rax), %r11
    mov 96(%rax), %r12
    mov 104(%rax), %r13
    mov 112(%rax), %r14
    mov 120(%rax), %r15
    push 128(%rax)
    popfq
    mov 0(%rax), %rax
    call *(%rsp)
    push %rax
    mov 16(%rsp), %rax
    pop 0(%rax)
    pushfq
    pop 128(%rax)
    mov %rcx, 8(%rax)
    mov %rdx, 16(%rax)
    mov %rbx, 24(%rax)
    mov %rbp, 40(%rax)
    mov %rsi, 48(%rax)
    mov %rdi, 56(%rax)
    mov %r8, 64(%rax)
    mov %r9, 72(%rax)
    mov %r10, 80(%rax)
    mov %r11, 88(%rax)
    mov %r12, 96(%rax)
    mov %r13, 104(%rax)
    mov %r14, 112(%rax)
    mov %r15, 120(%rax)
    vmovdqu %ymm0, 144(%rax)
    vmovdqu %ymm1, 176(%rax)
    vmovdqu %ymm2, 208(%rax)
    vmovdqu %ymm3, 240(%rax)
    vmovdqu %ymm4, 272(%rax)
    vmovdqu %ymm5, 304(%rax)
    vmovdqu %ymm6, 336(%rax)
    vmovdqu %ymm7, 368(%rax)
    vmovdqu %ymm8, 400(%rax)
    vmovdqu %ymm9, 432(%rax)
    vmovdqu %ymm10, 464(%rax)
    vmovdqu %ymm11, 496(%rax)
    vmovdqu %ymm12, 528(%rax)
    vmovdqu %ymm13, 560(%rax)
    vmovdqu %ymm14, 592(%rax)
    vmovdqu %ymm15, 624(%rax)
    vzeroupper
    pop %rdi
    pop %rsi
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret
)");

static double Now() {
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return double(ts.tv_sec) * 1e6 + double(ts.tv_nsec) * 1e-3;
}

static int GprIndex(ZydisRegister r) {
	if (r == ZYDIS_REGISTER_NONE) return -1;
	ZydisRegister full = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, r);
	if (full >= ZYDIS_REGISTER_RAX && full <= ZYDIS_REGISTER_R15) return int(full - ZYDIS_REGISTER_RAX);
	return -1;
}

struct Decoded {
	ZydisDecodedInstruction ins;
	ZydisDecodedOperand     ops[ZYDIS_MAX_OPERAND_COUNT];
	int                     mem = -1;
};

static bool Decode(const uint8_t* p, size_t n, Decoded& d) {
	if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&g_decoder, p, n, &d.ins, d.ops))) return false;
	d.mem = -1;
	for (int i = 0; i < d.ins.operand_count; i++) {
		if (d.ops[i].type == ZYDIS_OPERAND_TYPE_MEMORY && d.ops[i].visibility == ZYDIS_OPERAND_VISIBILITY_EXPLICIT) {
			if (d.mem >= 0) return true;
			d.mem = i;
		}
	}
	return true;
}

static const char* StoreRefusal(const Decoded& d) {
	if (d.mem < 0) return "no explicit memory operand";
	const auto& m = d.ops[d.mem];
	if (!(m.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE)) return "memory operand not written";
	if (m.mem.type != ZYDIS_MEMOP_TYPE_MEM) return "not a plain memory operand";
	if (m.mem.base == ZYDIS_REGISTER_RIP || m.mem.base == ZYDIS_REGISTER_EIP) return "rip relative";
	if (m.mem.base == ZYDIS_REGISTER_NONE && m.mem.index == ZYDIS_REGISTER_NONE) return "absolute address";
	if (GprIndex(m.mem.base) == 4) return "stack relative";
	if (m.mem.segment == ZYDIS_REGISTER_FS || m.mem.segment == ZYDIS_REGISTER_GS) return "segment override";
	if (d.ins.attributes & (ZYDIS_ATTRIB_HAS_REP | ZYDIS_ATTRIB_HAS_REPE | ZYDIS_ATTRIB_HAS_REPNE)) return "string prefix";
	if (d.ins.encoding != ZYDIS_INSTRUCTION_ENCODING_LEGACY && d.ins.encoding != ZYDIS_INSTRUCTION_ENCODING_VEX) return "encoding";
	return nullptr;
}

static const char* FollowerRefusal(const Decoded& d) {
	if (d.ins.attributes & ZYDIS_ATTRIB_IS_RELATIVE) return "relative follower";
	for (int i = 0; i < d.ins.operand_count; i++) {
		if (d.ops[i].type == ZYDIS_OPERAND_TYPE_MEMORY && (d.ops[i].mem.base == ZYDIS_REGISTER_RIP)) return "rip relative follower";
	}
	return nullptr;
}

struct Emit {
	uint8_t* p;
	void B(std::initializer_list<uint8_t> b) {
		for (uint8_t x: b) *p++ = x;
	}
	void Q(uint64_t v) {
		memcpy(p, &v, 8);
		p += 8;
	}
	void D(uint32_t v) {
		memcpy(p, &v, 4);
		p += 4;
	}
	void Raw(const uint8_t* s, size_t n) {
		memcpy(p, s, n);
		p += n;
	}
	bool Enc(ZydisEncoderRequest& r) {
		ZyanUSize len = 15;
		if (!ZYAN_SUCCESS(ZydisEncoderEncodeInstruction(&r, p, &len))) return false;
		p += len;
		return true;
	}
};

static ZydisEncoderRequest Req(ZydisMnemonic m, int n) {
	ZydisEncoderRequest r {};
	r.machine_mode  = ZYDIS_MACHINE_MODE_LONG_64;
	r.mnemonic      = m;
	r.operand_count = uint8_t(n);
	return r;
}

static void OpReg(ZydisEncoderRequest& r, int i, ZydisRegister reg) {
	r.operands[i].type      = ZYDIS_OPERAND_TYPE_REGISTER;
	r.operands[i].reg.value = reg;
}

static void OpImm(ZydisEncoderRequest& r, int i, uint64_t v) {
	r.operands[i].type  = ZYDIS_OPERAND_TYPE_IMMEDIATE;
	r.operands[i].imm.u = v;
}

static void OpMem(ZydisEncoderRequest& r, int i, ZydisRegister base, int64_t disp, uint16_t size) {
	r.operands[i].type             = ZYDIS_OPERAND_TYPE_MEMORY;
	r.operands[i].mem.base         = base;
	r.operands[i].mem.displacement = disp;
	r.operands[i].mem.size         = size;
}

static uint8_t* BuildTrampoline(const uint8_t* site, const Decoded& d, size_t patch_len, std::string& why) {
	bool used[16] = {};
	used[0] = used[4] = used[5] = true;
	for (int i = 0; i < d.ins.operand_count; i++) {
		const auto& o = d.ops[i];
		if (o.type == ZYDIS_OPERAND_TYPE_REGISTER) {
			int g = GprIndex(o.reg.value);
			if (g >= 0) used[g] = true;
		} else if (o.type == ZYDIS_OPERAND_TYPE_MEMORY) {
			int b = GprIndex(o.mem.base), x = GprIndex(o.mem.index);
			if (b >= 0) used[b] = true;
			if (x >= 0) used[x] = true;
		}
	}
	static const int order[] = {6, 7, 3, 2, 1, 8, 9, 10, 11, 12, 13, 14, 15};
	int s = -1, t = -1;
	for (int c: order) {
		if (used[c]) continue;
		if (s < 0) s = c;
		else if (t < 0) t = c;
	}
	if (t < 0) {
		why = "no scratch register";
		return nullptr;
	}
	const ZydisRegister S = ZydisRegister(ZYDIS_REGISTER_RAX + s), T = ZydisRegister(ZYDIS_REGISTER_RAX + t);
	uint8_t* start = g_tramp + g_tramp_used;
	Emit     e {start};
	const auto& m = d.ops[d.mem];

	auto r = Req(ZYDIS_MNEMONIC_LEA, 2);
	OpReg(r, 0, ZYDIS_REGISTER_RSP);
	OpMem(r, 1, ZYDIS_REGISTER_RSP, -0x80, 8);
	if (!e.Enc(r)) return nullptr;
	for (ZydisRegister reg: {ZYDIS_REGISTER_RAX, S, T}) {
		r = Req(ZYDIS_MNEMONIC_PUSH, 1);
		OpReg(r, 0, reg);
		if (!e.Enc(r)) return nullptr;
	}
	r = Req(ZYDIS_MNEMONIC_LEA, 2);
	OpReg(r, 0, S);
	r.operands[1].type             = ZYDIS_OPERAND_TYPE_MEMORY;
	r.operands[1].mem.base         = m.mem.base;
	r.operands[1].mem.index        = m.mem.index;
	r.operands[1].mem.scale        = m.mem.scale;
	r.operands[1].mem.displacement = m.mem.disp.size ? m.mem.disp.value : 0;
	r.operands[1].mem.size         = 8;
	if (!e.Enc(r)) {
		why = "lea encode";
		return nullptr;
	}
	e.B({0x9F, 0x0F, 0x90, 0xC0});
	r = Req(ZYDIS_MNEMONIC_MOV, 2);
	OpReg(r, 0, T);
	OpImm(r, 1, reinterpret_cast<uint64_t>(g_primary));
	if (!e.Enc(r)) return nullptr;
	r = Req(ZYDIS_MNEMONIC_SUB, 2);
	OpReg(r, 0, S);
	OpReg(r, 1, T);
	if (!e.Enc(r)) return nullptr;
	r = Req(ZYDIS_MNEMONIC_CMP, 2);
	OpReg(r, 0, S);
	OpImm(r, 1, REGION);
	if (!e.Enc(r)) return nullptr;
	e.B({0x0F, 0x83});
	uint8_t* jae_fix = e.p;
	e.D(0);
	r = Req(ZYDIS_MNEMONIC_MOV, 2);
	OpReg(r, 0, T);
	OpReg(r, 1, S);
	if (!e.Enc(r)) return nullptr;
	r = Req(ZYDIS_MNEMONIC_SHR, 2);
	OpReg(r, 0, T);
	OpImm(r, 1, 12);
	if (!e.Enc(r)) return nullptr;
	r = Req(ZYDIS_MNEMONIC_MOV, 2);
	OpMem(r, 0, T, int64_t(CARDS_FIXED), 1);
	OpImm(r, 1, 1);
	if (!e.Enc(r)) return nullptr;
	r = Req(ZYDIS_MNEMONIC_MOV, 2);
	OpReg(r, 0, T);
	OpImm(r, 1, reinterpret_cast<uint64_t>(g_alias));
	if (!e.Enc(r)) return nullptr;
	r = Req(ZYDIS_MNEMONIC_ADD, 2);
	OpReg(r, 0, S);
	OpReg(r, 1, T);
	if (!e.Enc(r)) return nullptr;
	e.B({0x04, 0x7F, 0x9E});
	r = Req(ZYDIS_MNEMONIC_POP, 1);
	OpReg(r, 0, T);
	if (!e.Enc(r)) return nullptr;
	r = Req(ZYDIS_MNEMONIC_MOV, 2);
	OpReg(r, 0, ZYDIS_REGISTER_RAX);
	OpMem(r, 1, ZYDIS_REGISTER_RSP, 8, 8);
	if (!e.Enc(r)) return nullptr;
	ZydisEncoderRequest store {};
	if (!ZYAN_SUCCESS(ZydisEncoderDecodedInstructionToEncoderRequest(&d.ins, d.ops, d.ins.operand_count_visible, &store))) {
		why = "store request";
		return nullptr;
	}
	for (int i = 0; i < store.operand_count; i++) {
		if (store.operands[i].type == ZYDIS_OPERAND_TYPE_MEMORY) {
			store.operands[i].mem.base         = S;
			store.operands[i].mem.index        = ZYDIS_REGISTER_NONE;
			store.operands[i].mem.scale        = 0;
			store.operands[i].mem.displacement = 0;
		}
	}
	if (!e.Enc(store)) {
		why = "store encode";
		return nullptr;
	}
	r = Req(ZYDIS_MNEMONIC_POP, 1);
	OpReg(r, 0, S);
	if (!e.Enc(r)) return nullptr;
	r = Req(ZYDIS_MNEMONIC_LEA, 2);
	OpReg(r, 0, ZYDIS_REGISTER_RSP);
	OpMem(r, 1, ZYDIS_REGISTER_RSP, 0x88, 8);
	if (!e.Enc(r)) return nullptr;
	e.Raw(site + d.ins.length, patch_len - d.ins.length);
	e.B({0xE9});
	e.D(uint32_t(int64_t(site + patch_len) - int64_t(e.p + 4)));
	uint32_t rel = uint32_t(e.p - (jae_fix + 4));
	memcpy(jae_fix, &rel, 4);
	e.B({0x04, 0x7F, 0x9E});
	for (ZydisRegister reg: {T, S, ZYDIS_REGISTER_RAX}) {
		r = Req(ZYDIS_MNEMONIC_POP, 1);
		OpReg(r, 0, reg);
		if (!e.Enc(r)) return nullptr;
	}
	r = Req(ZYDIS_MNEMONIC_LEA, 2);
	OpReg(r, 0, ZYDIS_REGISTER_RSP);
	OpMem(r, 1, ZYDIS_REGISTER_RSP, 0x80, 8);
	if (!e.Enc(r)) return nullptr;
	e.Raw(site, patch_len);
	e.B({0xE9});
	e.D(uint32_t(int64_t(site + patch_len) - int64_t(e.p + 4)));
	g_tramp_used += uint64_t(e.p - start);
	g_tramp_used = (g_tramp_used + 15) & ~15ull;
	return start;
}

static bool PatchSite(uint8_t* site, std::string& why) {
	Decoded d;
	if (!Decode(site, 15, d)) {
		why = "decode";
		return false;
	}
	if (const char* r = StoreRefusal(d)) {
		why = r;
		return false;
	}
	size_t len = d.ins.length;
	while (len < 5) {
		Decoded f;
		if (!Decode(site + len, 15, f)) {
			why = "follower decode";
			return false;
		}
		if (const char* r = FollowerRefusal(f)) {
			why = r;
			return false;
		}
		len += f.ins.length;
	}
	uint8_t* tramp = BuildTrampoline(site, d, len, why);
	if (!tramp) return false;
	uint8_t* page = reinterpret_cast<uint8_t*>(reinterpret_cast<uint64_t>(site) & ~(PAGE - 1));
	mprotect(page, 2 * PAGE, PROT_READ | PROT_WRITE | PROT_EXEC);
	site[0]     = 0xE9;
	int32_t rel = int32_t(int64_t(tramp) - int64_t(site + 5));
	memcpy(site + 1, &rel, 4);
	for (size_t i = 5; i < len; i++) site[i] = 0x90;
	mprotect(page, 2 * PAGE, PROT_READ | PROT_WRITE | PROT_EXEC);
	return true;
}

static void OnFault(int, siginfo_t* info, void* ctx) {
	ucontext_t* uc   = static_cast<ucontext_t*>(ctx);
	uint8_t*    site = reinterpret_cast<uint8_t*>(uc->uc_mcontext.gregs[REG_RIP]);
	uint64_t    off  = uint64_t(reinterpret_cast<uint8_t*>(info->si_addr) - g_primary);
	g_faults++;
	if (off >= REGION || site < g_code || site >= g_code + g_code_used) {
		char line[256];
		int  n = snprintf(line, sizeof(line), "unexpected fault: addr %p rip %p (code %p used %llx tramp %p used %llx) primary %p alias %p\n", info->si_addr,
		                  static_cast<void*>(site), static_cast<void*>(g_code), static_cast<unsigned long long>(g_code_used), static_cast<void*>(g_tramp),
		                  static_cast<unsigned long long>(g_tramp_used), static_cast<void*>(g_primary), static_cast<void*>(g_alias));
		(void)!write(2, line, size_t(n));
		if (site >= g_tramp && site < g_tramp + g_tramp_used) {
			for (uint8_t* q = g_tramp + ((uint64_t(site - g_tramp)) & ~0xFFull); q < g_tramp + g_tramp_used && q < site + 48; q++) {
				n = snprintf(line, sizeof(line), "%s%02x", q == site ? " | " : " ", *q);
				(void)!write(2, line, size_t(n));
			}
			(void)!write(2, "\n", 1);
		}
		_exit(4);
	}
	double      t0 = Now();
	std::string why;
	if (PatchSite(site, why)) {
		g_patched++;
		g_patch_us += Now() - t0;
		return;
	}
	g_refusal = why;
	mprotect(g_primary + (off & ~(PAGE - 1)), PAGE, PROT_READ | PROT_WRITE);
}

struct Site {
	uint64_t             rip;
	uint64_t             count;
	std::vector<uint8_t> bytes;
};

static std::vector<uint8_t> Hex(const std::string& s) {
	std::vector<uint8_t> v;
	for (size_t i = 0; i + 1 < s.size(); i += 2) v.push_back(uint8_t(strtoul(s.substr(i, 2).c_str(), nullptr, 16)));
	return v;
}

int main(int argc, char** argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: patch <sites.tsv> [calls]\n");
		return 2;
	}
	uint64_t calls = argc > 2 ? strtoull(argv[2], nullptr, 10) : 2000000;
	ZydisDecoderInit(&g_decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
	std::vector<Site> sites;
	uint64_t          total = 0;
	{
		std::ifstream f(argv[1]);
		std::string   line;
		while (std::getline(f, line)) {
			std::istringstream ss(line);
			Site               s;
			std::string        hex;
			if (!(ss >> s.rip >> s.count >> hex)) continue;
			s.bytes = Hex(hex);
			total += s.count;
			sites.push_back(s);
		}
	}
	int fd = memfd_create("guest", 0);
	if (fd < 0 || ftruncate(fd, off_t(REGION)) != 0) return 2;
	g_primary = static_cast<uint8_t*>(mmap(nullptr, REGION, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
	g_alias   = static_cast<uint8_t*>(mmap(nullptr, REGION, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
	g_cards   = static_cast<uint8_t*>(mmap(reinterpret_cast<void*>(CARDS_FIXED), REGION / PAGE, PROT_READ | PROT_WRITE,
	                                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0));
	g_code    = static_cast<uint8_t*>(mmap(nullptr, 4 << 20, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
	g_tramp   = g_code + (2 << 20);
	if (g_primary == MAP_FAILED || g_alias == MAP_FAILED || g_cards == MAP_FAILED || g_code == MAP_FAILED) return 2;
	memset(g_primary, 0, REGION);
	stack_t ss {};
	ss.ss_sp   = malloc(1 << 16);
	ss.ss_size = 1 << 16;
	sigaltstack(&ss, nullptr);
	struct sigaction sa {};
	sa.sa_sigaction = OnFault;
	sa.sa_flags     = SA_SIGINFO | SA_ONSTACK;
	sigaction(SIGSEGV, &sa, nullptr);

	uint64_t w_direct = 0, w_follow = 0, w_refused = 0, w_undecoded = 0;
	std::map<std::string, uint64_t> reasons;
	uint64_t n_ok = 0, n_bad = 0, w_ok = 0, w_bad = 0, w_tested = 0;
	double   base_ns_sum = 0, patched_ns_sum = 0, weight_ns = 0;
	std::vector<std::string> bad_list;
	for (const Site& s: sites) {
		Decoded d;
		if (s.bytes.size() < 15 || !Decode(s.bytes.data(), s.bytes.size(), d)) {
			w_undecoded += s.count;
			continue;
		}
		const char* refusal = StoreRefusal(d);
		size_t      len     = d.ins.length;
		std::string why     = refusal ? refusal : "";
		while (!refusal && len < 5) {
			Decoded f;
			if (len + 1 > s.bytes.size() || !Decode(s.bytes.data() + len, s.bytes.size() - len, f)) {
				why = "follower decode";
				break;
			}
			if (const char* r = FollowerRefusal(f)) {
				why = r;
				break;
			}
			len += f.ins.length;
		}
		if (!why.empty()) {
			w_refused += s.count;
			reasons[why] += s.count;
			continue;
		}
		if (d.ins.length >= 5) w_direct += s.count;
		else w_follow += s.count;

		const auto& m     = d.ops[d.mem];
		int         breg  = GprIndex(m.mem.base), xreg = GprIndex(m.mem.index);
		if (breg < 0 || breg == xreg) {
			reasons["harness: no base or base equals index"] += s.count;
			continue;
		}
		uint8_t* stub_a = g_code + g_code_used;
		memcpy(stub_a, s.bytes.data(), d.ins.length);
		memset(stub_a + d.ins.length, 0x90, 8);
		stub_a[d.ins.length + 8] = 0xC3;
		uint8_t* stub_b          = stub_a + 64;
		memcpy(stub_b, stub_a, 64);
		g_code_used += 128;

		Regs in {};
		for (int i = 0; i < 16; i++) in.gpr[i] = 0x1111111111111111ull * uint64_t(i + 1) + 0x0123456789ABCDEFull;
		for (int i = 0; i < 16; i++) {
			for (int k = 0; k < 32; k++) in.ymm[i][k] = uint8_t(i * 37 + k * 11 + 5);
		}
		in.rflags          = 0x202 | 0x1 | 0x40 | 0x800 | 0x80;
		const int64_t disp = m.mem.disp.size ? m.mem.disp.value : 0;
		uint64_t idx       = 0x40;
		uint64_t target    = 0x100000 + 0x200;
		uint64_t base_val  = reinterpret_cast<uint64_t>(g_primary) + target - uint64_t(disp) - (xreg >= 0 ? idx * m.mem.scale : 0);
		in.gpr[breg]       = base_val;
		if (xreg >= 0) in.gpr[xreg] = idx;
		uint8_t* ea       = g_primary + target;
		const uint64_t pg = target / PAGE;

		memset(ea - 64, 0xEE, 192);
		Regs a = in;
		CallStub(stub_a, &a);
		uint8_t expect[192];
		memcpy(expect, ea - 64, 192);

		memset(ea - 64, 0xEE, 192);
		g_cards[pg] = 0;
		mprotect(g_primary + pg * PAGE, PAGE, PROT_READ);
		uint64_t faults_before = g_faults, patched_before = g_patched;
		g_refusal.clear();
		Regs b = in;
		CallStub(stub_b, &b);
		bool patched = g_patched == patched_before + 1;
		bool same    = memcmp(expect, g_alias + target - 64, 192) == 0 && memcmp(&a, &b, sizeof(Regs)) == 0;
		bool card    = g_cards[pg] == 1;
		Regs c       = in;
		memset(g_alias + target - 64, 0xEE, 192);
		uint64_t faults_mid = g_faults;
		CallStub(stub_b, &c);
		bool again = g_faults == faults_mid && memcmp(expect, g_alias + target - 64, 192) == 0 && memcmp(&a, &c, sizeof(Regs)) == 0;
		Regs o     = in;
		o.gpr[breg] = reinterpret_cast<uint64_t>(expect) + 64 + 4096 * 0 - uint64_t(disp) - (xreg >= 0 ? idx * m.mem.scale : 0);
		uint8_t outside[192];
		memset(outside, 0xEE, 192);
		uint8_t want_out[192];
		{
			Regs oa      = in;
			static uint8_t buf_a[256] __attribute__((aligned(64)));
			static uint8_t buf_b[256] __attribute__((aligned(64)));
			memset(buf_a, 0xEE, 256);
			memset(buf_b, 0xEE, 256);
			oa.gpr[breg] = reinterpret_cast<uint64_t>(buf_a) + 64 - uint64_t(disp) - (xreg >= 0 ? idx * m.mem.scale : 0);
			CallStub(stub_a, &oa);
			o.gpr[breg] = reinterpret_cast<uint64_t>(buf_b) + 64 - uint64_t(disp) - (xreg >= 0 ? idx * m.mem.scale : 0);
			CallStub(stub_b, &o);
			memcpy(want_out, buf_a, 192);
			memcpy(outside, buf_b, 192);
			oa.gpr[breg] = o.gpr[breg] = 0;
			same = same && memcmp(want_out, outside, 192) == 0 && memcmp(&oa, &o, sizeof(Regs)) == 0;
		}
		mprotect(g_primary + pg * PAGE, PAGE, PROT_READ | PROT_WRITE);
		(void)faults_before;
		w_tested += s.count;
		if (patched && same && card && again) {
			n_ok++;
			w_ok += s.count;
			Regs   ta = in, tb = in;
			double t0 = Now();
			for (uint64_t i = 0; i < calls; i++) CallStub(stub_a, &ta);
			double t1 = Now();
			for (uint64_t i = 0; i < calls; i++) CallStub(stub_b, &tb);
			double t2 = Now();
			base_ns_sum += (t1 - t0) * 1e3 / double(calls) * double(s.count);
			patched_ns_sum += (t2 - t1) * 1e3 / double(calls) * double(s.count);
			weight_ns += double(s.count);
		} else {
			n_bad++;
			w_bad += s.count;
			char text[256];
			ZydisDisassembledInstruction dis;
			ZydisDisassembleATT(ZYDIS_MACHINE_MODE_LONG_64, 0, s.bytes.data(), s.bytes.size(), &dis);
			snprintf(text, sizeof(text), "%s : patched %d same %d card %d again %d refusal '%s'", dis.text, patched, same, card, again, g_refusal.c_str());
			bad_list.push_back(text);
		}
	}
	auto pct = [&](uint64_t v) { return double(v) * 100.0 / double(total); };
	printf("sites %zu, fautes d'écriture couvertes par ces sites %llu\n", sites.size(), static_cast<unsigned long long>(total));
	printf("statique : corrigeable sans voisin (instruction >= 5 octets) %.1f %% | corrigeable avec voisins déplaçables %.1f %% | refusé %.1f %% | non décodé %.1f %%\n",
	       pct(w_direct), pct(w_follow), pct(w_refused), pct(w_undecoded));
	for (const auto& [k, v]: reasons) printf("   refus ou hors banc : %-40s %.2f %%\n", k.c_str(), pct(v));
	printf("exécuté : %llu sites corrigés à la première faute et vérifiés (mémoire, registres, drapeaux, carte, pas de 2e faute, adresse hors zone) = %.1f %% des fautes ; %llu en échec = %.1f %%\n",
	       static_cast<unsigned long long>(n_ok), pct(w_ok), static_cast<unsigned long long>(n_bad), pct(w_bad));
	for (const auto& b: bad_list) printf("   échec : %s\n", b.c_str());
	if (weight_ns > 0) {
		printf("temps (pondéré par les fautes, %llu appels par site) : appel d'origine %.2f ns, appel corrigé %.2f ns, surcoût %.2f ns par écriture ; correction d'un site %.1f µs en moyenne\n",
		       static_cast<unsigned long long>(calls), base_ns_sum / weight_ns, patched_ns_sum / weight_ns, (patched_ns_sum - base_ns_sum) / weight_ns,
		       g_patch_us / double(std::max<uint64_t>(g_patched, 1)));
	}
	return 0;
}

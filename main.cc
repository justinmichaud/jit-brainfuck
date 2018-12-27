#include <iostream>
#include <stdlib.h>
#include <memory>
#include <assert.h>
#include <sys/mman.h>

class JITFnBlock {
	char* block;

public:
	JITFnBlock()
	{
		block = (char*) mmap(NULL,       // address
                      4096*24,             // size
                      PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS,
                      -1,               // fd (not used here)
                      0);               // offset (not used here)
		if (!block)
			throw "Could not allocate executable block";
	}

	~JITFnBlock()
	{
		munmap((void*) block, 4096*24);
		block = nullptr;
	}

	char* begin() const { return block; }
	char* end() const { return block + 4096*24; }
};

typedef struct REG_t {
	unsigned char val;
} REG;

const REG RAX = { 0 };
const REG RDI = { 0b0111 };
const REG RSI = { 0b0110 };
const REG RDX = { 0b0010 };
const REG RCX = { 0b0001 };
const REG RBP = { 0b0101 };
const REG RSP = { 0b0110 };

extern "C" { typedef uint64_t (*jit_fp)(register void*, register uint64_t, register uint64_t, register uint64_t); }

class JITFn {
	std::shared_ptr<JITFnBlock> block;
	char* start;
	char* end;

	inline void write(std::initializer_list<unsigned char>&& data)
	{
		for (auto c : data) {
			printf("%02x", (int) c);
			assert(end < block->end());
			*end = c;
			++end;
		}
	}

	JITFn(std::shared_ptr<JITFnBlock> block, char* start)
	{
		this->block = std::move(block);
		this->start = start;
		end = start;
	}

	JITFn(const JITFn&) = delete;
	JITFn(JITFn&&) = default;
public:
	JITFn()
	{
		block = std::make_shared<JITFnBlock>();
		start = block->begin();
		end = start;
	}

	void* current() const { return end; }
	JITFn makeNextFunction() const { return JITFn(block, end); }

	template <typename X>
	uint64_t operator()(X* i)
	{
		return ((jit_fp) start)((void*) i, 0, 0, 0);
	}

	void ret() { write({ 0xc3 }); }
	void mov(REG dst, REG src) { write({ 0x48, 0x8B, (unsigned char) (0b11000000 | dst.val<<3 | src.val) }); }
	void load(REG dst, uint64_t src) { write({ 0x48, (unsigned char) (0xB8 | dst.val) }); for (size_t i=0; i<8; ++i) write({ (unsigned char) ((src>>(i*8))&0x00FF) }); }
	void load(REG dst, REG base, int8_t offset) { write({ 0x48, 0x8b, (unsigned char) (0b01000000 | dst.val<<3 | base.val), *reinterpret_cast<unsigned char*>(&offset) }); }
	void store(REG base, int8_t offset, REG src) { write({ 0x48, 0x89, (unsigned char) (0b01000000 | src.val<<3 | base.val), *reinterpret_cast<unsigned char*>(&offset) });	}
	void add(REG dst, REG src) { write({ 0x48, 0x01, (unsigned char) (0b11000000 | src.val<<3 | dst.val) }); };
	void sub(REG dst, REG src) { write({ 0x48, 0x29, (unsigned char) (0b11000000 | src.val<<3 | dst.val) }); };

	void cmp(REG r, uint8_t val) { write({ 0x48, 0x83, (unsigned char) (0b11111000 | r.val), val }); }
	void je(uint8_t offset) { write({ 0x74, offset }); }
	void jne(uint8_t offset) { write({ 0x75, offset }); }
	void jmp(REG r) { write({ 0xFF, (unsigned char) (0b11100000 | r.val) }); }
	void call(REG r) { write({ 0xFF, (unsigned char) (0b11010000 | r.val) }); }

	void push(REG r) { write({ (unsigned char) (0b01010000 | r.val) }); }
	void pop(REG r) { write({ (unsigned char) (0b01011000 | r.val) }); }

	void jump_if_equal(REG r, REG addr, uint8_t val) {
		cmp(r, val);
		jne(2);
		jmp(addr);
	}

	char* make_patchpoint(REG dst) {
		load(dst, 0);
		return end - 8;
	}

	void patch(char* point, void* addr) {
		uint64_t asInt = (uint64_t) addr;

		for (size_t i=0; i<8; ++i)
			point[i] = (char) ((asInt>>(i*8))&0x00FF);
	}

	void prologue(const std::string& name) {
		std::cerr << "Emitting " << name << ": \n";
        	push(RBP);
        	mov(RBP, RSP);
	}

	void epilogue() {
		pop(RBP);
        	ret();
        	std::cerr << "\n*****\n";
	}
};

extern "C" uint64_t bf_print(register char* data) {
	std::cout << *data;
	return 0;
}

extern "C" uint64_t bf_input(register char* data) {
        std::cin >> *data;
        return 0;
}

// Calling convention: RAX - return
// Input/scratch: RDI, RSI, RDX, RCX, ... (2 more) then stack


// These helpers assume BF data is in RDI
void bf_jit_output(JITFn& f)
{
	f.push(RDI);
	f.load(RAX, (uint64_t) ((void*) bf_print));
	f.call(RAX);
	f.pop(RDI);
}

void bf_jit_input(JITFn& f)
{
        f.push(RDI);
        f.load(RAX, (uint64_t) ((void*) bf_input));
        f.call(RAX);
        f.pop(RDI);
}

void bf_jit_inc_data(JITFn& f)
{
	f.load(RAX, RDI, 0);
	f.load(RSI, 1);
	f.add(RAX, RSI);
	f.store(RDI, 0, RAX);
}

void bf_jit_inc_data_ptr(JITFn& f)
{
        f.load(RSI, 1);
        f.add(RDI, RSI);
}

void bf_jit_dec_data(JITFn& f)
{
        f.load(RAX, RDI, 0);
        f.load(RSI, 1);
        f.sub(RAX, RSI);
        f.store(RDI, 0, RAX);
}

void bf_jit_sub_data_ptr(JITFn& f)
{
        f.load(RSI, 1);
        f.sub(RDI, RSI);
}

char* bf_jit_jump(JITFn& f, void* to)
{
	char* patch = f.make_patchpoint(RSI);
	f.patch(patch, (void*) to);
	f.jmp(RSI);
	return patch;
}

char* bf_jit_jump_if_zero(JITFn& f, void* to)
{
	char* patch = f.make_patchpoint(RSI);
	f.patch(patch, (void*) to);
	f.load(RDX, RDI, 0);
	f.jump_if_equal(RDX, RSI, 0);
	return patch;
}

int main()
{
	JITFn fn;
	fn.prologue("jit_entry");
	auto* begin_loop = fn.current();

	bf_jit_input(fn);
	for (int i=0; i<'h'; ++i) bf_jit_dec_data(fn);
	auto* end_loop_patch = bf_jit_jump_if_zero(fn, (void*)nullptr);
	bf_jit_jump(fn, begin_loop);

	fn.patch(end_loop_patch, fn.current());
	for (int i=0; i<'h'; ++i) bf_jit_inc_data(fn);
	bf_jit_output(fn);
	fn.epilogue();

	uint64_t val = 0;
	fn(&val);

	return 0;
}

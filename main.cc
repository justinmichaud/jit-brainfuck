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
                      4096,             // size
                      PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS,
                      -1,               // fd (not used here)
                      0);               // offset (not used here)
		if (!block)
			throw "Could not allocate executable block";
	}

	~JITFnBlock()
	{
		munmap((void*) block, 4096);
		block = nullptr;
	}

	char* begin() const { return block; }
	char* end() const { return block + 4096; }
};

typedef struct REG_t {
	unsigned char val;
} REG;

const REG RAX = { 0 };
const REG RDI = { 0b0111 };
const REG RSI = { 0b0110 };
const REG RDX = { 0b0010 };
const REG RCX = { 0b0001 };

extern "C" { typedef uint64_t (*jit_fp)(register void*, register uint64_t, register uint64_t, register uint64_t); }

class JITFn {
	std::shared_ptr<JITFnBlock> block;
	char* start;
	char* end;
public:
	void write(std::initializer_list<unsigned char>&& data)
	{
		for (auto c : data) {
			printf("%02x", (int) c);
			assert(end < block->end());
			*end = c;
			++end;
		}
	}

	JITFn()
	{
		block = std::make_shared<JITFnBlock>();
		start = block->begin();
		end = start;
	}

	void* current() const { return end; }

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

};

uint64_t bf_print(char* data) {
	std::cout << "BF output:\n";
	std::cout << *data;
	std::cout << "BF output done\n";
	return 5;
}

// Calling convention: RAX - return
// Input/scratch: RDI, RSI, RDX, RCX, ... (2 more) then stack
void ident(JITFn& f)
{
	std::cerr << "Emitting ident: \n";
	f.load(RAX, RDI, 0);
	f.load(RSI, 2);
	f.add(RAX, RSI);
	f.store(RDI, 0, RAX);

	// TODO save / restore registers
	f.load(RSI, (uint64_t) ((void*) bf_print));
	f.call(RSI);
	
	f.ret();
	std::cerr << "\n*****\n";
}

int main()
{
	JITFn fn;
	ident(fn);

	uint64_t val = 42;

	std::cout << fn(&val) << "\n";
	std::cout << val << "\n";

	//for (int i=0; i<10; ++i)
	//	std::cout << "f(" << i << ") = " << fn(i) << ";\n";
	return 0;
}

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
const REG RDI = { 0 };
const REG RSI = { 0 };
const REG RDX = { 0 };
const REG RCX = { 0 };
const REG SCRATCH[4] = { RDI, RSI, RDX, RCX };

class JITFn {
	std::shared_ptr<JITFnBlock> block;
	char* start;
	char* end;
public:
	void write(std::initializer_list<unsigned char>&& data)
	{
		for (auto c : data) {
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

	uint64_t operator()(uint64_t i)
	{
		return ((uint64_t (*)(uint64_t)) start)(i);
	}

	void ret() { write({ 0xc3 }); }
	void movq(REG dst, REG src) { write({ 0x48, 0x8B, (unsigned char) (0b11000000 | dst.val<<3 | src.val) }); }
	void movq(REG dst, uint64_t src) { write({ 0x48, (unsigned char) (0xB8 | dst.val) }); for (size_t i=0; i<8; ++i) write({ (unsigned char) ((src>>(i*8))&0x00FF) }); }
};

void ident(JITFn& f)
{
	f.movq(RAX, 1234567890123456789);
	f.ret();
}

int main()
{
	JITFn fn;
	ident(fn);

	for (int i=0; i<10; ++i)
		std::cout << "f(" << i << ") = " << fn(i) << ";\n";
	return 0;
}

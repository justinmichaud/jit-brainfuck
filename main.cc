#include <stack>
#include <iostream>
#include <stdlib.h>
#include <map>
#include <memory>
#include <assert.h>
#include <sys/mman.h>
#include <fstream>
#include <string>
#include <streambuf>

constexpr bool debug = false;

class JITFnBlock {
	char* block;

public:
	JITFnBlock()
	{
		block = (char*) mmap(NULL,       // address
                      4096*48,             // size
                      PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS,
                      -1,               // fd (not used here)
                      0);               // offset (not used here)
		if (!block)
			throw "Could not allocate executable block";
	}

	~JITFnBlock()
	{
		munmap((void*) block, 4096*48);
		block = nullptr;
	}

	char* begin() const { return block; }
	char* end() const { return block + 4096*48; }
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
			if (debug) printf("%02x", (int) c);
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

public:
	JITFn(const JITFn&) = delete;
	JITFn(JITFn&&) = default;

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

	void zero(REG dst) { write({ 0x48, 0x31, (unsigned char) (0b11000000 | dst.val<<3 | dst.val) }); }
	void one(REG dst) { assert(dst.val == RAX.val || dst.val == RDX.val); zero(dst); loadb(dst, 1); } // Must have a low 8-bit register

	// Byte versions of above:
	void loadb(REG dst, uint8_t src) { write({ (unsigned char) (0xB0 | dst.val), src }); }
	void loadb(REG dst, REG base, int8_t offset) { write({ 0x8a, (unsigned char) (0b01000000 | dst.val<<3 | base.val), *reinterpret_cast<unsigned char*>(&offset) }); }
	void storeb(REG base, int8_t offset, REG src) { write({ 0x88, (unsigned char) (0b01000000 | src.val<<3 | base.val), *reinterpret_cast<unsigned char*>(&offset) }); }
	void addb(REG dst, REG src) { write({ 0x00, (unsigned char) (0b11000000 | src.val<<3 | dst.val) }); };
	void subb(REG dst, REG src) { write({ 0x28, (unsigned char) (0b11000000 | src.val<<3 | dst.val) }); };

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
		if (debug) std::cerr << "Emitting " << name << ": \n";
        	push(RBP);
        	mov(RBP, RSP);
	}

	void epilogue() {
		pop(RBP);
        	ret();
        	if (debug) std::cerr << "\n*****\n";
	}
};

extern "C" uint64_t bf_print(register char* data) {
	if (debug) std::cerr << "bf_print called with " << std::hex << reinterpret_cast<uint64_t>(data) << " containing " << (uint64_t) (*data) << "\n";
	std::cout << *data;
	std::cout.flush();
	return 0;
}

extern "C" uint64_t bf_input(register char* data) {
	if (debug) std::cerr << "bf_input called\n";
        std::cin >> *data;
        return 0;
}

// Calling convention: RAX - return
// Input/scratch: RDI, RSI, RDX, RCX, ... (2 more) then stack


// These helpers assume BF data is in RDI
void bf_jit_output(JITFn& f)
{
	f.push(RDI);
	f.push(RAX); // Alignment
	f.load(RAX, (uint64_t) ((void*) bf_print));
	f.call(RAX);
	f.pop(RAX);
	f.pop(RDI);
}

void bf_jit_input(JITFn& f)
{
        f.push(RDI);
	f.push(RAX);
        f.load(RAX, (uint64_t) ((void*) bf_input));
        f.call(RAX);
	f.pop(RAX);
        f.pop(RDI);
}

void bf_jit_inc_data(JITFn& f)
{
	f.loadb(RAX, RDI, 0);
	f.loadb(RSI, 1);
	f.addb(RAX, RSI);
	f.storeb(RDI, 0, RAX);
}

void bf_jit_inc_data_ptr(JITFn& f)
{
        f.one(RDX);
        f.add(RDI, RDX);
}

void bf_jit_dec_data(JITFn& f)
{
        f.loadb(RAX, RDI, 0);
        f.loadb(RSI, 1);
        f.subb(RAX, RSI);
        f.storeb(RDI, 0, RAX);
}

void bf_jit_dec_data_ptr(JITFn& f)
{
        f.one(RDX);
        f.sub(RDI, RDX);
}

char* bf_jit_jump(JITFn& f, void* to)
{
	char* patch = f.make_patchpoint(RSI);
	f.patch(patch, to);
	f.jmp(RSI);
	return patch;
}

char* bf_jit_jump_if_zero(JITFn& f, void* to)
{
	char* patch = f.make_patchpoint(RSI);
	f.patch(patch, to);
	f.zero(RDX);
	f.loadb(RDX, RDI, 0);
	f.jump_if_equal(RDX, RSI, 0);
	return patch;
}

JITFn compile_bf(const std::string& program, const std::map<size_t, size_t>& brackets)
{
	JITFn f;
	f.prologue("jit_entry");

	std::map<size_t, char*> patches;
	std::map<size_t, void*> addrs;

	for (size_t i = 0; i < program.length(); ++i) {
		switch(program[i]) {
		case '>':
			bf_jit_inc_data_ptr(f);
			break;
		case '<':
			bf_jit_dec_data_ptr(f);
			break;
		case '+':
			bf_jit_inc_data(f);
			break;
		case '-':
			bf_jit_dec_data(f);
			break;
		case '.':
			bf_jit_output(f);
			break;
		case ',':
			bf_jit_input(f);
			break;
		case '[':
			addrs[i] = f.current();
			patches[i] = bf_jit_jump_if_zero(f, nullptr);
			break;
		case ']':
			patches[i] = bf_jit_jump(f, nullptr);
			addrs[i] = f.current();
			break;
		}
	}

	for (auto pair : patches) {
		f.patch(pair.second, addrs[brackets.at(pair.first)]);
	}

	f.epilogue();
	return std::move(f);
}

int main(int argc, char** argv)
{
	if (argc != 2) {
		std::cerr << "Must give bf file";
		exit(1);
	}
	std::ifstream file(argv[1]);
	std::string program((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	std::map<size_t, size_t> brackets;

	std::stack<size_t> open;
	for (size_t i=0; i<program.length(); ++i) {
		switch(program[i]) {
		case '[':
			open.push(i);
			break;
		case ']':
			brackets[open.top()] = i;
			brackets[i] = open.top();
			open.pop();
			break;
		}
	}
	if (!open.empty()) {
		std::cerr << "Mismatched brackets";
		exit(1);
	}

	JITFn jit_entry = compile_bf(program, brackets);

	char* memory = new char[30000];
	for (size_t i=0; i<30000; ++i)
		memory[i] = 0;
	if (debug) std::cerr << "Memory is located at " << std::hex << reinterpret_cast<uint64_t>(memory) << "\n";
	jit_entry(memory);
	delete[] memory;

	std::cout << "\n";
	return 0;
}

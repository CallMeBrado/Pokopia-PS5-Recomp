#include <nxrecomp/cxx.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <windows.h>

void f_710124ccc0(Ctx*);

void call_indirect(Ctx *, uint64_t target)
{
    std::fprintf(stderr, "Unexpected indirect guest address 0x%llx\n", (unsigned long long)target);
    std::abort();
}

void guest_trap(const char *message, uint64_t target)
{
    std::fprintf(stderr, "Guest trap %s @0x%llx\n", message, (unsigned long long)target);
    std::abort();
}

static std::vector<uint8_t> read_file(const char *path)
{
    FILE *file = std::fopen(path, "rb");
    assert(file);
    assert(std::fseek(file, 0, SEEK_END) == 0);
    const long size = std::ftell(file);
    assert(size > 0);
    rewind(file);
    std::vector<uint8_t> bytes(size);
    assert(std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size());
    std::fclose(file);
    return bytes;
}

static void map_main_data(void)
{
    constexpr uint64_t base = 0x7100000000;
    const auto main = read_file("../../loader-probe/generated/main.bin");
    const void *mapping = VirtualAlloc(reinterpret_cast<void *>(base), main.size(),
                                       MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(reinterpret_cast<uint64_t>(mapping) == base);
    std::memcpy(const_cast<void *>(mapping), main.data(), main.size());
}

static Ctx *current;
static LONG WINAPI fault(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    std::fprintf(stderr, "AV address 0x%llx ip 0x%llx\n",
      (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1],
      (unsigned long long)ep->ContextRecord->Rip);
    if (current)
        for (unsigned i=0;i<32;++i)
            std::fprintf(stderr, "x%u=0x%llx\n", i,(unsigned long long)current->x[i]);
    return EXCEPTION_CONTINUE_SEARCH;
}
static void check(const char *path, uint32_t expected)
{
    const auto bank = read_file(path);
    alignas(16) uint8_t state[1024]{}, config[64]{}, out[120]{}, mixer[640]{}, stack[4096]{};
    const auto addr = [](const void *pointer) { return reinterpret_cast<uint64_t>(pointer); };
    wr64(addr(state+736), addr(config));
    wr64(addr(out+24), addr(mixer));
    Ctx c{};
    c.x[0]=addr(state);
    c.x[1]=addr(bank.data());
    c.x[2]=bank.size();
    c.x[4]=addr(out);
    c.x[31]=addr(stack+sizeof stack);
    current=&c;
    f_710124ccc0(&c);
    current=nullptr;
    assert(c.x[0]==expected && c.x[31]==addr(stack+sizeof stack));
    if (!expected) {
        assert(rd32(addr(out+16))==10);
        assert(rd32(addr(out+32))==2);
        assert(rd32(addr(out+100))==0xc0);
        const uint32_t bits[]={0x3f800000,0x3fff64c1,0x3fff64c1};
        for (unsigned i=0;i<3;++i) assert(rd32(addr(out+104+4*i))==bits[i]);
        std::printf("PASS %s exact generated parser: 10 mixer records, stereo, mask 0xc0, verified gains\n",path);
    } else {
        std::printf("PASS %s exact generated parser: short-header rejection %#llx\n",
                    path, (unsigned long long)c.x[0]);
    }
}

int main()
{
    AddVectoredExceptionHandler(1, fault);
    map_main_data();
    check("compact.bin",0);
    check("short.bin",uint32_t(-0x35));
    check("ranged.bin",0);
}

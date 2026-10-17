#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <array>
#include "loader/jit.h"
#include <xbyak/xbyak.h>

namespace {
constexpr uint64_t ArithmeticMask = 0x8d5; // CF PF AF ZF SF OF; no TF/DF/privileged bits.
constexpr std::array<uint64_t,8> Seed = {0x123456789abcdef0ull,0x1111222233334444ull,
  0x2222333344445555ull,0x3333444455556666ull,0,0x5555666677778888ull,
  0x6666777788889999ull,0x777788889999aaaaull};
constexpr std::array<uint64_t,4> VolatileSeed = {0x8182838485868788ull,0x9192939495969798ull,
  0xa1a2a3a4a5a6a7a8ull,0xb1b2b3b4b5b6b7b8ull};
alignas(16) uint8_t TcbFixture[32]{};
uint64_t CallbackCalls = 0;
uint8_t* KYTY_MS_ABI ReturnFixtureTcb() {
  ++CallbackCalls; // Actual compiled callback may alter arithmetic flags and caller-volatile GPs.
  return TcbFixture;
}
struct Result {
  uint64_t seed_flags, before_flags, after_flags, rsp_before, rsp_after;
  uint64_t gp[8]; // Architectural order RAX RCX RDX RBX RSP RBP RSI RDI.
  uint64_t volatile_gp[4]; // R8 R9 R10 R11.
  uint8_t before_branch[4], after_branch[4]; // Actual JZ/JC/JS/JO paths.
};
using Caller = void (KYTY_MS_ABI *)(const void*, Result*);

struct Fixture : Xbyak::CodeGenerator {
  Loader::Jit::SafeCall* safe = nullptr;
  std::array<Loader::Jit::TlsRegStub*,8> reg_stubs{};
  std::array<Loader::Jit::Call9*,8> near_calls{};
  std::array<const void*,8> mov_controls{};
  explicit Fixture(uint32_t extra_stack) : Xbyak::CodeGenerator(4096) {
    // Native MS ABI entry:RCX selected emitted Call9/control, RDX Result.
    push(rbx); push(rbp); push(rsi); push(rdi);
    push(r12); push(r13); push(r14); push(r15);
    lea(rsp,ptr[rsp-(40+extra_stack)]); // Proper shadow/alignment; no flag-sensitive setup yet.
    mov(r13,rdx); mov(r14,rcx);
    mov(ptr[r13+offsetof(Result,rsp_before)],rsp);
    mov(rax,Seed[0]); mov(rcx,Seed[1]); mov(rdx,Seed[2]); mov(rbx,Seed[3]);
    mov(rbp,Seed[5]); mov(rsi,Seed[6]); mov(rdi,Seed[7]);
    mov(r8,VolatileSeed[0]); mov(r9,VolatileSeed[1]);
    mov(r10,VolatileSeed[2]); mov(r11,VolatileSeed[3]);
    push(qword[r13+offsetof(Result,seed_flags)]); popfq();
    pushfq(); pop(qword[r13+offsetof(Result,before_flags)]);
    Branches(offsetof(Result,before_branch));
    call(r14); // Enters ACTUAL final-address Call9→SafeCall / supported TlsRegStub.
    pushfq(); pop(qword[r13+offsetof(Result,after_flags)]);
    Branches(offsetof(Result,after_branch));
    mov(ptr[r13+offsetof(Result,rsp_after)],rsp);
    for (uint32_t reg=0;reg<8;++reg) mov(ptr[r13+offsetof(Result,gp)+8*reg],Xbyak::Reg64(reg));
    for (uint32_t reg=0;reg<4;++reg) mov(ptr[r13+offsetof(Result,volatile_gp)+8*reg],Xbyak::Reg64(8+reg));
    lea(rsp,ptr[rsp+(40+extra_stack)]);
    pop(r15); pop(r14); pop(r13); pop(r12);
    pop(rdi); pop(rsi); pop(rbp); pop(rbx); ret();

    align(16);
    const size_t safe_offset=getSize();
    safe=reinterpret_cast<Loader::Jit::SafeCall*>(const_cast<uint8_t*>(getCurr()));
    Loader::Jit::SafeCall safe_template;
    db(safe_template.code,sizeof(safe_template.code));
    safe->SetFunc(ReturnFixtureTcb); // Absolute relocation is installed at its final executable address.
    for (const uint8_t reg : {uint8_t{1},uint8_t{2},uint8_t{3},uint8_t{5},uint8_t{6},uint8_t{7}}) {
      while(getSize()<safe_offset+Loader::Jit::TlsRegStub::GetOffset(reg)) nop();
      reg_stubs[reg]=reinterpret_cast<Loader::Jit::TlsRegStub*>(const_cast<uint8_t*>(getCurr()));
      Loader::Jit::TlsRegStub reg_template;
      db(reg_template.code,sizeof(reg_template.code));
      reg_stubs[reg]->SetFunc(safe); // FINAL relative address, never copy a relocated temporary.
      reg_stubs[reg]->SetOutputReg(reg);
    }
    for (const uint8_t reg : {uint8_t{0},uint8_t{1},uint8_t{2},uint8_t{3},uint8_t{5},uint8_t{6},uint8_t{7}}) {
      align(16);
      near_calls[reg]=reinterpret_cast<Loader::Jit::Call9*>(const_cast<uint8_t*>(getCurr()));
      Loader::Jit::Call9 call_template;
      db(call_template.code,sizeof(call_template.code));
      near_calls[reg]->SetFunc(reg==0?reinterpret_cast<void*>(safe):reinterpret_cast<void*>(reg_stubs[reg]));
      ret(); // Stop this path before the independent MOV control; never mask stub output.
      align(16); mov_controls[reg]=getCurr();
      mov(Xbyak::Reg64(reg),reinterpret_cast<uint64_t>(TcbFixture)); ret();
    }
    ready();
  }
  void Branches(size_t offset) {
    for(size_t i=0;i<4;++i){
      Xbyak::Label yes,done;
      if(i==0)jz(yes); else if(i==1)jc(yes); else if(i==2)js(yes); else jo(yes);
      mov(byte[r13+offset+i],0); jmp(done); L(yes); mov(byte[r13+offset+i],1); L(done);
    }
  }
};

bool GoldenRegisters(const Result& r,uint8_t destination) {
  if(r.rsp_before!=r.rsp_after || r.gp[4]!=r.rsp_before) return false;
  for(uint8_t reg=0;reg<8;++reg){
    if(reg==4)continue;
    const uint64_t expected=reg==destination?reinterpret_cast<uint64_t>(TcbFixture):Seed[reg];
    if(r.gp[reg]!=expected)return false;
  }
  for(size_t i=0;i<4;++i)if(r.volatile_gp[i]!=VolatileSeed[i])return false;
  return true;
}
bool GoldenFlags(const Result& r) {
  if((r.before_flags&ArithmeticMask)!=(r.seed_flags&ArithmeticMask) ||
     (r.after_flags&ArithmeticMask)!=(r.before_flags&ArithmeticMask))return false;
  return std::memcmp(r.before_branch,r.after_branch,sizeof(r.before_branch))==0;
}
} // namespace

int main() {
  unsigned observations=0, semantic_controls_failed=0, flags_failed=0;
  constexpr std::array<unsigned,6> Bits={0,2,4,6,7,11};
  for(const uint32_t extra_stack:{0u,16u}){
    Fixture fixture(extra_stack);
    if(Xbyak::GetError()!=0){std::fprintf(stderr,"fixture setup Xbyak error\n");return 2;}
    auto caller=fixture.getCode<Caller>();
    for(const uint8_t destination:{uint8_t{0},uint8_t{1},uint8_t{2},uint8_t{3},uint8_t{5},uint8_t{6},uint8_t{7}}){
      for(unsigned pattern=0;pattern<64;++pattern){
        Result control{},actual{};
        uint64_t flags=0x202; // Reservedbit1/normalIF; only ordinary arithmetic bits vary.
        for(size_t i=0;i<Bits.size();++i)if(pattern&(1u<<i))flags|=uint64_t{1}<<Bits[i];
        control.seed_flags=actual.seed_flags=flags;
        caller(fixture.mov_controls[destination],&control);
        const auto before_calls=CallbackCalls;
        caller(fixture.near_calls[destination],&actual);
        const bool setup=GoldenRegisters(control,destination)&&GoldenFlags(control)&&
            GoldenRegisters(actual,destination)&&CallbackCalls==before_calls+1&&
            (actual.before_flags&ArithmeticMask)==(flags&ArithmeticMask);
        semantic_controls_failed+=!setup;
        flags_failed+=!GoldenFlags(actual);
        ++observations;
      }
    }
  }
  std::printf("actual SafeCall/Call9/TlsRegStub observations=%u callback_calls=%llu controls_failed=%u flags_failed=%u mask=0x%llx\n",
              observations,static_cast<unsigned long long>(CallbackCalls),semantic_controls_failed,
              flags_failed,static_cast<unsigned long long>(ArithmeticMask));
  // Semantic/callback/MOV/RSP/GP controls are assessed BEFORE intended flag regression.
  if(semantic_controls_failed!=0){std::fprintf(stderr,"unrelated setup/ABI failure; NOT valid RED\n");return 2;}
  if(flags_failed!=0){std::fprintf(stderr,"INTENDED RED: replaced MOV/TLS path changes arithmetic FLAGS/branches\n");return 1;}
  std::puts("SafeCallFlags: native shared FLAGS/branches/RSP/GP/callback controls PASS");
  return 0;
}

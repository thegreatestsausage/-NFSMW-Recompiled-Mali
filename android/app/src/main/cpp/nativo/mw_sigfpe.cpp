// mw4droid: diagnostico SIGFPE (division por cero en codigo nativo). El
// manejador imprime el RIP del host y el offset dentro de libmain.so, para
// simbolizar con llvm-symbolizer. Constructor automatico al cargar la lib.
#include <signal.h>
#include <ucontext.h>
#include <dlfcn.h>
#include <cstring>
#include <cstdint>
#include <android/log.h>

namespace {

uint64_t mw_libmain_base = 0;

void MwSigfpeHandler(int signo, siginfo_t*, void* uctx) {
  ucontext_t* uc = reinterpret_cast<ucontext_t*>(uctx);
#if defined(__x86_64__)
  uint64_t rip = static_cast<uint64_t>(uc->uc_mcontext.gregs[16]);  // REG_RIP
#else
  uint64_t rip = 0;
#endif
  if (mw_libmain_base) {
    __android_log_print(ANDROID_LOG_ERROR, "NFSMW-rex",
                        "[sondeo-fpe] SIGFPE host RIP 0x%016llx, offset libmain.so 0x%llx",
                        static_cast<unsigned long long>(rip),
                        static_cast<unsigned long long>(rip - mw_libmain_base));
  } else {
    __android_log_print(ANDROID_LOG_ERROR, "NFSMW-rex",
                        "[sondeo-fpe] SIGFPE host RIP 0x%016llx (sin base)",
                        static_cast<unsigned long long>(rip));
  }
  signal(signo, SIG_DFL);
}

}  // namespace

__attribute__((constructor)) static void MwInstalarSigfpe() {
  Dl_info info;
  if (dladdr(reinterpret_cast<void*>(&MwInstalarSigfpe), &info) && info.dli_fbase) {
    mw_libmain_base = reinterpret_cast<uint64_t>(info.dli_fbase);
  }
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = MwSigfpeHandler;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGFPE, &sa, nullptr);
}

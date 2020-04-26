//------------------------------------------------------------------------------
// JIT.cpp
// Just-in-time code execution
// NOTE: Only included if slang is configured to use LLVM
//
// File is under the MIT license; see LICENSE for details
//------------------------------------------------------------------------------
#include "slang/codegen/JIT.h"

#include <llvm/ExecutionEngine/Orc/LLJIT.h>

using namespace llvm::orc;

namespace slang {

static void report(llvm::Error e) {
    std::string result;
    llvm::raw_string_ostream os(result);
    llvm::logAllUnhandledErrors(std::move(e), os);
    throw std::runtime_error(os.str());
}

template<typename T>
static void report(llvm::Expected<T>& e) {
    report(e.takeError());
}

JIT::JIT() {
    auto result = LLLazyJITBuilder().create();
    if (!result)
        report(result);

    jit = std::move(*result);
}

JIT::~JIT() = default;

void JIT::addCode(GeneratedCode code) {
    auto&& [ctx, module] = code.release();
    auto err = jit->addLazyIRModule(ThreadSafeModule(std::move(module), std::move(ctx)));
    if (err)
        report(std::move(err));
}

int JIT::run() {
    auto sym = jit->lookup("main");
    if (!sym)
        report(sym);

    auto fp = (int (*)())(intptr_t)sym->getAddress();
    return fp();
}

} // namespace slang
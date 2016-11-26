/*
 * llvm C frontend for perf. Support dynamically compile C file
 *
 * Inspired by clang example code:
 * http://llvm.org/svn/llvm-project/cfe/trunk/examples/clang-interpreter/main.cpp
 *
 * Copyright (C) 2016 Wang Nan <wangnan0@huawei.com>
 * Copyright (C) 2016 Huawei Inc.
 */

#include "clang/CodeGen/CodeGenAction.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/IR/Module.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/ManagedStatic.h"
#include <memory>

#include "clang.h"
#include "clang-c.h"

namespace perf {

static std::unique_ptr<llvm::LLVMContext> LLVMCtx;

using namespace clang;

static CompilerInvocation *
createCompilerInvocation(llvm::opt::ArgStringList CFlags, StringRef& Path,
			 DiagnosticsEngine& Diags)
{
	llvm::opt::ArgStringList CCArgs {
		"-cc1",
		"-triple", "bpf-pc-linux",
		"-fsyntax-only",
		"-ferror-limit", "19",
		"-fmessage-length", "127",
		"-O2",
		"-nostdsysteminc",
		"-nobuiltininc",
		"-vectorize-loops",
		"-vectorize-slp",
		"-Wno-unused-value",
		"-Wno-pointer-sign",
		"-x", "c"};

	CCArgs.append(CFlags.begin(), CFlags.end());
	CompilerInvocation *CI = tooling::newInvocation(&Diags, CCArgs);

	FrontendOptions& Opts = CI->getFrontendOpts();
	Opts.Inputs.clear();
	Opts.Inputs.emplace_back(Path, IK_C);
	return CI;
}

static std::unique_ptr<llvm::Module>
getModuleFromSource(llvm::opt::ArgStringList CFlags,
		    StringRef Path, IntrusiveRefCntPtr<vfs::FileSystem> VFS)
{
	CompilerInstance Clang;
	Clang.createDiagnostics();

	Clang.setVirtualFileSystem(&*VFS);

	IntrusiveRefCntPtr<CompilerInvocation> CI =
		createCompilerInvocation(std::move(CFlags), Path,
					 Clang.getDiagnostics());
	Clang.setInvocation(&*CI);

	std::unique_ptr<CodeGenAction> Act(new EmitLLVMOnlyAction(&*LLVMCtx));
	if (!Clang.ExecuteAction(*Act))
		return std::unique_ptr<llvm::Module>(nullptr);

	return Act->takeModule();
}

std::unique_ptr<llvm::Module>
getModuleFromSource(llvm::opt::ArgStringList CFlags,
		    StringRef Name, StringRef Content)
{
	using namespace vfs;

	llvm::IntrusiveRefCntPtr<OverlayFileSystem> OverlayFS(
			new OverlayFileSystem(getRealFileSystem()));
	llvm::IntrusiveRefCntPtr<InMemoryFileSystem> MemFS(
			new InMemoryFileSystem(true));

	/*
	 * pushOverlay helps setting working dir for MemFS. Must call
	 * before addFile.
	 */
	OverlayFS->pushOverlay(MemFS);
	MemFS->addFile(Twine(Name), 0, llvm::MemoryBuffer::getMemBuffer(Content));

	return getModuleFromSource(std::move(CFlags), Name, OverlayFS);
}

std::unique_ptr<llvm::Module>
getModuleFromSource(llvm::opt::ArgStringList CFlags, StringRef Path)
{
	IntrusiveRefCntPtr<vfs::FileSystem> VFS(vfs::getRealFileSystem());
	return getModuleFromSource(std::move(CFlags), Path, VFS);
}

}

extern "C" {
void perf_clang__init(void)
{
	perf::LLVMCtx.reset(new llvm::LLVMContext());
}

void perf_clang__cleanup(void)
{
	perf::LLVMCtx.reset(nullptr);
	llvm::llvm_shutdown();
}
}

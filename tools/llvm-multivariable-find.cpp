#include <cassert>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>
#include <string>

#include "dg/tools/llvm-slicer-opts.h"
#include "dg/tools/llvm-slicer-utils.h"
#include "dg/tools/llvm-slicer.h"

#if LLVM_VERSION_MAJOR >= 4
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#else
#include <llvm/Bitcode/ReaderWriter.h>
#endif

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/raw_ostream.h>

#include "dg/llvm/SystemDependenceGraph/SDG2Dot.h"
#include "dg/util/debug.h"

using namespace dg;
using namespace std;
using namespace llvm;

using llvm::errs;

class GlobalVar {
    string name;//变量名
    int type; //type = 1是普通变量, type = 2是数组变量， type = 3是结构体变量
    set<string> belongtofun;//所属的函数
    set<unsigned int>belongtoline;//所属的行号
    map<string, int> closevar;//存储与它相近的全局变量,以及相近了几次
    set<string> closename;//存储与它名称接近的全局变量
    set<string> brothers;//存储与它有兄弟关系的变量
    map<string, int> datadep;//存储与它有数据依赖的全局变量，以及依赖了几次
    map<string, int> controldep;//存储与它有控制依赖的全局变量，以及依赖了几次
    //以上存储的都是变量名
  public:
    GlobalVar(const string &name, const string &funname) {GlobalVar::name = name, belongtofun.insert(funname); }
    explicit GlobalVar(const string &name) : name(name) {}
    void setName(const string &name)  {GlobalVar::name = name; }
    const string &getName() {return name; }
    void addBelongtofun(string funname) {belongtofun.insert(std::move(funname)); }
    set<string> &getBelongtofun() {return belongtofun; }
    void clearBelongtofun() {belongtofun.clear(); }
    void addbelongtoline(unsigned int line) {belongtoline.insert(line); }
    set<unsigned int> &getBelongtoline() {return belongtoline; }
    void clearBelongtoline() {belongtoline.clear(); }
    int getType() {return type; }
    void setType(int t) {type = t; }
    void addClosedis(const string &that) {closevar[that]++; }
    map<string, int> &getClosedis() {return closevar; }
    void addBrother(const string &that) {brothers.insert(that); }
    set<string> &getBrother() {return brothers; }
    void addClosename(const string &that) {closename.insert(that); }
    set<string> &getClosename() {return closename; }
    void addDatadep(const string &that) {datadep[that]++; }
    map<string, int> &getDatadep() {return datadep; }
    void addControldep(const string &that) {controldep[that]++; }
    map<string, int> &getControldep() {return controldep; }
};
set<GlobalVar> globals;
set<string> funs;
set<tuple<int, int, string>> linetofun;//存储一个函数的startline, endline, funname
llvm::cl::opt<bool> enable_debug(
        "dbg", llvm::cl::desc("Enable debugging messages (default=false)."),
        llvm::cl::init(false), llvm::cl::cat(SlicingOpts));

llvm::cl::opt<bool> dump_bb_only(
        "dump-bb-only",
        llvm::cl::desc("Only dump basic blocks of dependence graph to dot"
                       " (default=false)."),
        llvm::cl::init(false), llvm::cl::cat(SlicingOpts));

int main(int argc, char *argv[]) {
    setupStackTraceOnError(argc, argv);
    SlicerOptions options = parseSlicerOptions(argc, argv);

    if (enable_debug) {
        DBG_ENABLE();
    }

    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> M =
            parseModule("llvm-sdg-dump", context, options);
    if (!M)
        return 1;

    if (!M->getFunction(options.dgOptions.entryFunction)) {
        llvm::errs() << "The entry function not found: "
                     << options.dgOptions.entryFunction << "\n";
        return 1;
    }

    DGLLVMPointerAnalysis PTA(M.get(), options.dgOptions.PTAOptions);
    PTA.run();
    LLVMDataDependenceAnalysis DDA(M.get(), &PTA, options.dgOptions.DDAOptions);
    DDA.run();
    LLVMControlDependenceAnalysis CDA(M.get(), options.dgOptions.CDAOptions);
    // CDA runs on-demand

    llvmdg::SystemDependenceGraph sdg(M.get(), &PTA, &DDA, &CDA);

    for (auto &F : *M) {
        auto *subprogram = F.getSubprogram();
        unsigned int startline, endline;
        if (subprogram) {
            startline = subprogram->getLine();
        }
        auto *lastinst = &(*--F.getBasicBlockList().back().end());
        if (lastinst && lastinst->getDebugLoc()) {
            endline = lastinst->getDebugLoc()->getLine();
        }
        auto funname = F.getName().str();
        if (funname.find("llvm.dbg") != string::npos)
            continue;
        tuple<int, int, string> t;
        std::get<0>(t) = startline;
        std::get<1>(t) = endline;
        std::get<2>(t) = funname;
        linetofun.insert(t);
    }
    for (auto &t : linetofun) {
        outs() << get<0>(t) << " " << get<1>(t) << " " << get<2>(t) << "\n";
    }
    return 0;
}

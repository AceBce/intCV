//
// Created by WZXPC on 2023/7/5.
//
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>

#include <cassert>
#include <cstdio>

#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

#if LLVM_VERSION_MAJOR >= 4
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#else
#include <llvm/Bitcode/ReaderWriter.h>
#endif

#include "dg/PointerAnalysis/PointerAnalysisFI.h"
#include "dg/PointerAnalysis/PointerAnalysisFS.h"
#include "dg/PointerAnalysis/PointerAnalysisFSInv.h"
#include "dg/llvm/DataDependence/DataDependence.h"
#include "dg/llvm/LLVMDG2Dot.h"
#include "dg/llvm/LLVMDependenceGraph.h"
#include "dg/llvm/LLVMDependenceGraphBuilder.h"
#include "dg/llvm/LLVMSlicer.h"
#include "dg/llvm/PointerAnalysis/PointerAnalysis.h"

#include "dg/tools/llvm-slicer-opts.h"
#include "dg/tools/llvm-slicer-utils.h"

#include "dg/util/TimeMeasure.h"



using namespace dg;
using namespace dg::debug;
using namespace std;
using namespace llvm;
using llvm::errs;

llvm::cl::opt<bool> enable_debug(
        "dbg", llvm::cl::desc("Enable debugging messages (default=false)."),
        llvm::cl::init(false), llvm::cl::cat(SlicingOpts));

int main(int argc, char *argv[]) {
    //读入json文件
    //解析json文件
    // 假设关联变量存储在vector<set>中
    vector<set<string>> varSet;
    setupStackTraceOnError(argc, argv);
    SlicerOptions options = parseSlicerOptions(argc, argv);

    if (enable_debug) {
        DBG_ENABLE();
    }

    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> M =
            parseModule("llvm-dg-dump", context, options);
    if (!M)
        return 1;
    // 遍历每个函数
    for (auto &F : *M) {
        // 获取函数名
        string funcName = F.getName().str();
        // 判断是不是中断函数
        if (funcName.find("interrupt") == string::npos) {
            // 如果是中断函数,检查中断里访问了哪些关联变量
            // 遍历这个函数
            for (auto &BB : F) {
                // 遍历这个基本块
                for (auto &I : BB) {
                    // 获取这条指令访问的变量
                    

                }
            }
        }
    }
}
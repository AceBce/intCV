//
// Created by WZX on 2023/7/5.
//
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>

#include <cassert>
#include <cstdio>
#include "json.h"

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

// 一个关联变量组
class CorrelatedVars {
    int len; // 这个组里有len个变量
    set<string> vars; // 这个组里的变量
  public:
    explicit CorrelatedVars(const set<string> &vars) : vars(vars) {
        len = vars.size();
    }
};

// 存放所有关联变量
// 用于检查中断里访问的变量是否是具有关联的变量
class AllCorrelatedVars {
    set<string> vars; // 所有关联变量
    int len; // 所有关联变量的个数
  public:
    explicit AllCorrelatedVars(const set<string> &vars) : vars(vars) {
        len = vars.size();
    }
    AllCorrelatedVars() {
        len = 0;
    }
    bool find(string varName) {
        return vars.find(varName) != vars.end();
    }
};

// 对变量的访问
class varVisit {
    string varName; // 变量名
    int visitMode; // 访问模式 0:读 1:写
    long long int line; // 行号
    set<string> *correlatedVars{}; // 指向该变量所属的关联变量组
    public:
    varVisit(const string &varName, int visitMode, long long int line,
             set<string> *correlatedVars)
            : varName(varName), visitMode(visitMode), line(line),
              correlatedVars(correlatedVars) {}
    varVisit(const string &varName, int visitMode, long long int line)
            : varName(varName), visitMode(visitMode), line(line) {}
    varVisit(string varName, int visitMode) : varName(varName), visitMode(visitMode) {}
};

// 存放某中断函数里的所有关联变量以及访问模式
class InterruptVars {
    string funcName; // 中断函数名
    Function *func; // 中断函数
    vector<varVisit> varVisits; // 中断函数里的所有关联变量以及访问模式
  public:
    InterruptVars(const string &funcName, Function *func,
                  const vector<varVisit> &varVisits)
            : funcName(funcName), func(func), varVisits(varVisits) {}
    InterruptVars() {}
    explicit InterruptVars(Function *func) : func(func) {funcName = func->getName().str();}
    void addVarVisit(const varVisit &varVisit) {
        varVisits.push_back(varVisit);
    }
};

llvm::cl::opt<bool> enable_debug(
        "dbg", llvm::cl::desc("Enable debugging messages (default=false)."),
        llvm::cl::init(false), llvm::cl::cat(SlicingOpts));

int main(int argc, char *argv[]) {
    //读入json文件
    Json::Reader reader;
    //解析json文件
    // 假设所有关联变量存储在中allVarSet中
    vector<set<string>> allVarSet;
    // 把中断里访问的变量存储到varSet中
    vector<set<string>> varSet;
    // 检查一个变量属于哪个变量组
    // 假设已经填好了
    unordered_map<string, set<string>> variableToGroup;
    AllCorrelatedVars allCorrelatedVars;
    // 放一个存储所有中断函数的容器
    vector<InterruptVars*> interruptVars;

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
        if (funcName.find("interrupt") != string::npos) {
            // 如果是中断函数,检查中断里访问了哪些关联变量
            // 获取这个函数的所有关联变量
            InterruptVars *interruptVar = new InterruptVars(&F);
            // 遍历这个函数
            for (auto &BB : F) {
                // 遍历这个基本块
                for (auto &I : BB) {
                    // 获取这条指令访问的变量
                    for (int i = 0; i < I.getNumOperands(); i++) {
                        auto *op = I.getOperand(i);
                        string varName = op->getName().str();
                        // 判断这个变量是不是关联变量
                        if (allCorrelatedVars.find(varName)) {
                            // 如果是关联变量,获取对这个变量的访问
                            // 判断读写类型
                            int visitMode = 0; // 0:读 1:写
                            if (I.getOpcode() == Instruction::Load) {
                                // 如果是读,把这个变量加入到varSet中
                                visitMode = 0;
                            } else if (I.getOpcode() == Instruction::Store) {
                                // 如果是写,把这个变量加入到varSet中
                                visitMode = 1;
                            } else if (I.getOpcode() == Instruction::Call) {
                                // 如果是读,把这个变量加入到varSet中
                                visitMode = 0;
                            }
                            // 获取行号
                            long long int line = I.getDebugLoc().getLine();
                            // 添加它属于哪个变量组
                            set<string> *correlatedVars = &variableToGroup[varName];
                            // 添加一个访问
                            varVisit visit(varName, visitMode, line, correlatedVars);
                            interruptVar->addVarVisit(visit);
                        }
                        // 如果不是就不管
                        outs() << op->getName() << "\n";
                    }
                }
            }
        }
    }
    interruptVars; // 这里面应该存放了所有的中断函数，存放了这个函数访问的所有关联变量和它所属的变量组
    // 应该把所有的中断函数里访问的关联变量组都收集起来，然后在主程序里找有没有对这些变量的访问,这主要是为了在主程序里划定原子区
    for (auto &F : *M) {
        string funcName = F.getName().str();
        if (funcName.find("interrupt") == string::npos) {
            // 不是中断函数就检查

        }
    }
}
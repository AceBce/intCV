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
#include <llvm/IR/Operator.h>

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
    string basename;//如果是数组或是结构体的话就有basename，eg: f[3]的basename为f
    int id;
    set<string> belongtofun;//所属的函数
    set<unsigned int>belongtoline;//所属的行号
    map<string, int> closevar;//存储与它相近的全局变量,以及相近了几次
    set<string> closename;//存储与它名称接近的全局变量
    set<string> brothers;//存储与它有兄弟关系的变量
    map<string, int> datadep;//存储与它有数据依赖的全局变量，以及依赖了几次
    map<string, int> controldep;//存储与它有控制依赖的全局变量，以及依赖了几次
    //以上存储的都是变量名
    set<dg::sdg::DGNode*> nodes;//存储它出现过的节点
  public:
    GlobalVar(const string &name, const string &funname) {GlobalVar::name = name, belongtofun.insert(funname); }
    explicit GlobalVar(const string &name) : name(name) {}
    void setName(const string &name)  {GlobalVar::name = name; }
    const string &getName() {return name; }
    void setBaseName(const string &name) {basename = name; }
    string &getBaseName() {return basename;;}
    void setID(int id) {this->id = id; }
    int getID() {return id; }
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
    void addnode(dg::sdg::DGNode *n) {nodes.insert(n); }
    set<dg::sdg::DGNode*> getnode() {return nodes; }
};
struct FunctionInfo {
    string funname;
    unsigned int startline, endline;
    vector<pair<string, int>> vars;
};
int codedistance = 10;
map<string, set<string>> basenametoname;
map<string, GlobalVar*> nametovar;
set<FunctionInfo*> funinfos;
struct Gvarpointercmp {
    bool operator () (GlobalVar *Gvar1, GlobalVar *Gvar2) {
        return Gvar1->getName() < Gvar2->getName();
    }
};
set<GlobalVar*, Gvarpointercmp> globals;
set<string> funs;
set<tuple<int, int, string>> linetofun;//存储一个函数的startline, endline, funname
map<string, set<GlobalVar*>> funhasglobalvar;//函数里有哪些变量
map<Instruction*, set<GlobalVar*>> Instrtovar;//指令里有哪些变量
llvm::cl::opt<bool> enable_debug(
        "dbg", llvm::cl::desc("Enable debugging messages (default=false)."),
        llvm::cl::init(false), llvm::cl::cat(SlicingOpts));

llvm::cl::opt<bool> dump_bb_only(
        "dump-bb-only",
        llvm::cl::desc("Only dump basic blocks of dependence graph to dot"
                       " (default=false)."),
        llvm::cl::init(false), llvm::cl::cat(SlicingOpts));
bool isStructArray(Type* type);//递归判断是不是结构体数组
bool isCloseName(const string &name1, const string &name2);
void Dep(llvmdg::SystemDependenceGraph &sdg, GlobalVar *Gvar);
void controldep(llvmdg::SystemDependenceGraph &sdg, dg::sdg::DGNode *node, GlobalVar *Gvar);
void datadep(llvmdg::SystemDependenceGraph &sdg, dg::sdg::DGNode *node, GlobalVar *Gvar);
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
    int id = 1;
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
        FunctionInfo *funinfo = new FunctionInfo();
        funinfo->funname = funname;
        funinfo->startline = startline;
        funinfo->endline = endline;
        funinfos.insert(funinfo);
        vector<pair<string, int>> funvars;

        for (auto &BB : F) {
            for (auto &I : BB) {
                if (GetElementPtrInst *gepinst = dyn_cast<GetElementPtrInst>(&I)) {
                    //数组下标是变量
                    if (auto *g0 = gepinst->getOperand(0)) {
                        if (isa<GlobalVariable>(g0)) {
//                            int nums = gepinst->getNumOperands();
//                            GlobalVariable *GV = dyn_cast<GlobalVariable>(g0);
//                            outs() << GV->getName().str() << "\n";
                        }
                    }

                }
                else {
                    int nums = I.getNumOperands();
                    for (int i = 0; i < nums; i++) {
                        auto *op = I.getOperand(i);
                        if (isa<GlobalVariable>(op)) {
                            GlobalVariable *GV = dyn_cast<GlobalVariable>(op);
                            string name = GV->getName().str();
                            GlobalVar *Gvar = new GlobalVar(name);
                            if (I.getDebugLoc()) {
                                unsigned line;
                                line = I.getDebugLoc().getLine();
                                Gvar->addbelongtoline(line);
                                funvars.push_back({name, line});
                            }
                            Gvar->setID(id++);
                            Gvar->addBelongtofun(funname);
                            globals.insert(Gvar);
                            nametovar.insert({name, Gvar});
                            auto it = Instrtovar.find(&I);
                            if (it != Instrtovar.end()) {
                                Instrtovar[&I].insert(Gvar);
                            }
                            else {
                                set<GlobalVar*> s;
                                s.insert(Gvar);
                                Instrtovar[&I] = s;
                            }
                        }
                        if (isa<GEPOperator>(op)) {
                            GEPOperator *GEPOp = dyn_cast<GEPOperator>(op);
                            int num = GEPOp->getNumOperands();
                            int level = num - 2; //剩下的是偏移的层数
                            auto *basevalue = GEPOp->getOperand(0);
                            if (!isa<GlobalVariable>(basevalue)) {
                                continue;
                            }
                            auto *basevariable = dyn_cast<GlobalVariable>(basevalue);
                            if (auto *PT = dyn_cast<PointerType>(basevalue->getType())) {
                                string fullname = "";
                                fullname += basevalue->getName().str();
                                string base = fullname;
                                if (auto *AT = dyn_cast<ArrayType>(PT->getElementType())) {
                                    //是数组
                                    Type *ElementTypeOfAT = AT->getElementType();
                                    string plusname;


                                    if (auto *Idx = GEPOp->getOperand(2)) {
                                        //获取第一个维度的下标
                                        if (auto *CIdx = dyn_cast<ConstantInt>(Idx)) {
                                            unsigned idx = CIdx->getZExtValue();
                                            fullname += "[" + to_string(idx) + "]";
                                        }
                                        else {
                                            //下标不是一个ConstantInt

                                        }
                                    }
                                    //判断操作数的类型，是不是高维数组
                                    Value *ope = GEPOp->getOperand(0);
                                    //outs() << *ope << "\n";
                                    int dimension = 1;//记录数组的维度
                                    if (auto *pointertype = dyn_cast<PointerType>(ope->getType())) {
                                        if (auto *arraytype = dyn_cast<ArrayType>(pointertype->getElementType())) {
                                            while (auto *nexttype = dyn_cast<ArrayType>(arraytype->getElementType())) {
                                                dimension++;
                                                arraytype = nexttype;
                                            }
                                        }
                                    }
                                    int tempdimension = dimension;
                                    for (unsigned int i = 3; i < GEPOp->getNumOperands() && dimension > 1; i++, dimension--) {
                                        //获取其他维度下标
                                        if (auto *Idx = GEPOp->getOperand(i)) {
                                            if (auto *CIdx = dyn_cast<ConstantInt>(Idx)) {
                                                unsigned idx = CIdx->getZExtValue();
                                                fullname += "[" + to_string(idx) + "]";
                                            }
                                        }
                                    }
                                    if (isStructArray(ElementTypeOfAT)) {
                                        //是结构体数组
                                        level = level - tempdimension + 1;
                                        auto mn = dyn_cast<MDNode>(basevariable->getMetadata("dbg")->getOperand(0));
                                        if (auto *mn2 = dyn_cast<MDNode>(mn->getOperand(3))) {
                                            if (auto *mn3 = dyn_cast<MDNode>(mn2->getOperand(3))) {
                                                if (auto *mn3dot5 = dyn_cast<MDNode>(mn3->getOperand(4))) {
                                                    function<void(int, MDNode*)> f = [&] (int depth, MDNode* mn0) { //处理结构体嵌套
                                                        if (depth == level)
                                                            return;
                                                        unsigned idx = cast<ConstantInt>(GEPOp->getOperand(tempdimension + 1 + depth))->getZExtValue();
                                                        if (auto *mn4 = dyn_cast<MDNode>(mn0->getOperand(idx))) {
                                                            if (MDString *mds = dyn_cast<MDString>(mn4->getOperand(2))) {
                                                                plusname = plusname + "." + mds->getString().str();
                                                            }
                                                            if (depth + 1 == level)
                                                                return;
                                                            if (auto *mn5 = dyn_cast<MDNode>(mn4->getOperand(3))) {
                                                                if (auto *mn6 = dyn_cast<MDNode>(mn5->getOperand(4))) {
                                                                    f(depth + 1, mn6);
                                                                }
                                                            }
                                                        }
                                                    };
                                                    f(1, mn3dot5);
                                                }

                                            }
                                        }
                                    }
                                    fullname += plusname;
                                    GlobalVar *Gvar = new GlobalVar(fullname);
                                    Gvar->setID(id++);
                                    Gvar->setType(2);
                                    Gvar->addBelongtofun(funname);
                                    Gvar->setBaseName(base);
                                    if (I.getDebugLoc()) {
                                        unsigned line = I.getDebugLoc().getLine();
                                        Gvar->addbelongtoline(line);
                                        funvars.push_back({fullname, line});
                                    }
                                    globals.insert(Gvar);
                                    nametovar.insert({fullname, Gvar});
                                    if (basenametoname.find(base) != basenametoname.end()) {
                                        basenametoname[base].insert(fullname);
                                    }
                                    else {
                                        set<string> s;
                                        s.insert(fullname);
                                        basenametoname[base] = s;
                                    }
                                    auto it = Instrtovar.find(&I);
                                    if (it != Instrtovar.end()) {
                                        Instrtovar[&I].insert(Gvar);
                                    }
                                    else {
                                        set<GlobalVar*> s;
                                        s.insert(Gvar);
                                        Instrtovar[&I] = s;
                                    }
                                }
                                else if (auto *ST = dyn_cast<StructType>(PT->getElementType())) {
                                    //是结构体
                                    string structname = std::move(fullname);
                                    //成员的名称
                                    string fieldname;

                                    auto mn = dyn_cast<MDNode>(basevariable->getMetadata("dbg")->getOperand(0));
                                    if (auto *mn2 = dyn_cast<MDNode>(mn->getOperand(3))) {
                                        if (auto *mn3 = dyn_cast<MDNode>(mn2->getOperand(4))) {
                                            function<void(int, MDNode*)> f = [&] (int depth, MDNode* mn0) { //处理结构体嵌套
                                                if (depth == level)
                                                    return;
                                                unsigned idx = cast<ConstantInt>(GEPOp->getOperand(2 + depth))->getZExtValue();
                                                if (auto *mn4 = dyn_cast<MDNode>(mn0->getOperand(idx))) {
                                                    if (MDString *mds = dyn_cast<MDString>(mn4->getOperand(2))) {
                                                        fieldname = fieldname + "." + mds->getString().str();
                                                    }
                                                    if (depth + 1 == level)
                                                        return;
                                                    if (auto *mn5 = dyn_cast<MDNode>(mn4->getOperand(3))) {
                                                        if (auto *mn6 = dyn_cast<MDNode>(mn5->getOperand(4))) {
                                                            f(depth + 1, mn6);
                                                        }
                                                    }
                                                }
                                            };
                                            f(0, mn3);
                                        }
                                    }
                                    fullname = structname + fieldname;
                                    GlobalVar *Gvar = new GlobalVar(fullname);
                                    Gvar->setID(id++);
                                    Gvar->setType(2);
                                    Gvar->setBaseName(base);
                                    Gvar->addBelongtofun(funname);
                                    if (I.getDebugLoc()) {
                                        unsigned line = I.getDebugLoc().getLine();
                                        Gvar->addbelongtoline(line);
                                        funvars.push_back({fullname, line});
                                    }
                                    globals.insert(Gvar);
                                    nametovar.insert({fullname, Gvar});
                                    if (basenametoname.find(base) != basenametoname.end()) {
                                        basenametoname[base].insert(fullname);
                                    }
                                    else {
                                        set<string> s;
                                        s.insert(fullname);
                                        basenametoname[base] = s;
                                    }
                                    auto it = Instrtovar.find(&I);
                                    if (it != Instrtovar.end()) {
                                        Instrtovar[&I].insert(Gvar);
                                    }
                                    else {
                                        set<GlobalVar*> s;
                                        s.insert(Gvar);
                                        Instrtovar[&I] = s;
                                    }
                                }
                            }
                        }
                    }
                }

            }
        }
        funinfo->vars = funvars;
    }

    //先搜集一下距离近的变量
//    for (auto *f : funinfos) {
//        sort(f->vars.begin(), f->vars.end(), [&] (const pair<string, int> &p1, const pair<string, int> &p2) -> bool {
//            return p1.second < p2.second;
//        });
//
//        for (auto &p : f->vars) {
//            for (auto &q : f->vars) {
//                if (p == q)
//                    continue;
//                if (q.first == p.first)
//                    continue;
//                if (q.second >= p.second - codedistance / 2 && q.second <= p.second + codedistance / 2) {
//                    //搞一个容器装所有的全局变量名字，再进行搜索
//                    //map<string, GlobalVar*>
//                    //添加每个GlobalVar的codeclose
//                    nametovar[p.first]->addClosedis(q.first);
//                }
//            }
//        }
//    }
//    for (auto &i : globals) {
//        outs() << i->getName() << ":\n";
//        if (i->getClosedis().empty()) {
//            continue;
//        }
//        for (auto &j : i->getClosedis()) {
//            outs() << j.first << " " << j.second << " times\n";
//        }
//    }

    //搜集具有兄弟关系,相似命名的变量
    for (auto &g : globals) {
        if (g->getType() == 1)
            continue;
        auto it = basenametoname.find(g->getBaseName());
        if (it != basenametoname.end()) {
            for (auto &bg : it->second) {
                if (bg == g->getName())
                    continue ;
                g->addBrother(bg);
            }
        }
        for (auto &g2 : globals) {
            string gn = g->getName(), g2n = g2->getName();
            if (g2n == gn)
                continue;
            if (isCloseName(gn, g2n)) {
                g->addClosename(g2n);
            }
        }
    }
    //输出兄弟关系的变量
    for (auto &g : globals) {
        outs() << g->getName() << ": ";
        for (auto &gb : g->getBrother()) {
            outs() << gb << " ";
        }
        outs() << "\n";
    }
    //搜集具有相似命名的变量
//    for (auto &g : globals) {
//        outs() << g->getName() << ": ";
//        for (auto &cn : g->getClosename()) {
//            outs() << cn << " ";
//        }
//        outs() << "\n";
//    }
    //搜集具有依赖关系的变量
    //先找每个变量对应哪些节点
    for (auto *dg : sdg.getSDG()) {
        for (auto *node : dg->getNodes()) {
            if (Value *v = sdg.getValue(node)) {
//                outs() << *v << "\n";
                if (auto *inst = dyn_cast<Instruction>(v)) {
                    auto it = Instrtovar.find(inst);
                    if (it != Instrtovar.end()) {
                        for (auto &g : it->second) {
//                            outs() << g->getName() << "\n";
                            g->addnode(node);
                        }
                    }
                }
            }
        }
    }
    //计算依赖
    for (auto &g : globals) {
        outs() << g->getName() << ": ";
        for (auto &node : g->getnode()) {
            outs() << node << " ";
        }
        outs() << "\n";
    }
    //输出ControlDep
    for (auto &g : globals) {
        outs() << g->getName() << ": ";
        for (auto &p : g->getControldep()) {
            outs() << p.first << " for " << p.second << " times ";
        }
        outs() << "\n";
    }
    return 0;
}
bool isStructArray(Type* type) {
    if (ArrayType* arrayType = dyn_cast<ArrayType>(type)) {
        Type* elementType = arrayType->getElementType();
        return isStructArray(elementType);
    } else if (llvm::StructType* structType = dyn_cast<StructType>(type)) {
        // 全局变量是结构体类型
        return true;
    }
    return false;
}
bool isCloseName(const string &name1, const string &name2) {
    //ddd[0]还是会匹配到d
    size_t len1 = name1.size(), len2 = name2.size();
    size_t minlen, i;
    if (len1 < len2) {
        minlen = len1;
        if (name2.find(name1) == 0) {
            return true;
        }
        for (i = 0; i < minlen; i++) {
            if (name1[i] != name2[i]) {
                break;
            }
        }
    }
    else if (len1 > len2) {
        minlen = len2;
        if (name1.find(name2) == 0) {
            return true;
        }
        for (i = 0; i < minlen; i++) {
            if (name1[i] != name2[i]) {
                break;
            }
        }
    }
    else {
        if (name1 == name2)
            return true;
        for (i = 0; i < len1; i++) {
            if (name1[i] != name2[i]) {
                break;
            }
        }

    }
    if (i == 0)
        return false;
    if (i == minlen)
        i--;
    string prefix = name1.substr(0, i);
    char lastchar = prefix.back();
    if (lastchar == '[' || lastchar == '.')
        return true;
    if (i < minlen && isalpha(lastchar) && (islower(lastchar) && isupper(name1[i + 1]) || isupper(lastchar) && islower(name1[i + 1])))
        return true;
    return false;
}
void Dep(llvmdg::SystemDependenceGraph &sdg, GlobalVar *Gvar) {
    for (auto *node : Gvar->getnode()) {
        controldep(sdg, node, Gvar);
//        datadep(sdg, node, Gvar);
    }
}
map<dg::sdg::DGNode*, int> visited;//visited[node] = 1是访问过的
set<dg::sdg::DGNode*> tofindcomesfrom;
void findcontrolflow(llvmdg::SystemDependenceGraph &sdg, dg::sdg::DGNode *node) {
    if (visited[node] == 1)
        return ;
    visited[node] = 1;
    auto *dg_block = node->getBBlock();
    for (auto *controlelement : dg_block->control_deps()) {
        if (controlelement == node)
            continue ;
        auto thisid = node->getID();
        auto thatid = dg::sdg::DGBBlock::get(controlelement)->back()->getID();
        if (thatid > thisid)
            continue ;
        if (auto *conblock = dg::sdg::DGBBlock::get(controlelement)) {
            if (auto *value = sdg.getValue(conblock->back())) {
                if (auto *inst = dyn_cast<Instruction>(value)) {
                    if (auto *brinst = dyn_cast<BranchInst>(inst)) {
                        if (!brinst->isConditional())
                            continue;
                        Value *curnodevalue = sdg.getValue(node);
                        Instruction *curnodeinst = dyn_cast<Instruction>(curnodevalue);
                        BasicBlock *block = nullptr;
                        if (curnodeinst->getParent() == brinst->getSuccessor(0)) {
                            block = brinst->getSuccessor(0);
                        }
                        else if (curnodeinst->getParent() == brinst->getSuccessor(1)) {
                            block = brinst->getSuccessor(1);
                        }
                        string labelname0;
                        if (block && block->hasName()) {
                            labelname0 = block->getName().str();
                            if (auto it = labelname0.find(".end")) {
                                if (it != std::string::npos) {
                                    //通往end分支，已经不属于这个控制范围了
                                    return;
                                }
                            }
                        }
                        auto *v = brinst->getOperand(0);
                        //outs() << *v << "\n";
                        tofindcomesfrom.insert(conblock->back());

                    }
                    else if (auto *switchinst = dyn_cast<SwitchInst>(inst)) {
                        Value *v = switchinst->getCondition();
                        //outs() << *v << "\n";
                        tofindcomesfrom.insert(conblock->back());
                    }
                }
            }
        }
    }
}
void findcontrolvar(llvmdg::SystemDependenceGraph &sdg, dg::sdg::DGNode * dg_node1, dg::sdg::DGNode *dg_node2, GlobalVar *Gvar) {
    if (visited[dg_node1] == 1)
        return ;
    visited[dg_node1] = 1;
    int thisid = dg_node1->getID();
    int thatid = 0;
    for (auto *usedep : dg_node1->users()) {
        if (auto *value = sdg.getValue(usedep)) {
            thatid = usedep->getID();
            if (thatid > thisid)
                continue;
            findcontrolvar(sdg, dg::sdg::DGNode::get(usedep), dg_node1, Gvar);
        }
    }
    if (Value *v = sdg.getValue(dg_node1)) {
        if (Instruction *inst = dyn_cast<Instruction>(v)) {
            if (inst && isa<LoadInst>(inst)) {
                LoadInst *loadInst = dyn_cast<LoadInst>(inst);
                Value *from = loadInst->getOperand(0);
                if (from && isa<GlobalVariable>(from)) {
                    if (from->hasName()) {
                        Gvar->addControldep(from->getName().str());
                    }
                }
            }
            else if (inst && isa<CallInst>(inst)) {
                CallInst *Cinst = dyn_cast<CallInst>(inst);
                int nums = Cinst->getNumArgOperands();
                for (int i = 0; i < nums; i++) {
                    Value *from = Cinst->getArgOperand(i);
                    if (from->hasName()) {
                        //outs() << from->getName() << " is from " << line << "\n";
                        Gvar->addControldep(from->getName().str());
                    }
                }
            }
        }
    }
}
void controldep(llvmdg::SystemDependenceGraph &sdg, dg::sdg::DGNode *node, GlobalVar *Gvar) {
    if (!node)
        return;
    visited.clear();
    findcontrolflow(sdg, node);
    visited.clear();
    for (auto *dg_node : tofindcomesfrom) {
        findcontrolvar(sdg, dg_node, nullptr, Gvar);
    }
}
void datadep(llvmdg::SystemDependenceGraph &sdg, dg::sdg::DGNode *node, GlobalVar *Gvar) {
    if (!node)
        return;
    map<dg::sdg::DGNode*, int> visited;//visited[node] = 1是访问过的
}
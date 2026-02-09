#include "RISCV.h"
#include "llvm/ADT/Statistic.h"
#include "RISCVInstrInfo.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "RISCVTargetMachine.h"
#include "llvm/Support/Debug.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include<vector>
#include<iostream>
#include<fstream>
#include<sstream>
#include<string>
//#include "llvm/Target/TargetRegisterInfo.h"

using namespace llvm;

#define RISCV_MIR_TO_SMT_VEC_PASS_NAME "MIR TO SMT supporting vector instructions"
#define DEBUG_TYPE "smt-vec"
STATISTIC(NumMachineInstrs, "Number of machine instructions");
STATISTIC(NumMachineBBs, "Number of machine basic blocks");
STATISTIC(NumScalarMemInstrs, "Number of scalar memory instructions (load/store)");
STATISTIC(NumScalarLoadInstrs, "Number of scalar memory load instructions");
STATISTIC(NumScalarStoreInstrs, "Number of scalar memory store instructions");
STATISTIC(NumVectorMemInstrs, "Number of vector memory instructions (load/store)");
STATISTIC(NumVectorLoadInstrs, "Number of vector memory load instructions");
STATISTIC(NumVectorStoreInstrs, "Number of vector memory store instructions");

static cl::opt<bool> EnableMirToSMTvec(
    "smt-generation-vec",
    cl::desc("Enable MIR to SMT transformation of every function in one SMT file, setting loop bounds and accounting for vector memory instructions"), cl::init(false),
    cl::Hidden);

namespace {

class RISCVmirToSMTvec : public MachineFunctionPass {
public:
    static char ID;
    MachineLoopInfo *MLI;   

    RISCVmirToSMTvec() : MachineFunctionPass(ID) {
        initializeRISCVmirToSMTvecPass(*PassRegistry::getPassRegistry());
    }

    // VLEN configuration: adjust based on your target hardware
    // Common values: 128, 256, 512, 1024
    static const unsigned VLEN = 1024;  // Vector register length in bits

    // Function to calculate VLMAX for different element widths
    // VLMAX = (VLEN / SEW) * LMUL
    // For conservative analysis, we assume LMUL=1 and calculate based on SEW
    static unsigned getVLMAX(unsigned elementWidth) {
    return VLEN / elementWidth;
    }

  // Functions to extract information from opcode names
  static unsigned extractSEW(StringRef opcodeName) {
  // Extract element width from name like "VLE32" or "VLSEG2E64"
  if (opcodeName.contains("E8") || opcodeName.ends_with("8_V")) return 8;
  if (opcodeName.contains("E16") || opcodeName.ends_with("16_V")) return 16;
  if (opcodeName.contains("E32") || opcodeName.ends_with("32_V")) return 32;
  if (opcodeName.contains("E64") || opcodeName.ends_with("64_V")) return 64;
  
  // Extract from patterns like "VLE8_V" or "PseudoVLE8_V_M1"
  if (opcodeName.contains("8_V")) return 8;
  if (opcodeName.contains("16_V")) return 16;
  if (opcodeName.contains("32_V")) return 32;
  if (opcodeName.contains("64_V")) return 64;
  // For whole register ops without E prefix
  // Look for patterns in the name
  size_t pos = opcodeName.find("RE");
  if (pos != StringRef::npos && pos > 2) {
    // Check digit before "RE"
    char digit = opcodeName[pos-1];
    if (digit == '8') return 8;
    if (digit == '6' && opcodeName[pos-2] == '1') return 16;
    if (digit == '2' && opcodeName[pos-2] == '3') return 32;
    if (digit == '4' && opcodeName[pos-2] == '6') return 64;
  }
  
  return 32; // Default fallback
  }

  static unsigned extractNumFields(StringRef opcodeName) {
  // Extract number of fields from segmented instructions like "VLSEG2E64"
  if (opcodeName.contains("SEG2")) return 2;
  if (opcodeName.contains("SEG3")) return 3;
  if (opcodeName.contains("SEG4")) return 4;
  if (opcodeName.contains("SEG5")) return 5;
  if (opcodeName.contains("SEG6")) return 6;
  if (opcodeName.contains("SEG7")) return 7;
  if (opcodeName.contains("SEG8")) return 8;
  return 1; // Non-segmented
  }

  static unsigned extractNumRegisters(StringRef opcodeName) {
  // Extract number of registers from whole register loads like "VL2RE32"
  if (opcodeName.contains("1RE") || opcodeName.contains("1R_V")) return 1;
  if (opcodeName.contains("2RE") || opcodeName.contains("2R_V")) return 2;
  if (opcodeName.contains("4RE") || opcodeName.contains("4R_V")) return 4;
  if (opcodeName.contains("8RE") || opcodeName.contains("8R_V")) return 8;
  return 1; // Default
  }

    std::string printCFGtoSMT(std::vector<std::vector<int>>);
    std::string printTrace(std::map<int,std::vector<int>>);
    std::string printTraceVector(std::map<int,std::vector<int>>);
    std::string printTraceVectorU(std::map<int,std::vector<int>>);
    std::ofstream MIRtoSMTLIB2(std::vector<std::vector<int>>, llvm::StringRef);
    std::ofstream MIRtoSMTLIB2Mem(std::vector<std::vector<int>>, llvm::StringRef, std::map<int,std::vector<int>> BinaryTraceScalar, std::map<int,std::vector<int>> BinaryTraceVector, std::map<int,std::vector<int>> BinaryTraceVectorU, std::map<int,std::string> exitMBBs, std::map<int,std::vector<std::string>> fctCalls, std::vector<std::tuple<std::string,unsigned int>> loops, std::map<int,std::vector<std::string>> loopTrace);
    std::vector<std::string> fctCallsMBB(MachineBasicBlock&);
    std::string exitMBB(MachineBasicBlock&);
    std::ofstream functionCalls(std::map<int,std::vector<std::string>>, llvm::StringRef);
    bool runOnMachineFunction(MachineFunction &MF) override;
    void getAnalysisUsage(AnalysisUsage &AU) const override {
    //AU.addRequiredTransitive<MachineLoopInfo>();    
    AU.addRequiredTransitive<MachineLoopInfoWrapperPass>();    
    MachineFunctionPass::getAnalysisUsage(AU);
  }
    StringRef getPassName() const override { return RISCV_MIR_TO_SMT_VEC_PASS_NAME; }
};

//generate for each machine basic block mbb a vector of its function calls
std::vector<std::string>RISCVmirToSMTvec::fctCallsMBB(MachineBasicBlock& MBB){
  int i = 0;
  std::vector<std::string> fctCalls;
  std::string fname = "";
  if(const BasicBlock* BB = MBB.getBasicBlock()) //BB is needed to extract Callee through CallInst. There isn't always a BB corresponding to an MBB hence the "if" construct .
  {
    outs() << "MBB" << MBB.getNumber() << "corresponds to BB: "<< BB->getName() << "\n";
    for(auto I=BB->begin(), IE=BB->end(); I!=IE; ++I ){ // for every instruction in a basic block BB, check if it's a function call.
      if (auto CI = dyn_cast<CallInst>(I)) {
        Function* Callee = CI->getCalledFunction(); //retrieve the called function.
       const Function* Caller = BB->getParent(); //retrieve the caller function.
        fname = std::string(Caller->getName()) + "_" + "bb" + std::to_string(MBB.getNumber()) + "_" + std::string(Callee->getName()) + "_" + std::to_string(i);
        outs() << "MBB" << MBB.getNumber() << "-fct: "<< fname << "\n";
        i++;
        fctCalls.push_back(fname);
      }
    }
  }
  return fctCalls;
}  

//check whether a given machine basic block is an exit basic block: return a string "t_exit" if so and an empty string if not.
std::string RISCVmirToSMTvec::exitMBB(MachineBasicBlock& MBB){
  std::string exitbb="";
  if(const BasicBlock* BB = MBB.getBasicBlock()){
    if (auto CI = dyn_cast<ReturnInst>( BB->getTerminator())) {
      exitbb = "t_" + std::to_string(MBB.getNumber()) + "_exit";
      outs() << "An exit MBB is detected: " << exitbb << "\n";
    }
  }  
  return exitbb;
}



//print the SMT Formula corresponding to the MIR CFG
std::string RISCVmirToSMTvec::printCFGtoSMT(std::vector<std::vector<int>> mbb){
  std::string cfg_smt; 
  std::string cond;
  std::map<int,bool> TransCond;//a map that keeps track of every condition (i.e. branch): if a transition between two bbs (e.g. t_0_1 between bb0 and bb1) is detected, its corresponding condition TransCond[0] is set to 1 (true). Otherwise, it remains false (0). 
  for(int i=0; i< mbb.size(); i++){TransCond[i]=0;}// initialize the conditions to 0.
  for(int i=0; i< mbb.size(); i++){
    std::string trans;
    for(int j=0; j< mbb[i].size(); j++){
      std::string bb = std::to_string(mbb[i][j]);
      std::string bb0 = std::to_string(mbb[i][0]);
      if (mbb[i].size()<=1)
        trans = trans + "bb" + bb + " & ";
      else if(j==0)
        trans = trans + "(bb" + bb + " = ";
      else if (j>0 & j<(mbb[i].size() -1))
        {trans = trans +  "t_" + bb + "_" + bb0 + " | ";
        if (!TransCond[mbb[i][j]])//if this is the first branch from bb mbb[i][j], then  trans= cond. e.g. bb1 has two branches (i.e. two transitions t_1_2 and t_1_4) and the first one (t_1_2) is detected then t_1_2=cond_1.   
          cond = cond + " & (t_" + bb + "_" + bb0 + " = bb" + bb + " & cond_" + bb + ")"; 
        else //if this is the second branch from bb mbb[i][j], then  trans= not cond. e.g. bb1 has two branches (i.e. two transitions t_1_2 and t_1_4) and the second one (t_1_4) is detected, then t_1_4 = not cond_1.
          cond = cond + " & (t_" + bb + "_" + bb0 + " = bb" + bb + " & not cond_" + bb + ")";
        TransCond[mbb[i][j]] = 1;}//set the condition to 1 for the source bb in the TransCond map when a transition is detected. 
      else if (j==(mbb[i].size() -1) & (i <(mbb.size() -1)) )
        {trans = trans +  "t_" + bb + "_" + bb0 + " ) & ";
        if (!TransCond[mbb[i][j]])
          cond = cond + " & (t_" + bb + "_" + bb0 + " = bb" + bb + " & cond_" + bb + ")";
        else
          cond = cond + " & (t_" + bb + "_" + bb0 + " = bb" + bb + " & not cond_" + bb + ")";
        TransCond[mbb[i][j]] = 1;} //set the condition to 1 for the source bb in the TransCond map when a transition is detected. 
      else if (j==(mbb[i].size() -1) & (i ==(mbb.size() - 1)) )
        {trans = trans +  "t_" + bb + "_" + bb0 + " )";
        if (!TransCond[mbb[i][j]])
          cond = cond + " & (t_" + bb + "_" + bb0 + " = bb" + bb + " & cond_" + bb + ")";
        else
          cond = cond + " & (t_" + bb + "_" + bb0 + " = bb" + bb + " & not cond_" + bb + ")";
        TransCond[mbb[i][j]] = 1;} //set the condition to 1 for the source bb in the TransCond map when a transition is detected.
    }
    cfg_smt = cfg_smt + trans;
  }
  cfg_smt = cfg_smt + cond;
  return cfg_smt;
}


//generate an SMT_LIB2 file containing the smt formula of the MIR CFG  
std::ofstream RISCVmirToSMTvec::MIRtoSMTLIB2(std::vector<std::vector<int>> mbb, llvm::StringRef MfName){
  std::ofstream mf;
  std::string fname = "smt_lib2_" + (std::string) MfName + ".smt";
  //mf.open("smt_lib2.txt");
  mf.open(fname);
  std::string cond;
  std::map<int,bool> TransCond;//a map that keeps track of every condition (2 branches spring from one condition).

  //setting options and variable declarations
  for(int i=0; i< mbb.size(); i++){TransCond[i]=0;}
  if(!mf) {outs() << fname << " file not created.\n\n";}
  else {
    outs() << fname << " file is created successfully.\n\n";
    mf << "(set-logic QF_LIA)\n";
    mf << "(set-info :smt-lib-version 2.0)\n";
    mf << "(set-option :produce-models true)\n";
    for(int i=0; i< mbb.size(); i++){
      std::string bb0 = std::to_string(mbb[i][0]);
      std::string block = "(declare-fun bb" + bb0 + " () Bool)\n"; //declare a boolean for each basic block.
      mf << block;
      std::string trans;
      for(int j=0; j< mbb[i].size(); j++){
        if(j!=0){
          std::string bb = std::to_string(mbb[i][j]);
          trans =  "(declare-fun t_" + bb + "_" + bb0 +" () Bool)\n"; //declare a boolean for each transition.
          mf << trans;
          if (!TransCond[mbb[i][j]]){ cond = "(declare-fun cond_" + bb + " () Bool)\n"; mf << cond; TransCond[mbb[i][j]] = 1;} //declare a boolean for each condition. 
        }
      }
    }

    //constraints over the variables
    std::string cnstrt = "(assert (= bb" + std::to_string(mbb[0][0]) + " true))\n";
    mf << cnstrt;
    for(int i=1; i< mbb.size(); i++){
      std::string eq, disj, cond;
      for(int j=1; j< mbb[i].size(); j++){
        if(j==1) {eq= eq + "(= bb" + std::to_string(mbb[i][0]) + " t_" + std::to_string(mbb[i][j]) + "_" + std::to_string(mbb[i][0]) + ")";}
        if(j>1) {eq= eq + " (= bb" + std::to_string(mbb[i][0]) + " t_" + std::to_string(mbb[i][j]) + "_" + std::to_string(mbb[i][0]) + "))"; disj = disj + "(or ";}
        //if (TransCond[mbb[i][j]]) {cond = "(assert (= t_" + std::to_string(mbb[i][j]) + "_" + std::to_string(mbb[i][0]) +  " cond_" + std::to_string(mbb[i][j]) + "))\n";  TransCond[mbb[i][j]] = 0;} else cond = "(assert (= t_" + std::to_string(mbb[i][j]) + "_" + std::to_string(mbb[i][0]) +  " (not cond_" + std::to_string(mbb[i][j]) + ")))\n";
        if (TransCond[mbb[i][j]]) {cond = "(assert (= t_" + std::to_string(mbb[i][j]) + "_" + std::to_string(mbb[i][0]) +  " (and bb"+ std::to_string(mbb[i][j]) + " cond_" + std::to_string(mbb[i][j]) + ")))\n";  TransCond[mbb[i][j]] = 0;} else cond = "(assert (= t_" + std::to_string(mbb[i][j]) + "_" + std::to_string(mbb[i][0]) +  " (and bb" + std::to_string(mbb[i][j]) + " (not cond_" + std::to_string(mbb[i][j]) + "))))\n";
        mf << cond ; 
      }
      mf << "(assert " << disj << eq << ")\n";
    }
    mf.close();
  }
  return mf;
}

//generate a text file containing a list of function calls for each function of the input application. These output files will later be parsed by another program in order to generate a unified SMT file.
std::ofstream RISCVmirToSMTvec::functionCalls(std::map<int,std::vector<std::string>> fctCalls, llvm::StringRef MfName){
  std::ofstream mf;
  std::string fname = (std::string) MfName + ".txt";
  mf.open(fname);
  std::string fctCaller = "smt_lib2_" + (std::string) MfName + ".smt" ;
  if(!mf) {outs() << fname << " file not created.\n\n";}
  else {
    mf << "# " << fctCaller << "\n";
    for(int i = 0; i < fctCalls.size(); i++){
      for(int j = 0; j < fctCalls[i].size(); j++){
        mf << fctCalls[i][j] << "\n";
      }
    }
  mf.close();
  }
  return mf;
}

//generate an SMT_LIB2 file containing the smt formula of the MIR CFG and a memory counter constraint (for vector and scalar memory accesses) to be verified.   
std::ofstream RISCVmirToSMTvec::MIRtoSMTLIB2Mem(std::vector<std::vector<int>> mbb, llvm::StringRef MfName, std::map<int,std::vector<int>> BinaryTraceScalar, std::map<int,std::vector<int>> BinaryTraceVector, std::map<int,std::vector<int>> BinaryTraceVectorU, std::map<int,std::string> exitMBBs, std::map<int,std::vector<std::string>> fctCalls, std::vector<std::tuple<std::string,unsigned int>> loops, std::map<int,std::vector<std::string>> loopTrace){
  std::ofstream mf;
  std::string fname = "smt_lib2_" + (std::string) MfName + ".smt";
  //mf.open("smt_lib2.txt");
  mf.open(fname);
  std::string cond;
  std::map<int,bool> TransCond;//a map that keeps track of every condition (2 branches spring from one condition).
  std::map<std::string,int> loopBound;//a map that associates a bound to each loop.
    //setting options and variable declarations
  for(int i=0; i< mbb.size(); i++){TransCond[i]=0;}
  if(!mf) {outs() << fname << " file not created.\n\n";}
  else {
    outs() << fname << " file is created successfully.\n\n";
    mf << "(set-logic QF_LIA)\n";
    mf << "(set-info :smt-lib-version 2.0)\n";
    mf << "(set-option :produce-models true)\n";
    std::string mem_cost_scalar = "(declare-fun mem_cost_scalar () Int)\n"; //declare a global memory cost variable that is the sum of the scalar memory access counters in each traversed basic block. 
    std::string mem_cost_vector = "(declare-fun mem_cost_vector () Int)\n"; //declare a global memory cost variable that is the sum of the vector memory access element counters in each traversed basic block. 
    std::string mem_cost_vectoru = "(declare-fun mem_cost_vectoru () Int)\n"; //declare a global memory cost variable that is the sum of the vector memory access counters in each traversed basic block. 
    std::string m_cost_scalar, m_cost_scalar_debug, mem_cost_scalar_debug; //m_cost_scalar_debug and mem_cost_scalar_debug are used for debugging purposes only.
    std::string m_cost_vector,  m_cost_vectoru, m_cost_vector_debug, m_cost_vector_debugu, mem_cost_vector_debug, mem_cost_vector_debugu ; //m_cost_vector_debug and mem_cost_vector_debug are used for debugging purposes only.

    mf << "\n;=============== Loop bound declaration and setting ================\n";
    //declare loops and attribute a bound to each declared loop variable
    for(int i=0; i<loops.size(); i++){
      //std::string loop = "(declare-fun " + loops[i] + "() Int)\n";
      std::string loop = "(declare-const " + get<0>(loops[i]) + " Int)\n";
      mf << loop;
      loop = ";Loop bounds are by default set to 50 but can be manually modified in this smt file\n";
      mf << loop;
      loop =  "(assert (= " + get<0>(loops[i]) + " 50" + "))\n"; //by default we set the maximum loop bound to 50, which could be manually modified in the generated smt file. 
      loopBound[get<0>(loops[i])]=50;//this map maps each loop to its bound.
      outs() << "loop bound of loop " << get<0>(loops[i]) << " is: " << loopBound[get<0>(loops[i])] << "\n";
      mf << loop;
    }

    mf << "\n;=============== Declaration of basic blocks, transitions, conditions and memory counters ================\n";
    for(int i=0; i< mbb.size(); i++){
      std::string bb0 = std::to_string(mbb[i][0]);
      std::string block = "(declare-fun bb" + bb0 + " () Bool)\n"; //declare a boolean for each basic block.
      mf << block;
      std::string trans;
      std::string mems, memv, memvu;
      for(int j=0; j< mbb[i].size(); j++){
        if(j!=0){
          std::string bb = std::to_string(mbb[i][j]);
          trans =  "(declare-fun t_" + bb + "_" + bb0 +" () Bool)\n"; //declare a boolean for each transition.
         mf << trans;
          if (!TransCond[mbb[i][j]]){ cond = "(declare-fun cond_" + bb + " () Bool)\n"; mf << cond; TransCond[mbb[i][j]] = 1;} //declare a boolean for each condition. 
          mems =  "(declare-fun ms_" + bb + "_" + bb0 +" () Int)\n"; //declare a scalar memory access counter for each basic block.
          mf << mems;
          memv =  "(declare-fun mv_" + bb + "_" + bb0 +" () Int)\n"; //declare a vector memory access counter for each basic block.
          mf << memv;
          memvu =  "(declare-fun mvu_" + bb + "_" + bb0 +" () Int)\n"; //declare a vector memory access counter for each basic block.
          mf << memvu;

          //summing the memory accesses of individual bbs to construct the global memory cost mem_cost_scalar.
          if((loopTrace[mbb[i][0]].size()>0) & (loopTrace[mbb[i][j]].size()>0)){//if the bb belongs to a loop then its memory counter should be multiplied by the loop bound.
            if(loopTrace[mbb[i][j]].size()==1)
            { m_cost_scalar_debug = m_cost_scalar_debug + " (* ms_" + bb + "_" + bb0 + " " + loopTrace[mbb[i][j]][0] + ") ";
              m_cost_scalar = m_cost_scalar + " (* ms_" + bb + "_" + bb0 + " " + std::to_string(loopBound[loopTrace[mbb[i][j]][0]]) + ") ";
              m_cost_vector_debug = m_cost_vector_debug + " (* mv_" + bb + "_" + bb0 + " " + loopTrace[mbb[i][j]][0] + ") ";
              m_cost_vector_debugu = m_cost_vector_debugu + " (* mvu_" + bb + "_" + bb0 + " " + loopTrace[mbb[i][j]][0] + ") ";
              m_cost_vector = m_cost_vector + " (* mv_" + bb + "_" + bb0 + " " + std::to_string(loopBound[loopTrace[mbb[i][j]][0]]) + ") ";
              m_cost_vectoru = m_cost_vectoru + " (* mvu_" + bb + "_" + bb0 + " " + std::to_string(loopBound[loopTrace[mbb[i][j]][0]]) + ") ";
            }
            else if (loopTrace[mbb[i][j]].size()>1){ //if the bb belongs to a nested loop then its memory counter should be multiplied by the bounds of the outer and inner loops.
              m_cost_scalar = m_cost_scalar + " (* ms_" + bb + "_" + bb0 + " ";
              m_cost_scalar_debug = m_cost_scalar_debug + " (* ms_" + bb + "_" + bb0 + " ";
              m_cost_vector = m_cost_vector + " (* mv_" + bb + "_" + bb0 + " ";
              m_cost_vectoru = m_cost_vectoru + " (* mvu_" + bb + "_" + bb0 + " ";
              m_cost_vector_debug = m_cost_vector_debug + " (* mv_" + bb + "_" + bb0 + " ";
              m_cost_vector_debugu = m_cost_vector_debugu + " (* mvu_" + bb + "_" + bb0 + " ";
              std::string bounds, bounds_debug; 
              if(loopTrace[mbb[i][j]].size() <= loopTrace[mbb[i][0]].size()){ //if the loop depth of the source bbi is less than or equal to destination bbj then the memory access counter ms_i_j should be multiplied by the the loop depth bbi.
                for(unsigned int s=0; s<loopTrace[mbb[i][j]].size(); s++)
                { bounds_debug = bounds_debug + " " + loopTrace[mbb[i][j]][s];
                  bounds = bounds + " " + std::to_string(loopBound[loopTrace[mbb[i][j]][s]]);
                }
              }
              else if(loopTrace[mbb[i][j]].size() > loopTrace[mbb[i][0]].size()){ //if the loop depth of the source bbi is greater than the destination bbj (i.e. bbi and bbj do not belong to the same inner loop) then the memory access counter ms_i_j should be multiplied by the loop depth of bbj.
                for(unsigned int s=0; s<loopTrace[mbb[i][0]].size(); s++)
                { bounds_debug = bounds_debug + " " + loopTrace[mbb[i][0]][s];
                  std::string loopName=loopTrace[mbb[i][0]][s]; bounds = bounds + " " + std::to_string(loopBound[loopTrace[mbb[i][0]][s]]);
                } 
              }

              m_cost_scalar = m_cost_scalar + bounds + ") ";
              m_cost_scalar_debug = m_cost_scalar_debug + bounds_debug + ") ";
              m_cost_vector = m_cost_vector + bounds + ") ";
              m_cost_vectoru = m_cost_vectoru + bounds + ") ";
              m_cost_vector_debug = m_cost_vector_debug + bounds_debug + ") ";
              m_cost_vector_debugu = m_cost_vector_debugu + bounds_debug + ") ";
            } 
          }
          else //if bb doesn't belong to a loop then its memory counter is simply added to the overall memory cost.
          { m_cost_scalar = m_cost_scalar + " ms_" + bb + "_" + bb0;
            m_cost_scalar_debug = m_cost_scalar_debug + " ms_" + bb + "_" + bb0;
            m_cost_vector = m_cost_vector + " mv_" + bb + "_" + bb0;
            m_cost_vectoru = m_cost_vectoru + " mvu_" + bb + "_" + bb0;
            m_cost_vector_debug = m_cost_vector_debug + " mv_" + bb + "_" + bb0;
            m_cost_vector_debugu = m_cost_vector_debugu + " mvu_" + bb + "_" + bb0;
          } 
        }
      }
    }
    
    //declare transitions, memory counters and conditions for exit MBBs
    for(std::map<int,std::string>::iterator em = exitMBBs.begin(); em != exitMBBs.end(); ++em){
      std::string mems = "(declare-fun ms_" + std::to_string(em->first) + "_exit "  +" () Int)\n";
      mf << mems;
      std::string memv = "(declare-fun mv_" + std::to_string(em->first) + "_exit "  +" () Int)\n";
      mf << memv;
      std::string memvu = "(declare-fun mvu_" + std::to_string(em->first) + "_exit "  +" () Int)\n";
      mf << memvu;
      m_cost_scalar = m_cost_scalar + " ms_" + std::to_string(em->first) + "_exit";
      m_cost_scalar_debug = m_cost_scalar_debug + " ms_" + std::to_string(em->first) + "_exit";
      m_cost_vector = m_cost_vector + " mv_" + std::to_string(em->first) + "_exit";
      m_cost_vectoru = m_cost_vectoru + " mvu_" + std::to_string(em->first) + "_exit";
      m_cost_vector_debug = m_cost_vector_debug + " mv_" + std::to_string(em->first) + "_exit";
      m_cost_vector_debugu = m_cost_vector_debugu + " mvu_" + std::to_string(em->first) + "_exit";
      std::string exitMBB = "(declare-fun " + em->second +" () Bool)\n";
      mf << exitMBB;
      cond = "(declare-fun cond_" + std::to_string(em->first) + " () Bool)\n"; 
      mf << cond;
    }
    mf << mem_cost_scalar;
    mf << mem_cost_vector;
    mf << mem_cost_vectoru;
    
    mf << "\n;=============== Function call declaration ================\n";;
    //declare function calls
    for(std::map<int,std::vector<std::string>>::iterator fc = fctCalls.begin(); fc != fctCalls.end(); ++fc){
      for(int i=0; i< (fc->second).size(); i++){
        std::string fct = "(declare-fun " + (fc->second)[i] + " () Bool)\n";
         mf << fct;
      }
    }

    mf << "\n;=============== Constraints ================\n";
    //constraints over the variables
    mf << "\n;=============== Entry block always executes ================\n";
    std::string cnstrt = "(assert (= bb" + std::to_string(mbb[0][0]) + " true))\n";
    mf << cnstrt;
    mem_cost_scalar = "(assert (= mem_cost_scalar (+ " + m_cost_scalar + ")))\n";
    mem_cost_scalar_debug = "(assert (= mem_cost_scalar (+ " + m_cost_scalar_debug + ")))\n";
    mem_cost_vector = "(assert (= mem_cost_vector (+ " + m_cost_vector + ")))\n";
    mem_cost_vectoru = "(assert (= mem_cost_vectoru (+ " + m_cost_vectoru + ")))\n";
    mem_cost_vector_debug = "(assert (= mem_cost_vector (+ " + m_cost_vector_debug + ")))\n";
    mem_cost_vector_debugu = "(assert (= mem_cost_vectoru (+ " + m_cost_vector_debugu + ")))\n";
    mf << "\n;================ Constraints over basic blocks, transitions and  memory counters================\n";
    for(int i=1; i< mbb.size(); i++){
      std::string eq, disj, cond, mscounter, m_cost_scalar, mvcounter, mvcounteru, m_cost_vector, m_cost_vectoru;
      int mcs=0;
      int mcv=0;
      int mcvu=0;
      for(int j=1; j< mbb[i].size(); j++){
        //contraints over bbs.
        if(j==1) {eq= eq + "(= bb" + std::to_string(mbb[i][0]) + " t_" + std::to_string(mbb[i][j]) + "_" + std::to_string(mbb[i][0]) + ")";}
        if(j>1) {eq= eq + " (= bb" + std::to_string(mbb[i][0]) + " t_" + std::to_string(mbb[i][j]) + "_" + std::to_string(mbb[i][0]) + "))"; disj = disj + "(or ";}
        //constraints over transitions.
        if (TransCond[mbb[i][j]]) {
          //cond = "(assert (= t_" + std::to_string(mbb[i][j]) + "_" + std::to_string(mbb[i][0]) +  " (and bb"+ std::to_string(mbb[i][j]) + " cond_" + std::to_string(mbb[i][j]) + ")))\n";  
          cond = "(assert (= t_" + std::to_string(mbb[i][j]) + "_" + std::to_string(mbb[i][0]) +  " (and bb"+ std::to_string(mbb[i][j]) + " cond_" + std::to_string(mbb[i][j]);  
          TransCond[mbb[i][j]] = 0;
        } 
        else 
          //cond = "(assert (= t_" + std::to_string(mbb[i][j]) + "_" + std::to_string(mbb[i][0]) +  " (and bb" + std::to_string(mbb[i][j]) + " (not cond_" + std::to_string(mbb[i][j]) + "))))\n";
          cond = "(assert (= t_" + std::to_string(mbb[i][j]) + "_" + std::to_string(mbb[i][0]) +  " (and bb" + std::to_string(mbb[i][j]) + " (not cond_" + std::to_string(mbb[i][j]) + ")";

        //function-call variables are added to the constraints over the transitions (e.g. T0-1=bb0 ^ con0 ^ bb0_fct_0).
        if(fctCalls.find(mbb[i][j]) == fctCalls.end()) //first, check if the element exists in the fctCalls map (i.e. does the current mbb have functions calls?).
          cond = cond  + ")))\n"; //if the current mbb doesn't exist in the fctCalls map then end the transition constraint without adding any function-call variables.
        else{ //if the current mbb is in the fctCalls map (i.e. contains function calls) then add the function call variables to the transition constraint. 
          for(int k=0; k < (fctCalls[mbb[i][j]].size()); k++){
            if(k==(fctCalls[mbb[i][j]].size() - 1)) {cond = cond  + " " + fctCalls[mbb[i][j]][k] + ")))\n"; }
            else { cond = cond  + " " + fctCalls[mbb[i][j]][k]; }  
          }
        }
        mf << cond;

        //count scalar memory access instructions inside each bb
        for (int k=0 ; k< (BinaryTraceScalar[mbb[i][j]]).size(); k++) {if (BinaryTraceScalar[mbb[i][j]][k]==1) mcs ++;}
        mscounter = "(assert (= ms_"  + std::to_string(mbb[i][j]) + "_" + std::to_string(mbb[i][0]) + " (ite t_" + std::to_string(mbb[i][j]) + "_" + std::to_string(mbb[i][0]) + " " + std::to_string(mcs) + " 0)))\n";
        mcs = 0;
        mf << mscounter;
      
        //count vector memory access instructions (element count) inside each bb
        for (int k=0 ; k< (BinaryTraceVector[mbb[i][j]]).size(); k++) {if (BinaryTraceVector[mbb[i][j]][k]>0) mcv+=BinaryTraceVector[mbb[i][j]][k];}
        mvcounter = "(assert (= mv_"  + std::to_string(mbb[i][j]) + "_" + std::to_string(mbb[i][0]) + " (ite t_" + std::to_string(mbb[i][j]) + "_" + std::to_string(mbb[i][0]) + " " + std::to_string(mcv) + " 0)))\n";
        mcv = 0;
        mf << mvcounter;

        //count vector memory access instructions (unit count) inside each bb
        for (int k=0 ; k< (BinaryTraceVectorU[mbb[i][j]]).size(); k++) {if (BinaryTraceVectorU[mbb[i][j]][k]==1) mcvu++;}
        mvcounteru = "(assert (= mvu_"  + std::to_string(mbb[i][j]) + "_" + std::to_string(mbb[i][0]) + " (ite t_" + std::to_string(mbb[i][j]) + "_" + std::to_string(mbb[i][0]) + " " + std::to_string(mcvu) + " 0)))\n";
        mcvu = 0;
        mf << mvcounteru;
      }
      //if (i == mbb.size() -1 ) { for (int k=0 ; k< (BinaryTraceScalar[mbb[i][0]]).size(); k++) {if (BinaryTraceScalar[mbb[i][0]][k]==1) mc ++;} mscounter = "(assert (= ms_"  +  std::to_string(mbb[i][0]) + "_exit " + std::to_string(mc) + "))\n"; mf << mscounter;} //count memory access instructions of exit basic block.
      mf << "(assert " << disj << eq << ")\n";
    
    }
    //constraints over exit variables (transitions and memory counters)
    for(std::map<int,std::string>::iterator em = exitMBBs.begin(); em != exitMBBs.end(); ++em){
      //cond = "(assert (= " + em->second +" (and bb" + std::to_string(em->first) + " cond_" + std::to_string(em->first) +  ")))\n";
      cond = "(assert (= " + em->second +" (and bb" + std::to_string(em->first) + " cond_" + std::to_string(em->first);
      //function-call variables are added to the constraints over the exit-transitions
      if(fctCalls.find(em->first) == fctCalls.end()) cond = cond  + ")))\n";
      else{
        for(int k=0; k < (fctCalls[em->first].size()); k++){
          if(k==(fctCalls[em->first].size() - 1)) {cond = cond  + " " + fctCalls[em->first][k] + ")))\n"; }
          else { cond = cond  + " " + fctCalls[em->first][k]; }
        }
      }
      mf << cond;
      int mcs = 0;
      int mcv=0;
      int mcvu=0;
      std::string mscounter;
      std::string mvcounter;
      std::string mvcounteru;
      for (int k=0 ; k< ((BinaryTraceScalar[em->first]).size()); k++) {if (BinaryTraceScalar[em->first][k]==1) mcs ++;} mscounter = "(assert (= ms_"  +  std::to_string(em->first) + "_exit " + std::to_string(mcs) + "))\n"; 
      mf << mscounter;
    
      for (int k=0 ; k< ((BinaryTraceVector[em->first]).size()); k++) {if (BinaryTraceVector[em->first][k]>0) mcv+=BinaryTraceVector[em->first][k];} mvcounter = "(assert (= mv_"  +  std::to_string(em->first) + "_exit " + std::to_string(mcv) + "))\n"; 
      mf << mvcounter;

      for (int k=0 ; k< ((BinaryTraceVectorU[em->first]).size()); k++) {if (BinaryTraceVectorU[em->first][k]==1) mcvu++;} mvcounteru = "(assert (= mvu_"  +  std::to_string(em->first) + "_exit " + std::to_string(mcvu) + "))\n"; 
      mf << mvcounteru;
    }
    mf << "\n;=============== Memory cost formulas (with loop multipliers) ================\n";
    mf << ";mem_cost_scalar expression with loop variables explicitly stated for debugging purposes: " << mem_cost_scalar_debug; //mem_cost_scalar_debug is used for debugging purposes only. It can be commented without changing the functionality of the code.
    mf << mem_cost_scalar;
    mf << ";mem_cost_vector expression with loop variables explicitly stated for debugging purposes: " << mem_cost_vector_debug; //mem_cost_vector_debug is used for debugging purposes only. It can be commented without changing the functionality of the code.
    mf << mem_cost_vector;
    mf << ";mem_cost_vectoru expression with loop variables explicitly stated for debugging purposes: " << mem_cost_vector_debugu; //mem_cost_vector_debugu is used for debugging purposes only. It can be commented without changing the functionality of the code.
    mf << mem_cost_vectoru;
    mf << "\n;=============== Queries ================\n";
    mf.close();
  }
  return mf;
}
//print the scalar binary trace corresponding to the CFG
std::string RISCVmirToSMTvec::printTrace(std::map<int,std::vector<int>> BinaryTraceScalar){
  std::string bin_trace;
  std::map<int,std::vector<int>>::iterator itr;
  for(itr=BinaryTraceScalar.begin(); itr != BinaryTraceScalar.end(); ++itr){
    bin_trace = bin_trace + "bb" + std::to_string(itr->first) + ":";
    std::string bb_trace;
    for(int i=0;i < ((itr->second).size()); i++ ){
      if((i==0) && (i!=((itr->second).size() -1)))
        bb_trace = "<" + std::to_string((itr->second)[i]) + ",";
      else if ((i==0) && (i==((itr->second).size() -1)))
        bb_trace = "<" + std::to_string((itr->second)[i]) + ">\n";
      else if (i>0 & (i< ((itr->second).size() -1)))
        bb_trace = bb_trace + std::to_string((itr->second)[i]) + ",";
      else if (i== ((itr->second).size() -1))
        bb_trace = bb_trace + std::to_string((itr->second)[i]) + ">\n";
    }
    bin_trace = bin_trace + bb_trace;
  }
  return bin_trace;
}
//print the vector trace (element count per vector instruction) corresponding to the CFG
std::string RISCVmirToSMTvec::printTraceVector(std::map<int,std::vector<int>> BinaryTraceVector){
  std::string bin_trace;
  std::map<int,std::vector<int>>::iterator itr;
  for(itr=BinaryTraceVector.begin(); itr != BinaryTraceVector.end(); ++itr){
    bin_trace = bin_trace + "bb" + std::to_string(itr->first) + ":";
    std::string bb_trace;
    for(int i=0;i < ((itr->second).size()); i++ ){
      if((i==0) && (i!=((itr->second).size() -1)))
        bb_trace = "<" + std::to_string((itr->second)[i]) + ",";
      else if ((i==0) && (i==((itr->second).size() -1)))
        bb_trace = "<" + std::to_string((itr->second)[i]) + ">\n";
      else if (i>0 & (i< ((itr->second).size() -1)))
        bb_trace = bb_trace + std::to_string((itr->second)[i]) + ",";
      else if (i== ((itr->second).size() -1))
        bb_trace = bb_trace + std::to_string((itr->second)[i]) + ">\n";
    }
    bin_trace = bin_trace + bb_trace;
  }
  return bin_trace;
}
//print the vector binary trace (vector instruction count) corresponding to the CFG
std::string RISCVmirToSMTvec::printTraceVectorU(std::map<int,std::vector<int>> BinaryTraceVectorU){
  std::string bin_trace;
  std::map<int,std::vector<int>>::iterator itr;
  for(itr=BinaryTraceVectorU.begin(); itr != BinaryTraceVectorU.end(); ++itr){
    bin_trace = bin_trace + "bb" + std::to_string(itr->first) + ":";
    std::string bb_trace;
    for(int i=0;i < ((itr->second).size()); i++ ){
      if((i==0) && (i!=((itr->second).size() -1)))
        bb_trace = "<" + std::to_string((itr->second)[i]) + ",";
      else if ((i==0) && (i==((itr->second).size() -1)))
        bb_trace = "<" + std::to_string((itr->second)[i]) + ">\n";
      else if (i>0 & (i< ((itr->second).size() -1)))
        bb_trace = bb_trace + std::to_string((itr->second)[i]) + ",";
      else if (i== ((itr->second).size() -1))
        bb_trace = bb_trace + std::to_string((itr->second)[i]) + ">\n";
    }
    bin_trace = bin_trace + bb_trace;
  }
  return bin_trace;
}

bool RISCVmirToSMTvec::runOnMachineFunction(MachineFunction &MF) {
  if(EnableMirToSMTvec){
    //MLI = &getAnalysis<MachineLoopInfo>();
    MLI = &getAnalysis<MachineLoopInfoWrapperPass>().getLI();
    const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
    MachineLoop *loopInfo;
    std::vector<std::vector<int>> mbb;//each element of this vector is a vector of a machine basic block and its predecessors (only their numbers), e.g. mbb= < <0>, <1,0>, <2,1,0>, <3,2>  >. 
    
   //Seperate binary traces for scalar and vector
    std::map<int,std::vector<int>> BinaryTraceScalar;//a map made of bbs and their corresponding traces of 1s (memory instructions) and 0s (other instructions), e.g. BinaryTrace[0] = <0,1,1>, BinaryTrace[1] = <1,0>, etc.
    std::map<int,std::vector<int>> BinaryTraceVector;//a map made of bbs and their corresponding traces of Ns (number of vector memory elements) and 0s (other instructions), e.g. BinaryTrace[0] = <0,3,1>, BinaryTrace[1] = <5,0>, etc.
    std::map<int,std::vector<int>> BinaryTraceVectorU;//a map made of bbs and their corresponding traces of 1s (number of vector memory instructions) and 0s (other instructions), e.g. BinaryTrace[0] = <0,1,1>, BinaryTrace[1] = <1,0>, etc.
   
    std::map<int,std::string> exitMBBs;//a map made of exit mbbs and their corresponding exit transitions.
    std::vector<std::string> fmbb;//a vector of function calls inside a mbb. 
    std::map<int,std::vector<std::string>> fctCalls;//a map made of bbs and their corresponding vector of function calls.
    //std::vector<std::string> loops;//a vector of loop names: it keeps track of every loop encountered in a given function. The loop names are constructed using the name of the basic block containing the loop header, e.g. loop_bb_1.
    std::vector<std::tuple<std::string,unsigned int>> loops;//a vector of loop names: it keeps track of every loop encountered in a given function. The loop names are constructed using the name of the basic block containing the loop header, e.g. loop_bb_1.
    std::map<int,std::vector<std::string>> loopTrace;//a map that associates to each mbb belonging to a loop a vector of the loop(s) it belongs to. The size of this vector is the loop depth (in case of nested loops), e.g. (loopTrace[5]=<loop_bb_3,loop_bb_1>). 

    outs() << "\n*****  MachineFunction: " << MF.getName() <<"   *****\n\n\n";
    for (auto &MBB : MF) { // an iterator over Machine Function returns a list of Machine basic blocks
      NumMachineBBs++;
      outs() << "\n*****  Contents of MachineBasicBlock:\n";
      outs() << MBB << "\n";
     
      //collecting loop information
      loopInfo = MLI->getLoopFor(&MBB);
      if(loopInfo != NULL){ //if the mbb belongs to a loop then proceed with information gathering.
        unsigned int loopDepth = MLI->getLoopDepth(&MBB);
        std::vector<std::string> temp_loops;//a temporary vector where we store the loop(s) name(s) containing the current mbb. 
        if(MLI->isLoopHeader(&MBB)) {//a loop is detected when a loop header is found. 
          std::string loopHeader = "loop_bb_" + std::to_string(MBB.getNumber());//a loop is named using the loop header's mbb number. e.g. if mbb_2 is identified as a loop header then the corresponding loop would be called loop_bb_2.  
          //loops.push_back(loopHeader);//the new loop is added to the vector "loops".
          loops.push_back(make_tuple(loopHeader,loopDepth));//the new loop is added to the vector "loops".
	  outs() << "a loop header is detected at MBB " << MBB.getNumber() << ": Loop_MBB_" << MBB.getNumber() << "_" << MBB.getName() << "\n";
        }

        if(loops.empty()) {
         loopTrace[MBB.getNumber()] = temp_loops; // Empty
         outs() << "MBB" << MBB.getNumber() << " processed (no loops yet)\n";
        }

        else if((loops.size()>1)&(loopDepth<=loops.size())){
            outs() << "size of vector loops is: " << loops.size() <<"\n";
            outs() << "bb" <<MBB.getNumber() << "has loop depth : " << loopDepth <<"\n";
            //for(unsigned int i=(loops.size() - 1) ; i>=(loops.size() - loopDepth); i--){// the loop vector "loops" is traversed "loopDepth" number of times in reverse to collect the loop names associated to current mbb.
            unsigned int j=0; 
            for(unsigned int i=(loops.size() - 1) ; i>=0; i--){// the loop vector "loops" is traversed "loopDepth" number of times in reverse to collect the loop names associated to current mbb.
              outs() << "loops index i= " << i<< "\n";
              //outs() << "loops content at index  " << i<< " is:" <<loops[i]<<"\n";
              outs() << "loops content at index  " << i<< " is:" << get<0>(loops[i]) <<"\n";
              //temp_loops.push_back(loops[i]);
              if((loopDepth-j) == get<1>(loops[i])){
                temp_loops.push_back(get<0>(loops[i]));
                if(j == loopDepth) break;
                j++;
                //if(i == (loops.size() - loopDepth)) break;
              }
	      if(i==0) break;
            }
        }
        else if(loops.size()==1) temp_loops.push_back(get<0>(loops[0])); //temp_loops.push_back(loops[0]);
        loopTrace[MBB.getNumber()]=temp_loops;//the temporary loop vector "temp_loops" associated to the current mbb is added to "loopTrace".
        outs() << " MBB" << MBB.getNumber() << " belongs to a loop of depth " << loopDepth << "\n"; 
      }
  
      //collecting information about the predecessors of each basic block, exit  basic blocks and function call instructions.
      std::vector<int> pmbb; //a vector containing the current machine basic block and its predecessors (only their numbers)
      pmbb.push_back(MBB.getNumber());
      fmbb = fctCallsMBB(MBB);
      std::string mbbexit = exitMBB(MBB);//mbbexit="" if MBB is not an exit basic block. mbbexit=mbbi_exit if mbb number i is an exit basic block.
      if(mbbexit!="") exitMBBs[MBB.getNumber()] = mbbexit; 
      for (MachineBasicBlock::pred_iterator PMBB = MBB.pred_begin(), PMBBE = MBB.pred_end(); PMBB != PMBBE; ++PMBB) {
        MachineBasicBlock* predBlock = *PMBB;
	outs() << "Predecessors to MBB." << MBB.getNumber()<< "." << MBB.getName() << ":MBB." << predBlock->getNumber() << "." << predBlock->getName() << "\n\n";
        pmbb.push_back(predBlock->getNumber());
      }
      mbb.push_back(pmbb);
      
      //collecting statistics of machine instructions (e.g. number of loads/stores)  and construct binary traces from basic blocks.
      //two separate traces for vector and scalar memory isntructions. 
      std::vector<int> trScalar; //scalar memory trace.
      std::vector<int> trVector; //vector memory trace (element count).
      std::vector<int> trVectorU; //vector memory trace (vector instruction count).
      for (auto &MI : MBB) { // an iterator over a machine basic block returns a list of instructions
        NumMachineInstrs++;
        //outs() << "MBB" << MBB.getNumber() << ": instr: " << MI << "\n";
        unsigned opcode = MI.getOpcode();
        StringRef opcodeName = TII->getName(opcode);
        //if(MI.isCall()) {
	  //outs() << "\n\n A function call is detected - OPCODE: " << opcode << ": " << MI << "\n";
          //for(MachineOperand &MO : MI.operands())
            //outs() << "operand  of type " << MO.getType() << "\n";
        //};

        bool isLoad = MI.mayLoad();
        bool isStore = MI.mayStore();
  
        if (!isLoad && !isStore) {
        // Not a memory instruction
        trScalar.push_back(0);
        trVector.push_back(0);
        trVectorU.push_back(0);
        continue;
        }
       // It's a memory instruction - determine if scalar or vector
  
       // Check for vector instruction patterns
       bool isVector = opcodeName.starts_with("VLE") || 
                  opcodeName.starts_with("VSE") ||
                  opcodeName.starts_with("VLSE") || 
                  opcodeName.starts_with("VSSE") ||
                  opcodeName.starts_with("VLUX") || 
                  opcodeName.starts_with("VSUX") ||
                  opcodeName.starts_with("VLOX") || 
                  opcodeName.starts_with("VSOX") ||
                  opcodeName.starts_with("VLSEG") || 
                  opcodeName.starts_with("VSSEG") ||
                  opcodeName.contains("PseudoVLE") ||
                  opcodeName.contains("PseudoVSE") ||
                  opcodeName.contains("PseudoVLSE") ||
                  opcodeName.contains("PseudoVSSE") ||
                  opcodeName.contains("PseudoVLUX") ||
                  opcodeName.contains("PseudoVSUX") ||
                  opcodeName.contains("PseudoVLOX") ||
                  opcodeName.contains("PseudoVSOX") ||
                  opcodeName.contains("PseudoVLSEG") ||
                  opcodeName.contains("PseudoVSSEG") ||
                  (opcodeName.starts_with("VL") && opcodeName.contains("RE")) ||
                  (opcodeName.starts_with("VS") && opcodeName.contains("R_V")) ||
                  opcodeName.contains("Pseudo") && opcodeName.contains("VL") && opcodeName.contains("RE") ||
                  opcodeName.contains("Pseudo") && opcodeName.contains("VS") && opcodeName.contains("R_V");
  
       if (isVector) {
       //========================================================================
       // VECTOR MEMORY INSTRUCTION
       //========================================================================
    
       unsigned sew = extractSEW(opcodeName);
       unsigned nf = extractNumFields(opcodeName);
       unsigned nr = extractNumRegisters(opcodeName);
    
       unsigned count;
       if (opcodeName.contains("RE") || opcodeName.contains("R_V")) {
        // Whole register loads/stores
        count = (VLEN / sew) * nr;
       } else {
      // Regular vector loads/stores
      count = getVLMAX(sew) * nf;
      }
    
    NumVectorMemInstrs++;
    if (isLoad) NumVectorLoadInstrs++;
    if (isStore) NumVectorStoreInstrs++;
    
    trScalar.push_back(0);
    trVector.push_back(count);
    trVectorU.push_back(1);
    
    outs() << "  -> VECTOR " << (isLoad ? "LOAD" : "STORE") 
           << ": " << opcodeName 
           << " (SEW=" << sew << ", nf=" << nf << ", count=" << count << ")\n";
    
    } else {
    //========================================================================
    // SCALAR MEMORY INSTRUCTION
    //========================================================================
    
    // It's a scalar load/store (anything that's not vector)
    NumScalarMemInstrs++;
    if (isLoad) NumScalarLoadInstrs++;
    if (isStore) NumScalarStoreInstrs++;
    
    trScalar.push_back(1);
    trVector.push_back(0);
    trVectorU.push_back(0);
    
    outs() << "  -> SCALAR " << (isLoad ? "LOAD" : "STORE") 
           << ": " << opcodeName << "\n";
    }
    }  

    
      BinaryTraceScalar[MBB.getNumber()]=trScalar; // map each bb to its corresponding trace trScalar.
      BinaryTraceVector[MBB.getNumber()] = trVector;      
      BinaryTraceVectorU[MBB.getNumber()] = trVectorU;      
      if (fmbb.size()!=0) 
        fctCalls[MBB.getNumber()]=fmbb;
    }

    //print the SMT Formula corresponding to the MIR CFG
    std::string cfg_smt = RISCVmirToSMTvec::printCFGtoSMT(mbb); 
    outs() << "***** cfg_smt of MachineFunction \""  << MF.getName() << "\" = " << cfg_smt << "\n\n";

    //generate the SMT_LIB2 file containing the SMT formula of the MIR CFG    
    llvm::StringRef MfName = MF.getName();
    //std::ofstream mf = RISCVmirToSMTfctCallAndLoopBound::MIRtoSMTLIB2(mbb,MfName);

    //generate the SMT_LIB2 file containing the SMT formula of the MIR CFG and a memory counter constraint to be verified.
    std::ofstream mf = RISCVmirToSMTvec::MIRtoSMTLIB2Mem(mbb,MfName,BinaryTraceScalar,BinaryTraceVector,BinaryTraceVectorU,exitMBBs,fctCalls,loops,loopTrace);
    /*for(std::map<int,std::string>::iterator em = exitMBBs.begin(); em != exitMBBs.end(); ++em){
      outs() << "exit BBs: " << em->second << "\n";
    }*/
    //generate a text file containing a list of function calls (if any) for each function of the input application.
    std::ofstream fc = RISCVmirToSMTvec::functionCalls(fctCalls, MfName);
    //print the binary trace corresponding to the CFG
    std::string bin_scalar_trace = RISCVmirToSMTvec::printTrace(BinaryTraceScalar);//scalar trace.
    std::string bin_vector_trace = RISCVmirToSMTvec::printTraceVector(BinaryTraceVector);//vector trace (element count per vector memory access).
    std::string bin_vector_traceU = RISCVmirToSMTvec::printTraceVectorU(BinaryTraceVectorU);//vector trace (unit count per vector memory access).

    outs() << "***** SCALAR Memory Trace (1 -> scalar load/store, 0 -> other) of MachineFunction \""
           << MF.getName() << "\" :  \n" << bin_scalar_trace << "\n\n";
    outs() << "***** VECTOR Memory Trace (N -> N vector elements accessed, 0 -> other) of MachineFunction \""
           << MF.getName() << "\" :  \n" << bin_vector_trace << "\n\n";
    outs() << "***** VECTOR Memory Trace (1 -> vector load/store, 0 -> other) of MachineFunction \""
           << MF.getName() << "\" :  \n" << bin_vector_traceU << "\n\n";
   // Print summary statistics
    outs() << "\n===== FINAL STATISTICS =====\n";
    outs() << "Total machine instructions: " << NumMachineInstrs << "\n";
    outs() << "Scalar memory instructions: " << NumScalarMemInstrs << "\n";
    outs() << "  Scalar loads: " << NumScalarLoadInstrs << "\n";
    outs() << "  Scalar stores: " << NumScalarStoreInstrs << "\n";
    outs() << "Vector memory instructions: " << NumVectorMemInstrs << "\n";
    outs() << "  Vector loads: " << NumVectorLoadInstrs << "\n";
    outs() << "  Vector stores: " << NumVectorStoreInstrs << "\n";
    outs() << "============================\n\n";
      

   
  }
  return false;
}

} // end of anonymous namespace

char RISCVmirToSMTvec::ID = 0;

INITIALIZE_PASS(RISCVmirToSMTvec, "MIR TO SMT with function calls, loop bound and vector memory instructions pass",
    RISCV_MIR_TO_SMT_VEC_PASS_NAME,
    true, // is CFG only?
    true  // is analysis?
)

namespace llvm {

FunctionPass *createRISCVmirToSMTvecPass() { return new RISCVmirToSMTvec(); }

}

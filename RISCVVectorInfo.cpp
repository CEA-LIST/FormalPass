#include "RISCV.h"
#include "llvm/ADT/Statistic.h"
#include "RISCVInstrInfo.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "RISCVTargetMachine.h"
#include "llvm/Support/Debug.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/MC/MCInstrDesc.h"

using namespace llvm;

#define RISCV_VECTOR_INFO_PASS_NAME "RISCV vector machine instruction printer pass"
#define DEBUG_TYPE "vector_machine_instr_printer"

STATISTIC(NumMachineInstrs, "Number of machine instructions");
STATISTIC(NumMachineBBs, "Number of machine basic blocks");
STATISTIC(NumScalarMemInstrs, "Number of scalar memory instructions (load/store)");
STATISTIC(NumScalarLoadInstrs, "Number of scalar memory load instructions");
STATISTIC(NumScalarStoreInstrs, "Number of scalar memory store instructions");
STATISTIC(NumVecInstrs, "Number of vector instructions");
STATISTIC(NumVecConfigInstrs, "Number of vector configuration instructions");
STATISTIC(NumVecMemInstrs, "Number of vector memory instructions (load/store)");
STATISTIC(NumVecLoadInstrs, "Number of vector memory load instructions");
STATISTIC(NumVecLoadElemInstrs, "Number of vector memory load-element instructions");
STATISTIC(NumVecLoadMaskInstrs, "Number of vector memory load-mask instructions");
STATISTIC(NumVecLoadFaultInstrs, "Number of vector memory load-fault-only-first instructions");
STATISTIC(NumVecLoadStridedInstrs, "Number of vector memory load-strided instructions");
STATISTIC(NumVecLoadIndexedOInstrs, "Number of vector memory load-indexed-ordered instructions");
STATISTIC(NumVecLoadIndexedUInstrs, "Number of vector memory load-indexed-un-ordered instructions");
STATISTIC(NumVecLoadSegmentedInstrs, "Number of vector memory load-segmented instructions");
STATISTIC(NumVecLoadSegmentedStridedInstrs, "Number of vector memory load-segmented-strided instructions");
STATISTIC(NumVecLoadSegmentedIndexedOInstrs, "Number of vector memory load-segmented-indexed-ordered instructions");
STATISTIC(NumVecLoadSegmentedIndexedUInstrs, "Number of vector memory load-segmented-indexed-unordered instructions");
STATISTIC(NumVecLoadSegmentedFaultInstrs, "Number of vector memory load-segmented-fault-only-first instructions");
STATISTIC(NumVecLoadWholeRegInstrs, "Number of vector whole register load instructions");
STATISTIC(NumVecStoreInstrs, "Number of vector memory store instructions");
STATISTIC(NumVecStoreElemInstrs, "Number of vector memory store-element instructions");
STATISTIC(NumVecStoreMaskInstrs, "Number of vector memory store-mask instructions");
STATISTIC(NumVecStoreStridedInstrs, "Number of vector memory store-strided instructions");
STATISTIC(NumVecStoreIndexedOInstrs, "Number of vector memory store-indexed-ordered instructions");
STATISTIC(NumVecStoreIndexedUInstrs, "Number of vector memory store-indexed-un-ordered instructions");
STATISTIC(NumVecStoreSegmentedInstrs, "Number of vector memory store-segmented instructions");
STATISTIC(NumVecStoreSegmentedStridedInstrs, "Number of vector memory store-segmented-strided instructions");
STATISTIC(NumVecStoreSegmentedIndexedOInstrs, "Number of vector memory store-segmented-indexed-ordered instructions");
STATISTIC(NumVecStoreSegmentedIndexedUInstrs, "Number of vector memory store-segmented-indexed-unordered instructions");
STATISTIC(NumVecStoreWholeRegInstrs, "Number of vector whole register store instructions");
STATISTIC(NumVecClassificationMismatches, "Number of load/store flag mismatches with classification");

static cl::opt<bool> EnableRISCVVectorInfo(
    "vec-instr-print",
    cl::desc("Enable vector machine instruction printing"), 
    cl::init(false),
    cl::Hidden);

static cl::opt<bool> EnableTSFlagsValidation(
    "vec-instr-validate-tsflags",
    cl::desc("Enable TSFlags validation for vector instructions"),
    cl::init(true),
   cl::Hidden);

namespace {

enum class VecInstrCategory {
  Unknown,
  Config,
  // Loads
  LoadUnit,
  LoadStrided,
  LoadIndexedOrdered,
  LoadIndexedUnordered,
  LoadSegmented,
  LoadSegmentedStrided,
  LoadSegmentedIndexedOrdered,
  LoadSegmentedIndexedUnordered,
  LoadFaultOnlyFirst,
  LoadSegmentedFaultOnlyFirst,
  LoadMask,
  LoadWholeReg,
  // Stores
  StoreUnit,
  StoreStrided,
  StoreIndexedOrdered,
  StoreIndexedUnordered,
  StoreSegmented,
  StoreSegmentedStrided,
  StoreSegmentedIndexedOrdered,
  StoreSegmentedIndexedUnordered,
  StoreMask,
  StoreWholeReg
  };

class RISCVVectorInfo : public MachineFunctionPass {
public:
  static char ID;
  
  RISCVVectorInfo() : MachineFunctionPass(ID) {
    initializeRISCVVectorInfoPass(*PassRegistry::getPassRegistry());
  }

  VecInstrCategory classifyVectorInstr(StringRef OpcodeName);
  bool detectAndClassifyVectorInstr(const MachineInstr &MI, const MachineFunction &MF);
  bool runOnMachineFunction(MachineFunction &MF) override;
  StringRef getPassName() const override { return RISCV_VECTOR_INFO_PASS_NAME; }

private:
  void updateStatistics(VecInstrCategory Category);
  StringRef getCategoryName(VecInstrCategory Category);
  StringRef normalizeOpcodeName(StringRef Name);
  bool validateWithTSFlags(const MachineInstr &MI, VecInstrCategory Category);
  bool isVectorInstruction(const MachineInstr &MI);
  bool isLoadCategory(VecInstrCategory Category);
  bool isStoreCategory(VecInstrCategory Category);
};

// Remove "Pseudo" prefix if present for consistent matching
StringRef RISCVVectorInfo::normalizeOpcodeName(StringRef Name) {
  if (Name.starts_with("Pseudo"))
    return Name.substr(6); // Remove "Pseudo"
  return Name;
  }

// Check if instruction is a vector instruction using opcode patterns
// In LLVM 20, we rely on opcode name patterns since TSFlags may not have
// explicit vector markers in all cases
bool RISCVVectorInfo::isVectorInstruction(const MachineInstr &MI) {
  // First check: opcode name starts with V or PseudoV
  StringRef OpcodeName = MI.getParent()->getParent()->getTarget()
                            .getMCInstrInfo()->getName(MI.getOpcode());
  
  if (OpcodeName.starts_with("PseudoV") || OpcodeName.starts_with("V"))
    return true;
  
  // Additional check: instruction has vector register operands
  // Vector registers in RISC-V are typically in the V0-V31 range
  for (const MachineOperand &MO : MI.operands()) {
    if (MO.isReg()) {
      Register Reg = MO.getReg();
      // Check if it's a RISC-V vector register
      // In LLVM 20, vector registers are typically >= RISCV::V0
      // You may need to adjust this based on your exact LLVM 20 build
      if (Reg.isPhysical() && Reg >= RISCV::V0 && Reg <= RISCV::V31)
        return true;
    }
  }
  
  return false;
}

bool RISCVVectorInfo::isLoadCategory(VecInstrCategory Category) {
  switch (Category) {
  case VecInstrCategory::LoadUnit:
    case VecInstrCategory::LoadStrided:
  case VecInstrCategory::LoadIndexedOrdered:
  case VecInstrCategory::LoadIndexedUnordered:
  case VecInstrCategory::LoadSegmented:
  case VecInstrCategory::LoadSegmentedStrided:
  case VecInstrCategory::LoadSegmentedIndexedOrdered:
  case VecInstrCategory::LoadSegmentedIndexedUnordered:
  case VecInstrCategory::LoadFaultOnlyFirst:
  case VecInstrCategory::LoadSegmentedFaultOnlyFirst:
  case VecInstrCategory::LoadMask:
  case VecInstrCategory::LoadWholeReg:
    return true;
  default:
    return false;
  }
}

bool RISCVVectorInfo::isStoreCategory(VecInstrCategory Category) {
  switch (Category) {
  case VecInstrCategory::StoreUnit:
  case VecInstrCategory::StoreStrided:
  case VecInstrCategory::StoreIndexedOrdered:
  case VecInstrCategory::StoreIndexedUnordered:
  case VecInstrCategory::StoreSegmented:
  case VecInstrCategory::StoreSegmentedStrided:
  case VecInstrCategory::StoreSegmentedIndexedOrdered:
  case VecInstrCategory::StoreSegmentedIndexedUnordered:
  case VecInstrCategory::StoreMask:
  case VecInstrCategory::StoreWholeReg:
    return true;
  default:
    return false;
  }
}

// Validate classification against MachineInstr flags
bool RISCVVectorInfo::validateWithTSFlags(const MachineInstr &MI, 
                                          VecInstrCategory Category) {
  if (!EnableTSFlagsValidation)
    return true;
  
  // Skip validation for config instructions
  if (Category == VecInstrCategory::Config || 
      Category == VecInstrCategory::Unknown)
    return true;
  
  // Check load/store consistency using mayLoad() and mayStore()
  bool IsLoad = isLoadCategory(Category);
  bool IsStore = isStoreCategory(Category);
  
  bool MayLoad = MI.mayLoad();
  bool MayStore = MI.mayStore();
  
  // Validate consistency
  if (IsLoad && !MayLoad) {
    LLVM_DEBUG(dbgs() << "Warning: Classified as load but mayLoad() is false: " 
                      << MI << "\n");
    NumVecClassificationMismatches++;
    return false;
  }
  
  if (IsStore && !MayStore) {
    LLVM_DEBUG(dbgs() << "Warning: Classified as store but mayStore() is false: " 
                      << MI << "\n");
    NumVecClassificationMismatches++;
    return false;
  }
  
  // Validate it's actually a vector instruction
  if (!isVectorInstruction(MI)) {
    LLVM_DEBUG(dbgs() << "Warning: Classified as vector but doesn't appear to be a vector instruction: " 
                      << MI << "\n");
    NumVecClassificationMismatches++;
    return false;
  }
  
  return true;
}

// Classify vector instruction by opcode name
// IMPORTANT: Check most specific patterns first!
VecInstrCategory RISCVVectorInfo::classifyVectorInstr(StringRef OpcodeName) {
  StringRef Name = normalizeOpcodeName(OpcodeName);
  
  // Not a vector instruction
  if (!Name.starts_with("V"))
    return VecInstrCategory::Unknown;

  // Vector configuration instructions
  if (Name.starts_with("VSETVLI") || Name.starts_with("VSETIVLI") || 
      Name.starts_with("VSETVL"))
    return VecInstrCategory::Config;
  // Whole register loads/stores (check before other patterns)
  if (Name.starts_with("VL1R") || Name.starts_with("VL2R") || 
      Name.starts_with("VL4R") || Name.starts_with("VL8R"))
    return VecInstrCategory::LoadWholeReg;
  
  if (Name.starts_with("VS1R") || Name.starts_with("VS2R") || 
      Name.starts_with("VS4R") || Name.starts_with("VS8R"))
    return VecInstrCategory::StoreWholeReg;

  // LOADS - Check segmented before non-segmented!
  
  // Segmented indexed unordered loads
  if (Name.starts_with("VLUXSEG"))
    return VecInstrCategory::LoadSegmentedIndexedUnordered;
  
  // Segmented indexed ordered loads
  if (Name.starts_with("VLOXSEG"))
    return VecInstrCategory::LoadSegmentedIndexedOrdered;
  
  // Segmented fault-only-first loads
  if (Name.starts_with("VLSEGEFF"))
    return VecInstrCategory::LoadSegmentedFaultOnlyFirst;
  
  // Segmented strided loads
  if (Name.starts_with("VLSSEG"))
    return VecInstrCategory::LoadSegmentedStrided;
  
  // Segmented unit-stride loads
  if (Name.starts_with("VLSEG"))
    return VecInstrCategory::LoadSegmented;
  
  // Indexed unordered loads
  if (Name.starts_with("VLUXEI"))
    return VecInstrCategory::LoadIndexedUnordered;
  
  // Indexed ordered loads
  if (Name.starts_with("VLOXEI"))
    return VecInstrCategory::LoadIndexedOrdered;
  
  // Fault-only-first loads
  if (Name.starts_with("VLEFF"))
    return VecInstrCategory::LoadFaultOnlyFirst;
  
  // Strided loads
  if (Name.starts_with("VLSE"))
    return VecInstrCategory::LoadStrided;
  
  // Mask loads
  if (Name.starts_with("VLM"))
    return VecInstrCategory::LoadMask;
  
  // Unit-stride loads
  if (Name.starts_with("VLE"))
    return VecInstrCategory::LoadUnit;

  // STORES - Check segmented before non-segmented!
  
  // Segmented indexed unordered stores
  if (Name.starts_with("VSUXSEG"))
    return VecInstrCategory::StoreSegmentedIndexedUnordered;
  
  // Segmented indexed ordered stores

  if (Name.starts_with("VSOXSEG"))
    return VecInstrCategory::StoreSegmentedIndexedOrdered;
  
  // Segmented strided stores
  if (Name.starts_with("VSSSEG"))
    return VecInstrCategory::StoreSegmentedStrided;
  
  // Segmented unit-stride stores
  if (Name.starts_with("VSSEG"))
    return VecInstrCategory::StoreSegmented;
  
  // Indexed unordered stores
  if (Name.starts_with("VSUXEI"))
    return VecInstrCategory::StoreIndexedUnordered;
  
  // Indexed ordered stores
  if (Name.starts_with("VSOXEI"))
    return VecInstrCategory::StoreIndexedOrdered;
  
  // Strided stores
  if (Name.starts_with("VSSE"))
    return VecInstrCategory::StoreStrided;
  
  // Mask stores
  if (Name.starts_with("VSM"))
    return VecInstrCategory::StoreMask;
  
  // Unit-stride stores
  if (Name.starts_with("VSE"))
    return VecInstrCategory::StoreUnit;

  return VecInstrCategory::Unknown;
}

void RISCVVectorInfo::updateStatistics(VecInstrCategory Category) {
  switch (Category) {
  case VecInstrCategory::Config:
    NumVecConfigInstrs++;
    break;
  
  // Loads
  case VecInstrCategory::LoadUnit:
    NumVecMemInstrs++;
    NumVecLoadInstrs++;
    NumVecLoadElemInstrs++;
    break;
  case VecInstrCategory::LoadStrided:
    NumVecMemInstrs++;
    NumVecLoadInstrs++;
    NumVecLoadStridedInstrs++;
    break;
  case VecInstrCategory::LoadIndexedOrdered:
    NumVecMemInstrs++;
    NumVecLoadInstrs++;
    NumVecLoadIndexedOInstrs++;
    break;
  case VecInstrCategory::LoadIndexedUnordered:
    NumVecMemInstrs++;
    NumVecLoadInstrs++;
    NumVecLoadIndexedUInstrs++;
    break;
  case VecInstrCategory::LoadSegmented:
    NumVecMemInstrs++;
    NumVecLoadInstrs++;
    NumVecLoadSegmentedInstrs++;
    break;
  case VecInstrCategory::LoadSegmentedStrided:
    NumVecMemInstrs++;
    NumVecLoadInstrs++;
    NumVecLoadSegmentedStridedInstrs++;
    break;
  case VecInstrCategory::LoadSegmentedIndexedOrdered:
    NumVecMemInstrs++;
    NumVecLoadInstrs++;
    NumVecLoadSegmentedIndexedOInstrs++;
    break;
  case VecInstrCategory::LoadSegmentedIndexedUnordered:
    NumVecMemInstrs++;
    NumVecLoadInstrs++;
    NumVecLoadSegmentedIndexedUInstrs++;
    break;
  case VecInstrCategory::LoadFaultOnlyFirst:
    NumVecMemInstrs++;
    NumVecLoadInstrs++;
    NumVecLoadFaultInstrs++;
    break;
  case VecInstrCategory::LoadSegmentedFaultOnlyFirst:
    NumVecMemInstrs++;
    NumVecLoadInstrs++;
    NumVecLoadSegmentedFaultInstrs++;
    break;
  case VecInstrCategory::LoadMask:
    NumVecMemInstrs++;
    NumVecLoadInstrs++;
    NumVecLoadMaskInstrs++;
    break;
  case VecInstrCategory::LoadWholeReg:
    NumVecMemInstrs++;
    NumVecLoadInstrs++;
    NumVecLoadWholeRegInstrs++;
    break;
  
  // Stores
  case VecInstrCategory::StoreUnit:
    NumVecMemInstrs++;
    NumVecStoreInstrs++;
    NumVecStoreElemInstrs++;
    break;
  case VecInstrCategory::StoreStrided:
    NumVecMemInstrs++;
    NumVecStoreInstrs++;
    NumVecStoreStridedInstrs++;
    break;
  case VecInstrCategory::StoreIndexedOrdered:
    NumVecMemInstrs++;
    NumVecStoreInstrs++;
    NumVecStoreIndexedOInstrs++;
    break;
  case VecInstrCategory::StoreIndexedUnordered:
    NumVecMemInstrs++;
    NumVecStoreInstrs++;
    NumVecStoreIndexedUInstrs++;
    break;
  case VecInstrCategory::StoreSegmented:
    NumVecMemInstrs++;
    NumVecStoreInstrs++;
    NumVecStoreSegmentedInstrs++;
    break;
  case VecInstrCategory::StoreSegmentedStrided:
    NumVecMemInstrs++;
    NumVecStoreInstrs++;
    NumVecStoreSegmentedStridedInstrs++;
    break;
  case VecInstrCategory::StoreSegmentedIndexedOrdered:
    NumVecMemInstrs++;
    NumVecStoreInstrs++;
    NumVecStoreSegmentedIndexedOInstrs++;
    break;
  case VecInstrCategory::StoreSegmentedIndexedUnordered:
    NumVecMemInstrs++;
    NumVecStoreInstrs++;
    NumVecStoreSegmentedIndexedUInstrs++;
    break;
  case VecInstrCategory::StoreMask:
    NumVecMemInstrs++;
    NumVecStoreInstrs++;
    NumVecStoreMaskInstrs++;
    break;
  case VecInstrCategory::StoreWholeReg:
    NumVecMemInstrs++;
    NumVecStoreInstrs++;
    NumVecStoreWholeRegInstrs++;
    break;
  
  case VecInstrCategory::Unknown:
    break;
  }
}

StringRef RISCVVectorInfo::getCategoryName(VecInstrCategory Category) {
  switch (Category) {
  case VecInstrCategory::Config: return "configuration";
  case VecInstrCategory::LoadUnit: return "unit-stride load";
  case VecInstrCategory::LoadStrided: return "strided load";
  case VecInstrCategory::LoadIndexedOrdered: return "indexed-ordered load";
  case VecInstrCategory::LoadIndexedUnordered: return "indexed-unordered load";
  case VecInstrCategory::LoadSegmented: return "segmented load";
  case VecInstrCategory::LoadSegmentedStrided: return "segmented-strided load";
  case VecInstrCategory::LoadSegmentedIndexedOrdered: return "segmented-indexed-ordered load";
  case VecInstrCategory::LoadSegmentedIndexedUnordered: return "segmented-indexed-unordered load";
  case VecInstrCategory::LoadFaultOnlyFirst: return "fault-only-first load";
  case VecInstrCategory::LoadSegmentedFaultOnlyFirst: return "segmented-fault-only-first load";
  case VecInstrCategory::LoadMask: return "mask load";
  case VecInstrCategory::LoadWholeReg: return "whole-register load";
  case VecInstrCategory::StoreUnit: return "unit-stride store";
  case VecInstrCategory::StoreStrided: return "strided store";
  case VecInstrCategory::StoreIndexedOrdered: return "indexed-ordered store";
  case VecInstrCategory::StoreIndexedUnordered: return "indexed-unordered store";
  case VecInstrCategory::StoreSegmented: return "segmented store";
  case VecInstrCategory::StoreSegmentedStrided: return "segmented-strided store";
  case VecInstrCategory::StoreSegmentedIndexedOrdered: return "segmented-indexed-ordered store";
  case VecInstrCategory::StoreSegmentedIndexedUnordered: return "segmented-indexed-unordered store";
  case VecInstrCategory::StoreMask: return "mask store";
  case VecInstrCategory::StoreWholeReg: return "whole-register store";
  default: return "unknown";
  }
}

bool RISCVVectorInfo::detectAndClassifyVectorInstr(const MachineInstr &MI, 
                                                    const MachineFunction &MF) {
  StringRef OpcodeName = MF.getTarget().getMCInstrInfo()->getName(MI.getOpcode());
  VecInstrCategory Category = classifyVectorInstr(OpcodeName);
  
  if (Category == VecInstrCategory::Unknown)
    return false;
  
  // Validate classification against instruction flags
  bool IsValid = validateWithTSFlags(MI, Category);
  
  // Update statistics (even if validation failed, for debugging purposes)
  updateStatistics(Category);
  
  // Print debug information
  LLVM_DEBUG(dbgs() << "Vector " << getCategoryName(Category) << " instruction"
                    << (IsValid ? "" : " [VALIDATION FAILED]")
                    << ": " << OpcodeName << " | " << MI << "\n");
  
  return true;
}

bool RISCVVectorInfo::runOnMachineFunction(MachineFunction &MF) {
  if (!EnableRISCVVectorInfo)
    return false;
  
  LLVM_DEBUG(dbgs() << "Running RISC-V Vector Info Pass on function: " 
                    << MF.getName() << "\n");
  for (auto &MBB : MF) {
    NumMachineBBs++;
    LLVM_DEBUG(dbgs() << "\n\nMachineBasicBlock " << MBB.getName() 
                      << "." << MBB.getNumber() << ":\n");
    
    for (auto &MI : MBB) {
      NumMachineInstrs++;

      // Detect vector instructions
      if (detectAndClassifyVectorInstr(MI, MF)) {
        NumVecInstrs++;
      }
      // Optional: Track scalar memory operations for comparison
      else if (MI.mayLoad() || MI.mayStore()) {
        NumScalarMemInstrs++;
        if (MI.mayLoad())
          NumScalarLoadInstrs++;
        if (MI.mayStore())
          NumScalarStoreInstrs++;
      }
    }
  }
  
  // Print summary statistics
  LLVM_DEBUG({
    dbgs() << "\n=== Vector Instruction Summary for " << MF.getName() << " ===\n";
    dbgs() << "Total Instructions: " << NumMachineInstrs << "\n";
    dbgs() << "Vector Instructions: " << NumVecInstrs << "\n";
    dbgs() << "Vector Memory Instrs: " << NumVecMemInstrs << "\n";
    dbgs() << "  Vector Loads: " << NumVecLoadInstrs << "\n";
    dbgs() << "  Vector Stores: " << NumVecStoreInstrs << "\n";
    dbgs() << "Vector Config Instrs: " << NumVecConfigInstrs << "\n";
    if (EnableTSFlagsValidation && NumVecClassificationMismatches > 0) {
      dbgs() << "Classification Mismatches: " << NumVecClassificationMismatches << "\n";
    }
  });
  
  return false;
}

} // end of anonymous namespace

char RISCVVectorInfo::ID = 0;

INITIALIZE_PASS(RISCVVectorInfo, "riscv-vector-instr-printer",
                RISCV_VECTOR_INFO_PASS_NAME,
                true, // is CFG only?
                true  // is analysis?
)

namespace llvm {
FunctionPass *createRISCVVectorInfoPass() { return new RISCVVectorInfo(); }
}


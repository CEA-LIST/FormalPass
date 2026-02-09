# A vector memory instruction detection pass: *RISCVVectorInfo.cpp*

This pass detects and classifies vector memory instructions through a function called *`classifyVectorInstr()`* that takes as input the *opcode name* of a given instruction. E.g.: 

`VL1R => Whole register load`

`VLE => Unit-Stride load`

`VLUXSEG => Segmented indexed unordered loads`

`VSSEG  => Segmented unit-stride store`

`VSSE => Strided store`

`VSM => Masked Store`

For each basic block, this pass prints out its vector memory instructions and their category.

```
***   Running RISC-V Vector Info Pass on function: stockham ***
MachineBasicBlock for.body68.14:
Vector unit-stride load instruction: PseudoVLE64_V_M1 | %41:vr = PseudoVLE64_V_M1 $noreg(tied-def 0), killed %40:gpr, 2, 6, 2 :: (load (s128) from %ir.arrayidx72, align 8)
Vector unit-stride store instruction: PseudoVSE64_V_M1 | PseudoVSE64_V_M1 killed %41:vr, killed %38:gpr, 2, 6 :: (store (s128) into %ir.arrayidx70, align 8)

```
This pass also generates for every function of the input application a summary (a count) of its vector memory instructions, if any. 
```
***    Vector Instruction Summary for stockham ***
Total Instructions: 318
Vector Instructions: 2
Vector Memory Instrs: 2
  Vector Loads: 1
  Vector Stores: 1
Vector Config Instrs: 0

```
Additionally, it provides overall statistics for the entire input application.

```
***   Statistics Collected  ***
  34 vector_machine_instr_printer - Number of machine basic blocks
 331 vector_machine_instr_printer - Number of machine instructions
  91 vector_machine_instr_printer - Number of scalar memory load instructions
 130 vector_machine_instr_printer - Number of scalar memory instructions (load/store)
  39 vector_machine_instr_printer - Number of scalar memory store instructions
   2 vector_machine_instr_printer - Number of vector instructions
   1 vector_machine_instr_printer - Number of vector memory load-element instructions
   1 vector_machine_instr_printer - Number of vector memory load instructions
   2 vector_machine_instr_printer - Number of vector memory instructions (load/store)
   1 vector_machine_instr_printer - Number of vector memory store-element instructions
   1 vector_machine_instr_printer - Number of vector memory store instructions

```
To validate the classification and add an extra layer of rigour, we call *`validateWithTSFlags()`*, which makes use of :
  - *`isVectorInstruction()`*, which validates if a given instruction is actually a vector instruction by checking
      - if the *opcode name* starts with *V* or *PseudoV*,
      - if the instruction has vector register operands. 
  - *`mayLoad()`* and *`mayStore()`*, which validate consistency as they check if a given instruction is a load or store instruction respectively.

This validation step can be disabled using a compilation flag (see below).

To invoke this pass during compilation, follow these steps: 
- First, compile your code using Clang with the vectorization flag:

`path-to-llvm-build/bin/clang  -O2 --target=riscv64 -march=rv64gcv   -emit-llvm -I/usr/riscv/sysroot/usr/include/ MyTests/fft.c -c -o MyTests/fft_vec.bc`
- Second, use llc to generate the assembly and don't forget to add the compilation flag for the new pass `-vec-instr-print`. Optionally, to view the overall stats, you can also pass the `-stats` flag. There is also a debug flag, to generate the vector memory instructions's classification for each basic block `-debug-only=vector_machine_instr_printer` :

`path-to-llvm-build/bin/llc -stats -vec-instr-print -debug-only=vector_machine_instr_printer -march=riscv64  MyTests/fft_vec.bc  -o MyTests/fft_vec.s` 

- To disable `TSFlags` validation if needed (it is enabled by default), add this flag: `-vec-instr-validate-tsflags=false` to the llc command line.

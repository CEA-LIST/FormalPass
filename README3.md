# The *RISCVmirToSMTfctCallLoopBound.cpp* pass
A common assumption in SMT solving is to consider loop-free programs without recursion, which is a restrictive class of programs. To avoid this restriction, a common practice is to firstly preprocess the code before transforming it into an SMT model. The pre-processing phase includes, but is not limited to, function inlining, loop unrolling and recursion unwinding so as to obtain a single loop-free function before constructing its SMT formula. 

Since we add a pass at the LLVM MIR level that we call *formal* pass or *SMT* pass that generates an SMT formula from a MIR, we opt to leverage the LLVM MIR infrastructure to flatten the call graph as explained in [README2.md](README2.md) for the *RISCVmirToSMTfctCall.cpp* pass. As for loops, we use the `getAnalysis()` method to gather information about loops from the LLVM class `MachineLoopInfo` in our *RISCVmirToSMTfctCallLoopBound.cpp* pass. This information is used to detect loop headers, basic blocks that belong to loop bodies as well as their loop depth (1 if it's a simple loop, 2 if it's a nested loop, etc.). Once a loop is detected, an SMT variable is created to represent the loop bound. The value of the variable can be determined through [loop bound analysis](https://ieeexplore.ieee.org/abstract/document/4032336) and is set by default in the SMT pass but can be modified in the generated SMT-LIB2 file.   
```
(declare-const loop_bb_1 Int)
(assert (= loop_bb_1 50)) //the default value of the loop bound is set to 50
```
Counters such as memory access counters, instruction counters and number of occurences that are associated to basic blocks that belong to loops are computed taking into consideration the loop bounds and loop depths. For example, the SMT constraint over a memory access counter (`mem_cost`) is the sum of memory counters of individual basic blocks (e.g. `m_0_1` is the number of memory accesses in `bb0` if the transition between `bb0` and `bb1`, `t_0_1`, is taken and 0 otherwise). However, if the basic block belongs to a loop then its memory counter is multiplied by the loop bound (e.g. `bb1` is a loop header and its memory counter `m_1_7` is multiplied by its loop bound `loop_bb_1` as shown in the assertion below).
```
(assert (= mem_cost (+  m_0_1 (* m_7_1 loop_bb_1)  (* m_1_2 loop_bb_1)  (* m_2_3 loop_bb_1)  (* m_5_3  loop_bb_3 loop_bb_1)  (* m_3_4  loop_bb_3 loop_bb_1)  (* m_4_5  loop_bb_3 loop_bb_1)  (* m_3_6  loop_bb_1)  (* m_6_7 loop_bb_1)  m_1_8 m_8_exit)))
``` 

To use the new pass, make sure to pass the following compilation flag to your  `llc` command line: `-smt-generation-fct-call-loop-bound`.

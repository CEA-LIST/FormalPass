#!/bin/bash
# This script takes as input file.bc (llvm bitcode) and generates for each function f its CFG in  .f.dot as well as its graphic representation in f.pdf (or f.png).
# The generated files are stored in a folder called dotFiles.


[ -d dotFiles ] || mkdir dotFiles
cd dotFiles
pwd

#for older LLVM versions ( <= v16) use the following opt flags:
#../../../../../build/bin/opt -dot-cfg-only -disable-output -enable-new-pm=0 ../$1
#to generate a CFG with instructions inside each basic block:
#../../../../../build/bin/opt -dot-cfg -disable-output -enable-new-pm=0 $1

#for newer LLVM versions (e.g. v20) use the following opt flags:
../../../../../build/bin/opt -passes=dot-cfg -disable-output  ../$1

CURRENTDIR=$(dirname $0)
for filename in $CURRENTDIR/.*.dot; do
        basefilename=`cut -d . -f 3 <<< $filename`
        #to generate a pdf file:
        echo "generating $basefilename.pdf from $basefilename.dot in $CURRENTDIR"
        `dot -Tpdf "-o$basefilename.pdf" ".$basefilename.dot"`
         #to generate a png file:
         echo "generating $basefilename.png from $basefilename.dot in $CURRENTDIR"
        `dot -Tpng "-o$basefilename.png" ".$basefilename.dot"`

done
#to visualize the CFG in the pdf file:
#xdg-open f.pdf

#to visualize the CFG in the png file:
#eog f.png
#xdg-open f.pdf





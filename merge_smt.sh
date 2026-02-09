#!/bin/bash

# SMT Files Merger - Bottom-up merging approach
# Usage: ./merge_smt.sh <entry_function_name>
# Example: ./merge_smt.sh main

set -e

if [ $# -ne 1 ]; then
    echo "Usage: $0 <entry_function_name>"
    echo "Example: $0 main"
    exit 1
fi

ENTRY_FUNCTION="$1"
WORK_DIR="work_smt"
FINAL_SMT="${ENTRY_FUNCTION}_merged.smt"

# Create work directory
rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"

# Function to get the call graph depth (for bottom-up processing)
# Returns the maximum depth of the call tree
get_call_depth() {
    local func="$1"
    local visited="$2"
    local txt_file="${func}.txt"

    # Check for cycles
    if echo "$visited" | grep -q "\b$func\b"; then
        echo "0"
        return
    fi

    # If no txt file, this is a leaf function (depth 0)
    if [ ! -f "$txt_file" ]; then
        echo "0"
        return
    fi

    local max_depth=0

    # Read into array to avoid subshell issue
    local call_sites=()
    while IFS= read -r call_site; do
        [ -z "$call_site" ] && continue
        call_sites+=("$call_site")
    done < <(grep -v '^#' "$txt_file")

    # Process each callee
    for call_site in "${call_sites[@]}"; do
        local callee=$(echo "$call_site" | sed -E 's/.*_bb[0-9]+_([a-zA-Z0-9_.]+)_[0-9]+$/\1/')
        local callee_smt="smt_lib2_${callee}.smt"

        # Skip system functions
        [ ! -f "$callee_smt" ] && continue

        local callee_depth=$(get_call_depth "$callee" "$visited $func")

        if [ "$callee_depth" -gt "$max_depth" ]; then
            max_depth="$callee_depth"
        fi
    done

    echo $((max_depth + 1))
}

# Function to merge a callee into a caller
# Args: caller_smt callee_smt call_site_var output_smt
merge_callee_into_caller() {
    local caller_smt="$1"
    local callee_smt="$2"
    local call_site_var="$3"
    local output_smt="$4"

    echo "  Merging $callee_smt into $caller_smt via $call_site_var"

    # Extract bb number from call_site_var (format: func_bbX_callee_N)
    local bb_num=$(echo "$call_site_var" | sed -E 's/.*_bb([0-9]+)_.*/\1/')

    # Start with caller's content (without mem_cost formula and footer)
    grep -v '^(assert (= mem_cost\|^(check-sat\|^(get-model' "$caller_smt" > "$output_smt.tmp"
    echo "" >> "$output_smt.tmp"
    echo "; ========================================" >> "$output_smt.tmp"
    echo "; Inlined callee: $call_site_var" >> "$output_smt.tmp"
    echo "; ========================================" >> "$output_smt.tmp"

    # Add callee's declarations and assertions with proper renaming
    # We need to rename ALL variables including function call variables
    awk -v prefix="${call_site_var}_" '
    # Skip headers and footers
    /^\(set-logic/ || /^\(set-info/ || /^\(set-option/ || /^\(check-sat/ || /^\(get-model/ { next }
    /^\(assert \(= mem_cost/ { next }

    # Function to rename a token if needed
    function rename_token(token) {
        # IMPORTANT: If a token already looks like a prefixed variable from a previous merge,
        # it needs to be renamed too (e.g., fft_bb7_stockham_0_bb0 -> main_bb0_fft_0_fft_bb7_stockham_0_bb0)

        # Check for already-prefixed basic blocks: *_bb[0-9]+
        # Pattern: anything_bb0, anything_bb1, etc.
        if (match(token, /^[a-zA-Z0-9_]+_bb[0-9]+$/)) {
            return prefix token
        }

        # Check for already-prefixed transitions: *_t_*
        if (match(token, /^[a-zA-Z0-9_]+_t_[0-9_a-z]+$/)) {
            return prefix token
        }

        # Check for already-prefixed conditions: *_cond_*
        if (match(token, /^[a-zA-Z0-9_]+_cond_[0-9]+$/)) {
            return prefix token
        }

        # Check for already-prefixed memory vars: *_m_*

        if (match(token, /^[a-zA-Z0-9_]+_m_[0-9_a-z]+$/)) {
            return prefix token
        }

        # Check for already-prefixed loop vars: *_loop_bb_*
        if (match(token, /^[a-zA-Z0-9_]+_loop_bb_[0-9]+$/)) {
            return prefix token
        }

        # Check for already-prefixed mem_cost: *_mem_cost
        if (match(token, /_mem_cost$/)) {
            return prefix token
        }

        # Match basic block names (unprefixed)
        if (match(token, /^bb[0-9]+$/)) {
            return prefix token
        }

        # Match transition variables (unprefixed)
        if (match(token, /^t_[0-9_a-z]+$/)) {
            return prefix token
        }

        # Match condition variables (unprefixed)
        if (match(token, /^cond_[0-9]+$/)) {
            return prefix token
        }

        # Match memory variables (unprefixed)
        if (match(token, /^m_[0-9_a-z]+$/)) {
            return prefix token
        }
        # Match loop variables (unprefixed)
        if (match(token, /^loop_bb_[0-9]+$/)) {
            return prefix token
        }

        # Match mem_cost (unprefixed)
        if (token == "mem_cost") {
            return prefix token
        }

        # Match function call variables (func_bbX_callee_N)
        # This catches things like fft_bb7_stockham_0 or main_bb0_fft_0_fft_bb7_stockham_0
        if (match(token, /_bb[0-9]+_[a-zA-Z0-9_.]+_[0-9]+$/)) {
            return prefix token
        }

        return token
    }

    {
        line = $0
        result = ""
        token = ""

        # Process character by character
        for (i = 1; i <= length(line); i++) {
            c = substr(line, i, 1)

            # Build tokens (alphanumeric and underscore and dot)
            if (match(c, /[a-zA-Z0-9_.]/)) {
                token = token c
            } else {
                # End of token
                if (token != "") {
                   result = result rename_token(token)
                    token = ""
                }
                result = result c
            }
        }

        # Handle last token
        if (token != "") {
            result = result rename_token(token)
        }

        print result
    }
    ' "$callee_smt" >> "$output_smt.tmp"

    echo "" >> "$output_smt.tmp"
    echo "; ========================================" >> "$output_smt.tmp"
    echo "; Linking constraint" >> "$output_smt.tmp"
    echo "; ========================================" >> "$output_smt.tmp"
    echo "(assert (= ${call_site_var} ${call_site_var}_bb0))" >> "$output_smt.tmp"

    echo "" >> "$output_smt.tmp"
    echo "; ========================================" >> "$output_smt.tmp"
    echo "; Memory cost absorption" >> "$output_smt.tmp"
    echo "; ========================================" >> "$output_smt.tmp"

    # Find all m_*_* variables in the bb that contains the call
    # and create _with_calls versions
    local m_vars=$(grep -oP "(?<=\(assert \(= )m_${bb_num}_[a-z0-9]+(?= )" "$caller_smt" | sort -u)

    for m_var in $m_vars; do
        # Get the original assertion for this m_var
        local original_assert=$(grep "^(assert (= ${m_var} " "$caller_smt")


        # Declare the _with_calls variable
        echo "(declare-fun ${m_var}_with_calls () Int)" >> "$output_smt.tmp"

        # Check if it's an ite expression
        if echo "$original_assert" | grep -q "(ite "; then
            # Extract the transition variable
            local transition_var=$(echo "$original_assert" | sed -E 's/.*\(ite ([a-zA-Z0-9_]+).*/\1/')
            echo "; Memory absorption for ${m_var} (conditional on ${transition_var})" >> "$output_smt.tmp"
            echo "(assert (= ${m_var}_with_calls (ite ${transition_var} (+ ${m_var} ${call_site_var}_mem_cost$
        else
            echo "; Memory absorption for ${m_var}" >> "$output_smt.tmp"
            echo "(assert (= ${m_var}_with_calls (+ ${m_var} ${call_site_var}_mem_cost)))" >> "$output_smt.tm$
        fi
        echo "" >> "$output_smt.tmp"
    done

    echo "; ========================================" >> "$output_smt.tmp"
    echo "; Updated mem_cost formula" >> "$output_smt.tmp"
    echo "; ========================================" >> "$output_smt.tmp"

    # Get the original mem_cost formula and update it
    local mem_cost_formula=$(grep "^(assert (= mem_cost" "$caller_smt")

    # Replace m_BB_* with m_BB_*_with_calls for the bb that has the call
    local updated_formula=$(echo "$mem_cost_formula" | sed -E "s/([[:space:]])m_${bb_num}_([a-z0-9]+)([[:spac$

    echo "$updated_formula" >> "$output_smt.tmp"

    mv "$output_smt.tmp" "$output_smt"
}

# Function to process a function and merge all its callees
# Args: function_name
process_and_merge_function() {
    local func="$1"
    local smt_file="smt_lib2_${func}.smt"
    local txt_file="${func}.txt"
    local output_smt="$WORK_DIR/merged_${func}.smt"

    echo "Processing function: $func"

    # If no txt file or no calls, just copy the original
    if [ ! -f "$txt_file" ]; then
        echo "  No calls found, copying original"
        cp "$smt_file" "$output_smt"
        return
    fi

    # Check if there are any actual calls (non-system functions)
    local has_calls=0
    while IFS= read -r call_site; do
        [ -z "$call_site" ] && continue
        local callee=$(echo "$call_site" | sed -E 's/.*_bb[0-9]+_([a-zA-Z0-9_.]+)_[0-9]+$/\1/')
        local callee_merged="$WORK_DIR/merged_${callee}.smt"
        local callee_orig="smt_lib2_${callee}.smt"

        if [ -f "$callee_merged" ] || [ -f "$callee_orig" ]; then
            has_calls=1
            break
        fi
    done < <(grep -v '^#' "$txt_file")

    if [ "$has_calls" -eq 0 ]; then
        echo "  No non-system calls found, copying original"
        cp "$smt_file" "$output_smt"
        return
    fi


    # Start with the original function
    local current_smt="$smt_file"
    local temp_counter=0

    echo "  Starting with: $current_smt"

    # Process each callee
    while IFS= read -r call_site; do
        [ -z "$call_site" ] && continue

        # Extract callee name
        local callee=$(echo "$call_site" | sed -E 's/.*_bb[0-9]+_([a-zA-Z0-9_.]+)_[0-9]+$/\1/')
        local callee_merged="$WORK_DIR/merged_${callee}.smt"

        # Use merged version if it exists, otherwise use original
        local callee_smt="$callee_merged"
        if [ ! -f "$callee_smt" ]; then
            callee_smt="smt_lib2_${callee}.smt"
        fi

        # Skip if callee doesn't exist (system function)
        if [ ! -f "$callee_smt" ]; then
            echo "  Skipping system function: $callee"
            continue
        fi

        echo "  Merging callee: $callee (via $call_site)"
        echo "    From: $current_smt"
        echo "    With: $callee_smt"

        # Merge this callee into current
        local temp_output="$WORK_DIR/temp_${func}_${temp_counter}.smt"
        echo "    Into: $temp_output"

        merge_callee_into_caller "$current_smt" "$callee_smt" "$call_site" "$temp_output"

        if [ ! -f "$temp_output" ]; then
            echo "    ERROR: temp_output was not created!"
        else
            current_smt="$temp_output"
            temp_counter=$((temp_counter + 1))
            echo "    Success! Updated current_smt to: $current_smt"
        fi
    done < <(grep -v '^#' "$txt_file")

    # Copy final result to output
    echo "  Final current_smt: $current_smt"
    if [ -f "$current_smt" ]; then
        cp "$current_smt" "$output_smt"
        echo "  Created: $output_smt"
    else
        echo "  ERROR: current_smt file not found: $current_smt"
        cp "$smt_file" "$output_smt"
        echo "  Fallback: copied original"
    fi
}

# Function to get all functions in call graph
get_all_functions() {
    local func="$1"
    local visited="$2"
    local txt_file="${func}.txt"

    # Check for cycles
    if echo "$visited" | grep -q "\b$func\b"; then
        return
    fi


    echo "$func"

    [ ! -f "$txt_file" ] && return

    grep -v '^#' "$txt_file" | while IFS= read -r call_site; do
        [ -z "$call_site" ] && continue

        local callee=$(echo "$call_site" | sed -E 's/.*_bb[0-9]+_([a-zA-Z0-9_.]+)_[0-9]+$/\1/')
        local callee_smt="smt_lib2_${callee}.smt"

        [ ! -f "$callee_smt" ] && continue

        get_all_functions "$callee" "$visited $func"
    done
}

# Main script execution
echo "========================================"
echo "SMT Merger Script (Bottom-up approach)"
echo "Entry function: $ENTRY_FUNCTION"
echo "========================================"

# Get all functions in the call graph
all_funcs=$(get_all_functions "$ENTRY_FUNCTION" "" | sort -u)

# Build a depth map
declare -A func_depths
for func in $all_funcs; do
    depth=$(get_call_depth "$func" "")
    func_depths[$func]=$depth
    echo "Function $func: depth $depth"
done
# Sort functions by depth (process leaves first)
sorted_funcs=$(for func in "${!func_depths[@]}"; do
    echo "${func_depths[$func]} $func"
done | sort -n | awk '{print $2}')

echo ""
echo "Processing order (bottom-up):"
for func in $sorted_funcs; do
    echo "  - $func (depth ${func_depths[$func]})"
done
echo ""

# Process functions in bottom-up order
for func in $sorted_funcs; do
    process_and_merge_function "$func"
done
# Copy final merged result
final_merged="$WORK_DIR/merged_${ENTRY_FUNCTION}.smt"

echo ""
echo "Creating final output file..."

if [ ! -f "$final_merged" ]; then
    echo "Error: Final merged file not found: $final_merged"
    echo "Files in work directory:"
    ls -la "$WORK_DIR"
    exit 1
fi

# Add proper header and footer
cat > "$FINAL_SMT" << 'EOF'
(set-logic QF_LIA)
(set-info :smt-lib-version 2.0)
(set-option :produce-models true)

; ========================================
; Merged SMT file for complete program
; ========================================

EOF

grep -v '^(set-logic\|^(set-info\|^(set-option' "$final_merged" >> "$FINAL_SMT"

cat >> "$FINAL_SMT" << 'EOF'

(check-sat)
(get-model)
EOF

echo ""

echo "========================================"
echo "Merge complete!"
echo "Output file: $FINAL_SMT"
echo "Work directory: $WORK_DIR/"
echo "========================================"









# minipx

A small C++17 compiler that translates integer assignment statements into PTX-style assembly.

**Pipeline:** lexer → recursive-descent parser → AST → constant folding / algebraic simplification → PTX-style codegen with virtual registers.

## Build & run
```
g++ -std=c++17 -O2 -o minipx minipx.cpp
./minipx example.txt            # optimized
./minipx example.txt --no-fold  # without optimization
```

## Example
Input: `a = 2 * 3 + x;` → `add.s32 %r2, 6, %r1;` (the constant `2 * 3` is folded at compile time; free variables become `.param` inputs).
On `example.txt`, folding reduces the output from 15 to 10 instructions.

## Grammar
```
stmt   := ID '=' expr ';'
expr   := term (('+'|'-') term)*
term   := factor (('*'|'/') factor)*
factor := NUM | ID | '(' expr ')' | '-' factor
```

## Limitations / next steps
32-bit integers only; no control flow. Planned: IR + CFG, dead code elimination, liveness-based register allocation, `if`/`while` with predicated branches (`setp`, `@p bra`).
